// filament_match.h - turning what the printer says about a tray into a guide
// entry, and that entry into an effective cooling profile (REWORK-SPEC 16.2/16.3).
//
// Deliberately free of Arduino *and* of ArduinoJson: everything here works on
// plain strings and integers that the caller has already extracted, so the exact
// code the device runs is exercised on the host (test/test_filament) against
// every row of tools/bambu_filament_ids.csv and against the captured AMS report.
//
// The tables live in the generated filament_db.h and are only ever reached
// through its accessors (filamentDbAt/bambuDbAt), which is what lets the same
// header compile against a PROGMEM blob on the device and plain const data on
// the host.
//
// Why this is more than a lookup table: a Bambu tray reports three loosely
// related things - `tray_type` ("PLA-CF"), `tray_sub_brands` ("PLA Basic") and
// `tray_info_idx` ("GFA00"). Any of them can be empty. A third-party spool in an
// AMS often has only the idx; a spool on the external holder frequently has
// nothing at all. So the matcher walks from the most specific evidence to the
// least, and says "unknown" rather than guessing when it runs out.

#ifndef BLSF_FILAMENT_MATCH_H
#define BLSF_FILAMENT_MATCH_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "filament_db.h"

namespace blsf {

// Where the active filament came from. `Manual` is the user telling us what is
// loaded because the printer cannot (an external spool without RFID, a P1
// without an AMS).
enum class TraySource : uint8_t { None = 0, Ams = 1, External = 2, Manual = 3 };

inline const char* traySourceName(TraySource s)
{
    switch (s) {
        case TraySource::Ams:      return "ams";
        case TraySource::External: return "external";
        case TraySource::Manual:   return "manual";
        case TraySource::None:     break;
    }
    return "none";
}

// The slot the printer is currently feeding from. `ams` is -1 for the external
// spool holder; `slot` is 254 there, matching Bambu's own numbering.
struct ActiveTray {
    TraySource source;
    // int16_t, not int8_t: an AMS-HT reports a unit id of 128 or more, which
    // would come back negative from a signed byte and never match a stored id.
    int16_t    ams;      // 0..3 (or an AMS-HT unit id >= 128), -1 = external/none
    int16_t    slot;     // 0..3, 254 = external, -1 = none
};

// Result of the match: a guide id (empty when nothing matched) plus the family
// string the UI shows, which keeps the modifier the guide has no entry for
// ("PA-GF" resolves to `pa`, but the user still wants to read "PA-GF").
struct FilamentIdent {
    char id[24];
    // 32, like FilamentInfo::name: a tray type never gets near that, but the
    // manual-material fallback copies a guide name in here.
    char family[32];
};

// The user-configurable half of the effective profile (REWORK-SPEC 16.3), in a
// form with no dependency on config.h so it can be built by a test.
struct FilamentOverrideRule {
    char    id[24];            // guide id, or "*" for every material
    int16_t chamberTarget;     // < 0 = keep whatever the guide said
    int16_t cooldownTarget;    // < 0 = keep
    int16_t ventFloor;         // < 0 = keep
    uint8_t cooling;           // 0 = keep, 1 = fast, 2 = gentle
};

static const uint8_t FIL_COOLING_KEEP = 0;
static const uint8_t FIL_COOLING_FAST = 1;
static const uint8_t FIL_COOLING_GENTLE = 2;

struct FilamentPolicy {
    bool     autoEnabled;          // config filament.auto
    uint8_t  ventFloorByVent[3];   // config filament.ventFloor {optional, recommended, required}
    uint8_t  fanChamberTarget;     // config fan.chamberTarget - the fallback
    uint8_t  fanCooldownTarget;    // config fan.cooldownTarget
    const FilamentOverrideRule* overrides;
    uint8_t  overrideCount;
};

struct FilamentEffective {
    uint8_t chamberTarget;     // degC
    uint8_t cooldownTarget;    // degC
    uint8_t ventFloor;         // %, 0 = no floor
    bool    gentle;            // post-print cooling: gentle rather than fast
    bool    keepCool;          // the chamber should stay as cool as it can
    bool    fromGuide;         // a guide entry contributed to these numbers
    bool    overridden;        // a user override changed at least one of them
};

namespace fmdetail {

inline void copyStr(char* dst, size_t dstSize, const char* src)
{
    if (dstSize == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    size_t i = 0;
    for (; i + 1 < dstSize && src[i]; i++) dst[i] = src[i];
    dst[i] = '\0';
}

inline char upper(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c; }

// Upper-cases, drops surrounding whitespace and squeezes the inner spaces of a
// Bambu type string. "  support for pla " -> "SUPPORT FOR PLA".
inline void normalise(const char* in, char* out, size_t outSize)
{
    if (outSize == 0) return;
    out[0] = '\0';
    if (!in) return;
    size_t w = 0;
    bool pendingSpace = false;
    for (const char* p = in; *p && w + 1 < outSize; ++p) {
        const char c = *p;
        if (c == ' ' || c == '\t' || c == '_') {
            if (w > 0) pendingSpace = true;
            continue;
        }
        if (pendingSpace) { out[w++] = ' '; pendingSpace = false; if (w + 1 >= outSize) break; }
        out[w++] = upper(c);
    }
    out[w] = '\0';
}

inline bool startsWith(const char* s, const char* prefix)
{
    while (*prefix) if (*s++ != *prefix++) return false;
    return true;
}

inline bool contains(const char* s, const char* needle) { return strstr(s, needle) != nullptr; }

// Splits a normalised type at the FIRST '-': "PLA-CF" -> "PLA" + "CF",
// "PAHT-CF" -> "PAHT" + "CF", "PLA" -> "PLA" + "".
inline void splitMod(const char* type, char* base, size_t baseSize, char* mod, size_t modSize)
{
    const char* dash = strchr(type, '-');
    if (!dash) { copyStr(base, baseSize, type); if (modSize) mod[0] = '\0'; return; }
    size_t n = (size_t)(dash - type);
    if (n > baseSize - 1) n = baseSize - 1;
    memcpy(base, type, n);
    base[n] = '\0';
    copyStr(mod, modSize, dash + 1);
}

struct BaseMap { const char* base; const char* id; };

// REWORK-SPEC 16.2 step 2. PAHT is Bambu's high-temperature nylon and shares the
// guide's generic `pa` profile; PA6/PA12 have entries of their own.
inline const BaseMap* baseMap(size_t& n)
{
    static const BaseMap kMap[] = {
        {"PLA", "pla"},   {"PETG", "petg"}, {"PCTG", "pctg"}, {"ABS", "abs"},
        {"ASA", "asa"},   {"PC", "pc"},     {"PA", "pa"},     {"PAHT", "pa"},
        {"PA6", "pa6"},   {"PA12", "pa12"}, {"PA66", "pa66"}, {"PPA", "ppa"},
        {"TPU", "tpu"},   {"TPE", "tpe"},   {"PVA", "pva"},   {"BVOH", "bvoh"},
        {"HIPS", "hips"}, {"PET", "pet"},   {"PPS", "pps"},   {"PP", "pp"},
        {"PE", "pe"},     {"EVA", "eva"},   {"PHA", "pha"},   {"PMMA", "pmma"},
        {"PVB", "pvb"},   {"PBT", "pbt"},   {"PPSU", "ppsu"}, {"PEEK", "peek"},
        {"PEKK", "pekk"}, {"PEI", "pei-ultem"},
    };
    n = sizeof(kMap) / sizeof(kMap[0]);
    return kMap;
}

// REWORK-SPEC 16.2 step 4: a support material is only interesting for what it is
// printed *next to*, so it inherits the paired material's chamber profile. The
// soluble ones (PVA/BVOH) and HIPS have entries of their own and never get here.
inline const char* supportPairId(const char* type)
{
    if (contains(type, "PLA")) return "pla";          // Support For PLA, Support For PLA/PETG
    if (contains(type, "PA/PET") || contains(type, "PET")) return "pa";
    if (contains(type, "ABS") || contains(type, "ASA")) return "abs";
    if (contains(type, "SUPPORT W")) return "pla";    // Bambu Support W is a PLA-family support
    if (contains(type, "SUPPORT G")) return "pa";     // ... and Support G a PA-family one
    if (contains(type, "PA")) return "pa";
    return "";
}

// REWORK-SPEC 16.2 step 3, last resort: the id prefix alone. Bambu's own scheme,
// so it only ever fires for ids the CSV does not carry (new products).
inline const char* prefixType(const char* idx)
{
    if (startsWith(idx, "GFA") || startsWith(idx, "GFL")) return "PLA";
    if (startsWith(idx, "GFB")) return "ABS";
    if (startsWith(idx, "GFC")) return "PC";
    if (startsWith(idx, "GFG")) return "PETG";
    if (startsWith(idx, "GFN")) return "PA";
    if (startsWith(idx, "GFP")) return "PP";
    if (startsWith(idx, "GFT")) return "PPS";
    if (startsWith(idx, "GFU")) return "TPU";
    return "";
}

}  // namespace fmdetail

// --- table lookups ---------------------------------------------------------

// Case-sensitive on purpose: guide ids are lower-case by construction.
inline const FilamentInfo* filamentInfoById(const char* id)
{
    if (!id || !*id) return nullptr;
    for (uint16_t i = 0; i < filamentDbCount(); i++) {
        const FilamentInfo* f = filamentDbAt(i);
        if (f && strcmp(f->id, id) == 0) return f;
    }
    return nullptr;
}

// "GFB00" -> "ABS". Empty when the id is not one of the 100 known ones.
inline const char* bambuTypeForIdx(const char* idx)
{
    if (!idx || !*idx) return "";
    char up[16];
    fmdetail::normalise(idx, up, sizeof(up));
    for (uint16_t i = 0; i < bambuDbCount(); i++) {
        const BambuFilament* b = bambuDbAt(i);
        if (b && strcmp(b->idx, up) == 0) return b->type;
    }
    return "";
}

// --- matching --------------------------------------------------------------

namespace fmdetail {

// Resolves one already-normalised type string. Fills `out` and returns true when
// a guide entry was found; on failure `family` is still set, so the UI can show
// "EVA" for a material the guide has never covered.
inline bool identifyType(const char* type, FilamentIdent& out)
{
    copyStr(out.family, sizeof(out.family), type);
    out.id[0] = '\0';
    if (!*type) return false;

    if (startsWith(type, "SUPPORT")) {
        const char* paired = supportPairId(type);
        if (*paired && filamentInfoById(paired)) { copyStr(out.id, sizeof(out.id), paired); return true; }
        return false;
    }

    char base[16], mod[16];
    splitMod(type, base, sizeof(base), mod, sizeof(mod));

    size_t n = 0;
    const BaseMap* map = baseMap(n);
    const char* baseId = nullptr;
    for (size_t i = 0; i < n; i++) {
        if (strcmp(map[i].base, base) == 0) { baseId = map[i].id; break; }
    }
    if (!baseId) return false;

    // A filled variant gets its own guide entry when one exists (pla-cf, pa6-gf,
    // ...); otherwise it falls back to the unfilled polymer, which is the right
    // answer for a chamber set point - the fibre changes the stiffness, not the
    // temperature the enclosure wants.
    if (strcmp(mod, "CF") == 0 || strcmp(mod, "GF") == 0) {
        char variant[24];
        copyStr(variant, sizeof(variant), baseId);
        const size_t len = strlen(variant);
        if (len + 3 < sizeof(variant)) {
            variant[len] = '-';
            variant[len + 1] = (char)(mod[0] - 'A' + 'a');
            variant[len + 2] = 'f';
            variant[len + 3] = '\0';
            if (filamentInfoById(variant)) { copyStr(out.id, sizeof(out.id), variant); return true; }
        }
    }
    // AERO (foaming PLA), AMS (the AMS-compatible TPU grade) and everything else
    // unknown are marketing suffixes, not different polymers.
    if (!filamentInfoById(baseId)) return false;
    copyStr(out.id, sizeof(out.id), baseId);
    return true;
}

}  // namespace fmdetail

// REWORK-SPEC 16.2, steps 1-4. Any argument may be null or empty.
//
//   tray_type first (the printer's own classification),
//   then tray_info_idx through the Bambu table,
//   then the first word of tray_sub_brands ("PLA Basic" -> "PLA"),
//   then the id prefix (GFB.. -> ABS).
inline FilamentIdent filamentIdentify(const char* trayType, const char* subBrands, const char* trayIdx)
{
    FilamentIdent out;
    out.id[0] = out.family[0] = '\0';

    char type[24];
    fmdetail::normalise(trayType, type, sizeof(type));
    if (*type && fmdetail::identifyType(type, out)) return out;

    // Keep the printer's own word for the material even if we cannot place it -
    // identifyType() has already put it in `family`.
    FilamentIdent fallback = out;

    const char* byIdx = bambuTypeForIdx(trayIdx);
    if (*byIdx) {
        char t[24];
        fmdetail::normalise(byIdx, t, sizeof(t));
        if (fmdetail::identifyType(t, out)) return out;
        if (!*fallback.family) fallback = out;
    }

    if (subBrands && *subBrands) {
        char sb[32];
        fmdetail::normalise(subBrands, sb, sizeof(sb));
        char* space = strchr(sb, ' ');
        if (space) *space = '\0';
        if (*sb && fmdetail::identifyType(sb, out)) return out;
    }

    char idx[16];
    fmdetail::normalise(trayIdx, idx, sizeof(idx));
    const char* byPrefix = fmdetail::prefixType(idx);
    if (*byPrefix && fmdetail::identifyType(byPrefix, out)) return out;

    return fallback;
}

// --- which tray is loaded --------------------------------------------------

// `print.ams.tray_now`, the encoding every printer except the H2D uses:
// 0..15 = AMS unit * 4 + slot, 254 = the external spool holder (vt_tray),
// 255 (or absent) = nothing loaded. AMS-HT units report their own id, which is
// >= 128 and has a single slot.
inline ActiveTray filamentActiveTray(int trayNow)
{
    ActiveTray t{TraySource::None, -1, -1};
    if (trayNow < 0 || trayNow == 255) return t;
    if (trayNow == 254) { t.source = TraySource::External; t.ams = -1; t.slot = 254; return t; }
    if (trayNow >= 128) { t.source = TraySource::Ams; t.ams = (int16_t)trayNow; t.slot = 0; return t; }
    t.source = TraySource::Ams;
    t.ams = (int16_t)(trayNow / 4);
    t.slot = (int16_t)(trayNow % 4);
    return t;
}

// The H2D reports per-extruder instead: `device.extruder.state` carries the
// active extruder in bits 4..7 and `device.extruder.info[i].snow` is the loaded
// slot as (ams << 8) | slot. 0xFF/0xFFFF in either half means "nothing".
inline ActiveTray filamentActiveTrayH2D(int extruderState, const uint16_t* snow, uint8_t snowCount)
{
    ActiveTray t{TraySource::None, -1, -1};
    if (extruderState < 0 || !snow || snowCount == 0) return t;
    const uint8_t active = (uint8_t)((extruderState >> 4) & 0x0F);
    if (active >= snowCount) return t;
    const uint16_t s = snow[active];
    if (s == 0xFFFFu) return t;
    const uint8_t ams = (uint8_t)((s >> 8) & 0xFF);
    const uint8_t slot = (uint8_t)(s & 0xFF);
    if (ams == 0xFF || slot == 0xFF) return t;
    if (ams == 0xFE || slot == 0xFE) { t.source = TraySource::External; t.ams = -1; t.slot = 254; return t; }
    t.source = TraySource::Ams;
    t.ams = (int16_t)ams;
    t.slot = (int16_t)(ams >= 128 ? 0 : slot);
    return t;
}

// --- effective cooling profile (REWORK-SPEC 16.3) --------------------------

namespace fmdetail {

inline uint8_t clampU8(int v, int lo, int hi)
{
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return (uint8_t)v;
}

inline void applyOverride(const FilamentOverrideRule& r, FilamentEffective& e, bool& touched)
{
    if (r.chamberTarget >= 0)  { e.chamberTarget = clampU8(r.chamberTarget, 20, 80); touched = true; }
    if (r.cooldownTarget >= 0) { e.cooldownTarget = clampU8(r.cooldownTarget, 15, 60); touched = true; }
    if (r.ventFloor >= 0)      { e.ventFloor = clampU8(r.ventFloor, 0, 100); touched = true; }
    if (r.cooling != FIL_COOLING_KEEP) { e.gentle = r.cooling == FIL_COOLING_GENTLE; touched = true; }
}

}  // namespace fmdetail

// Guide profile -> "*" override -> id override. With `auto` off the plain fan.*
// values win outright: the user has said "do not let the filament move my set
// points", and a vent floor pushed by a material would be exactly that.
inline FilamentEffective filamentEffective(const FilamentInfo* info, const FilamentPolicy& p)
{
    FilamentEffective e;
    e.chamberTarget = p.fanChamberTarget;
    e.cooldownTarget = p.fanCooldownTarget;
    e.ventFloor = 0;
    e.gentle = false;
    e.keepCool = false;
    e.fromGuide = false;
    e.overridden = false;
    if (!p.autoEnabled) return e;

    if (info) {
        // "Keep it cool" is either the guide saying the enclosure should be open
        // for cooling (PLA, PVA, TPU) or an ambient recommendation that is barely
        // above room temperature. In that case the ceiling of the band is the
        // right set point: it is the warmest the chamber may get, not a target.
        const bool openForCooling = (info->flags & FIL_ENCLOSURE_OPEN_FOR_COOLING) != 0;
        const bool coolRec = info->chamberRec != FIL_TEMP_NA && info->chamberRec < 35;
        e.keepCool = openForCooling || coolRec;

        int target = e.keepCool ? info->chamberMax : info->chamberRec;
        if (target == FIL_TEMP_NA) target = e.keepCool ? info->chamberRec : info->chamberMax;
        if (target != FIL_TEMP_NA) {
            e.chamberTarget = fmdetail::clampU8(target, 20, 80);
            e.fromGuide = true;
        }
        // A material the guide wants printed with the part fan off does not want
        // the chamber emptied at full blast the moment the print ends either -
        // that is when a tall ABS part cracks.
        e.gentle = info->partCoolRec != FIL_PARTCOOL_NA && info->partCoolRec < 50;
        const uint8_t vent = info->vent <= VENT_REQUIRED ? info->vent : (uint8_t)VENT_RECOMMENDED;
        e.ventFloor = p.ventFloorByVent[vent];
        e.fromGuide = true;
    }

    bool touched = false;
    for (uint8_t pass = 0; pass < 2; pass++) {
        for (uint8_t i = 0; i < p.overrideCount; i++) {
            const FilamentOverrideRule& r = p.overrides[i];
            if (!r.id[0]) continue;
            const bool isStar = strcmp(r.id, "*") == 0;
            if (pass == 0 ? !isStar : isStar) continue;
            if (pass == 1 && (!info || strcmp(r.id, info->id) != 0)) continue;
            fmdetail::applyOverride(r, e, touched);
        }
    }
    e.overridden = touched;
    return e;
}

}  // namespace blsf

#endif  // BLSF_FILAMENT_MATCH_H
