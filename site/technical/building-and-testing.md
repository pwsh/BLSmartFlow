# Building and testing

Requirements: [PlatformIO](https://platformio.org/) Core ≥ 6.1 and Python 3.

```sh
pio run                        # build esp32dev (the default env)
pio run -t upload              # flash over USB
pio test -e native             # host unit tests
python3 tools/mock_server.py   # API mock at http://localhost:8080
```

## Environments

| Env | Purpose |
|---|---|
| `esp32dev` | The firmware. pioarduino platform `55.03.311` (Arduino-ESP32 3.3.x), board `esp32dev`, LittleFS, `min_spiffs.csv`, `custom_version = 2.0.3`, `custom_project_name = BLSmartflow` |
| `native` | Host build of the **tests only** — `test_build_src = no`, `build_src_filter = -<*>`, `-I src/blflow`, C++17. Only ArduinoJson is pulled in |

**Build flags:** `-DSTRVERSION`, `-DCORE_DEBUG_LEVEL=0`, `-DCONFIG_ASYNC_TCP_STACK_SIZE=8192`,
`-DBLSF_SSDP`.

**Libraries:** ArduinoJson 7, PubSubClient 2.8, ESPAsyncWebServer 3.12 + AsyncTCP 3.5 (ESP32Async),
ESP32SSDP pinned to tag `2.0.3` — the registry copy only builds against core 2.x.

!!! warning "Do not override `[platformio] build_dir`"
    The pioarduino `elf2image` step fails when `$BUILD_DIR` is moved out of `.pio/build`. This is
    deliberately left at the default; the 1.x project set it and could not build on the pioarduino
    fork at all.

## Asset pipeline — `pre_build.py`

Runs as a `pre:` script. It gzips every `*.html`, `*.js`, `*.css`, `*.svg` and `*.png` under
`src/www/` into `src/www/www.h` as `PROGMEM` arrays plus `_len` and `_mime` symbols —
`index_html_gz`, `index_html_gz_len`, `index_html_gz_mime`.

`www.h` is **generated, not committed**. It also writes `src/blflow/build_stamp.h` on every build.

!!! note "Why the build stamp is its own file"
    `__DATE__` / `__TIME__` only refresh when their translation unit is recompiled. Writing a tiny
    header on every build is what keeps `/api/info.build` honest.

## Packaging — `merge_firmware.py`

Runs as a post-action on the app binary and produces:

| Output | What it is |
|---|---|
| `.firmware/BLSmartflow_V<version>.bin.ota` | Plain application image — **the OTA upload** |
| `.firmware/BLSmartflow_V<version>.bin` | Merged bootloader + partitions + app, **flash at `0x0`** |
| `firmware/esp32dev/BLSmartflow_<version>.bin` | Copy of the merged image that the web flasher serves |

It then rewrites `firmware/manifest.json`: `version` from `custom_version`, and `builds[0].parts[0]`
to `{ "path": "esp32dev/BLSmartflow_<version>.bin", "offset": 0 }`.

Set `ENABLE_MERGE_BIN = False` to skip the merged image; the OTA image is always produced. If
`merge-bin` fails, the manifest and the release copy are left untouched rather than half-written.

## Tests

`pio test -e native` runs six Unity suites against the Arduino-free headers:

| Suite | Covers |
|---|---|
| `test/test_curve` | `curve.h`: interpolation at / between / outside points, equal temperatures, unsorted input, clamping, `curveValidate()` rules |
| `test/test_parse` | `printer_parse.h` against the captured fixtures, door-edge semantics, the packed chamber target, every `reportPhase()` rule and the `stg_cur` name table |
| `test/test_buffer` | `AutoGrowBufferStream`, with local `Arduino.h` / `Stream.h` shims |
| `test/test_thermostat` | `thermostat.h`: proportional response, integral accumulation, the ±100/ki anti-windup clamp, the door and saturation freezes, the printing → cool-down set-point switch |
| `test/test_filament` | `filament_match.h` + the AMS half of `printer_parse.h`: **every row of `tools/bambu_filament_ids.csv`**, CF/GF fallbacks, support pairing, `tray_now` and H2D `snow` encodings, the live AMS fixture, partial-report merging, AMS-HT unit ids, effective-profile and override precedence |
| `test/test_thermal` | `thermal_math.h`: bucketing, the EMA blend, recovering a known cooling constant from a synthetic cool-down, and every reason a window is refused |

### Fixtures

`test/fixtures/` was captured from a live X1C and sanitised:

- `x1c_push_status.json` — a full `print.push_status`, notably **without** `chamber_temper`, with
  packed `device.*` temperatures and gear-scale fan strings;
- `x1c_gcode_line.json` — an acknowledgement the parser must ignore;
- `x1c_ams_trays.json` — a four-slot AMS with an ABS / PLA / PLA-AERO / PLA load, `tray_now: "0"` and
  an ASA spool on the external holder.

## Regenerating the filament database

```sh
python3 tools/gen_filament_db.py                          # fetch over HTTPS
python3 tools/gen_filament_db.py --src ../filament-field-guide
python3 tools/gen_filament_db.py --src DIR --check         # exit 1 when the header is stale
```

`src/blflow/filament_db.h` is **committed**, so a stale header is a review problem rather than a build
problem — hence `--check`, which compares everything except the `// Fetched:` line so re-running it on
a different day is not a failure.

→ [Filament matching](filament-matching.md)

## The UI mock server

`tools/mock_server.py` implements the whole API in Python (stdlib only) with a simulated printer and
fan controller, so `src/www/index.html` can be developed without hardware.

| Flag | Effect |
|---|---|
| `--port N` | Listen port (default `8080`) |
| `--host ADDR` | Bind address (default `0.0.0.0`) |
| `--ap` | Simulate AP / provisioning mode (`device.apMode = true`, no station) |
| `--offline` | The printer never connects: temps and counters `null`, `lastUpdateSec` `null`, `effectiveMode` `stale` |
| `--auth USER:PASS` | Require HTTP basic auth on every route, as `web.authEnabled` does |
| `--door` | Start with the door reported open — `doorKnown` still false until the first toggle |
| `--filament TYPE` | Overwrite the loaded tray's material, e.g. `--filament PETG` |

The simulated printer walks a whole job — idle → preheat → printing → finished/cooling → idle —
driving `stg_cur`, the temperature targets and therefore `printer.phase`, and it runs a Newtonian
chamber model (`dT/dt = heatIn − k·(T − ambient)`, with `k` raised by the fan output and by an open
door) so the thermostat and the cooling-rate learning have something real to chew on. It implements
the same evaluation order, the same PI step and the same window logic as the firmware.

The fake AMS is the captured `test/fixtures/x1c_ams_trays.json`, and the mock reads
`src/blflow/filament_db.h` back with a regular expression rather than carrying its own copy of the
guide — **one source of truth**, so a regenerated database needs no change in the mock.

Two routes are **not** part of the device API:

| Route | Body | Effect |
|---|---|---|
| `POST /mock/door` | `{"open":true}`, `{"open":false}` or `{"toggle":true}` | Stands in for someone opening the printer. Answers `{"ok":true,"changed":…,"doorOpen":…,"doorEdgeCount":…}` |
| `POST /mock/tray` | `{"now":"0"}` | Stands in for the printer switching trays — `0`–`15` for an AMS slot, `254` for the external holder, `255` for nothing loaded. Answers with the resulting `filament` block |

```sh
python3 tools/mock_server.py --port 8080 --filament ABS
curl -X POST -d '{"toggle":true}' http://localhost:8080/mock/door
curl -X POST -d '{"now":"254"}'   http://localhost:8080/mock/tray
```

Add `?poll=1` to the UI's URL to force polling instead of SSE — useful for screenshots and for
headless browsers.

## CI

`.github/workflows/build.yml` runs on every branch push, pull request, `v*` tag and manual dispatch:

```text
actions/checkout@v7
  → actions/setup-python@v7 (3.12)
  → actions/cache@v6 (PlatformIO)
  → pip install --upgrade platformio
  → pio test -e native
  → pio run -e esp32dev
  → actions/upload-artifact@v7 (.firmware/*, include-hidden-files: true, if-no-files-found: error)
```

On a tag, `softprops/action-gh-release@v3` publishes the same files as release assets.

This documentation site is built and deployed by a second workflow,
`.github/workflows/pages.yml`: `mkdocs build --strict` into `build/site`, then
`actions/upload-pages-artifact@v4` and `actions/deploy-pages@v4`.

## Pitfalls learned on hardware

- **ArduinoJson stores `const char*` by pointer.** Only `char*`, `String` and `std::string` are copied
  into the document. Assigning a field of a *local* `const` struct and serialising after the function
  returned produces garbage. `status.cpp` and `config.cpp` wrap every char-array field in `String()` —
  keep doing so.
- **SSE needs the `Accept: text/event-stream` header.** `AsyncEventSource` answers 404 to any other
  client, so `curl /api/events` without the header is not a valid test.
- **Captive-portal mini browsers may open an `EventSource` that never delivers.** The UI polls in AP
  mode, keeps polling until the stream delivers its first event, and falls back to polling after 5 s
  of silence.
- **4 KB is not enough AsyncTCP stack** once a handler writes LittleFS or serialises the status
  document.
