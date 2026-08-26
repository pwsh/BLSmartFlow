// Unity tests for the chamber thermostat's PI step (src/blflow/thermostat.h).
// The header is Arduino-free, so this is the exact code the device runs.
//
// Run with: pio test -e native

#include <unity.h>

#include <math.h>

#include "thermostat.h"

using namespace blsf;

namespace {
// The shipped defaults: 8 % per degC, 0.02 % per degC*s, 5 s period.
const float KP = 8.0f;
const float KI = 0.02f;
const float DT = 5.0f;
}  // namespace

void setUp(void) {}
void tearDown(void) {}

static void test_proportional_response(void)
{
    ThermostatState st;
    thermostatReset(st);
    // 2 degC over the set point with no integral yet: pure kp * e.
    const float out = thermostatStep(st, 47.0f, 45.0f, KP, 0.0f, DT, false);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 16.0f, out);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, st.integral);   // ki == 0 -> no integration
}

static void test_at_setpoint_is_zero_and_below_stays_off(void)
{
    ThermostatState st;
    thermostatReset(st);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, thermostatStep(st, 45.0f, 45.0f, KP, KI, DT, false));
    // Colder than the target: the fan can only make it worse, so it stays off.
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, thermostatStep(st, 30.0f, 45.0f, KP, KI, DT, false));
}

static void test_integral_accumulates_over_time(void)
{
    ThermostatState st;
    thermostatReset(st);
    // kp = 0 isolates the integral term: e = 5 degC for 5 s per step.
    thermostatStep(st, 50.0f, 45.0f, 0.0f, KI, DT, false);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 25.0f, st.integral);
    const float out2 = thermostatStep(st, 50.0f, 45.0f, 0.0f, KI, DT, false);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 50.0f, st.integral);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.0f, out2);          // 0.02 * 50
    // A small steady error that the proportional term alone cannot clear does
    // eventually push the output up - that is the whole point of the I term.
    for (int i = 0; i < 20; i++) thermostatStep(st, 50.0f, 45.0f, 0.0f, KI, DT, false);
    TEST_ASSERT_TRUE(st.integral > 500.0f);
}

static void test_anti_windup_clamps_the_integral(void)
{
    ThermostatState st;
    thermostatReset(st);
    // One enormous step: the raw integral would be 10000, the clamp is 100/ki.
    const float out = thermostatStep(st, 1045.0f, 45.0f, 0.0f, KI, 10.0f, false);
    TEST_ASSERT_FLOAT_WITHIN(0.5f, 100.0f / KI, st.integral);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 100.0f, out);         // output saturates at full
    // And it never grows past the clamp, however long the error lasts.
    for (int i = 0; i < 50; i++) thermostatStep(st, 1045.0f, 45.0f, 0.0f, KI, 10.0f, false);
    TEST_ASSERT_TRUE(st.integral <= 100.0f / KI + 0.001f);
}

static void test_integral_freezes_while_the_door_is_open(void)
{
    ThermostatState st;
    thermostatReset(st);
    thermostatStep(st, 50.0f, 45.0f, KP, KI, DT, false);
    const float before = st.integral;
    TEST_ASSERT_TRUE(before > 0.0f);

    // Door open: the error is real but the fan cannot fix it, so winding up
    // would leave the fan blasting for minutes after the door is shut again.
    for (int i = 0; i < 10; i++) {
        const float out = thermostatStep(st, 50.0f, 45.0f, KP, KI, DT, /*freeze=*/true);
        TEST_ASSERT_FLOAT_WITHIN(0.001f, KP * 5.0f + KI * before, out);   // P still responds
    }
    TEST_ASSERT_FLOAT_WITHIN(0.001f, before, st.integral);
}

static void test_integral_freezes_while_saturated(void)
{
    ThermostatState st;
    thermostatReset(st);
    // A 5 degC error puts the proportional term at 40 %; the integral supplies
    // the rest, so the output reaches 100 % through integration.
    int steps = 0;
    while (thermostatStep(st, 50.0f, 45.0f, KP, KI, DT, false) < 100.0f && steps < 2000) steps++;
    TEST_ASSERT_TRUE_MESSAGE(steps < 2000, "the integral never reached full output");
    const float saturated = st.integral;

    // Once the fan is flat out, more integration buys nothing and only delays
    // the recovery, so the integral must stand still.
    for (int i = 0; i < 50; i++) {
        TEST_ASSERT_FLOAT_WITHIN(0.001f, 100.0f,
                                 thermostatStep(st, 50.0f, 45.0f, KP, KI, DT, false));
    }
    TEST_ASSERT_FLOAT_WITHIN(0.001f, saturated, st.integral);

    // Unwinding must not be blocked by the same rule, or the fan would hang at
    // 100 % long after the chamber came back down.
    const float out = thermostatStep(st, 44.0f, 45.0f, KP, KI, DT, false);
    TEST_ASSERT_TRUE(st.integral < saturated);
    TEST_ASSERT_TRUE(out < 100.0f);
}

static void test_a_saturating_proportional_term_never_winds_up_at_all(void)
{
    ThermostatState st;
    thermostatReset(st);
    // 20 degC over target: kp alone already demands 160 %. There is nothing for
    // the integral to add, so it must never leave zero.
    for (int i = 0; i < 100; i++) {
        TEST_ASSERT_FLOAT_WITHIN(0.001f, 100.0f,
                                 thermostatStep(st, 65.0f, 45.0f, KP, KI, DT, false));
    }
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, st.integral);
}

static void test_setpoint_switch_printing_to_cooldown(void)
{
    ThermostatState st;
    thermostatReset(st);
    // Settled at the print set point: 45.5 degC against a 45 degC target.
    const float printing = thermostatStep(st, 45.5f, 45.0f, KP, KI, DT, false);
    TEST_ASSERT_FLOAT_WITHIN(0.2f, 4.0f, printing);

    // The print ends and the set point drops to the cool-down target. The same
    // chamber temperature is now 10.5 degC too hot, so the fan goes to full.
    ThermostatState cool;
    thermostatReset(cool);
    const float cooldown = thermostatStep(cool, 45.5f, 35.0f, KP, KI, DT, false);
    // e = 10.5 degC -> 8 * 10.5 + 0.02 * (10.5 * 5) = 85.05 %.
    TEST_ASSERT_FLOAT_WITHIN(0.05f, 85.05f, cooldown);
    TEST_ASSERT_TRUE(cooldown > printing);

    // Once the chamber reaches the cool-down target the demand falls away again.
    thermostatReset(cool);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, thermostatStep(cool, 35.0f, 35.0f, KP, KI, DT, false));
}

static void test_degenerate_inputs_are_safe(void)
{
    ThermostatState st;
    thermostatReset(st);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, thermostatStep(st, NAN, 45.0f, KP, KI, DT, false));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, thermostatStep(st, 50.0f, NAN, KP, KI, DT, false));
    // Negative gains are treated as zero rather than inverting the loop.
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, thermostatStep(st, 50.0f, 45.0f, -3.0f, -1.0f, DT, false));
    // A zero dt (two calls in the same millisecond) must not move the integral.
    thermostatReset(st);
    thermostatStep(st, 50.0f, 45.0f, KP, KI, 0.0f, false);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, st.integral);
}

int main(int, char**)
{
    UNITY_BEGIN();
    RUN_TEST(test_proportional_response);
    RUN_TEST(test_at_setpoint_is_zero_and_below_stays_off);
    RUN_TEST(test_integral_accumulates_over_time);
    RUN_TEST(test_anti_windup_clamps_the_integral);
    RUN_TEST(test_integral_freezes_while_the_door_is_open);
    RUN_TEST(test_integral_freezes_while_saturated);
    RUN_TEST(test_a_saturating_proportional_term_never_winds_up_at_all);
    RUN_TEST(test_setpoint_switch_printing_to_cooldown);
    RUN_TEST(test_degenerate_inputs_are_safe);
    return UNITY_END();
}
