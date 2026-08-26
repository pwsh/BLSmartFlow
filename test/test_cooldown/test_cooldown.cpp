// Unity tests for the post-print cool-down state machine (cooldown_logic.h),
// REWORK-SPEC 17.2. The header is Arduino-free, so this is the exact code the
// device runs - including the safety gate that decides whether a G-code line is
// allowed to leave the device at all.
//
// Run with: pio test -e native

#include <unity.h>

#include <math.h>
#include <string.h>

#include "cooldown_logic.h"

using namespace blsf;

namespace {

const uint32_t SAMPLE = 5000;      // the device's sample period

CooldownRules defaults()
{
    CooldownRules c;
    c.enabled = true;
    c.target = 35;
    c.usePrinterFans = true;       // the interesting half of the feature
    c.auxSpeed = 100;
    c.chamberFanSpeed = 100;
    c.maxMinutes = 30;
    c.gentleFromFilament = true;
    c.ownFan = CD_OWN_THERMOSTAT;
    return c;
}

CooldownInputs idleAt(float chamber)
{
    CooldownInputs in{};
    in.phase = Phase::Finished;
    in.gcodeState = "FINISH";
    in.chamber = chamber;
    in.linkOnline = true;
    in.gentleMaterial = false;
    in.chamberTarget = 50;
    in.target = 35;
    in.materialId = "abs";
    in.command = CooldownCommand::None;
    return in;
}

// Runs one sample and advances the clock, so a test reads as a timeline.
struct Sim {
    CooldownState st;
    CooldownRules cfg;
    uint32_t now;
    Sim() : cfg(defaults()), now(100000) { cooldownReset(st); }
    CooldownActions step(CooldownInputs in)
    {
        const CooldownActions a = cooldownStep(st, in, cfg, now);
        now += SAMPLE;
        return a;
    }
    // Seeds the state machine with a running print, so the next Finished sample
    // is a real *edge*. Without this the very first sample after a reboot would
    // look like a print that had just ended - which is precisely the false start
    // the edge rule exists to prevent.
    void seed()
    {
        CooldownInputs in = idleAt(60.0f);
        in.phase = Phase::Printing;
        in.gcodeState = "RUNNING";
        step(in);
    }
};

}  // namespace

void setUp(void) {}
void tearDown(void) {}

// --- starting ---------------------------------------------------------------

static void test_starts_on_the_finish_edge(void)
{
    Sim s;
    // A print is running: nothing starts, and the phase is remembered.
    CooldownInputs in = idleAt(48.0f);
    in.phase = Phase::Printing;
    in.gcodeState = "RUNNING";
    TEST_ASSERT_FALSE(s.step(in).startedNow);
    TEST_ASSERT_FALSE(s.st.active);

    // ... and now it finishes.
    const CooldownActions a = s.step(idleAt(48.0f));
    TEST_ASSERT_TRUE(a.startedNow);
    TEST_ASSERT_TRUE(s.st.active);
    TEST_ASSERT_EQUAL_UINT8(35, s.st.target);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 48.0f, s.st.startChamber);
    TEST_ASSERT_EQUAL_STRING("abs", s.st.material);
}

static void test_level_alone_does_not_restart_a_stopped_session(void)
{
    Sim s;
    s.seed();
    TEST_ASSERT_TRUE(s.step(idleAt(48.0f)).startedNow);
    CooldownInputs stop = idleAt(48.0f);
    stop.command = CooldownCommand::Stop;
    TEST_ASSERT_TRUE(s.step(stop).stoppedNow);
    TEST_ASSERT_FALSE(s.st.active);
    // The printer is still sitting at FINISH; without the edge rule this would
    // restart on the very next sample, for ever.
    for (int i = 0; i < 5; i++) TEST_ASSERT_FALSE(s.step(idleAt(48.0f)).startedNow);
}

static void test_disabled_means_no_automatic_start(void)
{
    Sim s;
    s.cfg.enabled = false;
    CooldownInputs in = idleAt(48.0f);
    in.phase = Phase::Printing;
    in.gcodeState = "RUNNING";
    s.step(in);
    TEST_ASSERT_FALSE(s.step(idleAt(48.0f)).startedNow);
    // ... but a manual start still works, because the user asked for it.
    CooldownInputs man = idleAt(48.0f);
    man.command = CooldownCommand::Start;
    TEST_ASSERT_TRUE(s.step(man).startedNow);
    TEST_ASSERT_TRUE(s.st.manual);
}

static void test_manual_start_is_refused_while_printing(void)
{
    Sim s;
    CooldownInputs in = idleAt(48.0f);
    in.phase = Phase::Printing;
    in.gcodeState = "RUNNING";
    in.command = CooldownCommand::Start;
    const CooldownActions a = s.step(in);
    TEST_ASSERT_FALSE(a.startedNow);
    TEST_ASSERT_FALSE(s.st.active);
    // Paused counts as printing too - the print still owns the machine.
    TEST_ASSERT_FALSE(cooldownCanStart(Phase::Paused));
    TEST_ASSERT_FALSE(cooldownCanStart(Phase::Preheat));
    TEST_ASSERT_TRUE(cooldownCanStart(Phase::Idle));
    TEST_ASSERT_TRUE(cooldownCanStart(Phase::Finished));
}

// --- commanding the printer's fans ------------------------------------------

static void test_sends_both_fans_on_the_first_sample(void)
{
    Sim s;
    s.seed();
    const CooldownActions a = s.step(idleAt(48.0f));
    TEST_ASSERT_TRUE(a.startedNow);
    TEST_ASSERT_TRUE(a.sendFans);
    TEST_ASSERT_EQUAL_UINT8(100, a.auxPct);
    TEST_ASSERT_EQUAL_UINT8(100, a.chamberPct);
    TEST_ASSERT_TRUE(s.st.everSent);
}

static void test_printer_fans_off_means_nothing_is_ever_sent(void)
{
    Sim s;
    s.cfg.usePrinterFans = false;
    s.seed();
    const CooldownActions a = s.step(idleAt(48.0f));
    TEST_ASSERT_TRUE(a.startedNow);
    TEST_ASSERT_FALSE(a.sendFans);
    for (int i = 0; i < 10; i++) TEST_ASSERT_FALSE(s.step(idleAt(48.0f - i)).sendFans);
    TEST_ASSERT_FALSE(s.st.everSent);
}

static void test_reassert_only_every_thirty_seconds(void)
{
    Sim s;
    s.seed();
    TEST_ASSERT_TRUE(s.step(idleAt(48.0f)).sendFans);       // t = 0
    // 5, 10, 15, 20, 25 s: the value has not changed, so nothing is sent.
    for (int i = 0; i < 5; i++) TEST_ASSERT_FALSE(s.step(idleAt(47.0f)).sendFans);
    // 30 s: the printer may have reset its fans by now, so say it again.
    TEST_ASSERT_TRUE(s.step(idleAt(46.0f)).sendFans);
    for (int i = 0; i < 5; i++) TEST_ASSERT_FALSE(s.step(idleAt(45.0f)).sendFans);
    TEST_ASSERT_TRUE(s.step(idleAt(44.0f)).sendFans);
}

static void test_a_changed_speed_is_sent_at_once(void)
{
    Sim s;
    s.seed();
    TEST_ASSERT_TRUE(s.step(idleAt(48.0f)).sendFans);
    s.cfg.auxSpeed = 60;
    const CooldownActions a = s.step(idleAt(47.0f));
    TEST_ASSERT_TRUE(a.sendFans);                 // no waiting for the 30 s tick
    TEST_ASSERT_EQUAL_UINT8(60, a.auxPct);
    TEST_ASSERT_EQUAL_UINT8(100, a.chamberPct);
}

// --- the safety gate --------------------------------------------------------

static void test_no_gcode_unless_finish_or_idle(void)
{
    TEST_ASSERT_TRUE(cooldownGcodeSafe("FINISH"));
    TEST_ASSERT_TRUE(cooldownGcodeSafe("IDLE"));
    TEST_ASSERT_FALSE(cooldownGcodeSafe("RUNNING"));
    TEST_ASSERT_FALSE(cooldownGcodeSafe("PAUSE"));
    TEST_ASSERT_FALSE(cooldownGcodeSafe("PREPARE"));
    TEST_ASSERT_FALSE(cooldownGcodeSafe(""));
    TEST_ASSERT_FALSE(cooldownGcodeSafe(nullptr));

    // A session that is running with an unknown state (a printer that has not
    // reported since boot) keeps the device's own fan going but must not put a
    // single M106 on the wire.
    Sim s;
    CooldownInputs man = idleAt(48.0f);
    man.phase = Phase::Idle;
    man.gcodeState = "";
    man.command = CooldownCommand::Start;
    CooldownActions a = s.step(man);
    TEST_ASSERT_TRUE(a.startedNow);
    TEST_ASSERT_FALSE(a.sendFans);

    CooldownInputs run = idleAt(47.0f);
    run.phase = Phase::Idle;
    run.gcodeState = "";
    for (int i = 0; i < 12; i++) {
        a = s.step(run);
        TEST_ASSERT_FALSE(a.sendFans);
        TEST_ASSERT_FALSE(a.sendStop);
    }
    TEST_ASSERT_TRUE(s.st.active);
    TEST_ASSERT_FALSE(s.st.everSent);
}

static void test_idle_state_is_a_valid_place_to_send(void)
{
    Sim s;
    CooldownInputs man = idleAt(48.0f);
    man.phase = Phase::Idle;
    man.gcodeState = "IDLE";
    man.command = CooldownCommand::Start;
    TEST_ASSERT_TRUE(s.step(man).sendFans);
}

// --- the gentle rule --------------------------------------------------------

static void test_gentle_holds_the_fans_until_ten_below_the_chamber_target(void)
{
    Sim s;
    s.seed();
    CooldownInputs in = idleAt(48.0f);
    in.gentleMaterial = true;
    in.chamberTarget = 50;          // gate at 40 degC

    CooldownActions a = s.step(in);
    TEST_ASSERT_TRUE(a.startedNow);
    TEST_ASSERT_FALSE(a.sendFans);  // 48 is nowhere near cool enough

    in.chamber = 41.0f;
    TEST_ASSERT_FALSE(s.step(in).sendFans);
    in.chamber = 40.0f;             // exactly at the gate is still too hot
    TEST_ASSERT_FALSE(s.step(in).sendFans);

    in.chamber = 39.5f;
    a = s.step(in);
    TEST_ASSERT_TRUE(a.sendFans);
    // ... and then only at half throttle, because the part is still soft.
    TEST_ASSERT_EQUAL_UINT8(50, a.auxPct);
    TEST_ASSERT_EQUAL_UINT8(50, a.chamberPct);
}

static void test_gentle_can_be_switched_off(void)
{
    Sim s;
    s.cfg.gentleFromFilament = false;
    s.seed();
    CooldownInputs in = idleAt(48.0f);
    in.gentleMaterial = true;
    in.chamberTarget = 50;
    const CooldownActions a = s.step(in);
    TEST_ASSERT_TRUE(a.sendFans);
    TEST_ASSERT_EQUAL_UINT8(100, a.auxPct);
}

// --- stopping ---------------------------------------------------------------

static void test_stops_when_the_target_is_held_for_two_samples(void)
{
    Sim s;
    s.seed();
    TEST_ASSERT_TRUE(s.step(idleAt(48.0f)).startedNow);
    // One reading at the target is not enough - a probe can dip.
    CooldownActions a = s.step(idleAt(34.5f));
    TEST_ASSERT_FALSE(a.stoppedNow);
    a = s.step(idleAt(36.0f));               // back above: the count resets
    TEST_ASSERT_FALSE(a.stoppedNow);
    a = s.step(idleAt(34.9f));
    TEST_ASSERT_FALSE(a.stoppedNow);
    a = s.step(idleAt(34.8f));
    TEST_ASSERT_TRUE(a.stoppedNow);
    TEST_ASSERT_EQUAL(CooldownReason::Target, a.reason);
    TEST_ASSERT_TRUE(a.sendStop);            // the fans were on, so release them
    TEST_ASSERT_FALSE(s.st.active);
    TEST_ASSERT_EQUAL_STRING("target", cooldownReasonName(s.st.reason));
}

static void test_stops_at_the_time_limit(void)
{
    Sim s;
    s.cfg.maxMinutes = 1;
    s.seed();
    TEST_ASSERT_TRUE(s.step(idleAt(60.0f)).startedNow);
    CooldownActions a{};
    for (int i = 0; i < 11; i++) {           // 11 samples = 55 s
        a = s.step(idleAt(59.0f));
        TEST_ASSERT_FALSE(a.stoppedNow);
    }
    a = s.step(idleAt(59.0f));               // 60 s
    TEST_ASSERT_TRUE(a.stoppedNow);
    TEST_ASSERT_EQUAL(CooldownReason::Timeout, a.reason);
    TEST_ASSERT_TRUE(a.sendStop);
}

static void test_a_new_job_ends_the_session_without_a_stop_command(void)
{
    Sim s;
    s.seed();
    TEST_ASSERT_TRUE(s.step(idleAt(48.0f)).sendFans);
    CooldownInputs in = idleAt(47.0f);
    in.phase = Phase::Preheat;
    in.gcodeState = "RUNNING";
    const CooldownActions a = s.step(in);
    TEST_ASSERT_TRUE(a.stoppedNow);
    TEST_ASSERT_EQUAL(CooldownReason::NewJob, a.reason);
    // The print has just set the fans it wants; S0 would undo that.
    TEST_ASSERT_FALSE(a.sendStop);
    TEST_ASSERT_FALSE(a.sendFans);
}

static void test_leaving_finish_for_a_running_state_alone_ends_it(void)
{
    Sim s;
    s.seed();
    TEST_ASSERT_TRUE(s.step(idleAt(48.0f)).sendFans);
    CooldownInputs in = idleAt(47.0f);
    in.phase = Phase::Finished;      // the phase has not caught up yet
    in.gcodeState = "PREPARE";       // ... but the printer is already working
    const CooldownActions a = s.step(in);
    TEST_ASSERT_TRUE(a.stoppedNow);
    TEST_ASSERT_EQUAL(CooldownReason::NewJob, a.reason);
    TEST_ASSERT_FALSE(a.sendStop);
}

static void test_an_explicit_stop_releases_the_fans(void)
{
    Sim s;
    s.seed();
    TEST_ASSERT_TRUE(s.step(idleAt(48.0f)).sendFans);
    CooldownInputs in = idleAt(47.0f);
    in.command = CooldownCommand::Stop;
    const CooldownActions a = s.step(in);
    TEST_ASSERT_TRUE(a.stoppedNow);
    TEST_ASSERT_EQUAL(CooldownReason::Stopped, a.reason);
    TEST_ASSERT_TRUE(a.sendStop);
}

static void test_stop_sends_nothing_when_the_fans_were_never_touched(void)
{
    Sim s;
    s.cfg.usePrinterFans = false;
    s.seed();
    s.step(idleAt(48.0f));
    CooldownInputs in = idleAt(47.0f);
    in.command = CooldownCommand::Stop;
    const CooldownActions a = s.step(in);
    TEST_ASSERT_TRUE(a.stoppedNow);
    TEST_ASSERT_FALSE(a.sendStop);
}

static void test_link_loss_ends_the_session_after_the_grace_period(void)
{
    Sim s;
    s.seed();
    TEST_ASSERT_TRUE(s.step(idleAt(48.0f)).sendFans);
    CooldownInputs in = idleAt(47.0f);
    in.linkOnline = false;
    CooldownActions a{};
    for (int i = 0; i < 7; i++) {            // 0 .. 30 s of grace
        a = s.step(in);
        TEST_ASSERT_FALSE(a.stoppedNow);
    }
    a = s.step(in);                          // past 30 s: give up
    TEST_ASSERT_TRUE(a.stoppedNow);
    TEST_ASSERT_EQUAL(CooldownReason::LinkLost, a.reason);
    TEST_ASSERT_FALSE(a.sendStop);           // there is nothing to send it over
}

static void test_a_brief_link_drop_is_survived(void)
{
    Sim s;
    s.seed();
    s.step(idleAt(48.0f));
    CooldownInputs down = idleAt(47.0f);
    down.linkOnline = false;
    s.step(down);
    s.step(down);
    TEST_ASSERT_TRUE(s.st.active);
    s.step(idleAt(46.0f));                   // back up: the timer is forgotten
    TEST_ASSERT_FALSE(s.st.linkLost);
    for (int i = 0; i < 6; i++) s.step(down);
    TEST_ASSERT_TRUE(s.st.active);            // the grace period restarted
}

static void test_switching_the_feature_off_ends_an_automatic_session(void)
{
    Sim s;
    s.seed();
    TEST_ASSERT_TRUE(s.step(idleAt(48.0f)).sendFans);
    s.cfg.enabled = false;
    const CooldownActions a = s.step(idleAt(47.0f));
    TEST_ASSERT_TRUE(a.stoppedNow);
    TEST_ASSERT_EQUAL(CooldownReason::Disabled, a.reason);
    TEST_ASSERT_TRUE(a.sendStop);
}

static void test_switching_the_feature_off_leaves_a_manual_session_alone(void)
{
    Sim s;
    s.cfg.enabled = false;
    CooldownInputs man = idleAt(48.0f);
    man.command = CooldownCommand::Start;
    TEST_ASSERT_TRUE(s.step(man).startedNow);
    for (int i = 0; i < 4; i++) TEST_ASSERT_FALSE(s.step(idleAt(47.0f)).stoppedNow);
    TEST_ASSERT_TRUE(s.st.active);
}

// --- odds and ends ----------------------------------------------------------

static void test_unknown_chamber_never_reaches_the_target(void)
{
    Sim s;
    CooldownInputs man = idleAt(NAN);
    man.command = CooldownCommand::Start;
    TEST_ASSERT_TRUE(s.step(man).startedNow);
    for (int i = 0; i < 10; i++) TEST_ASSERT_FALSE(s.step(idleAt(NAN)).stoppedNow);
    TEST_ASSERT_TRUE(s.st.active);
}

static void test_remaining_seconds_count_down(void)
{
    Sim s;
    s.seed();
    s.cfg.maxMinutes = 10;
    s.step(idleAt(48.0f));
    TEST_ASSERT_EQUAL_UINT32(600, cooldownRemainingSec(s.st, s.cfg, s.st.startedMs));
    TEST_ASSERT_EQUAL_UINT32(540, cooldownRemainingSec(s.st, s.cfg, s.st.startedMs + 60000));
    TEST_ASSERT_EQUAL_UINT32(0, cooldownRemainingSec(s.st, s.cfg, s.st.startedMs + 900000));
}

static void test_own_fan_names_round_trip(void)
{
    TEST_ASSERT_EQUAL_STRING("thermostat", cooldownOwnFanName(CD_OWN_THERMOSTAT));
    TEST_ASSERT_EQUAL_STRING("max", cooldownOwnFanName(CD_OWN_MAX));
    TEST_ASSERT_EQUAL_STRING("curve", cooldownOwnFanName(CD_OWN_CURVE));
    TEST_ASSERT_EQUAL_UINT8(CD_OWN_MAX, cooldownOwnFanFromName("max"));
    TEST_ASSERT_EQUAL_UINT8(CD_OWN_CURVE, cooldownOwnFanFromName("curve"));
    TEST_ASSERT_EQUAL_UINT8(CD_OWN_THERMOSTAT, cooldownOwnFanFromName("thermostat"));
    // Anything else keeps the caller's fallback rather than silently changing it.
    TEST_ASSERT_EQUAL_UINT8(CD_OWN_MAX, cooldownOwnFanFromName("nonsense", CD_OWN_MAX));
    TEST_ASSERT_EQUAL_UINT8(CD_OWN_MAX, cooldownOwnFanFromName(nullptr, CD_OWN_MAX));
}

static void test_reason_names_match_the_api(void)
{
    TEST_ASSERT_EQUAL_STRING("", cooldownReasonName(CooldownReason::None));
    TEST_ASSERT_EQUAL_STRING("target", cooldownReasonName(CooldownReason::Target));
    TEST_ASSERT_EQUAL_STRING("timeout", cooldownReasonName(CooldownReason::Timeout));
    TEST_ASSERT_EQUAL_STRING("newJob", cooldownReasonName(CooldownReason::NewJob));
    TEST_ASSERT_EQUAL_STRING("stopped", cooldownReasonName(CooldownReason::Stopped));
    TEST_ASSERT_EQUAL_STRING("linkLost", cooldownReasonName(CooldownReason::LinkLost));
    TEST_ASSERT_EQUAL_STRING("disabled", cooldownReasonName(CooldownReason::Disabled));
}

int main(int, char**)
{
    UNITY_BEGIN();
    RUN_TEST(test_starts_on_the_finish_edge);
    RUN_TEST(test_level_alone_does_not_restart_a_stopped_session);
    RUN_TEST(test_disabled_means_no_automatic_start);
    RUN_TEST(test_manual_start_is_refused_while_printing);
    RUN_TEST(test_sends_both_fans_on_the_first_sample);
    RUN_TEST(test_printer_fans_off_means_nothing_is_ever_sent);
    RUN_TEST(test_reassert_only_every_thirty_seconds);
    RUN_TEST(test_a_changed_speed_is_sent_at_once);
    RUN_TEST(test_no_gcode_unless_finish_or_idle);
    RUN_TEST(test_idle_state_is_a_valid_place_to_send);
    RUN_TEST(test_gentle_holds_the_fans_until_ten_below_the_chamber_target);
    RUN_TEST(test_gentle_can_be_switched_off);
    RUN_TEST(test_stops_when_the_target_is_held_for_two_samples);
    RUN_TEST(test_stops_at_the_time_limit);
    RUN_TEST(test_a_new_job_ends_the_session_without_a_stop_command);
    RUN_TEST(test_leaving_finish_for_a_running_state_alone_ends_it);
    RUN_TEST(test_an_explicit_stop_releases_the_fans);
    RUN_TEST(test_stop_sends_nothing_when_the_fans_were_never_touched);
    RUN_TEST(test_link_loss_ends_the_session_after_the_grace_period);
    RUN_TEST(test_a_brief_link_drop_is_survived);
    RUN_TEST(test_switching_the_feature_off_ends_an_automatic_session);
    RUN_TEST(test_switching_the_feature_off_leaves_a_manual_session_alone);
    RUN_TEST(test_unknown_chamber_never_reaches_the_target);
    RUN_TEST(test_remaining_seconds_count_down);
    RUN_TEST(test_own_fan_names_round_trip);
    RUN_TEST(test_reason_names_match_the_api);
    return UNITY_END();
}
