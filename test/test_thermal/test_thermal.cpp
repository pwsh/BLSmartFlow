// Unity tests for the cooling-rate learning arithmetic (src/blflow/thermal_math.h).
// Arduino-free, so this is the same code the device runs; thermal.cpp only adds
// the timing, the config and the JSON.
//
// Run with: pio test -e native

#include <unity.h>

#include <math.h>

#include "thermal_math.h"

using namespace blsf;

void setUp(void) {}
void tearDown(void) {}

static void test_bucket_rounds_to_nearest_quarter(void)
{
    TEST_ASSERT_EQUAL_UINT8(0, thermalBucket(0.0f));
    TEST_ASSERT_EQUAL_UINT8(0, thermalBucket(12.0f));
    TEST_ASSERT_EQUAL_UINT8(1, thermalBucket(13.0f));
    TEST_ASSERT_EQUAL_UINT8(1, thermalBucket(25.0f));
    TEST_ASSERT_EQUAL_UINT8(2, thermalBucket(50.0f));
    TEST_ASSERT_EQUAL_UINT8(3, thermalBucket(70.0f));
    TEST_ASSERT_EQUAL_UINT8(4, thermalBucket(100.0f));
    TEST_ASSERT_EQUAL_UINT8(4, thermalBucket(140.0f));      // clamped, never out of range
    TEST_ASSERT_EQUAL_UINT8(0, thermalBucket(NAN));
    TEST_ASSERT_EQUAL_UINT8(0, thermalBucket(-5.0f));
    TEST_ASSERT_EQUAL_FLOAT(75.0f, thermalBucketPercent(3));
}

static void test_blend_is_an_ema_seeded_by_the_first_sample(void)
{
    TEST_ASSERT_EQUAL_FLOAT(0.4f, thermalBlend(NAN, 0.4f, 0.3f));      // first sample wins
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.43f, thermalBlend(0.4f, 0.5f, 0.3f));
    TEST_ASSERT_EQUAL_FLOAT(0.4f, thermalBlend(0.4f, NAN, 0.3f));      // NaN sample ignored
    TEST_ASSERT_TRUE(isnan(thermalBlend(NAN, NAN, 0.3f)));
}

// Feeds a synthetic exponential cool-down and returns the last closed sample.
static ThermalSample runCooldown(ThermalWindow& w, float k, float ambient, float startTemp,
                                 float output, bool door, float seconds, float stepSec)
{
    ThermalSample last{false, NAN, NAN, 0, false};
    float t = startTemp;
    for (float s = 0.0f; s <= seconds; s += stepSec) {
        const ThermalSample got =
            thermalFeed(w, s, t, output, door, /*heaterActive=*/false, ambient);
        if (got.valid) last = got;
        // dT/dt = -k*(T-ambient), k in 1/min.
        t += -k / 60.0f * (t - ambient) * stepSec;
    }
    return last;
}

static void test_window_recovers_the_cooling_constant(void)
{
    ThermalWindow w;
    thermalWindowReset(w);
    // 0.5 /min from 60 degC into a 25 degC room, sampled every 5 s for 4 minutes.
    const ThermalSample s = runCooldown(w, 0.5f, 25.0f, 60.0f, 50.0f, false, 240.0f, 5.0f);
    TEST_ASSERT_TRUE(s.valid);
    // A finite-difference fit over a 60 s window is close, not exact.
    TEST_ASSERT_FLOAT_WITHIN(0.06f, 0.5f, s.k);
    TEST_ASSERT_TRUE(s.rateCPerMin < 0.0f);
    TEST_ASSERT_EQUAL_UINT8(2, s.bucket);                   // 50 %
    TEST_ASSERT_FALSE(s.door);
}

static void test_window_needs_sixty_seconds(void)
{
    ThermalWindow w;
    thermalWindowReset(w);
    // 55 s is not enough, however clean the slope is.
    const ThermalSample s = runCooldown(w, 0.5f, 25.0f, 60.0f, 0.0f, false, 55.0f, 5.0f);
    TEST_ASSERT_FALSE(s.valid);
    TEST_ASSERT_TRUE(w.active);
}

static void test_changing_fan_output_closes_the_window(void)
{
    ThermalWindow w;
    thermalWindowReset(w);
    float t = 60.0f;
    ThermalSample closed{false, NAN, NAN, 0, false};
    for (float s = 0.0f; s <= 90.0f; s += 5.0f) {
        // Jump from 0 % to 100 % at 65 s: past the 5 % tolerance, so the window
        // that was measuring "0 %" must be harvested and a new one started.
        const float out = s < 65.0f ? 0.0f : 100.0f;
        const ThermalSample got = thermalFeed(w, s, t, out, false, false, 25.0f);
        if (got.valid) closed = got;
        t += -0.5f / 60.0f * (t - 25.0f) * 5.0f;
    }
    TEST_ASSERT_TRUE(closed.valid);
    TEST_ASSERT_EQUAL_UINT8(0, closed.bucket);              // the *old* bucket
    TEST_ASSERT_EQUAL_FLOAT(100.0f, w.outputRef);           // restarted at the new output
}

static void test_a_wobble_inside_the_tolerance_does_not_break_the_window(void)
{
    ThermalWindow w;
    thermalWindowReset(w);
    float t = 60.0f;
    bool got = false;
    for (float s = 0.0f; s <= 90.0f; s += 5.0f) {
        const float out = 50.0f + (((int)s % 10) ? 4.0f : 0.0f);   // 4 % off, inside the 5 % band
        got |= thermalFeed(w, s, t, out, false, false, 25.0f).valid;
        t += -0.5f / 60.0f * (t - 25.0f) * 5.0f;
    }
    TEST_ASSERT_TRUE(got);
}

static void test_door_change_closes_the_window_and_labels_the_sample(void)
{
    ThermalWindow w;
    thermalWindowReset(w);
    const ThermalSample s = runCooldown(w, 1.2f, 25.0f, 60.0f, 0.0f, /*door=*/true, 120.0f, 5.0f);
    TEST_ASSERT_TRUE(s.valid);
    TEST_ASSERT_TRUE(s.door);

    // Now close the door mid-run: the open-door window is harvested.
    ThermalWindow w2;
    thermalWindowReset(w2);
    float t = 60.0f;
    ThermalSample closed{false, NAN, NAN, 0, false};
    for (float sec = 0.0f; sec <= 90.0f; sec += 5.0f) {
        const bool door = sec < 65.0f;
        const ThermalSample got = thermalFeed(w2, sec, t, 0.0f, door, false, 25.0f);
        if (got.valid) closed = got;
        t += -1.2f / 60.0f * (t - 25.0f) * 5.0f;
    }
    TEST_ASSERT_TRUE(closed.valid);
    TEST_ASSERT_TRUE(closed.door);
    TEST_ASSERT_FALSE(w2.doorRef);
}

static void test_heater_activity_and_unknown_chamber_abort_the_window(void)
{
    ThermalWindow w;
    thermalWindowReset(w);
    runCooldown(w, 0.5f, 25.0f, 60.0f, 0.0f, false, 40.0f, 5.0f);
    TEST_ASSERT_TRUE(w.active);
    // The bed switches on: whatever happens next says nothing about the fan.
    thermalFeed(w, 45.0f, 55.0f, 0.0f, false, /*heaterActive=*/true, 25.0f);
    TEST_ASSERT_FALSE(w.active);

    thermalWindowReset(w);
    runCooldown(w, 0.5f, 25.0f, 60.0f, 0.0f, false, 40.0f, 5.0f);
    thermalFeed(w, 45.0f, NAN, 0.0f, false, false, 25.0f);     // link went away
    TEST_ASSERT_FALSE(w.active);
}

static void test_a_chamber_at_room_temperature_teaches_nothing(void)
{
    ThermalWindow w;
    thermalWindowReset(w);
    // Only 1 degC above ambient: dividing by (T - ambient) would amplify sensor
    // noise into a wild k, so the window must be refused.
    const ThermalSample s = runCooldown(w, 0.5f, 25.0f, 26.0f, 0.0f, false, 300.0f, 5.0f);
    TEST_ASSERT_FALSE(s.valid);
}

static void test_a_warming_chamber_is_not_a_cooling_sample(void)
{
    ThermalWindow w;
    thermalWindowReset(w);
    ThermalSample closed{false, NAN, NAN, 0, false};
    float t = 40.0f;
    for (float s = 0.0f; s <= 200.0f; s += 5.0f) {
        const ThermalSample got = thermalFeed(w, s, t, 0.0f, false, false, 25.0f);
        if (got.valid) closed = got;
        t += 0.05f;                                    // slowly heating up
    }
    TEST_ASSERT_FALSE(closed.valid);
}

static void test_a_long_run_yields_repeated_samples(void)
{
    ThermalWindow w;
    thermalWindowReset(w);
    int n = 0;
    float t = 80.0f;
    for (float s = 0.0f; s <= 900.0f; s += 5.0f) {
        if (thermalFeed(w, s, t, 25.0f, false, false, 25.0f).valid) n++;
        t += -0.3f / 60.0f * (t - 25.0f) * 5.0f;
    }
    // A quarter-hour of steady cooling should contribute a measurement per
    // window, not one giant average - and it stops once the chamber gets close
    // enough to the room that the numbers would be noise.
    TEST_ASSERT_TRUE_MESSAGE(n >= 8, "too few windows harvested");
    TEST_ASSERT_TRUE_MESSAGE(n <= 15, "windows harvested past the useful range");
}

static void test_live_rate_appears_once_the_window_is_long_enough(void)
{
    ThermalWindow w;
    thermalWindowReset(w);
    thermalFeed(w, 0.0f, 60.0f, 0.0f, false, false, 25.0f);
    TEST_ASSERT_TRUE(isnan(w.rateCPerMin));
    thermalFeed(w, 10.0f, 59.0f, 0.0f, false, false, 25.0f);
    TEST_ASSERT_TRUE(isnan(w.rateCPerMin));              // under 20 s, no slope yet
    thermalFeed(w, 30.0f, 57.0f, 0.0f, false, false, 25.0f);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, -6.0f, w.rateCPerMin);   // -3 degC in 30 s
}

static void test_a_clock_that_went_backwards_restarts_the_window(void)
{
    ThermalWindow w;
    thermalWindowReset(w);
    runCooldown(w, 0.5f, 25.0f, 60.0f, 0.0f, false, 40.0f, 5.0f);
    // millis() wrapped after 49 days; a negative span would never close again.
    thermalFeed(w, 0.0f, 55.0f, 0.0f, false, false, 25.0f);
    TEST_ASSERT_TRUE(w.active);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, w.startSec);
}

int main(int, char**)
{
    UNITY_BEGIN();
    RUN_TEST(test_bucket_rounds_to_nearest_quarter);
    RUN_TEST(test_blend_is_an_ema_seeded_by_the_first_sample);
    RUN_TEST(test_window_recovers_the_cooling_constant);
    RUN_TEST(test_window_needs_sixty_seconds);
    RUN_TEST(test_changing_fan_output_closes_the_window);
    RUN_TEST(test_a_wobble_inside_the_tolerance_does_not_break_the_window);
    RUN_TEST(test_door_change_closes_the_window_and_labels_the_sample);
    RUN_TEST(test_heater_activity_and_unknown_chamber_abort_the_window);
    RUN_TEST(test_a_chamber_at_room_temperature_teaches_nothing);
    RUN_TEST(test_a_warming_chamber_is_not_a_cooling_sample);
    RUN_TEST(test_a_long_run_yields_repeated_samples);
    RUN_TEST(test_live_rate_appears_once_the_window_is_long_enough);
    RUN_TEST(test_a_clock_that_went_backwards_restarts_the_window);
    return UNITY_END();
}
