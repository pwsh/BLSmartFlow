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
    return UNITY_END();
}
