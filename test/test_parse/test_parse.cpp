// Unity tests for the Bambu report parser (src/blflow/printer_parse.h) against
// the fixtures captured from a live X1C. printer_parse.h is Arduino-free, so the
// same code the device runs is exercised here on the host.
//
// Run with: pio test -e native

#include <unity.h>

#include <math.h>
#include <stdio.h>
#include <string>
#include <vector>

#include "printer_parse.h"

using namespace blsf;

namespace {

std::string loadFixture(const char* name)
{
    // PlatformIO runs the test binary from the project root, but do not rely on
    // it: walk a few levels up so the test also works when invoked by hand.
    static const char* kPrefixes[] = {
        "test/fixtures/", "../test/fixtures/", "../../test/fixtures/", "../../../test/fixtures/",
    };
    for (const char* prefix : kPrefixes) {
        const std::string path = std::string(prefix) + name;
        FILE* f = fopen(path.c_str(), "rb");
        if (!f) continue;
        std::string out;
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
        fclose(f);
        return out;
    }
    return std::string();
}

// Parses a fixture through the production filter + parser.
bool parseFixture(const std::string& json, PrinterReport& out)
{
    JsonDocument filter;
    buildPrinterFilter(filter);
    JsonDocument doc;
    DeserializationError err =
        deserializeJson(doc, json.c_str(), json.size(), DeserializationOption::Filter(filter));
    TEST_ASSERT_FALSE_MESSAGE(err, err.c_str());
    return parsePrinterReport(doc.as<JsonVariantConst>(), out);
}

PrinterReport g_status;
bool g_statusParsed = false;

}  // namespace

void setUp(void) {}
void tearDown(void) {}

static void test_fixtures_present(void)
{
    TEST_ASSERT_TRUE_MESSAGE(loadFixture("x1c_push_status.json").size() > 100,
                             "test/fixtures/x1c_push_status.json not found");
    TEST_ASSERT_TRUE(loadFixture("x1c_gcode_line.json").size() > 10);
    TEST_ASSERT_TRUE(loadFixture("x1c_gcode_line_rejected.json").size() > 10);
}

static void test_push_status_is_accepted(void)
{
    printerReportInit(g_status);
    g_statusParsed = parseFixture(loadFixture("x1c_push_status.json"), g_status);
    TEST_ASSERT_TRUE(g_statusParsed);
}

static void test_gcode_line_ack_is_ignored(void)
{
    PrinterReport r;
    printerReportInit(r);
    r.progress = 42;                       // must survive an ignored message
    TEST_ASSERT_FALSE(parseFixture(loadFixture("x1c_gcode_line.json"), r));
    TEST_ASSERT_EQUAL_INT(42, r.progress);
}

static void test_packed_temperature_decode(void)
{
    // Values observed on the wire: extruder 140/140, bed 120/120, ctc 43/0.
    TEST_ASSERT_EQUAL_FLOAT(140.0f, packedCurrent(9175180u));
    TEST_ASSERT_EQUAL_FLOAT(140.0f, packedTarget(9175180u));
    TEST_ASSERT_EQUAL_FLOAT(120.0f, packedCurrent(7864440u));
    TEST_ASSERT_EQUAL_FLOAT(120.0f, packedTarget(7864440u));
    TEST_ASSERT_EQUAL_FLOAT(43.0f, packedCurrent(43u));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, packedTarget(43u));
}

static void test_classic_temperatures(void)
{
    TEST_ASSERT_TRUE(g_statusParsed);
    TEST_ASSERT_EQUAL_FLOAT(140.0f, g_status.nozzle);
    TEST_ASSERT_EQUAL_FLOAT(140.0f, g_status.nozzleTarget);
    TEST_ASSERT_EQUAL_FLOAT(120.0f, g_status.bed);
    TEST_ASSERT_EQUAL_FLOAT(120.0f, g_status.bedTarget);
}

static void test_chamber_comes_from_device_ctc(void)
{
    // This X1C firmware has no chamber_temper key at all.
    TEST_ASSERT_TRUE(g_statusParsed);
    TEST_ASSERT_FALSE(isnan(g_status.chamber));
    TEST_ASSERT_EQUAL_FLOAT(43.0f, g_status.chamber);
}

static void test_device_block_is_fallback_for_all_models(void)
{
    // Strip the classic keys; the packed device.* values must fill in.
    std::string json = loadFixture("x1c_push_status.json");
    JsonDocument doc;
    TEST_ASSERT_FALSE(deserializeJson(doc, json));
    JsonObject print = doc["print"];
    print.remove("nozzle_temper");
    print.remove("nozzle_target_temper");
    print.remove("bed_temper");
    print.remove("bed_target_temper");

    std::string stripped;
    serializeJson(doc, stripped);

    PrinterReport r;
    printerReportInit(r);
    TEST_ASSERT_TRUE(parseFixture(stripped, r));
    TEST_ASSERT_EQUAL_FLOAT(140.0f, r.nozzle);
    TEST_ASSERT_EQUAL_FLOAT(140.0f, r.nozzleTarget);
    TEST_ASSERT_EQUAL_FLOAT(120.0f, r.bed);
    TEST_ASSERT_EQUAL_FLOAT(120.0f, r.bedTarget);
    TEST_ASSERT_EQUAL_FLOAT(43.0f, r.chamber);
}

static void test_fan_gear_strings(void)
{
    TEST_ASSERT_TRUE(g_statusParsed);
    TEST_ASSERT_EQUAL_INT(0, g_status.fanPart);        // cooling_fan_speed "0"
    TEST_ASSERT_EQUAL_INT(40, g_status.fanAux);        // big_fan1_speed "6"  -> 40 %
    TEST_ASSERT_EQUAL_INT(0, g_status.fanChamber);     // big_fan2_speed "0"
    TEST_ASSERT_EQUAL_INT(87, g_status.fanHeatbreak);  // heatbreak_fan_speed "13"
}

static void test_fan_gear_word_is_not_used(void)
{
    // fan_gear read 25600 (0x6400) while big_fan1_speed said "6" (= 40 %).
    // The gear strings win; deriving from fan_gear would report 100 %.
    TEST_ASSERT_TRUE(g_statusParsed);
    TEST_ASSERT_EQUAL_INT(40, g_status.fanAux);
}

static void test_job_fields(void)
{
    TEST_ASSERT_TRUE(g_statusParsed);
    TEST_ASSERT_EQUAL_STRING("RUNNING", g_status.gcodeState);
    TEST_ASSERT_TRUE(reportIsPrinting(g_status));
    TEST_ASSERT_EQUAL_INT(1, g_status.stage);
    TEST_ASSERT_EQUAL_INT(4, g_status.progress);
    TEST_ASSERT_EQUAL_INT(117, g_status.remainingMin);
    TEST_ASSERT_EQUAL_INT(0, g_status.layer);
    TEST_ASSERT_EQUAL_INT(90, g_status.totalLayers);
    TEST_ASSERT_EQUAL_STRING("Example print.3mf", g_status.task);
    TEST_ASSERT_EQUAL_STRING("-32dBm", g_status.wifiSignal);
    TEST_ASSERT_EQUAL_UINT32(0, g_status.printError);
}

static void test_home_flag_read_as_unsigned(void)
{
    // -1058683593 == 0xC0E5C537; bit 23 is set.
    TEST_ASSERT_TRUE(g_statusParsed);
    TEST_ASSERT_TRUE(g_status.doorOpen);

    // And a value with bit 23 clear must read as closed.
    PrinterReport r;
    printerReportInit(r);
    JsonDocument doc;
    doc["print"]["home_flag"] = 0;
    doc["print"]["gcode_state"] = "IDLE";
    TEST_ASSERT_TRUE(parsePrinterReport(doc.as<JsonVariantConst>(), r));
    TEST_ASSERT_FALSE(r.doorOpen);
}

static void test_report_patches_incrementally(void)
{
    // A later, sparse report must only overwrite the keys it carries.
    PrinterReport r;
    printerReportInit(r);
    TEST_ASSERT_TRUE(parseFixture(loadFixture("x1c_push_status.json"), r));

    JsonDocument doc;
    doc["print"]["nozzle_temper"] = 215.5;
    TEST_ASSERT_TRUE(parsePrinterReport(doc.as<JsonVariantConst>(), r));
    TEST_ASSERT_EQUAL_FLOAT(215.5f, r.nozzle);
    TEST_ASSERT_EQUAL_FLOAT(120.0f, r.bed);              // untouched
    TEST_ASSERT_EQUAL_STRING("RUNNING", r.gcodeState);   // untouched
}

static void test_empty_and_malformed_documents(void)
{
    PrinterReport r;
    printerReportInit(r);
    JsonDocument empty;
    TEST_ASSERT_FALSE(parsePrinterReport(empty.as<JsonVariantConst>(), r));

    JsonDocument noPrint;
    noPrint["system"]["command"] = "ledctrl";
    TEST_ASSERT_FALSE(parsePrinterReport(noPrint.as<JsonVariantConst>(), r));

    JsonDocument emptyPrint;
    emptyPrint["print"].to<JsonObject>();
    TEST_ASSERT_FALSE(parsePrinterReport(emptyPrint.as<JsonVariantConst>(), r));

    // Nothing above should have disturbed the "never reported" sentinels.
    TEST_ASSERT_TRUE(isnan(r.nozzle));
    TEST_ASSERT_EQUAL_INT(-1, r.fanPart);
}

// The gating in fan_control (onlyWhilePrinting) and the `printing` flag in the
// status document both come from here, so the exact set matters: an over-broad
// set keeps the fans running through a finished print, an under-broad one stops
// them mid-job.
static void test_is_printing_states(void)
{
    PrinterReport r;
    printerReportInit(r);

    static const char* const kPrinting[] = {"RUNNING", "PAUSE", "PREPARE", "SLICING"};
    for (const char* state : kPrinting) {
        blsf::detail::copyStr(r.gcodeState, sizeof(r.gcodeState), state);
        TEST_ASSERT_TRUE_MESSAGE(reportIsPrinting(r), state);
    }

    static const char* const kNotPrinting[] = {"IDLE", "FINISH", "FAILED", "", "running", "UNKNOWN"};
    for (const char* state : kNotPrinting) {
        blsf::detail::copyStr(r.gcodeState, sizeof(r.gcodeState), state);
        TEST_ASSERT_FALSE_MESSAGE(reportIsPrinting(r), state);
    }
}

// --- REWORK-SPEC 15.1: door edges, chamber target, phase, stage names -------

static PrinterReport blank()
{
    PrinterReport r;
    printerReportInit(r);
    return r;
}

// Builds a minimal report document around the given "print" body.
static bool feed(const char* printBody, PrinterReport& r, uint32_t nowMs = 0)
{
    JsonDocument doc;
    std::string json = std::string("{\"print\":") + printBody + "}";
    DeserializationError err = deserializeJson(doc, json.c_str(), json.size());
    TEST_ASSERT_FALSE_MESSAGE(err, err.c_str());
    return parsePrinterReport(doc.as<JsonVariantConst>(), r, nowMs);
}

static void test_door_first_report_is_state_not_edge(void)
{
    PrinterReport r = blank();
    TEST_ASSERT_FALSE(r.doorRawSeen);
    TEST_ASSERT_FALSE(r.doorKnown);
    // home_flag with bit 23 set: the raw bit says open, but nobody just opened
    // anything - and on some X1C units this bit is simply stuck there.
    TEST_ASSERT_TRUE(feed("{\"home_flag\":8388608}", r, 1000));
    TEST_ASSERT_TRUE(r.doorRawSeen);
    TEST_ASSERT_FALSE_MESSAGE(r.doorKnown, "the first report is not an edge");
    TEST_ASSERT_TRUE(r.doorOpen);                    // the raw bit is recorded
    TEST_ASSERT_FALSE_MESSAGE(reportDoorOpen(r), "an unproven switch must read closed");
    TEST_ASSERT_EQUAL_UINT16(0, r.doorEdgeCount);
    TEST_ASSERT_EQUAL_UINT32(0, r.lastDoorOpenMs);
    TEST_ASSERT_EQUAL_UINT32(0, r.lastDoorCloseMs);
}

static void test_door_stuck_open_never_becomes_known(void)
{
    // Eric's X1C: the closed door does not actuate the switch, so bit 23 reads 1
    // for the whole print. Without the edge rule the fan would sit under the
    // door rule from boot to power-off.
    PrinterReport r = blank();
    for (uint32_t t = 1000; t <= 60000; t += 1000) feed("{\"home_flag\":8388615}", r, t);
    TEST_ASSERT_TRUE(r.doorOpen);
    TEST_ASSERT_FALSE(r.doorKnown);
    TEST_ASSERT_FALSE(reportDoorOpen(r));
    TEST_ASSERT_EQUAL_UINT16(0, r.doorEdgeCount);

    // Pressing the switch by hand produces the first edge; from then on the bit
    // is trustworthy in both directions.
    feed("{\"home_flag\":7}", r, 61000);
    TEST_ASSERT_TRUE(r.doorKnown);
    TEST_ASSERT_FALSE(r.doorOpen);
    TEST_ASSERT_FALSE(reportDoorOpen(r));
    TEST_ASSERT_EQUAL_UINT16(1, r.doorEdgeCount);
    TEST_ASSERT_EQUAL_UINT32(61000, r.lastDoorCloseMs);

    feed("{\"home_flag\":8388615}", r, 62000);
    TEST_ASSERT_TRUE(reportDoorOpen(r));
}

static void test_door_edges_are_counted_and_timestamped(void)
{
    PrinterReport r = blank();
    feed("{\"home_flag\":0}", r, 1000);            // first sighting: closed
    TEST_ASSERT_EQUAL_UINT16(0, r.doorEdgeCount);
    TEST_ASSERT_FALSE(r.doorKnown);

    feed("{\"home_flag\":0}", r, 2000);            // no change, no edge
    TEST_ASSERT_EQUAL_UINT16(0, r.doorEdgeCount);
    TEST_ASSERT_FALSE(r.doorKnown);

    feed("{\"home_flag\":8388608}", r, 3000);      // opened
    TEST_ASSERT_TRUE(r.doorOpen);
    TEST_ASSERT_TRUE(r.doorKnown);
    TEST_ASSERT_TRUE(reportDoorOpen(r));
    TEST_ASSERT_EQUAL_UINT16(1, r.doorEdgeCount);
    TEST_ASSERT_EQUAL_UINT32(3000, r.lastDoorOpenMs);
    TEST_ASSERT_EQUAL_UINT32(0, r.lastDoorCloseMs);

    feed("{\"home_flag\":0}", r, 4500);            // closed again
    TEST_ASSERT_FALSE(r.doorOpen);
    TEST_ASSERT_FALSE(reportDoorOpen(r));
    TEST_ASSERT_EQUAL_UINT16(2, r.doorEdgeCount);
    TEST_ASSERT_EQUAL_UINT32(3000, r.lastDoorOpenMs);
    TEST_ASSERT_EQUAL_UINT32(4500, r.lastDoorCloseMs);

    // Other home_flag bits must not disturb the door bookkeeping.
    feed("{\"home_flag\":7}", r, 5000);
    TEST_ASSERT_FALSE(r.doorOpen);
    TEST_ASSERT_EQUAL_UINT16(2, r.doorEdgeCount);
}

static void test_chamber_target_from_packed_high_word(void)
{
    PrinterReport r = blank();
    // 45 current, 50 target -> (50 << 16) | 45
    feed("{\"device\":{\"ctc\":{\"info\":{\"temp\":3276845}}}}", r);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 45.0f, r.chamber);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 50.0f, r.chamberTarget);

    // A printer with no chamber heater reports a zero high word, which is "no
    // target", not "target 0 degC".
    PrinterReport q = blank();
    feed("{\"device\":{\"ctc\":{\"info\":{\"temp\":43}}}}", q);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 43.0f, q.chamber);
    TEST_ASSERT_TRUE(isnan(q.chamberTarget));
}

static void test_phase_offline_rule(void)
{
    PrinterReport r = blank();
    TEST_ASSERT_EQUAL_INT((int)Phase::Offline, (int)reportPhase(r));          // nothing said yet
    blsf::detail::copyStr(r.gcodeState, sizeof(r.gcodeState), "OFFLINE");
    TEST_ASSERT_EQUAL_INT((int)Phase::Offline, (int)reportPhase(r));
    blsf::detail::copyStr(r.gcodeState, sizeof(r.gcodeState), "RUNNING");
    r.stage = -2;
    TEST_ASSERT_EQUAL_INT((int)Phase::Offline, (int)reportPhase(r));
}

static void test_phase_paused_rule(void)
{
    PrinterReport r = blank();
    blsf::detail::copyStr(r.gcodeState, sizeof(r.gcodeState), "PAUSE");
    TEST_ASSERT_EQUAL_INT((int)Phase::Paused, (int)reportPhase(r));

    // Every pause stage wins even while gcode_state still says RUNNING.
    static const int kPauseStages[] = {5, 6, 16, 17, 20, 21, 23, 26, 27, 28, 30, 32, 33, 34, 35};
    for (int stage : kPauseStages) {
        PrinterReport q = blank();
        blsf::detail::copyStr(q.gcodeState, sizeof(q.gcodeState), "RUNNING");
        q.stage = stage;
        TEST_ASSERT_EQUAL_INT_MESSAGE((int)Phase::Paused, (int)reportPhase(q), stageName(stage));
    }
}

static void test_phase_preheat_rule(void)
{
    static const int kPreheatStages[] = {2, 7, 49, 54, 58, 63, 64};
    for (int stage : kPreheatStages) {
        PrinterReport q = blank();
        blsf::detail::copyStr(q.gcodeState, sizeof(q.gcodeState), "RUNNING");
        q.stage = stage;
        TEST_ASSERT_EQUAL_INT_MESSAGE((int)Phase::Preheat, (int)reportPhase(q), stageName(stage));
    }

    // RUNNING with a bed still more than 3 degC below target is a preheat.
    PrinterReport r = blank();
    blsf::detail::copyStr(r.gcodeState, sizeof(r.gcodeState), "RUNNING");
    r.stage = 0;
    r.bed = 40.0f; r.bedTarget = 100.0f;
    TEST_ASSERT_EQUAL_INT((int)Phase::Preheat, (int)reportPhase(r));
    r.bed = 98.0f;                                   // within 3 degC -> printing
    TEST_ASSERT_EQUAL_INT((int)Phase::Printing, (int)reportPhase(r));

    // Same for the chamber, with a 2 degC band.
    PrinterReport c = blank();
    blsf::detail::copyStr(c.gcodeState, sizeof(c.gcodeState), "RUNNING");
    c.stage = 0;
    c.chamber = 30.0f; c.chamberTarget = 50.0f;
    TEST_ASSERT_EQUAL_INT((int)Phase::Preheat, (int)reportPhase(c));
    c.chamber = 49.0f;
    TEST_ASSERT_EQUAL_INT((int)Phase::Printing, (int)reportPhase(c));

    // An unknown target must never look like a preheat.
    PrinterReport u = blank();
    blsf::detail::copyStr(u.gcodeState, sizeof(u.gcodeState), "RUNNING");
    u.bed = 20.0f;                                   // bedTarget stays NaN
    TEST_ASSERT_EQUAL_INT((int)Phase::Printing, (int)reportPhase(u));
}

static void test_phase_cooling_printing_finished_failed_idle(void)
{
    static const int kCooling[] = {29, 50, 69};
    for (int stage : kCooling) {
        PrinterReport q = blank();
        blsf::detail::copyStr(q.gcodeState, sizeof(q.gcodeState), "RUNNING");
        q.stage = stage;
        TEST_ASSERT_EQUAL_INT_MESSAGE((int)Phase::Cooling, (int)reportPhase(q), stageName(stage));
    }
    struct { const char* state; Phase phase; } kCases[] = {
        {"RUNNING", Phase::Printing}, {"PREPARE", Phase::Printing}, {"SLICING", Phase::Printing},
        {"FINISH", Phase::Finished},  {"FAILED", Phase::Failed},    {"IDLE", Phase::Idle},
        {"INIT", Phase::Idle},        {"running", Phase::Idle},
    };
    for (auto& c : kCases) {
        PrinterReport q = blank();
        blsf::detail::copyStr(q.gcodeState, sizeof(q.gcodeState), c.state);
        TEST_ASSERT_EQUAL_INT_MESSAGE((int)c.phase, (int)reportPhase(q), c.state);
    }
}

static void test_phase_names_round_trip(void)
{
    TEST_ASSERT_EQUAL_STRING("offline",  phaseName(Phase::Offline));
    TEST_ASSERT_EQUAL_STRING("paused",   phaseName(Phase::Paused));
    TEST_ASSERT_EQUAL_STRING("preheat",  phaseName(Phase::Preheat));
    TEST_ASSERT_EQUAL_STRING("cooling",  phaseName(Phase::Cooling));
    TEST_ASSERT_EQUAL_STRING("printing", phaseName(Phase::Printing));
    TEST_ASSERT_EQUAL_STRING("finished", phaseName(Phase::Finished));
    TEST_ASSERT_EQUAL_STRING("failed",   phaseName(Phase::Failed));
    TEST_ASSERT_EQUAL_STRING("idle",     phaseName(Phase::Idle));
}

static void test_printing_matches_phase_definition(void)
{
    // 15.1: printing == phase in {preheat, printing, paused}.
    TEST_ASSERT_TRUE(phaseIsPrinting(Phase::Preheat));
    TEST_ASSERT_TRUE(phaseIsPrinting(Phase::Printing));
    TEST_ASSERT_TRUE(phaseIsPrinting(Phase::Paused));
    TEST_ASSERT_FALSE(phaseIsPrinting(Phase::Cooling));
    TEST_ASSERT_FALSE(phaseIsPrinting(Phase::Finished));
    TEST_ASSERT_FALSE(phaseIsPrinting(Phase::Failed));
    TEST_ASSERT_FALSE(phaseIsPrinting(Phase::Idle));
    TEST_ASSERT_FALSE(phaseIsPrinting(Phase::Offline));

    // A stage-2 preheat counts as printing even before gcode_state moves, which
    // is the whole point of gating on the phase rather than on gcode_state.
    PrinterReport r = blank();
    blsf::detail::copyStr(r.gcodeState, sizeof(r.gcodeState), "IDLE");
    r.stage = 2;
    TEST_ASSERT_TRUE(reportIsPrinting(r));
}

static void test_stage_names(void)
{
    TEST_ASSERT_EQUAL_STRING("idle", stageName(-1));
    TEST_ASSERT_EQUAL_STRING("idle", stageName(255));
    TEST_ASSERT_EQUAL_STRING("offline", stageName(-2));
    TEST_ASSERT_EQUAL_STRING("printing", stageName(0));
    TEST_ASSERT_EQUAL_STRING("heatbed_preheating", stageName(2));
    TEST_ASSERT_EQUAL_STRING("cooling_chamber", stageName(29));
    TEST_ASSERT_EQUAL_STRING("heating_chamber", stageName(49));
    TEST_ASSERT_EQUAL_STRING("heatbed_cooling", stageName(50));
    TEST_ASSERT_EQUAL_STRING("waiting_for_chamber_temperature", stageName(63));
    TEST_ASSERT_EQUAL_STRING("preparing_ams", stageName(77));
    TEST_ASSERT_EQUAL_STRING("unknown", stageName(78));
    TEST_ASSERT_EQUAL_STRING("unknown", stageName(-3));
    // Every code in 0..77 must resolve to something other than "unknown".
    for (int i = 0; i <= 77; i++) {
        TEST_ASSERT_FALSE_MESSAGE(strcmp(stageName(i), "unknown") == 0,
                                  ("stage " + std::to_string(i) + " has no name").c_str());
    }
}

static void test_filter_skips_bulky_blocks(void)
{
    // The filter must not admit ams/ipcam/xcam/net; if it did, a 16 KB report
    // would blow past the parse budget on the device.
    std::string json = loadFixture("x1c_push_status.json");
    JsonDocument filter;
    buildPrinterFilter(filter);
    JsonDocument doc;
    TEST_ASSERT_FALSE(deserializeJson(doc, json.c_str(), json.size(),
                                      DeserializationOption::Filter(filter)));
    JsonObjectConst print = doc["print"];
    TEST_ASSERT_TRUE(print["ams_status"].isNull());
    TEST_ASSERT_TRUE(print["stg"].isNull());
    TEST_ASSERT_TRUE(print["device"]["cam"].isNull());
    TEST_ASSERT_TRUE(print["device"]["nozzle"].isNull());
    // But the keys we do want survive.
    TEST_ASSERT_FALSE(print["device"]["ctc"]["info"]["temp"].isNull());
    TEST_ASSERT_FALSE(print["gcode_state"].isNull());
}

// --- gcode_line acknowledgements (2.0.4) ---------------------------------
// A printer with Developer Mode off keeps reporting but refuses every write
// command. The refusal is the only evidence, and it arrives on the same topic as
// Bambu Studio's own acks - so the sequence id is what decides whether it is
// ours.

// Runs a fixture through the production filter and the ack reader.
static bool ackFixture(const char* name, GcodeAck& ack)
{
    const std::string json = loadFixture(name);
    JsonDocument filter;
    buildPrinterFilter(filter);
    JsonDocument doc;
    DeserializationError err =
        deserializeJson(doc, json.c_str(), json.size(), DeserializationOption::Filter(filter));
    TEST_ASSERT_FALSE_MESSAGE(err, err.c_str());
    return parseGcodeAck(doc.as<JsonVariantConst>(), ack);
}

static void test_rejected_ack_is_decoded(void)
{
    GcodeAck ack;
    TEST_ASSERT_TRUE(ackFixture("x1c_gcode_line_rejected.json", ack));
    TEST_ASSERT_EQUAL_UINT32(5002u, ack.sequenceId);
    TEST_ASSERT_FALSE(ack.ok);
    TEST_ASSERT_EQUAL_UINT32(84033543u, ack.errCode);
    TEST_ASSERT_EQUAL_STRING("mqtt message verify failed", ack.reason);
}

static void test_successful_ack_is_decoded(void)
{
    GcodeAck ack;
    TEST_ASSERT_TRUE(ackFixture("x1c_gcode_line.json", ack));
    TEST_ASSERT_EQUAL_UINT32(2023u, ack.sequenceId);
    TEST_ASSERT_TRUE(ack.ok);                 // "SUCCESS", any case
    TEST_ASSERT_EQUAL_UINT32(0u, ack.errCode);
}

static void test_push_status_is_not_an_ack(void)
{
    GcodeAck ack;
    TEST_ASSERT_FALSE(ackFixture("x1c_push_status.json", ack));
}

// The id filter is the caller's job (printer_link.cpp keeps the ring of ids it
// published); this is the state change it drives on either side of that gate.
static void test_ours_updates_state_and_not_ours_does_not(void)
{
    GcodeAck ack;
    TEST_ASSERT_TRUE(ackFixture("x1c_gcode_line_rejected.json", ack));

    PrinterReport ours;
    printerReportInit(ours);
    TEST_ASSERT_FALSE(reportCommandFailed(ours));
    // sequence_id 5002 is in our range, so printer_link would apply it.
    applyGcodeAck(ours, ack, 123456u);
    TEST_ASSERT_TRUE(reportCommandFailed(ours));
    TEST_ASSERT_EQUAL_STRING("mqtt message verify failed", ours.lastGcodeReason);
    TEST_ASSERT_EQUAL_UINT32(84033543u, ours.lastGcodeErr);
    TEST_ASSERT_EQUAL_UINT32(123456u, ours.lastGcodeMs);

    // Bambu Studio's ack (a low id) is never applied, so the report is untouched.
    PrinterReport theirs;
    printerReportInit(theirs);
    GcodeAck studio;
    TEST_ASSERT_TRUE(ackFixture("x1c_gcode_line.json", studio));
    TEST_ASSERT_TRUE(studio.sequenceId < 5000u);
    TEST_ASSERT_FALSE(reportCommandFailed(theirs));
    TEST_ASSERT_EQUAL_UINT32(0u, theirs.lastGcodeMs);
}

static void test_a_success_clears_a_previous_rejection(void)
{
    PrinterReport r;
    printerReportInit(r);
    GcodeAck bad;
    TEST_ASSERT_TRUE(ackFixture("x1c_gcode_line_rejected.json", bad));
    applyGcodeAck(r, bad, 1000u);
    TEST_ASSERT_TRUE(reportCommandFailed(r));

    GcodeAck good;
    TEST_ASSERT_TRUE(ackFixture("x1c_gcode_line.json", good));
    applyGcodeAck(r, good, 2000u);
    TEST_ASSERT_FALSE(reportCommandFailed(r));
    TEST_ASSERT_EQUAL_STRING("", r.lastGcodeReason);
    TEST_ASSERT_EQUAL_UINT32(0u, r.lastGcodeErr);
}

// An ack still carries no printer state: the report parser must reject it, or a
// refusal would blank out the temperatures.
static void test_a_rejected_ack_is_still_not_a_report(void)
{
    PrinterReport r;
    printerReportInit(r);
    r.progress = 42;
    TEST_ASSERT_FALSE(parseFixture(loadFixture("x1c_gcode_line_rejected.json"), r));
    TEST_ASSERT_EQUAL_INT(42, r.progress);
}

int main(int, char**)
{
    UNITY_BEGIN();
    RUN_TEST(test_fixtures_present);
    RUN_TEST(test_push_status_is_accepted);
    RUN_TEST(test_gcode_line_ack_is_ignored);
    RUN_TEST(test_packed_temperature_decode);
    RUN_TEST(test_classic_temperatures);
    RUN_TEST(test_chamber_comes_from_device_ctc);
    RUN_TEST(test_device_block_is_fallback_for_all_models);
    RUN_TEST(test_fan_gear_strings);
    RUN_TEST(test_fan_gear_word_is_not_used);
    RUN_TEST(test_job_fields);
    RUN_TEST(test_home_flag_read_as_unsigned);
    RUN_TEST(test_report_patches_incrementally);
    RUN_TEST(test_empty_and_malformed_documents);
    RUN_TEST(test_is_printing_states);
    RUN_TEST(test_filter_skips_bulky_blocks);
    RUN_TEST(test_door_first_report_is_state_not_edge);
    RUN_TEST(test_door_stuck_open_never_becomes_known);
    RUN_TEST(test_door_edges_are_counted_and_timestamped);
    RUN_TEST(test_chamber_target_from_packed_high_word);
    RUN_TEST(test_phase_offline_rule);
    RUN_TEST(test_phase_paused_rule);
    RUN_TEST(test_phase_preheat_rule);
    RUN_TEST(test_phase_cooling_printing_finished_failed_idle);
    RUN_TEST(test_phase_names_round_trip);
    RUN_TEST(test_printing_matches_phase_definition);
    RUN_TEST(test_stage_names);
    RUN_TEST(test_rejected_ack_is_decoded);
    RUN_TEST(test_successful_ack_is_decoded);
    RUN_TEST(test_push_status_is_not_an_ack);
    RUN_TEST(test_ours_updates_state_and_not_ours_does_not);
    RUN_TEST(test_a_success_clears_a_previous_rejection);
    RUN_TEST(test_a_rejected_ack_is_still_not_a_report);
    return UNITY_END();
}
