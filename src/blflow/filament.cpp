// The one translation unit that carries the generated tables.
#define BLSF_FILAMENT_DB_DEFINE
#include "filament.h"

#include <Arduino.h>

namespace blsf {

namespace {

// Copies the tray the printer says is active. Returns false when that slot has
// never been described - a P1 without an AMS reports `tray_now` 254 and a
// vt_tray full of empty strings, which is "external holder, contents unknown".
bool activeTrayReport(const PrinterReport& p, const ActiveTray& at, TrayReport& out)
{
    memset(&out, 0, sizeof(out));
    if (at.source == TraySource::None) return false;
    const int amsId = at.source == TraySource::External ? -1 : at.ams;
    const TrayReport* t = reportTray(p, amsId, at.source == TraySource::External ? 0 : at.slot);
    if (!t) return false;
    out = *t;
    return trayLoaded(out);
}

// The config's overrides are already FilamentOverrideRule, so the policy is a
// view onto them rather than a copy.
FilamentPolicy policyFrom(const FilamentConfig& fc, const FanConfig& fan)
{
    FilamentPolicy p{};
    p.autoEnabled = fc.autoDetect;
    for (uint8_t i = 0; i < 3; i++) p.ventFloorByVent[i] = fc.ventFloor[i];
    p.fanChamberTarget = fan.chamberTarget;
    p.fanCooldownTarget = fan.cooldownTarget;
    p.overrides = fc.overrides;
    p.overrideCount = fc.overrideCount;
    return p;
}

void setStr(JsonObject o, const char* key, const char* v)
{
    // ArduinoJson stores a bare const char* by pointer; every one of these points
    // into a caller local or a snapshot, so they are all copied via String().
    if (v && *v) o[key] = String(v);
    else o[key] = nullptr;
}

}  // namespace

namespace {

// The material of the print that just ended. The AMS unloads at FINISH, so
// tray_now goes to "none" exactly when the cool-down rule (gentle vs fast)
// needs the material. Remembered until a new tray is loaded or a new job
// starts. Written from whichever task resolves first, hence the spinlock.
struct LastTray {
    bool       valid = false;
    TraySource source = TraySource::None;
    int16_t    ams = -1;
    int16_t    slot = -1;
    TrayReport tray{};
};
LastTray     g_last;
portMUX_TYPE g_lastMux = portMUX_INITIALIZER_UNLOCKED;

bool phaseKeepsLastTray(Phase ph)
{
    return ph == Phase::Finished || ph == Phase::Cooling || ph == Phase::Idle ||
           ph == Phase::Failed || ph == Phase::Offline;
}

}  // namespace

FilamentStatus filamentResolve(const PrinterReport& p, const FilamentConfig& fc,
                               const FanConfig& fan)
{
    FilamentStatus s{};
    s.source = TraySource::None;
    s.ams = -1;
    s.slot = -1;
    s.info = nullptr;
    s.id[0] = s.family[0] = '\0';

    const ActiveTray at = reportActiveTray(p);
    s.source = at.source;
    s.ams = at.ams;
    s.slot = at.slot;
    s.trayKnown = activeTrayReport(p, at, s.tray);

    if (s.trayKnown) {
        portENTER_CRITICAL(&g_lastMux);
        g_last.valid = true;
        g_last.source = s.source;
        g_last.ams = s.ams;
        g_last.slot = s.slot;
        g_last.tray = s.tray;
        portEXIT_CRITICAL(&g_lastMux);
    } else if (phaseKeepsLastTray(reportPhase(p))) {
        LastTray last;
        portENTER_CRITICAL(&g_lastMux);
        last = g_last;
        portEXIT_CRITICAL(&g_lastMux);
        if (last.valid) {
            s.source = TraySource::Last;
            s.ams = last.ams;
            s.slot = last.slot;
            s.tray = last.tray;
            s.trayKnown = true;
        }
    } else {
        // A new job started with no tray: forget the old material.
        portENTER_CRITICAL(&g_lastMux);
        g_last.valid = false;
        portEXIT_CRITICAL(&g_lastMux);
    }

    if (s.trayKnown) {
        const FilamentIdent ident = filamentIdentify(s.tray.type, s.tray.sub, s.tray.idx);
        memcpy(s.id, ident.id, sizeof(s.id));
        memcpy(s.family, ident.family, sizeof(s.family));
        s.info = filamentInfoById(s.id);
    }

    // Nothing usable from the printer: an external spool with no RFID tag, or a
    // P1 with no AMS at all. This is what filament.manualId exists for.
    if (!s.info && fc.manualId[0] != '\0') {
        const FilamentInfo* manual = filamentInfoById(fc.manualId);
        if (manual) {
            s.info = manual;
            memcpy(s.id, manual->id, sizeof(s.id));
            if (s.family[0] == '\0') strlcpy(s.family, manual->name, sizeof(s.family));
            if (!s.trayKnown) {
                s.source = TraySource::Manual;
                s.ams = -1;
                s.slot = -1;
            }
        }
    }

    const FilamentPolicy policy = policyFrom(fc, fan);
    s.eff = filamentEffective(s.info, policy);
    return s;
}

void filamentToJson(JsonObject out, const PrinterReport& p, const FilamentConfig& fc,
                    const FanConfig& fan)
{
    const FilamentStatus s = filamentResolve(p, fc, fan);

    out["source"] = traySourceName(s.source);
    out["auto"] = fc.autoDetect;

    if (s.trayKnown) {
        JsonObject t = out["tray"].to<JsonObject>();
        t["ams"] = s.ams;
        t["slot"] = s.slot;
        setStr(t, "type", s.tray.type);
        setStr(t, "subBrand", s.tray.sub);
        setStr(t, "idx", s.tray.idx);
        setStr(t, "color", s.tray.color);
    } else {
        out["tray"] = nullptr;
    }

    setStr(out, "id", s.id);
    setStr(out, "name", s.info ? s.info->name : "");
    setStr(out, "family", s.family);

    if (s.info) {
        JsonObject pr = out["profile"].to<JsonObject>();
        // FIL_TEMP_NA is "the guide has no figure", which is a null, not a -1.
        if (s.info->chamberRec != FIL_TEMP_NA) pr["chamberRec"] = s.info->chamberRec;
        else pr["chamberRec"] = nullptr;
        if (s.info->chamberMax != FIL_TEMP_NA) pr["chamberMax"] = s.info->chamberMax;
        else pr["chamberMax"] = nullptr;
        if (s.info->partCoolRec != FIL_PARTCOOL_NA) pr["partCoolRec"] = s.info->partCoolRec;
        else pr["partCoolRec"] = nullptr;
        pr["vent"] = filamentVentName(s.info->vent);
        pr["openForCooling"] = (s.info->flags & FIL_ENCLOSURE_OPEN_FOR_COOLING) != 0;
        pr["heatedRequired"] = (s.info->flags & FIL_HEATED_CHAMBER_REQUIRED) != 0;
    } else {
        out["profile"] = nullptr;
    }

    JsonObject e = out["effective"].to<JsonObject>();
    e["chamberTarget"] = s.eff.chamberTarget;
    e["cooldownTarget"] = s.eff.cooldownTarget;
    e["ventFloor"] = s.eff.ventFloor;
    e["postPrintCooling"] = s.eff.gentle ? "gentle" : "fast";
    e["overridden"] = s.eff.overridden;

    // Every tray the printer has ever described, for the UI's AMS list. Empty
    // slots are included (with null fields) so the list keeps its shape.
    JsonArray trays = out["trays"].to<JsonArray>();
    for (uint8_t u = 0; u < REPORT_MAX_AMS; u++) {
        if (p.amsId[u] < 0) continue;
        for (uint8_t slot = 0; slot < REPORT_MAX_SLOTS; slot++) {
            const TrayReport& t = p.trays[u][slot];
            if (!trayLoaded(t)) continue;
            JsonObject o = trays.add<JsonObject>();
            o["ams"] = p.amsId[u];
            o["slot"] = slot;
            setStr(o, "type", t.type);
            setStr(o, "subBrand", t.sub);
            setStr(o, "idx", t.idx);
            setStr(o, "color", t.color);
            const FilamentIdent id = filamentIdentify(t.type, t.sub, t.idx);
            setStr(o, "id", id.id);
        }
    }
    if (p.externalSeen && trayLoaded(p.external)) {
        JsonObject o = trays.add<JsonObject>();
        o["ams"] = -1;
        o["slot"] = 254;
        setStr(o, "type", p.external.type);
        setStr(o, "subBrand", p.external.sub);
        setStr(o, "idx", p.external.idx);
        setStr(o, "color", p.external.color);
        const FilamentIdent id = filamentIdentify(p.external.type, p.external.sub, p.external.idx);
        setStr(o, "id", id.id);
    }
}

void filamentDbToJson(JsonObject out)
{
    out["count"] = filamentDbCount();
    out["source"] = "https://pwsh.github.io/filament-field-guide";
    out["licence"] = "CC BY 4.0";
    // Short keys and no nesting: 90 records go out on one 8 kB response, which
    // the UI fetches once and keeps.
    JsonArray arr = out["filaments"].to<JsonArray>();
    for (uint16_t i = 0; i < filamentDbCount(); i++) {
        const FilamentInfo* f = filamentDbAt(i);
        if (!f) continue;
        JsonObject o = arr.add<JsonObject>();
        o["id"] = f->id;                 // PROGMEM literal: stable for the document's life
        o["name"] = f->name;
        o["cls"] = filamentClassName(f->polymerClass);
        if (f->chamberMin != FIL_TEMP_NA) o["cMin"] = f->chamberMin; else o["cMin"] = nullptr;
        if (f->chamberRec != FIL_TEMP_NA) o["cRec"] = f->chamberRec; else o["cRec"] = nullptr;
        if (f->chamberMax != FIL_TEMP_NA) o["cMax"] = f->chamberMax; else o["cMax"] = nullptr;
        if (f->partCoolRec != FIL_PARTCOOL_NA) o["cool"] = f->partCoolRec; else o["cool"] = nullptr;
        o["vent"] = filamentVentName(f->vent);
        o["flags"] = f->flags;
        o["voc"] = filamentLevelName(f->voc);
        o["part"] = filamentLevelName(f->particulate);
    }
}

}  // namespace blsf
