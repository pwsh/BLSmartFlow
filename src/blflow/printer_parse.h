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
//     the door bit (23) is tested.

#ifndef BLSF_PRINTER_PARSE_H
#define BLSF_PRINTER_PARSE_H

#include <ArduinoJson.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

namespace blsf {

// Sentinels for "the printer has not told us yet".
static const float REPORT_TEMP_UNKNOWN = NAN;
static const int8_t REPORT_FAN_UNKNOWN = -1;

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
    bool     doorOpen;
    uint32_t printError;
    char     wifiSignal[12];     // e.g. "-32dBm"

    float    nozzle, nozzleTarget;
    float    bed, bedTarget;
    float    chamber;

    int8_t   fanPart, fanAux, fanChamber, fanHeatbreak;   // percent, -1 unknown
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
    r.nozzle = r.nozzleTarget = r.bed = r.bedTarget = r.chamber = REPORT_TEMP_UNKNOWN;
    r.fanPart = r.fanAux = r.fanChamber = r.fanHeatbreak = REPORT_FAN_UNKNOWN;
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

// Builds the deserialisation filter. The full report has around 90 top-level
// keys including large `ams`, `ipcam`, `xcam`, `net` and `upload` blocks; keeping
// the filter tight is what makes a 16 KB message cheap to parse.
inline void buildPrinterFilter(JsonDocument& filter)
{
    JsonObject p = filter["print"].to<JsonObject>();
    p["command"] = true;
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
    dev["extruder"] = true;
    dev["bed"] = true;
    dev["ctc"] = true;
    dev["airduct"] = true;
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

inline bool packedFrom(JsonVariantConst v, uint32_t& out)
{
    if (!v.is<float>()) return false;
    const double d = v.as<double>();
    if (d < 0 || d > 4294967295.0) return false;
    out = (uint32_t)d;
    return true;
}

}  // namespace detail

// Patches `out` with whatever `root` (a full report document, i.e. the object
// containing "print") carries. Returns false when the message is an ack, is
// empty after filtering, or has no "print" object - in which case `out` is
// untouched and the caller should not treat it as a fresh update.
inline bool parsePrinterReport(JsonVariantConst root, PrinterReport& out)
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
        if (!hasChamber && detail::packedFrom(dev["ctc"]["info"]["temp"], packed)) {
            out.chamber = packedCurrent(packed);
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

    // home_flag is signed on the wire; bit 23 is the door/cover switch.
    if (p["home_flag"].is<float>()) {
        const uint32_t hf = (uint32_t)(int64_t)p["home_flag"].as<int64_t>();
        out.doorOpen = (hf & (1UL << 23)) != 0;
    }

    return true;
}

// True when the printer is actively working on a job.
inline bool reportIsPrinting(const PrinterReport& r)
{
    return strcmp(r.gcodeState, "RUNNING") == 0 ||
           strcmp(r.gcodeState, "PAUSE") == 0 ||
           strcmp(r.gcodeState, "PREPARE") == 0 ||
           strcmp(r.gcodeState, "SLICING") == 0;
}

}  // namespace blsf

#endif  // BLSF_PRINTER_PARSE_H
