// Unity tests for filament matching and the effective cooling profile
// (src/blflow/filament_match.h, REWORK-SPEC 16.2/16.3), plus the AMS half of the
// report parser, against the real X1C AMS capture in test/fixtures/.
//
// The interesting test is the exhaustive one: every row of
// tools/bambu_filament_ids.csv - the 100 filament ids Bambu Studio ships - must
// resolve to a guide entry, both from its `filament_type` and from the bare id
// alone. Exactly one documented exception is allowed (see kKnownUnmatched).
//
// Run with: pio test -e native

// This test binary carries the generated tables.
#define BLSF_FILAMENT_DB_DEFINE

#include <unity.h>

#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

#include "filament_match.h"
#include "printer_parse.h"

using namespace blsf;

namespace {

std::string loadFile(const char* rel)
{
    static const char* kPrefixes[] = {"", "../", "../../", "../../../"};
    for (const char* prefix : kPrefixes) {
        const std::string path = std::string(prefix) + rel;
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

// The guide has no entry for EVA (a niche foaming/adhesive material). The
// matcher reports family "EVA" with an empty id, the status block shows the
// material name with no profile, and the fan falls back to the configured
// targets - which is the right behaviour, so this is a documented exception
// rather than a bug to work around.
const char* const kKnownUnmatched[] = {"GFR99"};

bool isKnownUnmatched(const std::string& idx)
{
    for (const char* k : kKnownUnmatched) if (idx == k) return true;
    return false;
}

struct CsvRow { std::string idx, name, type; };

std::vector<CsvRow> loadCsv()
{
    std::vector<CsvRow> rows;
    const std::string csv = loadFile("tools/bambu_filament_ids.csv");
    size_t pos = 0;
    bool header = true;
    while (pos < csv.size()) {
        size_t nl = csv.find('\n', pos);
        if (nl == std::string::npos) nl = csv.size();
        std::string line = csv.substr(pos, nl - pos);
        pos = nl + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.empty()) continue;
        if (header) { header = false; continue; }
        const size_t c1 = line.find(',');
        const size_t c2 = line.find(',', c1 + 1);
        if (c1 == std::string::npos || c2 == std::string::npos) continue;
        rows.push_back({line.substr(0, c1), line.substr(c1 + 1, c2 - c1 - 1), line.substr(c2 + 1)});
    }
    return rows;
}

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

// The default policy: filament.auto on, the shipped vent floors, fan.* fallbacks
// at the 2.0.1 defaults.
FilamentPolicy defaultPolicy()
{
    FilamentPolicy p{};
    p.autoEnabled = true;
    p.ventFloorByVent[VENT_OPTIONAL] = 0;
    p.ventFloorByVent[VENT_RECOMMENDED] = 0;
    p.ventFloorByVent[VENT_REQUIRED] = 10;
    p.fanChamberTarget = 45;
    p.fanCooldownTarget = 35;
    p.overrides = nullptr;
    p.overrideCount = 0;
    return p;
}

std::string g_amsFixture;

}  // namespace

void setUp(void) {}
void tearDown(void) {}

// --- the generated table ---------------------------------------------------

static void test_db_is_populated(void)
{
    TEST_ASSERT_GREATER_THAN_UINT16(50, filamentDbCount());
    TEST_ASSERT_GREATER_THAN_UINT16(50, bambuDbCount());
    // Ids are the lookup key and the URL fragment: lower case, no spaces.
    for (uint16_t i = 0; i < filamentDbCount(); i++) {
        const FilamentInfo* f = filamentDbAt(i);
        TEST_ASSERT_NOT_NULL(f);
        TEST_ASSERT_TRUE(f->id[0] != '\0');
        TEST_ASSERT_TRUE(f->name[0] != '\0');
        for (const char* c = f->id; *c; ++c) {
            TEST_ASSERT_TRUE_MESSAGE(*c == '-' || (*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9'),
                                     f->id);
        }
    }
    // The ids the matcher's base map depends on must exist.
    static const char* const kRequired[] = {"pla", "petg", "abs", "asa", "pc", "pa", "tpu",
                                            "pva", "hips", "pps", "pp", "pe", "pha", "pctg"};
    for (const char* id : kRequired) TEST_ASSERT_NOT_NULL_MESSAGE(filamentInfoById(id), id);
    TEST_ASSERT_NULL(filamentInfoById("definitely-not-a-material"));
    TEST_ASSERT_NULL(filamentInfoById(""));
}

// --- REWORK-SPEC 16.4: every Bambu id resolves ----------------------------

static void test_every_bambu_id_resolves_from_its_type(void)
{
    const std::vector<CsvRow> rows = loadCsv();
    TEST_ASSERT_GREATER_THAN_UINT32(90, (uint32_t)rows.size());
    for (const CsvRow& r : rows) {
        const FilamentIdent id = filamentIdentify(r.type.c_str(), r.name.c_str(), r.idx.c_str());
        if (isKnownUnmatched(r.idx)) {
            TEST_ASSERT_EQUAL_STRING_MESSAGE("", id.id, r.idx.c_str());
            // Even unmatched, the user still sees what the printer said.
            TEST_ASSERT_TRUE(id.family[0] != '\0');
            continue;
        }
        TEST_ASSERT_TRUE_MESSAGE(id.id[0] != '\0', (r.idx + " " + r.type).c_str());
        TEST_ASSERT_NOT_NULL_MESSAGE(filamentInfoById(id.id), id.id);
    }
}

static void test_every_bambu_id_resolves_from_the_id_alone(void)
{
    // A third-party spool in an AMS often reports only tray_info_idx.
    const std::vector<CsvRow> rows = loadCsv();
    for (const CsvRow& r : rows) {
        const FilamentIdent id = filamentIdentify("", "", r.idx.c_str());
        if (isKnownUnmatched(r.idx)) continue;
        TEST_ASSERT_TRUE_MESSAGE(id.id[0] != '\0', r.idx.c_str());
    }
}

static void test_bambu_type_lookup(void)
{
    TEST_ASSERT_EQUAL_STRING("ABS", bambuTypeForIdx("GFB00"));
    TEST_ASSERT_EQUAL_STRING("ASA", bambuTypeForIdx("GFB01"));
    TEST_ASSERT_EQUAL_STRING("PLA", bambuTypeForIdx("GFA00"));
    TEST_ASSERT_EQUAL_STRING("PLA-AERO", bambuTypeForIdx("GFA11"));
    TEST_ASSERT_EQUAL_STRING("PLA", bambuTypeForIdx("gfa00"));      // case insensitive
    TEST_ASSERT_EQUAL_STRING("", bambuTypeForIdx("GFZ99"));
    TEST_ASSERT_EQUAL_STRING("", bambuTypeForIdx(""));
}

// --- REWORK-SPEC 16.2 steps 1-3 -------------------------------------------

static void test_plain_types(void)
{
    TEST_ASSERT_EQUAL_STRING("pla", filamentIdentify("PLA", "", "").id);
    TEST_ASSERT_EQUAL_STRING("abs", filamentIdentify("ABS", "", "").id);
    TEST_ASSERT_EQUAL_STRING("asa", filamentIdentify("asa", "", "").id);       // case
    TEST_ASSERT_EQUAL_STRING("petg", filamentIdentify("  PETG  ", "", "").id); // whitespace
    TEST_ASSERT_EQUAL_STRING("pctg", filamentIdentify("PCTG", "", "").id);
    TEST_ASSERT_EQUAL_STRING("pa", filamentIdentify("PAHT", "", "").id);       // PAHT -> pa
    TEST_ASSERT_EQUAL_STRING("pa6", filamentIdentify("PA6", "", "").id);
    TEST_ASSERT_EQUAL_STRING("", filamentIdentify("", "", "").id);
    TEST_ASSERT_EQUAL_STRING("", filamentIdentify(nullptr, nullptr, nullptr).id);
}

static void test_cf_and_gf_variants(void)
{
    // The guide has these filled grades, so they win.
    TEST_ASSERT_EQUAL_STRING("pla-cf", filamentIdentify("PLA-CF", "", "").id);
    TEST_ASSERT_EQUAL_STRING("abs-gf", filamentIdentify("ABS-GF", "", "").id);
    TEST_ASSERT_EQUAL_STRING("pa6-cf", filamentIdentify("PA6-CF", "", "").id);
    TEST_ASSERT_EQUAL_STRING("pa-cf", filamentIdentify("PAHT-CF", "", "").id);
    TEST_ASSERT_EQUAL_STRING("pps-cf", filamentIdentify("PPS-CF", "", "").id);

    // ... and these it does not, so the unfilled polymer is the answer while the
    // family string keeps the modifier for the UI.
    const FilamentIdent gf = filamentIdentify("PA-GF", "", "");
    TEST_ASSERT_EQUAL_STRING("pa", gf.id);
    TEST_ASSERT_EQUAL_STRING("PA-GF", gf.family);
    TEST_ASSERT_EQUAL_STRING("pe", filamentIdentify("PE-CF", "", "").id);
    TEST_ASSERT_EQUAL_STRING("ppa", filamentIdentify("PPA-GF", "", "").id);

    // A marketing suffix is not a different polymer.
    const FilamentIdent aero = filamentIdentify("PLA-AERO", "", "");
    TEST_ASSERT_EQUAL_STRING("pla", aero.id);
    TEST_ASSERT_EQUAL_STRING("PLA-AERO", aero.family);
    TEST_ASSERT_EQUAL_STRING("tpu", filamentIdentify("TPU-AMS", "", "").id);
    TEST_ASSERT_EQUAL_STRING("asa", filamentIdentify("ASA-AERO", "", "").id);
}

static void test_support_materials_take_the_paired_profile(void)
{
    TEST_ASSERT_EQUAL_STRING("pla", filamentIdentify("Support For PLA", "", "").id);
    TEST_ASSERT_EQUAL_STRING("pla", filamentIdentify("Support For PLA/PETG", "", "").id);
    TEST_ASSERT_EQUAL_STRING("pa", filamentIdentify("Support For PA/PET", "", "").id);
    TEST_ASSERT_EQUAL_STRING("abs", filamentIdentify("Support for ABS", "", "").id);
    TEST_ASSERT_EQUAL_STRING("pla", filamentIdentify("Support W", "", "").id);
    TEST_ASSERT_EQUAL_STRING("pa", filamentIdentify("Support G", "", "").id);
    // The soluble ones are materials in their own right.
    TEST_ASSERT_EQUAL_STRING("pva", filamentIdentify("PVA", "", "").id);
    TEST_ASSERT_EQUAL_STRING("bvoh", filamentIdentify("BVOH", "", "").id);
    TEST_ASSERT_EQUAL_STRING("hips", filamentIdentify("HIPS", "", "").id);
    // ... and so are the GFS0x support ids, through the Bambu table.
    TEST_ASSERT_EQUAL_STRING("pla", filamentIdentify("", "", "GFS00").id);
    TEST_ASSERT_EQUAL_STRING("pa", filamentIdentify("", "", "GFS03").id);
    TEST_ASSERT_EQUAL_STRING("abs", filamentIdentify("", "", "GFS06").id);
}

static void test_fallbacks_when_the_type_is_missing(void)
{
    // step 3: through the Bambu id table
    TEST_ASSERT_EQUAL_STRING("abs", filamentIdentify("", "", "GFB00").id);
    // ... then the sub-brand's first word
    TEST_ASSERT_EQUAL_STRING("pla", filamentIdentify("", "PLA Basic", "").id);
    // ... then the id prefix alone, for products newer than our table
    TEST_ASSERT_EQUAL_STRING("abs", filamentIdentify("", "", "GFB77").id);
    TEST_ASSERT_EQUAL_STRING("pla", filamentIdentify("", "", "GFA77").id);
    TEST_ASSERT_EQUAL_STRING("pla", filamentIdentify("", "", "GFL77").id);
    TEST_ASSERT_EQUAL_STRING("pc", filamentIdentify("", "", "GFC77").id);
    TEST_ASSERT_EQUAL_STRING("petg", filamentIdentify("", "", "GFG77").id);
    TEST_ASSERT_EQUAL_STRING("pa", filamentIdentify("", "", "GFN77").id);
    TEST_ASSERT_EQUAL_STRING("pp", filamentIdentify("", "", "GFP77").id);
    TEST_ASSERT_EQUAL_STRING("pps", filamentIdentify("", "", "GFT77").id);
    TEST_ASSERT_EQUAL_STRING("tpu", filamentIdentify("", "", "GFU77").id);
    // An unknown material keeps its name and gets no profile.
    const FilamentIdent eva = filamentIdentify("EVA", "", "GFR99");
    TEST_ASSERT_EQUAL_STRING("", eva.id);
    TEST_ASSERT_EQUAL_STRING("EVA", eva.family);
}

// --- REWORK-SPEC 16.2 step 5: which tray is loaded -------------------------

static void test_tray_now_encoding(void)
{
    ActiveTray t = filamentActiveTray(0);
    TEST_ASSERT_EQUAL(TraySource::Ams, t.source);
    TEST_ASSERT_EQUAL_INT(0, t.ams);
    TEST_ASSERT_EQUAL_INT(0, t.slot);

    t = filamentActiveTray(6);            // ams 1, slot 2
    TEST_ASSERT_EQUAL(TraySource::Ams, t.source);
    TEST_ASSERT_EQUAL_INT(1, t.ams);
    TEST_ASSERT_EQUAL_INT(2, t.slot);

    t = filamentActiveTray(15);           // ams 3, slot 3
    TEST_ASSERT_EQUAL_INT(3, t.ams);
    TEST_ASSERT_EQUAL_INT(3, t.slot);

    t = filamentActiveTray(254);          // external spool holder
    TEST_ASSERT_EQUAL(TraySource::External, t.source);
    TEST_ASSERT_EQUAL_INT(-1, t.ams);
    TEST_ASSERT_EQUAL_INT(254, t.slot);

    TEST_ASSERT_EQUAL(TraySource::None, filamentActiveTray(255).source);
    TEST_ASSERT_EQUAL(TraySource::None, filamentActiveTray(-1).source);

    // An AMS-HT reports its own unit id and has a single slot. The id must
    // survive as 128, not as a negative byte.
    t = filamentActiveTray(128);
    TEST_ASSERT_EQUAL(TraySource::Ams, t.source);
    TEST_ASSERT_EQUAL_INT(128, t.ams);
    TEST_ASSERT_EQUAL_INT(0, t.slot);
    TEST_ASSERT_EQUAL_INT(129, filamentActiveTray(129).ams);

    // ... and through the H2D encoding too.
    const uint16_t htSnow[2] = {0x8000, 0xFFFF};
    const ActiveTray ht = filamentActiveTrayH2D(0x00, htSnow, 2);
    TEST_ASSERT_EQUAL_INT(128, ht.ams);
    TEST_ASSERT_EQUAL_INT(0, ht.slot);
}

static void test_h2d_snow_decode(void)
{
    // state 0x10 -> extruder 1 is active; snow = (ams << 8) | slot.
    const uint16_t snow[2] = {0x0002, 0x0103};
    ActiveTray t = filamentActiveTrayH2D(0x10, snow, 2);
    TEST_ASSERT_EQUAL(TraySource::Ams, t.source);
    TEST_ASSERT_EQUAL_INT(1, t.ams);
    TEST_ASSERT_EQUAL_INT(3, t.slot);

    t = filamentActiveTrayH2D(0x00, snow, 2);   // extruder 0
    TEST_ASSERT_EQUAL_INT(0, t.ams);
    TEST_ASSERT_EQUAL_INT(2, t.slot);

    // Nothing loaded on the active extruder.
    const uint16_t none[2] = {0xFFFF, 0xFFFF};
    TEST_ASSERT_EQUAL(TraySource::None, filamentActiveTrayH2D(0x00, none, 2).source);
    // The external holder, and a nonsense extruder index.
    const uint16_t ext[2] = {0xFE00, 0xFFFF};
    TEST_ASSERT_EQUAL(TraySource::External, filamentActiveTrayH2D(0x00, ext, 2).source);
    TEST_ASSERT_EQUAL(TraySource::None, filamentActiveTrayH2D(0x70, snow, 2).source);
    TEST_ASSERT_EQUAL(TraySource::None, filamentActiveTrayH2D(-1, snow, 2).source);
}

// --- the live X1C AMS capture ---------------------------------------------

static void test_fixture_is_present(void)
{
    g_amsFixture = loadFile("test/fixtures/x1c_ams_trays.json");
    TEST_ASSERT_TRUE_MESSAGE(g_amsFixture.size() > 100,
                             "test/fixtures/x1c_ams_trays.json not found");
}

static void test_fixture_trays_are_parsed(void)
{
    PrinterReport r;
    printerReportInit(r);
    TEST_ASSERT_TRUE(parseFixture(g_amsFixture, r));

    TEST_ASSERT_EQUAL_INT(0, r.trayNow);
    TEST_ASSERT_EQUAL_INT(0, r.amsId[0]);
    TEST_ASSERT_EQUAL_INT(-1, r.amsId[1]);

    const TrayReport* t0 = reportTray(r, 0, 0);
    TEST_ASSERT_NOT_NULL(t0);
    TEST_ASSERT_EQUAL_STRING("ABS", t0->type);
    TEST_ASSERT_EQUAL_STRING("GFB00", t0->idx);
    TEST_ASSERT_EQUAL_STRING("FFFFFFFF", t0->color);
    TEST_ASSERT_EQUAL_STRING("", t0->sub);

    const TrayReport* t1 = reportTray(r, 0, 1);
    TEST_ASSERT_EQUAL_STRING("PLA", t1->type);
    TEST_ASSERT_EQUAL_STRING("161616FF", t1->color);
    const TrayReport* t2 = reportTray(r, 0, 2);
    TEST_ASSERT_EQUAL_STRING("PLA-AERO", t2->type);
    TEST_ASSERT_EQUAL_STRING("GFA11", t2->idx);
    const TrayReport* t3 = reportTray(r, 0, 3);
    TEST_ASSERT_EQUAL_STRING("PLA", t3->type);
    TEST_ASSERT_EQUAL_STRING("F330F9FF", t3->color);

    // vt_tray: the external spool holder, reached as ams -1.
    TEST_ASSERT_TRUE(r.externalSeen);
    const TrayReport* ext = reportTray(r, -1, 0);
    TEST_ASSERT_NOT_NULL(ext);
    TEST_ASSERT_EQUAL_STRING("ASA", ext->type);
    TEST_ASSERT_EQUAL_STRING("GFB01", ext->idx);

    // A slot nobody reported.
    TEST_ASSERT_NULL(reportTray(r, 2, 0));
}

static void test_fixture_active_tray_resolves(void)
{
    PrinterReport r;
    printerReportInit(r);
    TEST_ASSERT_TRUE(parseFixture(g_amsFixture, r));

    // tray_now 0 -> AMS 0 slot 0 -> ABS
    ActiveTray at = reportActiveTray(r);
    TEST_ASSERT_EQUAL(TraySource::Ams, at.source);
    TEST_ASSERT_EQUAL_INT(0, at.ams);
    TEST_ASSERT_EQUAL_INT(0, at.slot);
    const TrayReport* t = reportTray(r, at.ams, at.slot);
    TEST_ASSERT_EQUAL_STRING("abs", filamentIdentify(t->type, t->sub, t->idx).id);

    // 254 -> the external holder -> ASA
    r.trayNow = 254;
    at = reportActiveTray(r);
    TEST_ASSERT_EQUAL(TraySource::External, at.source);
    t = reportTray(r, -1, 0);
    TEST_ASSERT_EQUAL_STRING("asa", filamentIdentify(t->type, t->sub, t->idx).id);

    // 255 -> nothing loaded
    r.trayNow = 255;
    TEST_ASSERT_EQUAL(TraySource::None, reportActiveTray(r).source);

    // An H2D report wins over tray_now when it has one.
    r.trayNow = 0;
    r.extruderState = 0x00;
    r.extruderSnow[0] = 0x0002;      // ams 0, slot 2 -> PLA-AERO
    at = reportActiveTray(r);
    TEST_ASSERT_EQUAL_INT(2, at.slot);
    t = reportTray(r, at.ams, at.slot);
    TEST_ASSERT_EQUAL_STRING("PLA-AERO", t->type);
}

static void test_partial_ams_reports_merge(void)
{
    // A P1 sends only what changed. The other trays must survive.
    PrinterReport r;
    printerReportInit(r);
    TEST_ASSERT_TRUE(parseFixture(g_amsFixture, r));

    const char* partial =
        "{\"print\":{\"command\":\"push_status\",\"ams\":{\"tray_now\":\"3\","
        "\"ams\":[{\"id\":\"0\",\"tray\":[{\"id\":\"3\",\"tray_type\":\"PETG\","
        "\"tray_info_idx\":\"GFG00\",\"tray_color\":\"00FF00FF\"}]}]}}}";
    PrinterReport before = r;
    TEST_ASSERT_TRUE(parseFixture(partial, r));

    TEST_ASSERT_EQUAL_INT(3, r.trayNow);
    TEST_ASSERT_EQUAL_STRING("PETG", reportTray(r, 0, 3)->type);
    TEST_ASSERT_EQUAL_STRING("00FF00FF", reportTray(r, 0, 3)->color);
    // ... and slots 0..2 are exactly as they were.
    TEST_ASSERT_EQUAL_STRING(before.trays[0][0].type, reportTray(r, 0, 0)->type);
    TEST_ASSERT_EQUAL_STRING(before.trays[0][1].type, reportTray(r, 0, 1)->type);
    TEST_ASSERT_EQUAL_STRING("GFA11", reportTray(r, 0, 2)->idx);
    // ... and so is the external holder, which the partial report never mentioned.
    TEST_ASSERT_EQUAL_STRING("ASA", r.external.type);

    // An emptied slot is reported explicitly, and that does clear it.
    const char* emptied =
        "{\"print\":{\"command\":\"push_status\",\"ams\":{"
        "\"ams\":[{\"id\":\"0\",\"tray\":[{\"id\":\"3\",\"tray_type\":\"\",\"tray_info_idx\":\"\"}]}]}}}";
    TEST_ASSERT_TRUE(parseFixture(emptied, r));
    TEST_ASSERT_FALSE(trayLoaded(*reportTray(r, 0, 3)));
}

static void test_ams_ht_unit_id_is_not_an_index(void)
{
    PrinterReport r;
    printerReportInit(r);
    const char* ht =
        "{\"print\":{\"command\":\"push_status\",\"ams\":{\"tray_now\":\"128\","
        "\"ams\":[{\"id\":\"128\",\"tray\":[{\"id\":\"0\",\"tray_type\":\"ABS\","
        "\"tray_info_idx\":\"GFB00\"}]}]}}}";
    TEST_ASSERT_TRUE(parseFixture(ht, r));
    TEST_ASSERT_EQUAL_INT(128, r.amsId[0]);
    const ActiveTray at = reportActiveTray(r);
    TEST_ASSERT_EQUAL(TraySource::Ams, at.source);
    TEST_ASSERT_EQUAL_INT(128, at.ams);
    TEST_ASSERT_EQUAL_INT(0, at.slot);
    // The lookup must work through the resolved id, not only through a literal.
    TEST_ASSERT_NOT_NULL(reportTray(r, at.ams, at.slot));
    const TrayReport* t = reportTray(r, 128, 0);
    TEST_ASSERT_NOT_NULL(t);
    TEST_ASSERT_EQUAL_STRING("abs", filamentIdentify(t->type, t->sub, t->idx).id);
}

// --- REWORK-SPEC 16.3: the effective profile ------------------------------

static void test_effective_profile_keeps_pla_cool(void)
{
    const FilamentPolicy p = defaultPolicy();
    const FilamentEffective e = filamentEffective(filamentInfoById("pla"), p);
    // PLA's guide entry says the enclosure should be open for cooling, so the
    // ceiling of its ambient band is the set point, not the middle of it.
    TEST_ASSERT_TRUE(e.keepCool);
    TEST_ASSERT_EQUAL_UINT8(30, e.chamberTarget);
    TEST_ASSERT_EQUAL_UINT8(35, e.cooldownTarget);     // untouched fan.* value
    TEST_ASSERT_EQUAL_UINT8(0, e.ventFloor);           // vent optional
    TEST_ASSERT_FALSE(e.gentle);                       // 100 % part cooling
    TEST_ASSERT_TRUE(e.fromGuide);
    TEST_ASSERT_FALSE(e.overridden);
}

static void test_effective_profile_warm_materials(void)
{
    const FilamentPolicy p = defaultPolicy();

    const FilamentEffective abs = filamentEffective(filamentInfoById("abs"), p);
    TEST_ASSERT_FALSE(abs.keepCool);
    TEST_ASSERT_EQUAL_UINT8(50, abs.chamberTarget);
    TEST_ASSERT_TRUE(abs.gentle);                      // part cooling off
    TEST_ASSERT_EQUAL_UINT8(10, abs.ventFloor);        // ventilation required

    TEST_ASSERT_EQUAL_UINT8(55, filamentEffective(filamentInfoById("asa"), p).chamberTarget);
    TEST_ASSERT_EQUAL_UINT8(55, filamentEffective(filamentInfoById("pc"), p).chamberTarget);
    // PETG's recommendation is barely above room temperature, so it counts as a
    // keep-cool material and takes the top of its band.
    const FilamentEffective petg = filamentEffective(filamentInfoById("petg"), p);
    TEST_ASSERT_TRUE(petg.keepCool);
    TEST_ASSERT_EQUAL_UINT8(35, petg.chamberTarget);
}

static void test_effective_profile_without_a_match(void)
{
    const FilamentPolicy p = defaultPolicy();
    const FilamentEffective e = filamentEffective(nullptr, p);
    TEST_ASSERT_EQUAL_UINT8(45, e.chamberTarget);      // the plain fan.* values
    TEST_ASSERT_EQUAL_UINT8(35, e.cooldownTarget);
    TEST_ASSERT_EQUAL_UINT8(0, e.ventFloor);
    TEST_ASSERT_FALSE(e.fromGuide);
}

static void test_auto_off_ignores_the_guide(void)
{
    FilamentPolicy p = defaultPolicy();
    p.autoEnabled = false;
    const FilamentOverrideRule rules[1] = {{"abs", 48, -1, 5, FIL_COOLING_GENTLE}};
    p.overrides = rules;
    p.overrideCount = 1;
    const FilamentEffective e = filamentEffective(filamentInfoById("abs"), p);
    // Neither the guide nor an override may move anything: the user has said no.
    TEST_ASSERT_EQUAL_UINT8(45, e.chamberTarget);
    TEST_ASSERT_EQUAL_UINT8(0, e.ventFloor);
    TEST_ASSERT_FALSE(e.gentle);
    TEST_ASSERT_FALSE(e.overridden);
}

static void test_override_precedence(void)
{
    FilamentPolicy p = defaultPolicy();
    // "*" first, then the material-specific rule on top of it.
    const FilamentOverrideRule rules[2] = {
        {"*",   -1, 30, 3, FIL_COOLING_KEEP},
        {"abs", 48, -1, 5, FIL_COOLING_FAST},
    };
    p.overrides = rules;
    p.overrideCount = 2;

    const FilamentEffective abs = filamentEffective(filamentInfoById("abs"), p);
    TEST_ASSERT_EQUAL_UINT8(48, abs.chamberTarget);    // id override wins
    TEST_ASSERT_EQUAL_UINT8(30, abs.cooldownTarget);   // only "*" set this
    TEST_ASSERT_EQUAL_UINT8(5, abs.ventFloor);         // id override beats "*"
    TEST_ASSERT_FALSE(abs.gentle);                     // forced back to fast
    TEST_ASSERT_TRUE(abs.overridden);

    // A material with no rule of its own still gets the "*" rule.
    const FilamentEffective pla = filamentEffective(filamentInfoById("pla"), p);
    TEST_ASSERT_EQUAL_UINT8(30, pla.chamberTarget);    // from the guide
    TEST_ASSERT_EQUAL_UINT8(30, pla.cooldownTarget);   // from "*"
    TEST_ASSERT_EQUAL_UINT8(3, pla.ventFloor);
    TEST_ASSERT_TRUE(pla.overridden);

    // Rule order in the array must not matter: id always wins over "*".
    const FilamentOverrideRule reversed[2] = {
        {"abs", 48, -1, 5, FIL_COOLING_FAST},
        {"*",   -1, 30, 3, FIL_COOLING_KEEP},
    };
    p.overrides = reversed;
    const FilamentEffective abs2 = filamentEffective(filamentInfoById("abs"), p);
    TEST_ASSERT_EQUAL_UINT8(48, abs2.chamberTarget);
    TEST_ASSERT_EQUAL_UINT8(5, abs2.ventFloor);
}

static void test_override_values_are_clamped(void)
{
    FilamentPolicy p = defaultPolicy();
    const FilamentOverrideRule rules[1] = {{"abs", 200, 200, 200, FIL_COOLING_GENTLE}};
    p.overrides = rules;
    p.overrideCount = 1;
    const FilamentEffective e = filamentEffective(filamentInfoById("abs"), p);
    TEST_ASSERT_EQUAL_UINT8(80, e.chamberTarget);
    TEST_ASSERT_EQUAL_UINT8(60, e.cooldownTarget);
    TEST_ASSERT_EQUAL_UINT8(100, e.ventFloor);
}

static void test_vent_floor_follows_the_vent_demand(void)
{
    FilamentPolicy p = defaultPolicy();
    p.ventFloorByVent[VENT_OPTIONAL] = 5;
    p.ventFloorByVent[VENT_RECOMMENDED] = 15;
    p.ventFloorByVent[VENT_REQUIRED] = 40;
    TEST_ASSERT_EQUAL_UINT8(5, filamentEffective(filamentInfoById("pla"), p).ventFloor);
    TEST_ASSERT_EQUAL_UINT8(15, filamentEffective(filamentInfoById("petg"), p).ventFloor);
    TEST_ASSERT_EQUAL_UINT8(40, filamentEffective(filamentInfoById("abs"), p).ventFloor);
}

int main(int, char**)
{
    UNITY_BEGIN();
    RUN_TEST(test_db_is_populated);
    RUN_TEST(test_every_bambu_id_resolves_from_its_type);
    RUN_TEST(test_every_bambu_id_resolves_from_the_id_alone);
    RUN_TEST(test_bambu_type_lookup);
    RUN_TEST(test_plain_types);
    RUN_TEST(test_cf_and_gf_variants);
    RUN_TEST(test_support_materials_take_the_paired_profile);
    RUN_TEST(test_fallbacks_when_the_type_is_missing);
    RUN_TEST(test_tray_now_encoding);
    RUN_TEST(test_h2d_snow_decode);
    RUN_TEST(test_fixture_is_present);
    RUN_TEST(test_fixture_trays_are_parsed);
    RUN_TEST(test_fixture_active_tray_resolves);
    RUN_TEST(test_partial_ams_reports_merge);
    RUN_TEST(test_ams_ht_unit_id_is_not_an_index);
    RUN_TEST(test_effective_profile_keeps_pla_cool);
    RUN_TEST(test_effective_profile_warm_materials);
    RUN_TEST(test_effective_profile_without_a_match);
    RUN_TEST(test_auto_off_ignores_the_guide);
    RUN_TEST(test_override_precedence);
    RUN_TEST(test_override_values_are_clamped);
    RUN_TEST(test_vent_floor_follows_the_vent_demand);
    return UNITY_END();
}
