# BLSmartFlow 2.0 — Architecture & API Specification

This document is the contract between the firmware and the web UI. It was written after a full review of upstream `c77982e` (see `docs/CODE-REVIEW.md`) and drives the 2.0 rework.

## 1. Goals

1. **Correctness & robustness** — fix every defect in the review (unpersisted settings, crashes on empty/malformed config, blocking loops that freeze the UI, WiFi dead-ends).
2. **Modern responsive UI** — one self-contained page, mobile-first, light/dark, tooltips on every control, drag-editable fan curve, live dashboard.
3. **Feature-complete API** — a versioned JSON REST API (`/api/*`) + live event stream, with the legacy endpoints kept for compatibility.
4. **Feature-complete MQTT** — full Bambu status parsing (X1/P1/A1 + best-effort H2D), `pushall`, staleness failsafe; plus an optional external MQTT broker link with Home Assistant auto-discovery and command topics.
5. **Up-to-date toolchain** — pioarduino platform (Arduino-ESP32 3.3.x), current libraries, pinned versions, CI build.

## 2. Hardware (unchanged)

| Function | GPIO | Notes |
|---|---|---|
| Fan output 1 | 17 | LEDC PWM, 25 kHz default, 8-bit |
| Fan output 2 | 16 | same duty as output 1 (can be disabled individually) |
| Status LED | 21 | non-blocking blink patterns |

## 3. Toolchain & dependencies

```ini
[env:esp32dev]
platform = https://github.com/pioarduino/platform-espressif32/releases/download/55.03.311/platform-espressif32.zip
board = esp32dev
framework = arduino
board_build.filesystem = littlefs
board_build.partitions = min_spiffs.csv      ; 1.875 MB per OTA slot, 128 KB LittleFS (config is < 4 KB)
custom_version = 2.0.0
build_flags = -DSTRVERSION=\"${this.custom_version}\" -DCORE_DEBUG_LEVEL=0
lib_deps =
    bblanchon/ArduinoJson@^7.4.3
    knolleary/PubSubClient@^2.8
    ESP32Async/ESPAsyncWebServer@^3.12.0
    ESP32Async/AsyncTCP@^3.5.0
    https://github.com/luc-github/ESP32SSDP.git#2.0.3   ; registry only has 1.2.1 (core 2.x); tag 2.0.3 builds on core 3.3.x
```

* `pre_build.py` stays the single asset pipeline: gzip everything in `src/www/` into `src/www/www.h` (gitignored). Delete the stale hand-made `fanpage.h`, `updatePage.h`, `compress_html.py`, and the two PNGs (moved to `docs/img/`).
* Version string comes **only** from `custom_version` via `STRVERSION`. `firmware/manifest.json` is updated by `merge_firmware.py`.
* Arduino-ESP32 3.x API: `ledcAttach(pin, freq, res)` / `ledcWrite(pin, duty)`; no `ledcSetup`/`ledcAttachPin`.
* Note: because the partition table changes, upgrading a device that runs the 2025.x firmware requires one full flash (USB / web flasher). After that, OTA works normally.

## 4. Firmware architecture

Proper modules (`.h` + `.cpp`), no globals defined in headers, single-responsibility. Everything in the main loop is **non-blocking** (no `delay()` except a few ms of yield). The printer MQTT client runs in its own FreeRTOS task so a TLS handshake or a printer that is switched off can never stall fan control or the web UI.

```
src/main.cpp
src/blflow/version.h            STRVERSION → FW_VERSION, build date
src/blflow/log.h/.cpp           LOGI/LOGW/LOGE → Serial + 64-line ring buffer (for /api/log)
src/blflow/config.h/.cpp        Config struct (section 5), defaults, validate(), load/save (LittleFS /config.json), migration from legacy /blledconfig.json, toJson(masked)/fromJson(partial merge)
src/blflow/curve.h              PURE, Arduino-free: FanCurve (max 16 points), sort/validate, interpolate(temp) → 0..100. Unit-tested natively.
src/blflow/state.h/.cpp         PrinterState + FanState + DeviceState; copy-on-read snapshots guarded by a portMUX spinlock (written from MQTT task, read from loop/web)
src/blflow/fan_control.h/.cpp   LEDC setup, control state machine (section 6), loop()
src/blflow/printer_link.h/.cpp  Bambu MQTT over TLS (section 7)
src/blflow/ha_mqtt.h/.cpp       External MQTT broker + Home Assistant discovery (section 8)
src/blflow/web_server.h/.cpp    ESPAsyncWebServer: static UI, REST API, SSE, OTA, captive portal, basic auth (section 9)
src/blflow/wifi_manager.h/.cpp  Non-blocking STA state machine, BSSID lock, AP fallback + DNSServer, async scan (section 10)
src/blflow/indicator.h/.cpp     Non-blocking LED patterns (section 11)
src/blflow/serial_provision.h/.cpp  JSON-over-USB provisioning (legacy keys kept)
src/blflow/ssdp.h/.cpp          UPnP self-advertisement (optional, -DBLSF_SSDP)
src/www/index.html              The UI (single file, inline CSS/JS, no external resources)
test/test_curve/test_curve.cpp  Unity tests for curve.h, run with `pio test -e native`
tools/mock_server.py            Python mock of the API for UI development (`python3 tools/mock_server.py` → http://localhost:8080)
```

## 5. Configuration (persisted JSON, `/config.json`)

```jsonc
{
  "version": 2,
  "wifi":    { "ssid": "", "password": "", "bssid": "", "lockBssid": false, "hostname": "blsmartflow" },
  "printer": { "ip": "", "accessCode": "", "serial": "", "model": "auto" },      // model: auto|x1|p1|a1|h2d  (p1/a1 → periodic pushall every 5 min)
  "fan": {
    "curve": [ {"temp":0,"speed":0}, {"temp":50,"speed":0}, {"temp":180,"speed":50}, {"temp":245,"speed":80}, {"temp":350,"speed":100} ],
    "source": "nozzle",             // nozzle|bed|chamber|max   (max = hottest of nozzle/bed/chamber)
    "mode": "auto",                 // auto|manual|off   (persisted; manual w/o duration survives reboot)
    "manualSpeed": 50,
    "minSpeed": 0,                  // % — outputs below this are clamped to 0 (fans that stall at low duty). 0 = off
    "kickStart": true, "kickMs": 500,   // full duty for kickMs when leaving 0 %
    "hysteresis": 2.0,              // °C — ignore source changes smaller than this before recomputing the curve
    "rampRate": 0,                  // %/s — 0 = instant, else slew output towards target
    "pwmFreq": 25000, "pwmInvert": false,
    "output1": true, "output2": true,
    "onlyWhilePrinting": false,     // curve active only while gcode_state ∈ {RUNNING, PAUSE, PREPARE, SLICING} (+ cooldown)
    "cooldownMin": 10,              // keep curve active this long after the print ends (when onlyWhilePrinting)
    "staleSec": 120,                // no printer data for this long → staleMode
    "staleMode": "off",             // hold|off|fixed
    "staleSpeed": 0                 // % used by staleMode=fixed
  },
  "mqtt": { "enabled": false, "host": "", "port": 1883, "user": "", "password": "",
            "baseTopic": "",         // "" → "blsmartflow/<chipid>"
            "haDiscovery": true, "haPrefix": "homeassistant", "publishIntervalSec": 10 },
  "web":   { "authEnabled": false, "user": "admin", "password": "" },
  "debug": { "serial": true, "mqttDump": false },
  "ssdp":  { "enabled": true }
}
```

Rules
* `validate()` clamps/normalises, it never rejects: curve sorted by temp, 2..16 points, temps clamped to 0..400 and deduped, speeds clamped to 0..100; `manualSpeed`/`minSpeed`/`staleSpeed` 0..100; `kickMs` 0..5000; `hysteresis` 0..50; `rampRate` 0..1000; `pwmFreq` 500..40000; `cooldownMin` 0..1440; `staleSec` 10..3600; `publishIntervalSec` 1..3600; `fan.source` ∈ nozzle|bed|chamber|max (else nozzle); `fan.mode` ∈ auto|manual|off (else auto); `fan.staleMode` ∈ hold|off|fixed (else off); `printer.model` ∈ auto|x1|p1|a1|h2d (else auto); serial upper-cased; `wifi.hostname` lower-cased and reduced to `[a-z0-9-]`, max 32 chars; `mqtt.baseTopic` trailing `/` stripped.
* **Access code: exactly 8 characters.** `POST /api/config` rejects any other length with `400 {"error":"access code must be exactly 8 characters"}` — a value consisting only of `*` is the "unchanged" marker and is not length-checked. The UI enforces the same rule before it sends.
* `load()` never crashes on missing keys (every key has a default; use `| default` extraction, `strlcpy`, bounded `String`). A corrupt file is renamed to `/config.bad` and defaults are used. A legacy `/blledconfig.json` (keys `ssid, appw, printerIp, accessCode, serialNumber, bssi, fanPoints[]`) is migrated once and then removed.
* Secrets (`wifi.password`, `printer.accessCode`, `mqtt.password`, `web.password`) are serialised as `"********"` when `masked=true`; on input, a value consisting only of `*` means "leave unchanged".
* `save()` writes to `/config.tmp` then renames.

## 6. Fan control state machine (`fan_control`)

Runs every loop iteration (cheap), recomputes at most every 100 ms.

```
printing    = gcode_state ∈ {RUNNING, PAUSE, PREPARE, SLICING}
sourceTemp  = select(config.fan.source, printer temps)        // NaN when not available
effectiveMode:
   "off"      if config.fan.mode == off
   "manual"   if mode == manual (with optional expiry → back to auto)
   "stale"    if mode == auto && printer data older than staleSec (or never received / printer offline)
   "idle"     if mode == auto && onlyWhilePrinting && !printing && cooldown expired
   "cooldown" if mode == auto && onlyWhilePrinting && !printing && cooldown running
   "auto"     otherwise
target:
   off → 0; manual → manualSpeed; stale → hold last / 0 / staleSpeed; idle → 0; auto|cooldown → curve.interpolate(sourceTemp) with hysteresis on sourceTemp
output:
   apply minSpeed clamp, rampRate slew, kick-start pulse; duty = output*255/100 (inverted if pwmInvert); write to enabled outputs; disabled outputs get 0.
```
`FanState` exposes: `output`, `target`, `effectiveMode`, `sourceTemp`, `pwmDuty`, `manualExpiresAt`.
`pwmDuty` is reported **post-inversion**: it is the byte actually written to the pin, so with `pwmInvert` on an output of 0 % reports 255. The UI labels it that way.

`curve.h` API (no Arduino includes; must compile on host):
```cpp
struct CurvePoint { float temp; uint8_t speed; };
struct FanCurve { CurvePoint pts[16]; uint8_t count; };
bool  curveValidate(FanCurve&);            // sorts, dedups equal temps (keeps last), clamps, returns false if < 2 points → caller falls back to default
float curveInterpolate(const FanCurve&, float temp);  // clamps to first/last point, safe for equal temps
```

## 7. Printer link (Bambu MQTT)

* `WiFiClientSecure::setInsecure()`, port 8883, user `bblp`, password = access code, client id `BLSF-<chipid>`. `setKeepAlive(30)`, `setSocketTimeout(10)`, `setBufferSize(2048)` for TX; RX uses the `AutoGrowBufferStream` (fixed: `size_t` length, 64 KB cap, bounds-checked `get_string`, checked `realloc`).
* Runs in `xTaskCreatePinnedToCore(task, "printer", 16384, …, 1, core 1)`. Reconnect with exponential backoff 3 s → 60 s; bad credentials (state 5) → backoff 60 s and `printer.mqttStateText="unauthorized"`. Never blocks the main loop.
* On connect: subscribe `device/<serial>/report`, publish `{"pushing":{"sequence_id":"0","command":"pushall","version":1,"push_target":1}}` to `device/<serial>/request`. If `printer.model ∈ {p1,a1}` (or auto-detected diff-style pushes) repeat `pushall` every 5 min.
* Re-initialise (disconnect + new server/topic) whenever printer config changes (`printerLink.reconfigure()`), no reboot.
* Filtered `deserializeJson` keeping only:
  `print.{command, nozzle_temper, nozzle_target_temper, bed_temper, bed_target_temper, chamber_temper, cooling_fan_speed, big_fan1_speed, big_fan2_speed, heatbreak_fan_speed, fan_gear, gcode_state, mc_percent, mc_remaining_time, layer_num, total_layer_num, subtask_name, stg_cur, print_error, wifi_signal, home_flag, lights_report, device}`.
  Ignore `print.command` in {gcode_line, project_prepare, project_file, clean_print_error, resume, get_accessories, prepare, extrusion_cali_get}.
  Fan speeds: strings "0".."15" → percent (`round(v*100/15)`). Door = bit 23 of `home_flag`.
  **Packed `device.*` block (all models, verified on a live X1C — see `test/fixtures/README.md`):** current X1 firmware no longer sends `chamber_temper`; the chamber temperature is only at `print.device.ctc.info.temp` (mirrored at `print.info.temp`). Decode rule for `device.extruder.info[0].temp`, `device.bed.info.temp`, `device.ctc.info.temp`: current = `v & 0xFFFF`, target = `v >> 16`. Prefer the classic float keys when present, fall back to the packed values otherwise (this also covers the H2D). `print.device.airduct.parts[]` state = fan percent when present. Read `home_flag` as `uint32_t` before testing bit 23.
  Each accepted message updates `printer.lastUpdateMs`; `printer.online = connected && age < staleSec`.
* `debug.mqttDump` prints the filtered document.

## 8. External MQTT / Home Assistant (`ha_mqtt`)

Plain `WiFiClient` + second `PubSubClient`, runs in the main loop (non-blocking, reconnect every 10 s, `setSocketTimeout(2)`). `base = config.mqtt.baseTopic || "blsmartflow/<chipid>"`.

| Topic | Dir | Payload |
|---|---|---|
| `<base>/availability` | pub, retained, LWT | `online` / `offline` |
| `<base>/state` | pub, retained, every `publishIntervalSec` and on fan change | JSON = `/api/status` object |
| `<base>/fan/speed` | pub | `0..100` (output) |
| `<base>/fan/on_state` | pub | `ON` if output > 0 else `OFF` |
| `<base>/mode` | pub | `auto` / `manual` / `off` |
| `<base>/fan/set` | sub | `0..100` → mode manual, manualSpeed = value |
| `<base>/fan/on` | sub | `ON` → manual at manualSpeed (≥1), `OFF` → mode off |
| `<base>/mode/set` | sub | `auto` / `manual` / `off` |
| `<base>/curve/set` | sub | `{"points":[…]}` |
| `<base>/restart` | sub | any → restart |

HA discovery (retained, on connect and when `haDiscovery` toggles on; empty payload published to remove when toggled off). `device` block: `identifiers:["blsmartflow_<chipid>"]`, `name:"BLSmartFlow <chipid>"`, `manufacturer:"DutchDeveloper"`, `model:"BLSmartFlow"`, `sw_version`, `configuration_url:"http://<ip>/"`. Entities (unique_id prefix `blsmartflow_<chipid>_`):
* `fan.fan` — command_topic `fan/on`, state_topic `fan/on_state`, percentage_command_topic `fan/set`, percentage_state_topic `fan/speed`, speed_range 1..100
* `select.mode` — options auto/manual/off, command `mode/set`, state `mode`
* `sensor` — `nozzle_temp`, `bed_temp`, `chamber_temp` (°C, device_class temperature, state_class measurement, `value_template` over `state`), `fan_output` (%), `printer_state` (`printer.state`: RUNNING/IDLE/…), `printer_stage` (`printer.stageText`), `print_progress` (%), `remaining_time` (min, device_class duration), `printer_wifi`, `device_rssi` (dBm, signal_strength), `uptime`
* Every numeric sensor whose source can be `null` (temperatures, `print_progress`, `remaining_time`) uses a template that emits `unknown` rather than an empty string, e.g.
  `{{ value_json.printer.progress if value_json.printer.progress is not none else 'unknown' }}`, so Home Assistant shows "Unknown" instead of logging a parse error.
* `binary_sensor` — `printer_online` (connectivity), `door` (opening), `printing` (running)
* `button.restart` — command `restart`

## 9. Web server & HTTP API

ESPAsyncWebServer on :80. `GET /` serves `index.html` (gzip, `Cache-Control: no-cache`). Optional HTTP Basic Auth (`web.authEnabled`) on every route except in AP mode. All API responses `application/json`; errors are `{"error":"…"}` with 400/401/404/500. Bodies are JSON (`AsyncCallbackJsonWebHandler`, 4 KB limit) — form-encoded only for the legacy routes.

| Method & path | Body | Response |
|---|---|---|
| `GET /api/status` | — | status object (below) |
| `GET /api/events` | — | SSE, `event: status` with the status object every 1 s; `event: log` with new log lines |
| `GET /api/config` | — | config object, secrets masked |
| `POST /api/config` | partial config (deep-merge) | `{"ok":true,"restartRequired":bool,"config":{…masked}}`; applies live: fan/mqtt/debug/ssdp/**web** immediately, printer → `printerLink.reconfigure()`. **Only `wifi.*` sets `restartRequired`.** A non-masked `printer.accessCode` whose length ≠ 8 → `400 {"error":"access code must be exactly 8 characters"}`. Everything else is clamped by `validate()` (section 5), never rejected |
| `GET /api/curve` | — | `{"points":[…],"source":"nozzle"}` |
| `PUT /api/curve` | `{"points":[…]}` | `{"ok":true,"points":[…]}` — saves. Temps are **clamped** to 0..400 and speeds to 0..100, so out-of-range input returns 200 with the adjusted points. Hard `400` only for non-numeric points, fewer than 2, or more than 16 points |
| `POST /api/fan` | `{"mode":"auto"\|"manual"\|"off","speed":0..100,"durationSec":0}` | `{"ok":true,"fan":{…}}` — manual with duration reverts to auto; persists mode/speed when durationSec == 0. A `speed` outside 0..100 is **not clamped**: `400 {"error":"speed must be 0..100"}` |
| `POST /api/restart` | — | `{"ok":true}` then restart after 500 ms |
| `POST /api/factoryreset` | `{"confirm":true}` | wipes config, restarts into AP |
| `GET /api/wifi/scan` | — | `202 {"scanning":true}` while an async scan runs, else `{"networks":[{"ssid","bssid","rssi","channel","secure"}]}` sorted by rssi, deduped by ssid (strongest), uppercase BSSIDs, 2.4 GHz channels only. A result younger than **20 s** is served from the cache. `?force=1` discards the cache and starts a new scan, so it always answers `202` — the UI sends `force=1` on an explicit Scan click and then polls **without** it |
| `POST /api/wifi` | `{"ssid","password","bssid","lockBssid","hostname"}` | `{"ok":true,"restartRequired":true}` — saves (through `validate()`), then restarts: after **1 s** in AP mode, after **1.5 s** otherwise |
| `GET /api/backup` | — | full config **with secrets**, `Content-Disposition: attachment; filename="blsmartflow-<chipid>.json"` |
| `POST /api/restore` | full config | `{"ok":true}` — **replaces** the whole config (defaults + body, so keys the body omits fall back to their defaults rather than keeping the current value), validates, saves, then restarts |
| `POST /api/update` | multipart, **any** file field name (the UI uses `firmware`) | `{"ok":true}` / `{"error"}`; restart on success. `/update` is an alias |
| `GET /api/log` | — | `{"lines":["…"]}` — each line is `[<uptime ms, %7d>] [<I\|W\|E>] <message>`, e.g. `[   1234] [E] mqtt: connect failed`. The UI colours a line by the `[E]` / `[W]` tag |
| `GET /api/info` | — | `{"fw","build","chipId","sdk","flashSize","sketchSize","freeSketchSpace","partition","resetReason"}`. `resetReason` is a **string**, one of `UNKNOWN, POWERON, EXT, SW, PANIC, INT_WDT, TASK_WDT, WDT, DEEPSLEEP, BROWNOUT, SDIO` |
| Legacy endpoints (1.x compatible) | see below | see below |
| Captive portal (AP mode only): `/generate_204`, `/gen_204`, `/hotspot-detect.html`, `/library/test/success.html`, `/connecttest.txt`, `/ncsi.txt`, `/fwlink`, `/redirect`, `/success.txt`, `/canonical.html` | — | 302 → `http://192.168.4.1/` |

**Legacy endpoints** (kept so 1.x tooling and the old setup page keep working):

| Method & path | Body | Response |
|---|---|---|
| `GET /getOptions` | — | `{"firmwareversion","ip","code","id","staticfans","staticfanspeed","debuging","debugingchange","mqttdebug"}`. `code` (access code) and `id` (serial) are **obfuscated**: every character but the last 3 becomes `*` (values of 3 characters or fewer are returned unchanged). `staticfans` = `fan.mode == "manual"`, `staticfanspeed` = `fan.manualSpeed`, `debuging` = `debug.serial`, `mqttdebug` = `debug.mqttDump`. `debugingchange` is a 1.x-only flag with no 2.0 equivalent and is always `false` |
| `POST /submitOptions` | form: `ip, code, serial, staticfan (on/off), staticfanspeed, debuging, mqttdebug` | applied to the config (`staticfan=on` → `fan.mode="manual"`, else `"auto"`; serial upper-cased), validated and saved; `200`. As in 1.x, a value that still contains a `*` came back from the obfuscated `/getOptions` payload and is left unchanged |
| `GET /getFanConfig` | — | `{"points":[…],"source":"…"}` |
| `POST /updateFanConfig` | form field `points`: either `{"points":[…]}` or a bare `[…]` array | validated exactly like `PUT /api/curve` (clamping; `400` only for non-numeric points, < 2 or > 16 points), then saved |
| `GET /sensorData` | — | `{"temp": <source temperature, 2 decimals, 0 when unknown>, "speed": <fan output, integer %>}` |
| `POST /update` | multipart | alias of `POST /api/update` |

**Status object** (also the SSE payload and the MQTT `state` payload):
```jsonc
{
  "device":  { "fw":"2.0.0", "uptimeSec":123, "heapFree":123456, "heapMin":100000, "chipId":"a1b2c3", "hostname":"blsmartflow", "ip":"10.0.1.5", "apMode":false },
  "wifi":    { "connected":true, "ssid":"…", "bssid":"…", "rssi":-61, "channel":6 },   // ssid is "" when not connected (AP mode included)
  "printer": { "configured":true, "connected":true, "online":true, "lastUpdateSec":2, "mqttState":0, "mqttStateText":"connected",
               "state":"RUNNING", "printing":true, "stage":0, "stageText":"printing", "progress":42, "remainingMin":87,
               "layer":12, "totalLayers":210, "task":"Benchy.3mf", "doorOpen":false, "printError":0, "wifiSignal":"-45dBm",
               "temps": { "nozzle":220.4, "nozzleTarget":220, "bed":60.1, "bedTarget":60, "chamber":38.0 },
               "fans":  { "part":100, "aux":0, "chamber":40, "heatbreak":100 } },
  "fan":     { "output":55, "target":55, "mode":"auto", "effectiveMode":"auto", "source":"nozzle", "sourceTemp":220.4,
               "manualSpeed":50, "manualExpiresSec":0, "pwmDuty":140, "output1":true, "output2":true },
  "mqttExt": { "enabled":true, "connected":true }
}
```
**Null rules** — the status object never invents placeholder values:

* Unknown temperatures are `null`.
* The printer counters `printer.stage`, `printer.progress`, `printer.remainingMin`, `printer.layer` and `printer.totalLayers` are `null` while the printer has not reported them (no link, or no job loaded) — never `-1` or `0`. `stageText` is `""` when `stage` is `null`.
* `printer.lastUpdateSec` is `null` until the very first report ever arrives; the UI renders that as "never".
* `wifi.ssid` is `""` when the device is not associated (AP mode included).
* `fan.pwmDuty` is the byte actually written to the pin, already inverted when `fan.pwmInvert` is on (section 6).
* `printer.printing` is true for `gcode_state` ∈ {RUNNING, PAUSE, PREPARE, SLICING}.

`stageText` uses the ha-bambulab stage table.

## 10. WiFi manager

Non-blocking state machine driven from `loop()`:
`IDLE → CONNECTING (WiFi.begin with bssid if lockBssid) → CONNECTED` ; on failure/timeout (20 s) retry with backoff (5 s, 10 s, 30 s …), after 3 failed cycles drop the BSSID lock, after 90 s total start **AP+STA** (`BLSmartFlow-<chipid>`, open, 192.168.4.1, DNSServer catch-all) while still retrying STA every 60 s. When credentials are empty → AP only. When STA succeeds while AP is up, AP is kept for 5 more minutes then closed. `WiFi.setSleep(false)`, tx power 19.5 dBm, `WiFi.setAutoReconnect(false)` (we manage it). `scanAsync()` for `/api/wifi/scan`. mDNS: `<hostname>.local`, `_http._tcp` service with txt `model=BLSmartFlow`. Serial prints `IP_ADDRESS:<ip>` on connect (the WebSerial page parses it).

## 11. Indicator LED

Non-blocking `millis()` patterns, priority: 1 blink = unprovisioned/AP mode, 2 = WiFi down, 3 = printer MQTT down, 4 = stale data, solid = OK, short double-flash every 3 s = manual mode.

## 12. Web UI (`src/www/index.html`)

* **Single file**, vanilla JS, no external requests (no CDN, no fonts, no images; inline SVG icons). Target ≤ 90 KB uncompressed.
* Responsive: sidebar navigation ≥ 900 px, bottom tab bar below; cards in a fluid grid; canvas curve editor scales to width; touch-friendly hit targets.
* Theme: CSS variables, auto `prefers-color-scheme`, manual toggle persisted in `localStorage`.
* **Tooltips on every control**: a `data-tip="…"` attribute + a small "?" icon; shown on hover, focus and tap (mobile); `aria-describedby` wiring; tooltip text must say what the setting does and its effect/unit/range.
* Sections (nav): **Dashboard** (live temps, fan gauge with animated SVG fan, printer state/progress, connection badges, quick manual override slider with duration), **Fan Curve** (canvas editor: drag points, click on line to add, delete key/button, snap to 1 °C / 1 %, table editor in sync, live marker of current source temp/output, presets *Quiet / Balanced / Aggressive / Chamber-ABS*, source selector, hysteresis/ramp/min/kick/stale/gating settings, unsaved-changes bar with Save/Revert), **Printer** (IP, access code, serial, model, connection status + MQTT state text, "Test" = shows live connection), **Network** (WiFi scan list, SSID/password, BSSID lock, hostname, current RSSI/IP; in AP mode this is the landing page), **Integrations** (external MQTT broker settings, HA discovery toggle, topic reference with copy buttons, REST API reference table with example `curl`), **System** (firmware version/info, OTA upload with progress, restart, factory reset with confirm, backup download / restore upload, log viewer (SSE), debug toggles, basic-auth settings).
* Data flow: initial `GET /api/config` + `GET /api/status`; live updates via `EventSource('/api/events')` with fallback to 2 s polling if SSE fails. Save = `POST /api/config` with only the changed sections. Toast notifications for every action result; errors are always shown on-page (never `console` only).
* Works in AP mode (no internet). Must work when `fetch` returns 401 (prompt is handled by the browser's basic-auth dialog).

## 13. Serial provisioning (`serial_provision`)

Line-delimited JSON on USB serial. Accepts legacy keys `{ssid, pass, printerip, printercode, printerserial}` and the full config object (`{"config":{…}}`). Also commands: `{"cmd":"status"}`, `{"cmd":"restart"}`, `{"cmd":"factoryreset"}`. Never `strcpy` from a missing key.

## 14. Tests & CI

* `pio test -e native` — Unity tests for `curve.h` (interpolation at/between/outside points, equal temps, unsorted input, min/max clamp, validate rules).
* `tools/mock_server.py` — implements the whole API in Python (stdlib only) with a simulated printer so the UI can be developed and demoed without hardware.
* `.github/workflows/build.yml` — `actions/checkout@v7`, `actions/setup-python@v7`, `pip install platformio`, `pio test -e native`, `pio run`, upload `.firmware/*` with `actions/upload-artifact@v7`; on tag → `softprops/action-gh-release@v3`.
