# BLSmartFlow

ESP32 firmware for the **SmartFlow** fan module. It reads your Bambu Lab printer's temperatures over
the printer's local MQTT link and drives two PWM fan outputs from a temperature → speed curve you
edit in a browser. Works with X1 / X1C / X1E, P1P / P1S, A1 / A1 mini and (best effort) H2D printers
in LAN Only Mode.

![The BLSmartFlow dashboard: fan gauge, manual override, temperatures and print job](docs/img/ui-dashboard.png)

![The fan curve editor with drag-editable points and the matching table](docs/img/ui-curve.png)

> **Version 2.0** is a ground-up rework: responsive web UI, JSON REST API, live event stream, Home
> Assistant / MQTT integration, WiFi setup over a captive portal, and a long list of robustness
> fixes.

## Features

- **Temperature-driven fan curve** – 2 to 16 points, drag-editable, with presets. Source can be the
  nozzle, bed, chamber or the hottest of the three.
- **Control behaviour** – hysteresis, ramp rate, minimum running speed, kick-start pulse, 25 kHz PWM
  (configurable), inverted outputs, per-output enable.
- **Filament-aware cooling** – reads the loaded filament from the AMS or the external spool and picks
  the chamber target, the post-print cool-down style and a ventilation floor to match it, from an
  embedded copy of the Filament Field Guide. Per-material overrides, or switch it off entirely.
- **Safety** – stale-data failsafe (off / hold / fixed speed when the printer stops reporting),
  optional "only while printing" with a post-print cooldown, manual override with automatic expiry.
- **Dashboard** – live temperatures, print progress, printer fans, connection state and fan output,
  pushed once per second over server-sent events (polling fallback).
- **Printer link** – TLS MQTT to the printer in its own FreeRTOS task (a printer that is switched off
  never stalls the UI), `pushall` on connect, exponential reconnect back-off, current X1 firmware
  `device.*` report format including the chamber temperature.
- **Integrations** – optional external MQTT broker with Home Assistant auto-discovery (fan entity,
  mode select, temperature / progress / state sensors, restart button) and command topics.
- **REST API** – `/api/*` JSON endpoints for everything the UI does, plus the 1.x endpoints
  (`/getOptions`, `/sensorData`, …).
- **Setup without USB** – on first boot (or after a WiFi outage) the device raises the
  `BLSmartFlow-xxxx` setup network with a captive portal. USB serial provisioning still works.
- **System** – OTA update from the browser, backup / restore, restart, factory reset, optional login,
  on-device log viewer, mDNS (`http://blsmartflow.local/`).

## Quick start

1. **Flash** over USB with the [web flasher](https://pwsh.github.io/BLSmartFlow/getting-started/web-flasher/)
   (Chrome/Edge) or `esptool write_flash 0x0 firmware/esp32dev/BLSmartflow_<version>.bin`.
   Upgrading from a 2025.x firmware needs this full flash once, and it erases the settings.
2. **Join `BLSmartFlow-xxxx`** (open network). The setup page opens by itself; if it does not, browse
   to <http://192.168.4.1/>.
3. **Enter your WiFi**, save, and reconnect your phone or laptop to your own network. The device is
   then at <http://blsmartflow.local/> (or the IP shown by your router).
4. **Add the printer** on the *Printer* page: IP address, 8-character LAN access code and serial
   number, all from the printer's network screen with LAN Only Mode enabled.
5. **Shape the curve** on the *Fan curve* page and save.

Every step, with screenshots and troubleshooting: **[docs/USER-GUIDE.md](docs/USER-GUIDE.md)**.

## Documentation

| Document | What is in it |
|---|---|
| [docs/USER-GUIDE.md](docs/USER-GUIDE.md) | Flashing, setup, printer, curve, Home Assistant, LED patterns, troubleshooting |
| [docs/TECHNICAL.md](docs/TECHNICAL.md) | Architecture, config schema, REST API, MQTT contract, build & test, limitations |
| [docs/REWORK-SPEC.md](docs/REWORK-SPEC.md) | The 2.0 design record and firmware ↔ UI contract |
| [docs/CODE-REVIEW.md](docs/CODE-REVIEW.md) | What was wrong with the 1.x firmware and why 2.0 exists |

## Hardware

| Function | GPIO | Notes |
|---|---|---|
| Fan output 1 (PWM) | 17 | LEDC, 25 kHz default, 8-bit |
| Fan output 2 (PWM) | 16 | Same duty as output 1, can be disabled |
| Status LED | 21 | Non-blocking blink patterns |

Status LED: solid = OK · 1 blink = setup network · 2 = WiFi down · 3 = printer MQTT down ·
4 = printer data stale · solid with a short double dip = manual override.

## Building

Requirements: [PlatformIO](https://platformio.org/) (Core ≥ 6.1) and Python 3.

```sh
pio run                        # firmware + merged image in .firmware/
pio run -t upload              # flash over USB
pio test -e native             # host-side unit tests (curve, report parser, RX buffer)
python3 tools/mock_server.py   # serve the UI at http://localhost:8080 with a simulated printer
```

The UI is a single file, `src/www/index.html`; `pre_build.py` gzips it into `src/www/www.h` at build
time (generated, not committed). The version lives only in `platformio.ini` (`custom_version`);
`merge_firmware.py` produces the OTA and merged images and updates `firmware/manifest.json`.
CI builds and tests every push (`.github/workflows/build.yml`).

## Project layout

```
src/main.cpp                 boot + non-blocking main loop
src/blflow/config.*          persisted configuration (LittleFS /config.json), validation, migration
src/blflow/curve.h           pure fan-curve math (unit-tested on the host)
src/blflow/fan_control.*     PWM outputs and the control state machine
src/blflow/printer_link.*    Bambu MQTT client (FreeRTOS task)  ·  printer_parse.h: report parser
src/blflow/filament.*        loaded-filament matching  ·  filament_match.h + generated filament_db.h
src/blflow/ha_mqtt.*         external MQTT broker + Home Assistant discovery
src/blflow/web_server.*      REST API, SSE, OTA, captive portal  ·  status.*: the status object
src/blflow/wifi_manager.*    STA/AP state machine, scan, mDNS
src/www/index.html           the web UI
tools/mock_server.py         API simulator for UI development
tools/gen_filament_db.py     regenerates src/blflow/filament_db.h from the Filament Field Guide
test/                        Unity tests + real X1C MQTT fixtures
docs/                        guides, spec, code review, WebSerial provisioning page, images
```

## License

Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International (CC BY-NC-SA 4.0). See [LICENSE](LICENSE).

## Credits

- **[DutchDeveloper](https://dutchdevelop.com/)** – original author and lead programmer
- **[xps3riments](https://github.com/xps3riments)** – inspiration for the foundation of the code
- **[longrackslabs](https://github.com/longrackslabs)** – build process, documentation, community support
- **sschwetz** – chamber-temperature source and dependency fixes (upstream PRs #4 / #5)
- **[Filament Field Guide](https://github.com/pwsh/filament-field-guide)** – the per-material data
  behind filament-aware cooling (`src/blflow/filament_db.h`), used under
  [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/)
