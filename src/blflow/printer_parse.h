// printer_parse.h - extraction of printer state from a Bambu MQTT report.
//
// Deliberately Arduino-free (ArduinoJson only) so the exact same code that runs
// on the device can be exercised on the host against the captured fixtures in
// test/fixtures/. Everything here is header-only and side-effect free: it patches
// a plain PrinterReport and touches nothing else.
//
// Field notes, from a live X1C capture (see test/fixtures/README.md):
//   * The X1 series pushes the whole ~16 KB `push_status` object about once a
//     second, interleaved with small `gcode_line` acknowledgements that must be
//     ignored - parsing them would blank out good data.
//   * `chamber_temper` is absent on current X1C firmware. The chamber is only
//     reachable through `print.device.ctc.info.temp` (mirrored at
//     `print.info.temp`), so the `device.*` block is a fallback for every model,
//     not just H2D.
//   * `device.*` temperatures are packed: low 16 bits current, high 16 bits
//     target (extruder 9175180 -> 140/140, bed 7864440 -> 120/120, ctc 43 -> 43/0).
//   * Fan speeds are decimal strings on a 0..15 gear scale.
//   * `home_flag` arrives as a negative int32 and must be read as uint32 before
//     the door bit (23) is tested. It is the front-door plunger switch; the top
//     lid has no sensor at all.
//   * The AMS block is *incremental* on P1/A1 firmware: a report may carry one
//     tray and nothing else. Trays are therefore merged by (ams id, slot id) and
//     never cleared because a report did not mention them - only an explicit
//     empty `tray_type` means "this slot is empty now".
//   * On some X1C units the closed door does not actuate that switch, so the bit
//     sits at "open" forever (pressing the switch by hand flips it). A raw bit is
//     therefore not evidence of anything until it has been seen to *change*:
//     `doorKnown` only becomes true on the first edge, and until then the door is
//     treated as closed. The very first report establishes the raw state and is
//     never an edge - otherwise every MQTT reconnect would look like a door event.

#ifndef BLSF_PRINTER_PARSE_H
#define BLSF_PRINTER_PARSE_H

#include <ArduinoJson.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// For the active-tray encodings (tray_now / H2D snow). filament_match.h is pure
// and pulls in nothing but the generated table declarations.
#include "filament_match.h"

namespace blsf {

// Sentinels for "the printer has not told us yet".
static const float REPORT_TEMP_UNKNOWN = NAN;
static const int8_t REPORT_FAN_UNKNOWN = -1;

// One AMS slot (or the external spool holder) as the printer describes it.
// Fixed-size and POD like everything else in the report: the whole struct is
// memcpy'd under a spinlock.
//   type  - `tray_type`, the printer's own classification ("PLA", "PLA-CF", "")
//   sub   - `tray_sub_brands`, the product name ("PLA Basic")
//   idx   - `tray_info_idx`, the Bambu filament id ("GFA00")
//   color - `tray_color`, 8 hex digits RGBA ("161616FF")
// An empty `type` means the slot is empty, which is exactly how the printer
// reports an unloaded tray.
struct TrayReport {
    char type[16];
    char sub[24];
    char idx[8];
    char color[9];
};

// Four AMS units of four slots is the largest configuration the firmware tracks
// (16 slots); an AMS-HT counts as a unit with one slot. Unit ids are stored
// alongside the trays rather than used as an index, because AMS-HT units report
// ids >= 128.
static const uint8_t REPORT_MAX_AMS = 4;
static const uint8_t REPORT_MAX_SLOTS = 4;
static const uint8_t REPORT_MAX_EXTRUDERS = 2;
static const uint16_t REPORT_SNOW_UNKNOWN = 0xFFFFu;

// The subset of printer state we care about. POD by design: it is copied under a
// spinlock on the device (see state.h) so it must contain no String or vector.
struct PrinterReport {
    char     gcodeState[16];     // RUNNING / PAUSE / IDLE / FINISH / FAILED / PREPARE
    int      stage;              // stg_cur, -1 unknown
    int      progress;           // percent, -1 unknown
    int      remainingMin;       // -1 unknown
    int      layer;
    int      totalLayers;
    char     task[64];           // subtask_name
    bool     doorOpen;           // raw bit 23 - meaningless until doorKnown
    bool     doorRawSeen;        // a report has carried home_flag at least once
    bool     doorKnown;          // an EDGE has been seen, i.e. the switch works
    uint16_t doorEdgeCount;      // transitions since boot (the first report is not one)
    uint32_t lastDoorOpenMs;     // millis() of the last open edge, 0 = never
    uint32_t lastDoorCloseMs;    // millis() of the last close edge, 0 = never
    uint32_t printError;
    char     wifiSignal[12];     // e.g. "-32dBm"

    float    nozzle, nozzleTarget;
    float    bed, bedTarget;
    float    chamber, chamberTarget;

    int8_t   fanPart, fanAux, fanChamber, fanHeatbreak;   // percent, -1 unknown

    // --- filament / AMS (REWORK-SPEC 16) ---
    // ~1 kB of the report, and the only part of it that is merged rather than
    // replaced (see the field note about incremental AMS updates).
    TrayReport trays[REPORT_MAX_AMS][REPORT_MAX_SLOTS];
    int16_t    amsId[REPORT_MAX_AMS];      // reported unit id, -1 = slot unused
    TrayReport external;                   // vt_tray, the external spool holder
    bool       externalSeen;
    int16_t    trayNow;                    // ams.tray_now, -1 = not reported
    int16_t    extruderState;              // H2D device.extruder.state, -1 = none
    uint16_t   extruderSnow[REPORT_MAX_EXTRUDERS];   // (ams << 8) | slot

    // --- last acknowledgement of a command *we* sent (2.0.4) ----------------
    // A Bambu printer with Developer Mode switched off still publishes its
    // reports, but signature-checks every write command and answers a
    // `gcode_line` request with result "failed" / reason "mqtt message verify
    // failed". Nothing else in the firmware would ever notice, so the ack is
    // recorded here and surfaced as printer.lastCommandError.
    // lastGcodeMs is 0 until an ack for one of our own sequence ids arrives.
    bool     lastGcodeResult;         // true = accepted
    uint32_t lastGcodeErr;            // err_code, 0 when the printer sent none
    uint32_t lastGcodeMs;             // nowMs of the ack, 0 = never
    char     lastGcodeReason[48];
};

// A `gcode_line` acknowledgement, before it is matched against the ids we sent.
struct GcodeAck {
    uint32_t sequenceId;
    bool     ok;
    uint32_t errCode;
    char     reason[48];
};

// Print phase, derived from gcode_state + stg_cur + the temperature targets
// (REWORK-SPEC 15.1). This is what the fan logic reasons about: "RUNNING" alone
// cannot tell a chamber that is still heating from one that is at temperature.
enum class Phase : uint8_t {
    Offline,    // no report yet / link down
    Paused,
    Preheat,
    Cooling,
    Printing,
    Finished,
    Failed,
    Idle,
};

namespace detail {

// strlcpy is not portable to every host libc, and we need identical behaviour
// on device and host.
inline void copyStr(char* dst, size_t dstSize, const char* src)
{
    if (dstSize == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    size_t i = 0;
    for (; i + 1 < dstSize && src[i]; i++) dst[i] = src[i];
    dst[i] = '\0';
}

// strcasecmp lives in <strings.h> on POSIX and in <string.h> on the ESP32
// toolchain; rather than guess, compare ASCII case-insensitively by hand.
inline bool equalsIgnoreCase(const char* a, const char* b)
{
    if (!a || !b) return false;
    for (; *a && *b; a++, b++) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) return false;
    }
    return *a == *b;
}

}  // namespace detail

inline float packedCurrent(uint32_t packed) { return (float)(packed & 0xFFFFu); }
inline float packedTarget(uint32_t packed)  { return (float)((packed >> 16) & 0xFFFFu); }

inline void printerReportInit(PrinterReport& r)
{
    memset(&r, 0, sizeof(r));
    r.stage = -1;
    r.progress = -1;
    r.remainingMin = -1;
    r.layer = -1;
    r.totalLayers = -1;
    r.nozzle = r.nozzleTarget = r.bed = r.bedTarget = REPORT_TEMP_UNKNOWN;
    r.chamber = r.chamberTarget = REPORT_TEMP_UNKNOWN;
    r.fanPart = r.fanAux = r.fanChamber = r.fanHeatbreak = REPORT_FAN_UNKNOWN;
    for (uint8_t i = 0; i < REPORT_MAX_AMS; i++) r.amsId[i] = -1;
    r.trayNow = -1;
    r.extruderState = -1;
    for (uint8_t i = 0; i < REPORT_MAX_EXTRUDERS; i++) r.extruderSnow[i] = REPORT_SNOW_UNKNOWN;
    r.lastGcodeResult = true;      // nothing has failed yet
}

// True when the newest ack we matched to one of our own commands was a refusal.
inline bool reportCommandFailed(const PrinterReport& r)
{
    return r.lastGcodeMs != 0 && !r.lastGcodeResult;
}

// ha-bambulab CURRENT_STAGE_IDS, in the snake_case spelling the 2.0 API already
// publishes as `printer.stageText` (REWORK-SPEC 9 pins -1/255 to "idle"). Codes
// 36..77 come from H2D-era firmware and are best-effort. -2 is our own "the
// printer is not reachable" value.
inline const char* stageName(int stage)
{
    switch (stage) {
        case -2: return "offline";
        case -1:
        case 255: return "idle";
        case 0:  return "printing";
        case 1:  return "auto_bed_leveling";
        case 2:  return "heatbed_preheating";
        case 3:  return "sweeping_xy_mech_mode";
        case 4:  return "changing_filament";
        case 5:  return "m400_pause";
        case 6:  return "paused_filament_runout";
        case 7:  return "heating_hotend";
        case 8:  return "calibrating_extrusion";
        case 9:  return "scanning_bed_surface";
        case 10: return "inspecting_first_layer";
        case 11: return "identifying_build_plate_type";
        case 12: return "calibrating_micro_lidar";
        case 13: return "homing_toolhead";
        case 14: return "cleaning_nozzle_tip";
        case 15: return "checking_extruder_temperature";
        case 16: return "paused_user";
        case 17: return "paused_front_cover_falling";
        case 18: return "calibrating_lidar";
        case 19: return "calibrating_extrusion_flow";
        case 20: return "paused_nozzle_temperature_malfunction";
        case 21: return "paused_heat_bed_temperature_malfunction";
        case 22: return "filament_unloading";
        case 23: return "paused_skipped_step";
        case 24: return "filament_loading";
        case 25: return "calibrating_motor_noise";
        case 26: return "paused_ams_lost";
        case 27: return "paused_low_fan_speed_heat_break";
        case 28: return "paused_chamber_temperature_control_error";
        case 29: return "cooling_chamber";
        case 30: return "paused_by_gcode";
        case 31: return "motor_noise_showoff";
        case 32: return "paused_nozzle_filament_covered_detected";
        case 33: return "paused_cutter_error";
        case 34: return "paused_first_layer_error";
        case 35: return "paused_nozzle_clog";
        case 36: return "check_absolute_accuracy_before_calibration";
        case 37: return "absolute_accuracy_calibration";
        case 38: return "check_absolute_accuracy_after_calibration";
        case 39: return "calibrate_nozzle_offset";
        case 40: return "bed_level_high_temperature";
        case 41: return "check_quick_release";
        case 42: return "check_door_and_cover";
        case 43: return "laser_calibration";
        case 44: return "check_platform";
        case 45: return "check_birds_eye_camera_position";
        case 46: return "calibrate_birds_eye_camera";
        case 47: return "bed_level_phase_1";
        case 48: return "bed_level_phase_2";
        case 49: return "heating_chamber";
        case 50: return "heatbed_cooling";
        case 51: return "print_calibration_lines";
        case 52: return "check_material";
        case 53: return "calibrating_live_view_camera";
        case 54: return "waiting_for_heatbed_temperature";
        case 55: return "check_material_position";
        case 56: return "calibrating_cutter_model_offset";
        case 57: return "measuring_surface";
        case 58: return "thermal_preconditioning";
        case 59: return "homing_blade_holder";
        case 60: return "calibrating_camera_offset";
        case 61: return "calibrating_blade_holder_position";
        case 62: return "hotend_pick_place_test";
        case 63: return "waiting_for_chamber_temperature";
        case 64: return "preparing_hotend";
        case 65: return "calibrating_nozzle_clumping_detection";
        case 66: return "purifying_chamber_air";
        case 67: return "measuring_rotary_attachment";
        case 68: return "moving_toolhead_above_purge_chute";
        case 69: return "cooling_nozzle";
        case 70: return "moving_toolhead_to_center_of_heatbed";
        case 71: return "active_arc_fitting";
        case 72: return "hotend_type_detection";
        case 73: return "build_plate_alignment_detection";
        case 74: return "heatbed_surface_foreign_object_detection";
        case 75: return "heatbed_underside_foreign_object_detection";
        case 76: return "pre_extrusion_before_printing";
        case 77: return "preparing_ams";
        default: return "unknown";
    }
}

// Stages the printer sits in while a job is suspended (BLLED stageIsPause()).
inline bool stageIsPause(int stage)
{
    switch (stage) {
        case 5: case 6: case 16: case 17: case 20: case 21: case 23: case 26:
        case 27: case 28: case 30: case 32: case 33: case 34: case 35:
            return true;
        default:
            return false;
    }
}

// Stages that are unambiguously "still bringing something up to temperature".
inline bool stageIsPreheat(int stage)
{
    switch (stage) {
        case 2: case 7: case 49: case 54: case 58: case 63: case 64: return true;
        default: return false;
    }
}

// Stages that are unambiguously "actively getting rid of heat".
inline bool stageIsCooling(int stage)
{
    return stage == 29 || stage == 50 || stage == 69;
}

// Commands that carry acknowledgements rather than state.
inline bool isIgnoredCommand(const char* cmd)
{
    if (!cmd) return false;
    static const char* const kIgnored[] = {
        "gcode_line", "project_prepare", "project_file", "clean_print_error",
        "resume", "get_accessories", "prepare", "extrusion_cali_get",
    };
    for (const char* i : kIgnored) if (strcmp(cmd, i) == 0) return true;
    return false;
}

// Reads a `gcode_line` acknowledgement out of a (filtered) report document.
// Returns false for anything that is not one, or for the bare acks that carry
// no `result` at all - the caller then treats the message exactly as before.
//
// The ack is *not* applied here: only printer_link.cpp knows which sequence ids
// we actually sent, and Bambu Studio's acks travel over the same topic. See
// applyGcodeAck().
inline bool parseGcodeAck(JsonVariantConst root, GcodeAck& out)
{
    JsonVariantConst print = root["print"];
    if (!print.is<JsonObjectConst>()) return false;
    JsonObjectConst p = print.as<JsonObjectConst>();
    const char* cmd = p["command"] | (const char*)nullptr;
    if (!cmd || strcmp(cmd, "gcode_line") != 0) return false;
    const char* result = p["result"] | (const char*)nullptr;
    if (!result || !*result) return false;

    memset(&out, 0, sizeof(out));
    // The id is a decimal *string* in every firmware seen so far, but a couple
    // of P1 builds send it as a number, so both are accepted.
    JsonVariantConst seq = p["sequence_id"];
    if (seq.is<const char*>()) out.sequenceId = (uint32_t)strtoul(seq.as<const char*>(), nullptr, 10);
    else if (seq.is<unsigned long>() || seq.is<int>()) out.sequenceId = (uint32_t)seq.as<unsigned long>();

    // "SUCCESS" on an X1C, "success" on some P-series builds; "failed" is the
    // refusal we care about.
    out.ok = detail::equalsIgnoreCase(result, "success");
    out.errCode = (uint32_t)(p["err_code"] | 0UL);
    detail::copyStr(out.reason, sizeof(out.reason), p["reason"] | "");
    return true;
}

// Records an ack the caller has confirmed is a reply to one of our own commands.
// `nowMs` is millis(); it is coerced away from 0 so lastGcodeMs stays a reliable
// "an ack has arrived" flag.
inline void applyGcodeAck(PrinterReport& r, const GcodeAck& ack, uint32_t nowMs)
{
    r.lastGcodeResult = ack.ok;
    r.lastGcodeErr = ack.ok ? 0 : ack.errCode;
    r.lastGcodeMs = nowMs ? nowMs : 1;
    // A success clears the message: the status document shows lastCommandError
    // as null again the moment the printer accepts something.
    detail::copyStr(r.lastGcodeReason, sizeof(r.lastGcodeReason), ack.ok ? "" : ack.reason);
}

// Builds the deserialisation filter. The full report has around 90 top-level
// keys including large `ams`, `ipcam`, `xcam`, `net` and `upload` blocks; keeping
// the filter tight is what makes a 16 KB message cheap to parse.
inline void buildPrinterFilter(JsonDocument& filter)
{
    JsonObject p = filter["print"].to<JsonObject>();
    p["command"] = true;
    // Kept for the `gcode_line` acknowledgement path (parseGcodeAck): a printer
    // with Developer Mode off refuses our M106 lines and says so right here.
    p["sequence_id"] = true;
    p["result"] = true;
    p["reason"] = true;
    p["err_code"] = true;
    p["nozzle_temper"] = true;
    p["nozzle_target_temper"] = true;
    p["bed_temper"] = true;
    p["bed_target_temper"] = true;
    p["chamber_temper"] = true;
    p["cooling_fan_speed"] = true;
    p["big_fan1_speed"] = true;
    p["big_fan2_speed"] = true;
    p["heatbreak_fan_speed"] = true;
    p["fan_gear"] = true;
    p["gcode_state"] = true;
    p["mc_percent"] = true;
    p["mc_remaining_time"] = true;
    p["layer_num"] = true;
    p["total_layer_num"] = true;
    p["subtask_name"] = true;
    p["stg_cur"] = true;
    p["print_error"] = true;
    p["wifi_signal"] = true;
    p["home_flag"] = true;
    p["lights_report"] = true;
    p["info"]["temp"] = true;
    // Only the four sub-objects we decode - not the whole `device` block, which
    // also carries camera, plate and nozzle-wear data we have no use for.
    JsonObject dev = p["device"].to<JsonObject>();
    dev["bed"] = true;
    dev["ctc"] = true;
    dev["airduct"] = true;
    // The extruder block is spelled out rather than waved through: on an H2D it
    // also carries per-nozzle wear, offsets and calibration data. `snow` is the
    // loaded slot as (ams << 8) | slot and `state` has the active extruder in
    // bits 4..7 (REWORK-SPEC 16.2 step 5).
    JsonObject ext = dev["extruder"].to<JsonObject>();
    ext["state"] = true;
    JsonObject extInfo = ext["info"][0].to<JsonObject>();
    extInfo["id"] = true;
    extInfo["temp"] = true;
    extInfo["snow"] = true;

    // AMS. A filter array applies its first element to every array element, so
    // one tray filter covers all four slots of all four units.
    JsonObject ams = p["ams"].to<JsonObject>();
    ams["tray_now"] = true;
    JsonObject unit = ams["ams"][0].to<JsonObject>();
    unit["id"] = true;
    JsonObject tray = unit["tray"][0].to<JsonObject>();
    tray["id"] = true;
    tray["tray_type"] = true;
    tray["tray_sub_brands"] = true;
    tray["tray_info_idx"] = true;
    tray["tray_color"] = true;
    // The external spool holder is a tray-shaped object of its own.
    JsonObject vt = p["vt_tray"].to<JsonObject>();
    vt["id"] = true;
    vt["tray_type"] = true;
    vt["tray_sub_brands"] = true;
    vt["tray_info_idx"] = true;
    vt["tray_color"] = true;
}

namespace detail {

// "0".."15" (or the same value as a number) -> 0..100 %.
inline int8_t gearToPercent(JsonVariantConst v)
{
    int gear;
    if (v.is<const char*>()) gear = atoi(v.as<const char*>());
    else if (v.is<float>()) gear = (int)v.as<double>();
    else return REPORT_FAN_UNKNOWN;
    if (gear < 0) gear = 0;
    if (gear > 15) gear = 15;
    return (int8_t)((gear * 100 + 7) / 15);   // round to nearest
}

// Bambu writes small integers as decimal strings about as often as numbers
// ("id": "0" in one firmware, 0 in the next), so every id goes through this.
inline int intFrom(JsonVariantConst v, int fallback)
{
    if (v.is<const char*>()) {
        const char* s = v.as<const char*>();
        if (!s || !*s) return fallback;
        char* end = nullptr;
        const long n = strtol(s, &end, 10);
        if (end == s) return fallback;
        return (int)n;
    }
    if (v.is<float>()) return (int)v.as<double>();
    return fallback;
}

// Copies one tray field, but only when the report actually carries it: a P1
// sends partial `ams` objects and an absent key means "unchanged", not "empty".
inline void mergeTrayStr(JsonObjectConst o, const char* key, char* dst, size_t dstSize)
{
    JsonVariantConst v = o[key];
    if (!v.is<const char*>()) return;
    copyStr(dst, dstSize, v.as<const char*>());
}

inline void mergeTray(JsonObjectConst o, TrayReport& t)
{
    mergeTrayStr(o, "tray_type", t.type, sizeof(t.type));
    mergeTrayStr(o, "tray_sub_brands", t.sub, sizeof(t.sub));
    mergeTrayStr(o, "tray_info_idx", t.idx, sizeof(t.idx));
    mergeTrayStr(o, "tray_color", t.color, sizeof(t.color));
}

inline bool packedFrom(JsonVariantConst v, uint32_t& out)
{
    if (!v.is<float>()) return false;
    const double d = v.as<double>();
    if (d < 0 || d > 4294967295.0) return false;
    out = (uint32_t)d;
    return true;
}

}  // namespace detail

// --- AMS lookup ------------------------------------------------------------
// Unit ids are stored, not used as an index, because an AMS-HT reports an id of
// 128 or more. `create` claims a free row for a unit seen for the first time.
inline int reportAmsIndex(PrinterReport& r, int amsId, bool create)
{
    for (uint8_t i = 0; i < REPORT_MAX_AMS; i++) if (r.amsId[i] == amsId) return i;
    if (!create) return -1;
    for (uint8_t i = 0; i < REPORT_MAX_AMS; i++) {
        if (r.amsId[i] < 0) { r.amsId[i] = (int16_t)amsId; return i; }
    }
    return -1;
}

inline int reportAmsIndex(const PrinterReport& r, int amsId)
{
    for (uint8_t i = 0; i < REPORT_MAX_AMS; i++) if (r.amsId[i] == amsId) return i;
    return -1;
}

// The tray at (ams unit id, slot), or the external holder for ams < 0.
// Null when that slot has never been reported.
inline const TrayReport* reportTray(const PrinterReport& r, int amsId, int slot)
{
    if (amsId < 0) return r.externalSeen ? &r.external : nullptr;
    const int i = reportAmsIndex(r, amsId);
    if (i < 0 || slot < 0 || slot >= REPORT_MAX_SLOTS) return nullptr;
    return &r.trays[i][slot];
}

// A slot holds filament exactly when the printer put a type on it.
inline bool trayLoaded(const TrayReport& t) { return t.type[0] != '\0' || t.idx[0] != '\0'; }

// Patches `out` with whatever `root` (a full report document, i.e. the object
// containing "print") carries. Returns false when the message is an ack, is
// empty after filtering, or has no "print" object - in which case `out` is
// untouched and the caller should not treat it as a fresh update.
// `nowMs` timestamps the door edges (millis() on the device). Passing 0 - the
// default, used by tests that do not care - simply records 0.
inline bool parsePrinterReport(JsonVariantConst root, PrinterReport& out, uint32_t nowMs = 0)
{
    JsonVariantConst print = root["print"];
    if (!print.is<JsonObjectConst>()) return false;
    JsonObjectConst p = print.as<JsonObjectConst>();
    if (p.size() == 0) return false;
    if (isIgnoredCommand(p["command"] | (const char*)nullptr)) return false;

    // --- classic temperature fields ---
    const bool hasNozzle = p["nozzle_temper"].is<float>();
    const bool hasNozzleTarget = p["nozzle_target_temper"].is<float>();
    const bool hasBed = p["bed_temper"].is<float>();
    const bool hasBedTarget = p["bed_target_temper"].is<float>();
    const bool hasChamber = p["chamber_temper"].is<float>();

    if (hasNozzle) out.nozzle = p["nozzle_temper"].as<float>();
    if (hasNozzleTarget) out.nozzleTarget = p["nozzle_target_temper"].as<float>();
    if (hasBed) out.bed = p["bed_temper"].as<float>();
    if (hasBedTarget) out.bedTarget = p["bed_target_temper"].as<float>();
    if (hasChamber) out.chamber = p["chamber_temper"].as<float>();

    // --- packed device.* temperatures, used wherever the classic key is absent ---
    JsonVariantConst dev = p["device"];
    if (dev.is<JsonObjectConst>()) {
        uint32_t packed = 0;
        JsonVariantConst extInfo = dev["extruder"]["info"];
        if (extInfo.is<JsonArrayConst>() && extInfo.size() > 0 &&
            detail::packedFrom(extInfo[0]["temp"], packed)) {
            if (!hasNozzle) out.nozzle = packedCurrent(packed);
            if (!hasNozzleTarget) out.nozzleTarget = packedTarget(packed);
        }
        if (detail::packedFrom(dev["bed"]["info"]["temp"], packed)) {
            if (!hasBed) out.bed = packedCurrent(packed);
            if (!hasBedTarget) out.bedTarget = packedTarget(packed);
        }
        if (detail::packedFrom(dev["ctc"]["info"]["temp"], packed)) {
            if (!hasChamber) out.chamber = packedCurrent(packed);
            // The high word is the active-chamber-heater set point. It reads 0 on
            // every printer without one, which is "no target", not "target 0 C".
            const float ct = packedTarget(packed);
            out.chamberTarget = ct > 0.0f ? ct : REPORT_TEMP_UNKNOWN;
        }
        // H2D air duct: each part's "state" is already a percentage.
        JsonArrayConst parts = dev["airduct"]["parts"];
        if (!parts.isNull()) {
            uint8_t idx = 0;
            for (JsonObjectConst part : parts) {
                if (part["state"].is<float>()) {
                    int pct = (int)part["state"].as<double>();
                    if (pct < 0) pct = 0;
                    if (pct > 100) pct = 100;
                    if (idx == 0) out.fanPart = (int8_t)pct;
                    else if (idx == 1) out.fanAux = (int8_t)pct;
                    else if (idx == 2) out.fanChamber = (int8_t)pct;
                }
                idx++;
            }
        }
    }
    // Last resort for the chamber: some firmwares mirror ctc at print.info.temp.
    if (!hasChamber && isnan(out.chamber)) {
        uint32_t packed = 0;
        if (detail::packedFrom(p["info"]["temp"], packed)) out.chamber = packedCurrent(packed);
    }

    // --- fans ---
    // Note: `fan_gear` is intentionally NOT used to derive percentages. On a live
    // X1C it read 0x6400 while big_fan1_speed was "6" (= 40 %), so the two do not
    // agree; the gear strings are the trustworthy source.
    if (!p["cooling_fan_speed"].isNull())   out.fanPart = detail::gearToPercent(p["cooling_fan_speed"]);
    if (!p["big_fan1_speed"].isNull())      out.fanAux = detail::gearToPercent(p["big_fan1_speed"]);
    if (!p["big_fan2_speed"].isNull())      out.fanChamber = detail::gearToPercent(p["big_fan2_speed"]);
    if (!p["heatbreak_fan_speed"].isNull()) out.fanHeatbreak = detail::gearToPercent(p["heatbreak_fan_speed"]);

    // --- job state ---
    if (p["gcode_state"].is<const char*>())
        detail::copyStr(out.gcodeState, sizeof(out.gcodeState), p["gcode_state"].as<const char*>());
    if (p["stg_cur"].is<float>())           out.stage = (int)p["stg_cur"].as<double>();
    if (p["mc_percent"].is<float>())        out.progress = (int)p["mc_percent"].as<double>();
    if (p["mc_remaining_time"].is<float>()) out.remainingMin = (int)p["mc_remaining_time"].as<double>();
    if (p["layer_num"].is<float>())         out.layer = (int)p["layer_num"].as<double>();
    if (p["total_layer_num"].is<float>())   out.totalLayers = (int)p["total_layer_num"].as<double>();
    if (p["subtask_name"].is<const char*>())
        detail::copyStr(out.task, sizeof(out.task), p["subtask_name"].as<const char*>());
    if (p["print_error"].is<float>())       out.printError = (uint32_t)p["print_error"].as<double>();
    if (p["wifi_signal"].is<const char*>())
        detail::copyStr(out.wifiSignal, sizeof(out.wifiSignal), p["wifi_signal"].as<const char*>());

    // home_flag is signed on the wire; bit 23 is the door/cover switch (front
    // door OR top lid - the printer does not distinguish them).
    if (p["home_flag"].is<float>()) {
        const uint32_t hf = (uint32_t)(int64_t)p["home_flag"].as<int64_t>();
        const bool open = (hf & (1UL << 23)) != 0;
        if (!out.doorRawSeen) {
            // First sighting: this is the raw state, not a transition, and it is
            // not proof that the switch works - on some X1C units it reads
            // "open" from boot to power-off.
            out.doorRawSeen = true;
            out.doorOpen = open;
        } else if (open != out.doorOpen) {
            out.doorOpen = open;
            out.doorKnown = true;     // it moved, so this printer really reports it
            if (out.doorEdgeCount < 0xFFFFu) out.doorEdgeCount++;
            if (open) out.lastDoorOpenMs = nowMs;
            else      out.lastDoorCloseMs = nowMs;
        }
    }

    // --- AMS trays (REWORK-SPEC 16) -----------------------------------------
    // Everything here MERGES. The X1 repeats the whole `ams` block every second,
    // but a P1 sends only what changed - often a single tray - and clearing the
    // other fifteen slots because this report did not mention them would make
    // the filament flicker in and out of existence.
    JsonVariantConst amsv = p["ams"];
    if (amsv.is<JsonObjectConst>()) {
        JsonObjectConst amso = amsv.as<JsonObjectConst>();
        if (!amso["tray_now"].isNull()) {
            const int now = detail::intFrom(amso["tray_now"], -1);
            if (now >= 0 && now <= 255) out.trayNow = (int16_t)now;
        }
        JsonArrayConst units = amso["ams"];
        for (JsonObjectConst unit : units) {
            const int id = detail::intFrom(unit["id"], -1);
            if (id < 0) continue;
            const int slotIdx = reportAmsIndex(out, id, /*create=*/true);
            if (slotIdx < 0) continue;                 // more than four units
            for (JsonObjectConst tr : unit["tray"].as<JsonArrayConst>()) {
                const int slot = detail::intFrom(tr["id"], -1);
                if (slot < 0 || slot >= REPORT_MAX_SLOTS) continue;
                detail::mergeTray(tr, out.trays[slotIdx][slot]);
            }
        }
    }
    JsonVariantConst vt = p["vt_tray"];
    if (vt.is<JsonObjectConst>()) {
        detail::mergeTray(vt.as<JsonObjectConst>(), out.external);
        out.externalSeen = true;
    }
    // H2D: which extruder is active, and what each one has loaded.
    if (dev.is<JsonObjectConst>()) {
        JsonVariantConst ext = dev["extruder"];
        if (ext["state"].is<float>()) out.extruderState = (int16_t)ext["state"].as<double>();
        JsonArrayConst infos = ext["info"];
        uint8_t fallbackId = 0;
        for (JsonObjectConst e : infos) {
            const int id = detail::intFrom(e["id"], fallbackId);
            fallbackId++;
            if (id < 0 || id >= REPORT_MAX_EXTRUDERS) continue;
            if (!e["snow"].isNull()) {
                const int snow = detail::intFrom(e["snow"], -1);
                if (snow >= 0 && snow <= 0xFFFF) out.extruderSnow[id] = (uint16_t)snow;
            }
        }
    }

    return true;
}

// Print phase (REWORK-SPEC 15.1). First rule that matches wins. Pure: the caller
// substitutes Offline when the link itself is down, which this function cannot
// see - it only knows that a report with no gcode_state has told it nothing.
inline Phase reportPhase(const PrinterReport& r)
{
    const char* gs = r.gcodeState;
    if (gs[0] == '\0' || strcmp(gs, "OFFLINE") == 0 || r.stage == -2) return Phase::Offline;

    if (strcmp(gs, "PAUSE") == 0 || stageIsPause(r.stage)) return Phase::Paused;

    const bool running = strcmp(gs, "RUNNING") == 0;
    // A bed or chamber still climbing towards its set point is a preheat even
    // when the printer already calls itself RUNNING - that is exactly the window
    // in which an exhaust fan would be fighting the heaters.
    const bool warmingUp =
        running && ((!isnan(r.bedTarget) && r.bedTarget > 0.0f &&
                     !isnan(r.bed) && r.bed < r.bedTarget - 3.0f) ||
                    (!isnan(r.chamberTarget) && r.chamberTarget > 0.0f &&
                     !isnan(r.chamber) && r.chamber < r.chamberTarget - 2.0f));
    if (stageIsPreheat(r.stage) || warmingUp) return Phase::Preheat;

    if (stageIsCooling(r.stage)) return Phase::Cooling;

    if (running || strcmp(gs, "PREPARE") == 0 || strcmp(gs, "SLICING") == 0) return Phase::Printing;
    if (strcmp(gs, "FINISH") == 0) return Phase::Finished;
    if (strcmp(gs, "FAILED") == 0) return Phase::Failed;
    return Phase::Idle;
}

inline const char* phaseName(Phase p)
{
    switch (p) {
        case Phase::Offline:  return "offline";
        case Phase::Paused:   return "paused";
        case Phase::Preheat:  return "preheat";
        case Phase::Cooling:  return "cooling";
        case Phase::Printing: return "printing";
        case Phase::Finished: return "finished";
        case Phase::Failed:   return "failed";
        case Phase::Idle:     break;
    }
    return "idle";
}

// True when the printer is busy with a job, i.e. what `onlyWhilePrinting` gates
// on. REWORK-SPEC 15.1 defines it as phase in {preheat, printing, paused}, which
// keeps a stage-2 preheat inside the print even before gcode_state moves.
// The door state the control loop is allowed to act on. Until an edge has proved
// that the switch works, "closed" is the only safe reading: a printer whose bit
// is stuck at 1 would otherwise sit under the door rule for every print.
inline bool reportDoorOpen(const PrinterReport& r) { return r.doorKnown && r.doorOpen; }

// Which tray the printer is feeding from (REWORK-SPEC 16.2 step 5). The H2D
// answer wins when the printer has given us one, because on a two-extruder
// machine `tray_now` cannot express "the other tool head is loaded too".
inline ActiveTray reportActiveTray(const PrinterReport& r)
{
    if (r.extruderState >= 0) {
        const ActiveTray h2d = filamentActiveTrayH2D(r.extruderState, r.extruderSnow,
                                                     REPORT_MAX_EXTRUDERS);
        if (h2d.source != TraySource::None) return h2d;
    }
    return filamentActiveTray(r.trayNow);
}

inline bool phaseIsPrinting(Phase p)
{
    return p == Phase::Preheat || p == Phase::Printing || p == Phase::Paused;
}

inline bool reportIsPrinting(const PrinterReport& r) { return phaseIsPrinting(reportPhase(r)); }

}  // namespace blsf

#endif  // BLSF_PRINTER_PARSE_H
