# BLSmartFlow

ESP32 firmware for the **SmartFlow** fan module: it reads your Bambu Lab printer's temperatures over the
printer's local MQTT link and drives two PWM fan outputs from a temperature → speed curve you edit in a
browser. Works with X1 / X1C / X1E, P1P / P1S, A1 / A1 mini and (best effort) H2D printers in LAN mode.

> **Version 2.0** is a ground-up rework of the original firmware: a new responsive web UI, a JSON REST
> API, a live event stream, Home Assistant / MQTT integration, WiFi setup over an access point, and a long
> list of robustness fixes. See [docs/CODE-REVIEW.md](docs/CODE-REVIEW.md) for what was wrong before and
> [docs/REWORK-SPEC.md](docs/REWORK-SPEC.md) for how 2.0 is designed.

## Features

- **Temperature-driven fan curve** – 2 to 16 points, drag-editable in the UI, with presets. Source can be
  the nozzle, bed, chamber or the hottest of the three.
- **Control behaviour** – hysteresis, ramp rate, minimum running speed, kick-start pulse, 25 kHz PWM
  (configurable), inverted outputs, per-output enable.
- **Safety** – stale-data failsafe (off / hold / fixed speed when the printer stops reporting), optional
  "only while printing" with a post-print cooldown window, manual override with an automatic expiry.
- **Dashboard** – live temperatures, print progress, printer fans, connection state and fan output,
  pushed once per second (server-sent events, polling fallback).
- **Printer link** – TLS MQTT to the printer in its own task (a printer that is switched off never stalls
  the UI), `pushall` on connect, exponential reconnect back-off, current X1 firmware `device.*` report
  format supported (chamber temperature).
- **Integrations** – optional external MQTT broker with Home Assistant auto-discovery (fan entity, mode
  select, temperature / progress / state sensors, restart button) and command topics.
- **REST API** – `/api/*` JSON endpoints for everything the UI does, documented in the UI and in
  [docs/REWORK-SPEC.md §9](docs/REWORK-SPEC.md). The 1.x endpoints (`/getOptions`, `/sensorData`, …) still work.
- **Setup without USB** – on first boot (or when WiFi fails) the device starts the `BLSmartFlow-xxxx`
  access point with a captive portal. USB serial provisioning (`{"ssid":…,"pass":…}`) still works.
- **System** – OTA update from the browser, backup / restore of the configuration, restart, factory reset,
  optional login, on-device log viewer, mDNS (`http://blsmartflow.local/`).

## Hardware

| Function | GPIO |
|---|---|
| Fan output 1 (PWM) | 17 |
| Fan output 2 (PWM, same signal) | 16 |
| Status LED | 21 |

Status LED: solid = OK · 1 blink = setup mode (access point) · 2 = WiFi down · 3 = printer MQTT down ·
4 = printer data stale · solid with a short double dip = manual override active.

## Installing

1. Flash the merged image `firmware/esp32dev/BLSmartflow_<version>.bin` at offset `0x0` with the
   [web flasher](https://www.dutchdevelop.com/blsmartflow) or `esptool write_flash 0x0 <file>`.
   **Upgrading from a 2025.x firmware requires this full flash once** (the partition layout changed);
   afterwards you can use OTA from the System page.
2. Power the device. Join the WiFi network `BLSmartFlow-xxxx`, a setup page opens (or browse to
   `http://192.168.4.1/`). Enter your WiFi credentials and save; the device restarts and joins your network.
3. Open `http://blsmartflow.local/` (or the IP shown on your router). On the **Printer** page enter the
   printer's IP address, LAN access code and serial number (all on the printer's network settings screen;
   LAN mode / developer mode must be enabled). The dashboard shows live data within a few seconds.
4. Shape your curve on the **Fan curve** page and save.

## Building

Requirements: [PlatformIO](https://platformio.org/) (Core ≥ 6.1) and Python 3.

```sh
pio run                 # firmware (.pio/build/esp32dev/firmware.bin, merged image in .firmware/)
pio run -t upload       # flash over USB
pio test -e native      # host-side unit tests (fan curve, printer report parser)
python3 tools/mock_server.py   # serve the UI at http://localhost:8080 with a simulated printer
```

The web UI is a single file, `src/www/index.html`; `pre_build.py` gzips it into `src/www/www.h` at build
time (generated, not committed). Version and project name live in `platformio.ini` (`custom_version`);
`merge_firmware.py` produces the merged flasher image and updates `firmware/manifest.json`.

Toolchain: pioarduino ESP32 platform (Arduino-ESP32 3.3.x), ArduinoJson 7, PubSubClient, ESPAsyncWebServer
(ESP32Async), ESP32SSDP. CI builds and tests every push (`.github/workflows/build.yml`).

## Project layout

```
src/main.cpp                 boot + non-blocking main loop
src/blflow/config.*          persisted configuration (LittleFS /config.json), validation, migration
src/blflow/curve.h           pure fan-curve math (unit-tested on the host)
src/blflow/fan_control.*     PWM outputs and the control state machine
src/blflow/printer_link.*    Bambu MQTT client (FreeRTOS task)  ·  printer_parse.h: report parser
src/blflow/ha_mqtt.*         external MQTT broker + Home Assistant discovery
src/blflow/web_server.*      REST API, SSE, OTA, captive portal  ·  status.*: the status object
src/blflow/wifi_manager.*    STA/AP state machine, scan, mDNS
src/www/index.html           the web UI
tools/mock_server.py         API simulator for UI development
test/                        Unity tests + real X1C MQTT fixtures
docs/                        spec, code review, WebSerial provisioning page, images
```

## License

Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International (CC BY-NC-SA 4.0). See [LICENSE](LICENSE).

## Credits

- **[DutchDeveloper](https://dutchdevelop.com/)** – original author and lead programmer
- **[xps3riments](https://github.com/xps3riments)** – inspiration for the foundation of the code
- **[longrackslabs](https://github.com/longrackslabs)** – build process, documentation, community support
- **sschwetz** – chamber-temperature source and dependency fixes (upstream PRs #4 / #5)
