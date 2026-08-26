#!/usr/bin/env python3
"""Generate src/blflow/filament_db.h from the Filament Field Guide.

The guide (https://github.com/pwsh/filament-field-guide, data CC BY 4.0) records,
per material, the recommended ambient/chamber temperature, whether the enclosure
should be open for cooling, the part-cooling demand and the ventilation demand.
That is exactly what the firmware needs to pick a chamber set point and a
ventilation floor for the filament the printer says is loaded.

Only the handful of fields the firmware uses are emitted, as fixed-size records,
so the whole table costs a few kilobytes of flash and needs no parser at runtime.

  python3 tools/gen_filament_db.py                       # fetch over HTTPS
  python3 tools/gen_filament_db.py --src ../filament-field-guide
  python3 tools/gen_filament_db.py --src DIR --check      # CI: is the header current?

Standard library only, so it runs in the same environment as the PlatformIO build.
"""

import argparse
import csv
import datetime
import json
import os
import re
import sys
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

BASE_URL = "https://pwsh.github.io/filament-field-guide"
INDEX_URL = BASE_URL + "/data/index.json"
ENTRY_URL = BASE_URL + "/data/filaments/%s.json"
SITE_URL = "https://pwsh.github.io/filament-field-guide/#/filaments/%s"

DEFAULT_CSV = os.path.join(HERE, "bambu_filament_ids.csv")
DEFAULT_OUT = os.path.join(ROOT, "src", "blflow", "filament_db.h")

# Field widths, mirrored by the C++ struct. Anything longer is a data error, not
# something to silently truncate: the id is a lookup key and the name is shown
# in the UI, so both are checked rather than cut.
ID_LEN, NAME_LEN = 24, 32
IDX_LEN, TYPE_LEN = 8, 12

VENT = {"optional": 0, "recommended": 1, "required": 2}
LEVEL = {"none": 0, "low": 1, "moderate": 2, "high": 3}

# bit0 enclosure recommended, bit1 heated chamber required,
# bit2 enclosure open for cooling, bit3 hardened nozzle required
FLAG_ENCLOSURE, FLAG_HEATED, FLAG_OPEN_COOL, FLAG_HARDENED = 1, 2, 4, 8

TEMP_NA = -1          # the guide has no ambient figure for this material
PARTCOOL_NA = 255     # ... and none for part cooling


# --------------------------------------------------------------------------
# reading the guide
# --------------------------------------------------------------------------

def fetch_json(url):
    with urllib.request.urlopen(url, timeout=30) as r:
        return json.loads(r.read().decode("utf-8"))


def read_json(path):
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def load_guide(src):
    """Returns (index, {id: entry}, source_description)."""
    if src:
        index = read_json(os.path.join(src, "data", "index.json"))
        entries = {}
        for stub in index["filaments"]:
            entries[stub["id"]] = read_json(
                os.path.join(src, "data", "filaments", stub["id"] + ".json"))
        return index, entries, os.path.abspath(src)
    index = fetch_json(INDEX_URL)
    entries = {}
    for stub in index["filaments"]:
        entries[stub["id"]] = fetch_json(ENTRY_URL % stub["id"])
    return index, entries, INDEX_URL


def short_name(name):
    """'ABS (Acrylonitrile Butadiene Styrene)' -> 'ABS'. The parenthesis is the
    guide's long-form gloss; the UI has no room for it and the tooltip carries
    the detail instead."""
    return name.split(" (")[0].strip()


def temp(block, key):
    v = (block or {}).get(key)
    if v is None:
        return TEMP_NA
    v = int(round(v))
    return max(-1, min(127, v))


def class_token(polymer_class):
    return "FCLASS_" + re.sub(r"[^A-Z0-9]+", "_", (polymer_class or "other").upper())


def record(entry):
    p = entry.get("printing") or {}
    amb = p.get("ambient_temp_c") or {}
    cool = p.get("part_cooling_fan_pct") or {}
    em = entry.get("emissions") or {}

    flags = 0
    if p.get("enclosure_recommended"):
        flags |= FLAG_ENCLOSURE
    if p.get("heated_chamber_required"):
        flags |= FLAG_HEATED
    if p.get("enclosure_open_for_cooling"):
        flags |= FLAG_OPEN_COOL
    if p.get("requires_hardened_nozzle"):
        flags |= FLAG_HARDENED

    pc = cool.get("recommended")
    return {
        "id": entry["id"],
        "name": short_name(entry["name"]),
        "cls": class_token(entry.get("polymer_class")),
        "cmin": temp(amb, "min"),
        "crec": temp(amb, "recommended"),
        "cmax": temp(amb, "max"),
        "partcool": PARTCOOL_NA if pc is None else max(0, min(100, int(round(pc)))),
        "vent": VENT.get(em.get("ventilation"), VENT["recommended"]),
        "flags": flags,
        "voc": LEVEL.get(em.get("voc_level"), 0),
        "part": LEVEL.get(em.get("particulate_level"), 0),
    }


def load_bambu(path):
    rows = []
    with open(path, "r", encoding="utf-8", newline="") as f:
        for row in csv.DictReader(f):
            idx = (row.get("filament_id") or "").strip()
            typ = (row.get("filament_type") or "").strip().upper()
            if not idx or not typ:
                continue
            rows.append((idx, typ))
    rows.sort(key=lambda r: r[0])
    return rows


# --------------------------------------------------------------------------
# emitting the header
# --------------------------------------------------------------------------

DATE_MARK = "// Fetched:"


def c_str(s, width, what):
    if len(s) >= width:
        raise SystemExit("%s %r does not fit in char[%d]" % (what, s, width))
    return '"%s"' % s.replace("\\", "\\\\").replace('"', '\\"')


def emit(records, bambu, index, date):
    # NOTE: the header never records *where* the data was read from - a --src path
    # is one developer's scratch directory and would make --check fail for everyone
    # else. The canonical URL is the provenance; --src is only a transport.
    classes = sorted({r["cls"] for r in records})
    counts = index.get("counts", {})

    o = []
    w = o.append
    w("// filament_db.h - the Filament Field Guide, as a flash-resident table.")
    w("//")
    w("// GENERATED by tools/gen_filament_db.py - do not edit by hand. Regenerate with")
    w("//   python3 tools/gen_filament_db.py [--src <clone of the guide>]")
    w("// and verify a committed copy with `--check` (exits non-zero when stale).")
    w("//")
    w("// Source:  %s" % INDEX_URL)
    w("//          (+ data/filaments/<id>.json for each entry)")
    w("%s %s" % (DATE_MARK, date))
    w("// Records: %d filaments (guide index reports %s), %d Bambu filament ids"
      % (len(records), counts.get("filaments", "?"), len(bambu)))
    w("//")
    w("// The filament data is (c) the Filament Field Guide contributors and is used")
    w("// under the Creative Commons Attribution 4.0 International licence")
    w("// (CC BY 4.0, https://creativecommons.org/licenses/by/4.0/). The UI carries the")
    w('// same credit next to every value it shows ("Data: Filament Field Guide, CC BY 4.0").')
    w("//")
    w("// Only the fields the fan controller needs are kept: the ambient/chamber band,")
    w("// the part-cooling recommendation, the ventilation demand and four flags. Strings")
    w("// are fixed-size so the whole table is one contiguous PROGMEM blob with no")
    w("// pointers to relocate and no parsing at runtime.")
    w("//")
    w("// Definitions are compiled into exactly one translation unit: the file that")
    w("// defines BLSF_FILAMENT_DB_DEFINE before including this header (filament.cpp on")
    w("// the device, the test binary on the host). Everyone else gets the declarations.")
    w("")
    w("#ifndef BLSF_FILAMENT_DB_H")
    w("#define BLSF_FILAMENT_DB_H")
    w("")
    w("#include <stdint.h>")
    w("")
    w("#if defined(ARDUINO) || defined(ESP_PLATFORM)")
    w("#include <pgmspace.h>")
    w("#endif")
    w("#ifndef PROGMEM")
    w("#define PROGMEM")   # host builds: the tables are ordinary const data
    w("#endif")
    w("")
    w("namespace blsf {")
    w("")
    w("// Polymer family, straight from the guide's `polymer_class`.")
    w("enum FilamentClass : uint8_t {")
    for i, c in enumerate(classes):
        w("    %s = %d," % (c, i))
    w("};")
    w("")
    w("// `emissions.ventilation`: how badly the fumes need to go outside.")
    w("enum FilamentVent : uint8_t { VENT_OPTIONAL = 0, VENT_RECOMMENDED = 1, VENT_REQUIRED = 2 };")
    w("")
    w("// `emissions.voc_level` / `emissions.particulate_level`.")
    w("enum FilamentLevel : uint8_t { LEVEL_NONE = 0, LEVEL_LOW = 1, LEVEL_MODERATE = 2, LEVEL_HIGH = 3 };")
    w("")
    w("// FilamentInfo::flags")
    w("static const uint8_t FIL_ENCLOSURE_RECOMMENDED = 0x01;")
    w("static const uint8_t FIL_HEATED_CHAMBER_REQUIRED = 0x02;")
    w("static const uint8_t FIL_ENCLOSURE_OPEN_FOR_COOLING = 0x04;")
    w("static const uint8_t FIL_HARDENED_NOZZLE = 0x08;")
    w("")
    w("// Sentinels: the guide simply has no figure for some materials.")
    w("static const int8_t  FIL_TEMP_NA = %d;" % TEMP_NA)
    w("static const uint8_t FIL_PARTCOOL_NA = %d;" % PARTCOOL_NA)
    w("")
    w("struct FilamentInfo {")
    w("    char    id[%d];         // guide id, e.g. \"pla-cf\" - also the URL fragment" % ID_LEN)
    w("    char    name[%d];       // display name, e.g. \"ABS\"" % NAME_LEN)
    w("    uint8_t polymerClass;   // FilamentClass")
    w("    int8_t  chamberMin;     // degC ambient band, FIL_TEMP_NA when the guide has none")
    w("    int8_t  chamberRec;")
    w("    int8_t  chamberMax;")
    w("    uint8_t partCoolRec;    // recommended part-cooling fan, %, FIL_PARTCOOL_NA if unknown")
    w("    uint8_t vent;           // FilamentVent")
    w("    uint8_t flags;          // FIL_* bits")
    w("    uint8_t voc;            // FilamentLevel")
    w("    uint8_t particulate;    // FilamentLevel")
    w("};")
    w("")
    w("// A bare `tray_info_idx` (\"GFB00\") with no usable tray_type still has to")
    w("// resolve to a material, so the Bambu Studio id list travels with us.")
    w("struct BambuFilament {")
    w("    char idx[%d];           // \"GFA00\" .. \"GFSNL08\"" % IDX_LEN)
    w("    char type[%d];          // Bambu filament_type, e.g. \"PLA-CF\"" % TYPE_LEN)
    w("};")
    w("")
    w("static const uint16_t FILAMENT_DB_COUNT = %d;" % len(records))
    w("static const uint16_t BAMBU_DB_COUNT = %d;" % len(bambu))
    w("")
    w("extern const FilamentInfo FILAMENT_DB[FILAMENT_DB_COUNT] PROGMEM;")
    w("extern const BambuFilament BAMBU_DB[BAMBU_DB_COUNT] PROGMEM;")
    w("")
    w("// ESP32 flash is memory-mapped, so a PROGMEM record is read like any other")
    w("// const object. These accessors exist so filament_match.h never touches the")
    w("// arrays directly and stays compilable on the host.")
    w("inline const FilamentInfo* filamentDbAt(uint16_t i)")
    w("{")
    w("    return i < FILAMENT_DB_COUNT ? &FILAMENT_DB[i] : nullptr;")
    w("}")
    w("inline uint16_t filamentDbCount() { return FILAMENT_DB_COUNT; }")
    w("inline const BambuFilament* bambuDbAt(uint16_t i)")
    w("{")
    w("    return i < BAMBU_DB_COUNT ? &BAMBU_DB[i] : nullptr;")
    w("}")
    w("inline uint16_t bambuDbCount() { return BAMBU_DB_COUNT; }")
    w("")
    w("inline const char* filamentClassName(uint8_t c)")
    w("{")
    w("    switch (c) {")
    for c in classes:
        label = c[len("FCLASS_"):].lower().replace("_", "-")
        w('        case %s: return "%s";' % (c, label))
    w('        default: return "other";')
    w("    }")
    w("}")
    w("")
    w('inline const char* filamentVentName(uint8_t v)')
    w("{")
    w('    return v == VENT_REQUIRED ? "required" : (v == VENT_RECOMMENDED ? "recommended" : "optional");')
    w("}")
    w("")
    w('inline const char* filamentLevelName(uint8_t v)')
    w("{")
    w('    switch (v) {')
    w('        case LEVEL_LOW: return "low";')
    w('        case LEVEL_MODERATE: return "moderate";')
    w('        case LEVEL_HIGH: return "high";')
    w('        default: return "none";')
    w("    }")
    w("}")
    w("")
    w("#ifdef BLSF_FILAMENT_DB_DEFINE")
    w("")
    w("// { id, name, class, chamberMin/Rec/Max, partCool, vent, flags, voc, particulate }")
    w("const FilamentInfo FILAMENT_DB[FILAMENT_DB_COUNT] PROGMEM = {")
    for r in records:
        w("    {%s, %s, %s, %d, %d, %d, %d, %d, 0x%02X, %d, %d},"
          % (c_str(r["id"], ID_LEN, "filament id"),
             c_str(r["name"], NAME_LEN, "filament name"),
             r["cls"], r["cmin"], r["crec"], r["cmax"], r["partcool"],
             r["vent"], r["flags"], r["voc"], r["part"]))
    w("};")
    w("")
    w("const BambuFilament BAMBU_DB[BAMBU_DB_COUNT] PROGMEM = {")
    for idx, typ in bambu:
        w("    {%s, %s}," % (c_str(idx, IDX_LEN, "bambu id"), c_str(typ, TYPE_LEN, "bambu type")))
    w("};")
    w("")
    w("#endif  // BLSF_FILAMENT_DB_DEFINE")
    w("")
    w("}  // namespace blsf")
    w("")
    w("#endif  // BLSF_FILAMENT_DB_H")
    w("")
    return "\n".join(o)


def strip_date(text):
    """--check compares content, not the day it was generated on."""
    return "\n".join(l for l in text.splitlines() if not l.startswith(DATE_MARK))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--src", metavar="DIR",
                    help="local clone of filament-field-guide (offline); default: fetch over HTTPS")
    ap.add_argument("--csv", default=DEFAULT_CSV, help="Bambu filament id list (default: %(default)s)")
    ap.add_argument("--out", default=DEFAULT_OUT, help="header to write (default: %(default)s)")
    ap.add_argument("--check", action="store_true",
                    help="do not write; exit 1 if the committed header differs from a fresh generation")
    args = ap.parse_args()

    index, entries, _source = load_guide(args.src)
    records = sorted((record(e) for e in entries.values()), key=lambda r: r["id"])
    bambu = load_bambu(args.csv)
    date = datetime.date.today().isoformat()
    text = emit(records, bambu, index, date)

    if args.check:
        if not os.path.exists(args.out):
            print("filament_db: %s does not exist" % args.out, file=sys.stderr)
            return 1
        with open(args.out, "r", encoding="utf-8") as f:
            have = f.read()
        if strip_date(have) != strip_date(text):
            print("filament_db: %s is out of date - rerun tools/gen_filament_db.py"
                  % os.path.relpath(args.out, ROOT), file=sys.stderr)
            return 1
        print("filament_db: %s is up to date (%d filaments, %d Bambu ids)"
              % (os.path.relpath(args.out, ROOT), len(records), len(bambu)))
        return 0

    with open(args.out, "w", encoding="utf-8") as f:
        f.write(text)
    print("filament_db: wrote %s (%d filaments, %d Bambu ids, %d bytes)"
          % (os.path.relpath(args.out, ROOT), len(records), len(bambu), len(text)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
