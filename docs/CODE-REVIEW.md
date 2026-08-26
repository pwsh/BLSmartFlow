# Code review of upstream BLSmartFlow (`main` @ c77982e, 2025-07-07)

Reviewed 2026-08-25: every file in `src/`, the build scripts, workflow and the shipped firmware headers. Baseline builds were run on both the official espressif32 6.13.0 platform (Arduino core 2.0.17) and pioarduino 55.03.311 (core 3.3.11). Each finding below is fixed in the 2.0 rework (`docs/REWORK-SPEC.md`).

Severity: **C** = crash/data loss, **H** = feature broken, **M** = robustness/UX, **L** = hygiene.

## Build & release integrity

| # | Sev | Finding |
|---|---|---|
| B1 | H | `src/blflow/web-server.h:17` includes `../www/updatepage.h`; the file is `updatePage.h`. Compilation fails on case-sensitive filesystems (Linux/CI). |
| B2 | H | The committed `src/www/fanpage.h` is stale versus `fanpage.html` (e.g. still contains the "Bebugging" typo fixed in PR #2). `updatePage.h` is a much older page altogether and its symbol names don't match (`updatepPage_html_gz` vs `updatePage_html_gz_len`). `pre_build.py` regenerates `www.h` on every build, but nothing includes `www.h`. `compress_html.py` (which produced the stale headers) is not wired into the build. |
| B3 | H | The OTA page is never served: no route returns `updatePage_html_gz`, so `POST /update` is only reachable with curl. |
| B4 | M | `platform = espressif32` is unpinned; on a machine with the pioarduino fork installed it resolves to that and fails, on core 3 the pinned `ESP32SSDP@^1.2.1` does not compile. PR #4 reports the ArduinoJson pin failure. |
| B5 | M | `[platformio] build_dir = build` breaks the pioarduino `elf2image` step (relative path). |
| B6 | L | Three different version strings: `custom_version = 1.0.0`, `FWVersion = "Stable 2025.2.13"`, `manifest.json 2025.2.13`. `-DCONFIG_ASYNC_TCP_STACK_SIZE` is set but AsyncTCP is not used. |
| B7 | L | `esphome.html`, `updatepage.html` and `wifiSetup.html` still carry BLLEDController branding/logo URLs; `esphome.html` points at the **BLLEDController** manifest, i.e. flashes the wrong firmware. |
| B8 | L | 80 KB of gzip PNGs are compiled into `www.h` but the page loads the same images from raw.githubusercontent.com — the fan graphic breaks without internet. Chart.js and Font Awesome are loaded unpinned from CDNs. |

## Firmware — crashes and data loss

| # | Sev | Finding |
|---|---|---|
| F1 | C | `POST /submitOptions` never calls `saveFileSystem()`: printer IP / access code / serial / static-fan / debug changes are lost at reboot. It also answers twice (`send(202)` then `send(204)`), and the page only shows success on 200, so the user never gets confirmation. |
| F2 | C | `staticFan`/`staticFanSpeed` are not part of the config JSON at all. |
| F3 | C | `loadFileSystem()` does `strcpy(dst, json["key"])` — a missing key yields `NULL` → crash. The file is read into a buffer without a NUL terminator and passed to `deserializeJson(char*)`. No length check into `SSID[32]`, `APPW[63]`, `printerIP[16]`, `accessCode[9]`, `serialNumber[16]`. Same null-`strcpy` pattern in `serialmanager.h`. |
| F4 | C | `fanGraph.clear()` before validating the new points (`/updateFanConfig`, `loadFileSystem`): an empty curve makes `fanloop()` call `fanGraph.back()` on an empty vector (UB). The web UI lets the user delete every point. |
| F5 | C | `fanloop()` interpolation divides by `(t2 - t1)`: two points with equal temperatures → division by zero; unsorted points give wrong speeds. |
| F6 | H | `connectToWifi()` returns `false` on the first transient `WL_DISCONNECTED`; `setup()` then returns with `started = false` and `loop()` never retries WiFi — the device is dead until power-cycled. |
| F7 | H | No AP mode / captive portal: an unprovisioned device can only be configured over USB with a Chrome WebSerial page hosted on dutchdevelop.com. |
| F8 | H | `mqttloop()` while the printer is unreachable: `delay(500)` + a TLS connect with a 20 s socket timeout on every pass, in the main loop → the web UI and the fan output freeze whenever the printer is off. `mqttattempt` is never updated so the intended 3 s throttle is dead. |
| F9 | H | No `pushall` is sent after subscribing; P1/A1 printers only send diffs, so data can be sparse. Only `nozzle_temper` and `fan_gear` are parsed; chamber/bed temperatures, print state, etc. are unavailable. `chamberfan` is parsed but never used. |
| F10 | H | No stale-data handling: if the printer goes away mid-print the fan keeps the last speed forever. |
| F11 | M | `indicatorloop()` uses blocking `delay()` (up to ~2 s per loop iteration in error states). |
| F12 | M | `MDNS.begin()` failure → `while(1)` hang. No `_http._tcp` service advertised. |
| F13 | M | `scanNetwork()` compares `printerConfig.BSSID == bestBSSID.c_str()` — pointer comparison, always false. `WiFi.setTxPower` is called twice. `main.cpp` reconnect logic comment says 10 attempts, code says 2, and `WiFi.reconnect()` is called on every loop iteration while disconnected. |
| F14 | M | `obfuscate()` leaks a `new char[]` on every `/getOptions`. `submitOptions()` copies the serial into `char[20]` then into `serialNumber[16]` (overflow for ≥ 16 chars); IP and access code are copied unbounded. |
| F15 | M | `AutoGrowBufferStream`: `uint16_t` length wraps above 64 KB, `get_string()` writes one byte past the buffer when `_len == buffer_size`, `flush()` ignores `realloc` failure. |
| F16 | M | `analogWrite()` default (~1 kHz) PWM is audible on many fans; no minimum-speed clamp, no kick-start, no hysteresis. |
| F17 | L | `fans.h` include guard `_FAN`/`_Fan` mismatch; `types.h` wraps C++ structs (with `String`, `std::vector`) in `extern "C"`; globals defined in headers; `lastMQTTupdate`, `previousMillis`, `shouldSaveConfig` unused. |
| F18 | L | No authentication anywhere, including OTA. |

## Web UI (fanpage.html)

| # | Sev | Finding |
|---|---|---|
| U1 | H | "Save" success alert can never appear (see F1); errors are only logged to the console. |
| U2 | H | Points cannot be dragged — the chart only selects; editing is via a separate form. "Create point" always inserts `{25,50}` and locates it by value, so a second click edits the wrong point. No range validation (`min`/`max` attributes are cosmetic because values are read in a click handler). |
| U3 | M | Removing the last point is allowed (→ F4). |
| U4 | M | Not responsive (no viewport meta), fixed 500 px chart, hard-coded dark theme, no tooltips, no ARIA on the custom switches. |
| U5 | L | `getInterpolatedSpeed()` dead code, duplicated particle-canvas script, conflicting `type="text" … type="number"` on one input, CDN dependencies (B8). `updatepage.html` references a non-existent `#loaded_n_total` (TypeError on file select) and sets CORS *response* headers on the request. |

## Upstream issues / PRs addressed by 2.0

* Issue #1 (spelling in UI) — new UI. Issue #6 (reboot button) — `POST /api/restart` + System page.
* PR #4 (dependency pin) — new `platformio.ini`. PR #5 (use chamber temperature on X1) — `fan.source = nozzle|bed|chamber|max`.
