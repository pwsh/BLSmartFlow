# Filament matching

The printer already knows what is loaded. The [Filament Field Guide](https://github.com/pwsh/filament-field-guide)
already knows what each material wants from an enclosure. Joining the two removes the main reason to
touch the chamber target by hand.

## The pipeline

```text
filament-field-guide  data/index.json + data/filaments/<id>.json
        │  tools/gen_filament_db.py  (stdlib only, --src for an offline clone)
        ▼
src/blflow/filament_db.h     90 x FilamentInfo (PROGMEM) + 100 x BambuFilament
        │                    committed; `--check` fails CI when it is stale
        ▼
filament_match.h    tray_type / tray_sub_brands / tray_info_idx  ->  guide id + family
        │           tray_now / device.extruder.snow              ->  active tray
        │           FilamentInfo + FilamentPolicy                ->  FilamentEffective
        ▼
filament.cpp        PrinterReport + Config  ->  FilamentStatus  ->  status JSON
        ▼
fan_control.cpp     chamber set point, cool-down target, vent floor, gentle cool-down
```

## The embedded table

`FilamentInfo` keeps only what the controller needs:

| Field | Notes |
|---|---|
| `id[24]`, `name[32]` | Guide id and display name |
| `polymerClass` | Enum |
| `chamberMin` / `Rec` / `Max` | int8 °C, `-1` = the guide has no figure |
| `partCoolRec` | uint8 %, `255` = unknown |
| `vent` | 0 optional / 1 recommended / 2 required |
| `flags` | bit0 enclosure recommended · bit1 heated chamber required · bit2 enclosure open for cooling · bit3 hardened nozzle |
| `voc`, `particulate` | 0–3 emission levels |

That is **≈ 6 KB** of flash for the guide plus **≈ 2 KB** for the Bambu id table.

The generator writes declarations for everyone and the definitions behind `BLSF_FILAMENT_DB_DEFINE`,
which exactly one translation unit defines — `filament.cpp` on the device, the test binary on the
host — so the table is linked once however many modules read it.

!!! note "`PROGMEM` is a no-op on ESP32"
    ESP32 flash is memory-mapped, so the records are read like any other const data. The macro is
    kept for portability and costs nothing.

### Regenerating

```sh
python3 tools/gen_filament_db.py                           # fetch over HTTPS
python3 tools/gen_filament_db.py --src ../filament-field-guide
python3 tools/gen_filament_db.py --src DIR --check          # exit 1 when the header is stale
```

`--check` compares everything **except** the `// Fetched:` line, so re-running it on a different day
is not a failure. The header carries the source URL, the fetch date, the record counts and the
CC BY 4.0 attribution.

## Parser additions

`buildPrinterFilter()` lets through `ams.tray_now`, `ams.ams[*].{id, tray[*].{id, tray_type,
tray_sub_brands, tray_info_idx, tray_color}}`, `vt_tray.{…}` and
`device.extruder.{state, info[*].{id, temp, snow}}`.

A filter array applies its first element to **every** element, so one tray filter covers all sixteen
slots.

`PrinterReport` gains `trays[4][4]`, `amsId[4]`, `external`, `trayNow`, `extruderState` and
`extruderSnow[2]` — about 1 KB, still POD, still `memcpy`-able under the spinlock.

Two rules matter:

!!! warning "Trays merge, they are never cleared"
    P1/A1 firmware sends **partial** `ams` objects. A report that mentions one tray must not blank
    the other fifteen. Only an explicit empty `tray_type` empties a slot.

!!! warning "Unit ids are stored, not used as an index"
    An **AMS-HT reports id 128 or above**. `amsId[]` records what the printer said;
    `reportTray(report, amsId, slot)` does the lookup. `ams = -1` is the external holder.

## Matcher rules

`filamentIdentify(trayType, subBrands, trayIdx)` returns a guide `id` (empty = no entry) and a
`family` string that keeps what the printer said even when the guide has nothing.

1. **Normalise `tray_type`** — upper-case, trim, collapse inner spaces; split at the **first** `-`
   into BASE and MOD. `PLA-CF` → `PLA` + `CF`; `PAHT-CF` → `PAHT` + `CF`.
2. **BASE → id** through a fixed table (`PLA`→`pla`, `PAHT`→`pa`, `PA6`→`pa6`, `PPA`→`ppa`, …).
    - `MOD ∈ {CF, GF}` prefers `<id>-cf` / `<id>-gf` **when the guide has that entry**, otherwise the
      unfilled polymer. *The fibre changes the stiffness, not the temperature the enclosure wants.*
    - Any other MOD (`AERO`, `AMS`, …) is a marketing suffix and resolves to the base id.
3. **No usable type** → `tray_info_idx` through the Bambu table → the first word of
   `tray_sub_brands` ("PLA Basic" → `PLA`) → the **id prefix**:

    | Prefix | Material | Prefix | Material |
    |---|---|---|---|
    | `GFA`, `GFL` | PLA | `GFN` | PA |
    | `GFB` | ABS | `GFP` | PP |
    | `GFC` | PC | `GFT` | PPS |
    | `GFG` | PETG | `GFU` | TPU |

4. **Support materials** (`tray_type` starting `SUPPORT`) take the *paired* material's profile:
   Support For PLA/PETG → `pla`, Support For PA/PET → `pa`, Support for ABS → `abs`, Support W →
   `pla`, Support G → `pa`. PVA, BVOH and HIPS are materials in their own right and never reach this
   rule.
5. A resolved id is always **verified against the table**, so `id` is either a real guide entry or
   empty.

## Which tray is active

| Encoding | Meaning |
|---|---|
| `tray_now = 0..15` | `ams * 4 + slot` |
| `tray_now = 254` | The external holder (`vt_tray`) |
| `tray_now = 255` or absent | Nothing loaded |
| `tray_now ≥ 128` | An **AMS-HT** unit id, which has a single slot |
| H2D: `device.extruder.info[active].snow` | `(ams << 8) \| slot`, with the active extruder in `device.extruder.state >> 4 & 0xF` |

An H2D answer **wins over `tray_now`**, which cannot express two tool heads.

Only the active tray is matched for control; every tray is kept for the UI.

## The effective profile

```text
keepCool         = flags.enclosureOpenForCooling || (chamberRec != n/a && chamberRec < 35)
chamberTarget    = keepCool ? chamberMax : chamberRec      // PLA 30, PETG 35, ABS 50, ASA 55, PC 55
cooldownTarget   = fan.cooldownTarget
postPrintCooling = partCoolRec >= 50 ? "fast" : "gentle"
ventFloor        = filament.ventFloor[vent]
```

For a keep-cool material the **top** of the guide's ambient band is the set point: it is a ceiling
("do not let it get warmer than this"), not a temperature to reach. When the guide has no ambient
figure at all the other end of the band is tried, and if there is none the configured `fan.*` value
stands.

**Resolution order:** guide → `"*"` override → id override.

With `filament.auto` off, the plain `fan.*` values win outright and **no override is applied** — the
user has said "do not let the filament move my set points", and a vent floor pushed by a material
would be exactly that.

## Remembering the last material

When the AMS unloads at the end of a job, `tray_now` reports **none** — and the material whose
cool-down rule matters most disappears at exactly the wrong moment.

So the device remembers it. Through the `finished`, `cooling` and `idle` phases the last known
material stays in force:

- `filament.source` becomes **`"last"`** (the UI shows a *last print* badge),
- the effective profile — in particular the gentle-versus-fast cool-down rule — still follows that
  material.

The memory is cleared when a **new tray is loaded**, or when a **new job starts without one**.

## What it changes in the control loop

The profile is resolved **every tick** from the snapshot the control loop already holds — 90 string
compares, microseconds — so there is no cache to invalidate and a tool change mid-print takes effect
on the next 100 ms pass. See
[where the filament comes in](control-loop.md#where-the-filament-comes-in).

## Testing

`test/test_filament` covers **every row of `tools/bambu_filament_ids.csv`**, resolved both from its
type and from its bare id, plus CF/GF fallbacks, support pairing, the `tray_now` and H2D `snow`
encodings, the live AMS fixture, partial-report merging, AMS-HT unit ids, and the effective-profile
rules including override precedence.

!!! note "Exactly one id is allowed not to resolve"
    `GFR99` (Generic EVA), because the guide has no EVA entry. The matcher reports family `EVA` with
    an empty id, the status block shows the material with no profile, and the configured targets
    stand. The exception is named in `kKnownUnmatched`, so a **second** unresolvable id is a test
    failure rather than a shrug.

---

Related: [Filament-aware cooling](../using/filament-aware-cooling.md) ·
[Control loop](control-loop.md) · [Status object](rest-api.md#status-object)
