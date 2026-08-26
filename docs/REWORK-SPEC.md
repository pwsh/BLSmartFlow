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
test/test_curve, test_parse, test_buffer   Unity tests (curve.h, printer_parse.h against real X1C fixtures, AutoGrowBufferStream), run with `pio test -e native`
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
* Runs in `xTaskCreatePinnedToCore(task, "printer", 20480, …, 1, core 1)` (the mbedTLS handshake runs on this stack). Reconnect with exponential backoff 3 s → 60 s; bad credentials (state 5) → backoff 60 s and `printer.mqttStateText="unauthorized"`. Never blocks the main loop.
* On connect: subscribe `device/<serial>/report`, publish `{"pushing":{"sequence_id":"0","command":"pushall","version":1,"push_target":1}}` to `device/<serial>/request`. Repeat `pushall` every 5 min for `printer.model ∈ {p1,a1}`, every 10 min for `auto`, never for `x1`/`h2d`.
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
| `POST /api/restore` | full config | `{"ok":true}` — **replaces** the whole config (defaults + body, so keys the body omits fall back to their defaults), except that masked (`****`) secrets keep the currently stored value; `400 {"error":"backup has no wifi.ssid"}` if the result has no SSID; validates, saves, then restarts |
| `POST /api/update` | multipart, **any** file field name (the UI uses `firmware`) | `{"ok":true}` / `{"error"}`; restart on success. `/update` is an alias |
| `GET /api/log` | — | `{"lines":["…"]}` — each line is `[<uptime ms, %7d>] [<I\|W\|E>] <message>`, e.g. `[   1234] [E] mqtt: connect failed`. The UI colours a line by the `[E]` / `[W]` tag |
| `GET /api/info` | — | `{"fw","build","chipId","sdk","flashSize","sketchSize","freeSketchSpace","partition","resetReason"}`. `resetReason` is a **string**, one of `UNKNOWN, POWERON, EXT, SW, PANIC, INT_WDT, TASK_WDT, WDT, DEEPSLEEP, BROWNOUT, SDIO` |
| Legacy endpoints (1.x compatible) | see below | see below |
| Captive portal (requests arriving on the AP interface only): `/generate_204`, `/gen_204`, `/hotspot-detect.html`, `/library/test/success.html`, `/connecttest.txt`, `/ncsi.txt`, `/fwlink`, `/redirect`, `/success.txt`, `/canonical.html`, `/check_network_status.txt`, `/chat`, and any unknown path | — | 302 → `http://<softAP IP>/` with no-cache headers (LAN-side unknown paths stay 404) |
| `GET /description.xml` | — | UPnP device description (SSDP schema); 404 when SSDP is disabled |

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
* The printer counters `printer.stage`, `printer.progress`, `printer.remainingMin`, `printer.layer` and `printer.totalLayers` are `null` while the printer has not reported them (no link, or no job loaded) — never `-1` or `0`. `stageText` is `"idle"` when `stage` is `null`/-1/255.
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

## 15. Thermal states: door/lid, print phase, chamber thermostat, cooling-rate learning (2.0.1)

Motivation: an exhaust/chamber fan that follows a temperature curve alone fights the printer during
warm-up and ignores the single biggest disturbance — an open door. Bambu printers report one door bit
(`home_flag` bit 23, a plunger/reed switch at the front-door edge; **the top lid has no sensor**) and a print
stage (`stg_cur`, table below). These become inputs to the control loop.

**Door reliability rule (from BLLED hardware findings, 2026-08-26):** on some X1C units the closed door does not
actuate the switch, so the bit sits at "open" forever (pressing the switch by hand flips it). Therefore
`doorKnown` = "a door *edge* has been seen since boot"; while `doorKnown == false` the door is treated as
**closed** for control purposes, the status reports `doorOpen: null` and the UI shows "Door: not reported"
with the explanation. Door feed-forward only acts once `doorKnown` is true. Wording everywhere is "door", not "door/lid".

### 15.1 Parser / state additions (`printer_parse.h`, `state.h`)
* `doorOpen` (existing) plus `doorKnown` (false until the first *edge*), `doorEdgeCount`, `lastDoorOpenMs`, `lastDoorCloseMs`. The
  first report only establishes the raw state; it is not an edge and does not set `doorKnown`.
* Full `stg_cur` table (ha-bambulab, 0..77, −1/255 idle, −2 offline) — copy the table from
  `/home/eric/Documents/repos/BLLEDController/src/blled/stages.h` (`STAGE_NAMES`) into `state.cpp`.
* `chamberTarget` (°C, from the packed `device.ctc.info.temp` high word when > 0, else NaN).
* **Phase** (`phase`, derived, pure function `reportPhase(const PrinterReport&)` in `printer_parse.h`):
  | phase | rule (first match) |
  |---|---|
  | `offline` | no report yet / link down |
  | `paused` | `gcode_state == PAUSE` or stage ∈ {5,6,16,17,20,21,23,26,27,28,30,32,33,34,35} |
  | `preheat` | stage ∈ {2,7,49,54,58,63,64} or (`RUNNING` and (`bedTarget>0 && bed < bedTarget−3` or `chamberTarget>0 && chamber < chamberTarget−2`)) |
  | `cooling` | stage ∈ {29,50,69} |
  | `printing` | `gcode_state ∈ {RUNNING, PREPARE, SLICING}` |
  | `finished` | `gcode_state == FINISH` (until it becomes IDLE) |
  | `failed` | `gcode_state == FAILED` |
  | `idle` | otherwise |
  `printing` for the existing `onlyWhilePrinting` logic = phase ∈ {preheat, printing, paused}.

### 15.2 Config additions (`fan.*`)
```jsonc
"mode": "auto|manual|off|chamber",          // chamber = thermostat on the chamber temperature
"doorMode": "ignore",   "doorSpeed": 0,      // ignore|off|fixed — output while door/lid is open (auto mode)
"doorResumeSec": 5,                          // after the door closes, wait this long before resuming (anti-flap)
"preheatMode": "off",   "preheatSpeed": 0,   // ignore|off|fixed — output during phase == preheat (auto + chamber modes)
"chamberTarget": 45,                         // °C, thermostat set point while printing (chamber mode)
"cooldownTarget": 35,                        // °C, after the print: run until the chamber is this cool (chamber mode; also used by auto mode when onlyWhilePrinting: cooldown ends at target OR cooldownMin, whichever first)
"kp": 8.0, "ki": 0.02,                       // thermostat gains: % per °C, % per °C·s (integral, anti-windup)
"thermostatPeriodSec": 5,                    // controller update period
"ambientTemp": 25                            // °C assumed room temperature for the cooling-rate estimate
```
Validation: doorSpeed/preheatSpeed 0..100; doorResumeSec 0..300; chamberTarget 20..80; cooldownTarget 15..60;
kp 0..50; ki 0..1; thermostatPeriodSec 1..60; ambientTemp 0..40.

### 15.3 Control (`fan_control.cpp`) — evaluation order
1. `off` / `manual` (with expiry) unchanged.
2. Stale → existing failsafe.
3. **Door** (`doorMode != ignore`, `doorKnown && doorOpen`, and phase ∉ {finished, cooling, idle}): target = 0 (`off`) or `doorSpeed` (`fixed`); `effectiveMode = "door"`. After the door closes, keep this for `doorResumeSec`. During cool-down phases an open door is *helpful*, so it is ignored there.
4. **Preheat** (`preheatMode != ignore`, phase == preheat): target = 0 or `preheatSpeed`; `effectiveMode = "preheat"`.
5. Mode `chamber` (thermostat): every `thermostatPeriodSec`, `e = chamber − setpoint` where setpoint = `chamberTarget` while phase ∈ {preheat, printing, paused} and `cooldownTarget` while phase ∈ {finished, cooling, idle with a print having ended ≤ cooldownMin ago}. `out = kp·e + ki·∫e`, clamped 0..100, integral clamped to ±100/ki (anti-windup) and **frozen while the door is open or the output is saturated**. `effectiveMode = "chamber"` while printing, `"cooldown"` after. When phase is idle and no recent print: target 0 (`"idle"`). If `chamber` is NaN → fall back to the curve (`"auto"`).
6. Mode `auto` (curve) unchanged, except the cool-down window ends when `chamber ≤ cooldownTarget` **or** `cooldownMin` elapses.
7. Ramp / minSpeed / kick / invert unchanged (a pure-function `thermostatStep()` in `thermostat.h`, Arduino-free, host-tested).

### 15.4 Cooling-rate learning (`thermal.h/.cpp`, passive)
Sample `chamber` every 5 s. A *window* is a run of ≥ 60 s in which fan output stays within ±5 %, the door state is constant, and no heater is active (`bedTarget == 0 && nozzleTarget == 0`, i.e. after a print). For each window compute `k = −(dT/dt) / (T − ambientTemp)` (per minute) and blend into `k[bucket][door]` (buckets 0/25/50/75/100 % by nearest, door open/closed) with an EMA (α = 0.3); persist in config `thermal.k` (10 floats, samples count) at most every 10 min. Status exposes
`"thermal": { "rateCPerMin": -0.42, "kClosed":[…5], "kOpen":[…5], "samples": n }` (NaN → null). No automatic gain tuning yet — the UI shows the numbers and explains them; MQTT publishes `rateCPerMin` as a sensor.

### 15.5 Status / API / MQTT
* `printer.phase` (string), `printer.doorOpen` (`null` while `doorKnown` is false), `printer.doorKnown`, `printer.doorEdgeCount`, `printer.chamberTarget` (null when unknown); `fan.effectiveMode` gains `door`, `preheat`, `chamber`; `fan.setpoint` (thermostat set point or null); `thermal` block as above.
* `POST /api/fan` accepts `"mode":"chamber"`; MQTT `mode/set` likewise; HA `select.mode` options auto/manual/off/chamber; HA sensors `phase`, `cooling_rate` (°C/min); `number.chamber_target` (20..80) and `number.cooldown_target` (15..60) writable via `<base>/target/set` `{"chamberTarget":..,"cooldownTarget":..}` (or two topics `chamber_target/set`, `cooldown_target/set`).
* Mock server: simulate door toggles (`POST /mock/door` or a `--door` flag), phases through the fake print (preheat → printing → finished → idle), a chamber model that reacts to fan output and door state, so the thermostat can be exercised.

### 15.6 UI
* Fan curve page: mode selector (Curve / Chamber thermostat / Manual / Off) with a "Chamber thermostat" card (target, cool-down target, Kp/Ki advanced, period); a "Printer state rules" card (door mode + speed + resume delay, preheat mode + speed) — tooltips explain *why* (preheat: fan would fight the heaters; door: nothing to exhaust / dust; cool-down: open door speeds it up, fan keeps running) and note that the top lid is not sensed and that the door rule stays inert until the printer has reported a door change.
* Dashboard: phase chip (Preheating / Printing / Paused / Finished – cooling / Idle), door badge ("not reported" until doorKnown), thermostat set point and error when in chamber mode, cooling rate (°C/min) with the learned table in a collapsible "Thermal" card.

## 16. Filament-aware cooling (2.0.2)

The printer reports the loaded filament (`print.ams.ams[].tray[]`, `print.vt_tray`, `print.ams.tray_now`). The
[Filament Field Guide](https://github.com/pwsh/filament-field-guide) (data CC BY 4.0) records per material the
recommended ambient/chamber temperature, whether the enclosure should be *open* for cooling, whether a heated
chamber is required, the part-cooling demand and the ventilation demand. Combining the two lets the device pick
the chamber set point, cool-down behaviour and ventilation floor automatically, with user overrides.

### 16.1 Data (`tools/gen_filament_db.py` → `src/blflow/filament_db.h`, committed)
* Generator reads the guide (default: `https://pwsh.github.io/filament-field-guide/data/index.json` + per-entity
  `data/filaments/<id>.json`; `--src <clone dir>` for offline) and emits a PROGMEM table of `FilamentInfo`:
  `id[24]`, `name[32]`, `polymerClass` (enum), `chamberMin/Rec/Max` (int8 °C, −1 = n/a), `partCoolRec` (uint8 %),
  `vent` (0 optional / 1 recommended / 2 required), `flags` (bit0 enclosureRecommended, bit1 heatedChamberRequired,
  bit2 enclosureOpenForCooling, bit3 hardenedNozzle), `voc`, `particulate` (0..3). Header carries the source URL,
  fetch date, record count and the CC BY 4.0 attribution. ~90 records ≈ 7 KB flash.
* `tools/bambu_filament_ids.csv` (from Bambu Studio profiles, 100 rows: id,name,type) is embedded as a compact
  `BambuFilament` table (`idx[8]`, `type[12]`) so a bare `tray_info_idx` still resolves to a material.

### 16.2 Matching (`filament_match.h`, pure, host-tested)
Input: `tray_type`, `tray_sub_brands`, `tray_info_idx`. Output: guide `id` (or "" = unknown) + `family` string.
1. Normalise `tray_type`: upper-case, trim; split at the first `-` into BASE and MOD (`PLA-CF` → `PLA`,`CF`;
   `PLA-AERO` → `PLA`,`AERO`; `PAHT-CF` → `PAHT`,`CF`; `TPU-AMS` → `TPU`; `SUPPORT…` → see 4).
2. BASE → guide id: PLA→pla, PETG→petg, PCTG→pctg, ABS→abs, ASA→asa, PC→pc, PA/PAHT→pa, PA6→pa6, PA12→pa12,
   PPA→ppa, TPU→tpu, PVA→pva, BVOH→bvoh, HIPS→hips, PET→pet, PPS→pps, PP→pp, PE→pe, EVA→eva, PHA→pha.
   MOD ∈ {CF, GF}: use `<id>-cf`/`<id>-gf` when that guide id exists, else the base id (family keeps the "CF" hint).
   MOD = AERO → base id. Unknown BASE → step 3.
3. If `tray_type` is empty/unknown, use `tray_info_idx` through the Bambu table to get a `type`, then step 1–2.
   As a last resort the prefix rule: GFA/GFL→PLA, GFB→ABS, GFC→PC, GFG→PETG, GFN→PA, GFP→PP, GFT→PPS, GFU→TPU.
4. Support materials (`tray_type` starting `SUPPORT`, or `GFS0x`): profile of the *paired* material
   (Support For PLA/PETG → pla, Support For PA/PET → pa, Support for ABS → abs, Support W → pla, Support G → pa);
   PVA/BVOH/HIPS map to their own ids.
5. The active tray: `tray_now` = `ams*4+slot` (0..15), `254` = `vt_tray`, `255`/none = no filament; H2D:
   `device.extruder.info[active].snow` = `(ams<<8)|slot`, active extruder from `device.extruder.state>>4`.
   Only the active tray is resolved (all trays are kept for the UI: id, type, colour, idx). AMS-HT ids ≥128 have one slot.

### 16.3 Effective cooling profile (`filament.cpp`)
From the matched `FilamentInfo`:
```
keepCool          = flags.enclosureOpenForCooling || chamberRec < 35
chamberTarget     = keepCool ? chamberMax : chamberRec          // PLA → 30, PETG → 35, ABS → 50, ASA → 55, PC → 55
cooldownTarget    = config.fan.cooldownTarget                    // unchanged unless overridden
postPrintCooling  = partCoolRec >= 50 ? "fast" : "gentle"       // gentle: fan off until chamber < chamberTarget−10, then ≤ 50 %
ventFloor         = config.filament.ventFloor[vent]              // % minimum output while printing (0 disables)
heatedRequired    = flags.heatedChamberRequired                  // informational (X1/P1 cannot heat)
```
User config:
```jsonc
"filament": {
  "auto": true,                 // use the loaded filament to set chamberTarget / cool-down / vent floor
  "manualId": "",               // force a guide id when no tray data (external spool without RFID, P1 without AMS)
  "ventFloor": { "optional": 0, "recommended": 0, "required": 10 },   // % — Bambu keeps the exhaust OFF for warm-chamber materials; raise only with a filtered exhaust
  "overrides": [                // max 12; keys are guide ids or "*"; every value optional (null = keep)
    { "id": "abs", "chamberTarget": 48, "cooldownTarget": 35, "ventFloor": 5, "postPrintCooling": "gentle" }
  ]
}
```
Resolution order: guide profile → `"*"` override → id override → (when `auto` is false) the plain `fan.*` values.
Applies to: `chamber` mode set points (integrator reset when the set point moves > 5 °C), the cool-down window
in both modes, the vent floor in both modes (never during `preheat`/`door` gating or `off`/`manual`), and the
cool-down cap for `gentle`. Tray changes take effect immediately (multi-material prints).

### 16.4 Status / API / MQTT / UI
* Status: `"filament": { "source": "ams|external|manual|none", "tray": {"ams":0,"slot":0,"type":"ABS","subBrand":"","idx":"GFB00","color":"FFFFFFFF"},
  "id":"abs", "name":"ABS", "family":"ABS", "profile": {"chamberRec":50,"chamberMax":60,"partCoolRec":0,"vent":"required","openForCooling":false,"heatedRequired":false},
  "effective": {"chamberTarget":48,"cooldownTarget":35,"ventFloor":5,"postPrintCooling":"gentle","overridden":true},
  "trays":[{"ams":0,"slot":0,"type":"ABS","idx":"GFB00","color":"FFFFFFFF","id":"abs"}, …, {"ams":-1,"slot":254,"type":"ASA","idx":"GFB01","id":"asa"}] }`
* `GET /api/filaments` → the embedded guide table (id, name, class, chamber, vent, flags) for the UI's override editor.
* HA: `sensor.filament` (state = name; attributes type/idx/id/vent/chamberTarget), `sensor.filament_chamber_target`.
* UI: new **Filament** card on the Fan curve page — detected tray (colour swatch, type, Bambu id, sub-brand), matched
  guide entry with its properties and a link `https://pwsh.github.io/filament-field-guide/#/filaments/<id>` ("Data: Filament
  Field Guide, CC BY 4.0"), the *effective* targets with an "override for this material" editor (chamber target,
  cool-down target, vent floor, post-print cooling) and the global vent-floor table; all trays listed; auto toggle
  and manual id select (searchable list from `/api/filaments`). Dashboard: filament chip (colour + name) next to the phase chip.
* Mock: `--filament ABS` and `POST /mock/tray {"now":"0"}`; the fake AMS is `test/fixtures/x1c_ams_trays.json`.
* Tests: `test/test_filament` — matcher against every row of `tools/bambu_filament_ids.csv` (must resolve to a non-empty id
  or a documented exception), the live fixture (`tray_now 0` → abs; `254` → asa), CF/GF fallbacks, support pairing,
  effective-profile derivation (PLA keepCool → 30, ABS → 50, override precedence).
