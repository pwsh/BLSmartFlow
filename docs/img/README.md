# Images

Screenshots of the 2.0 web UI, captured in the dark theme against `tools/mock_server.py` (so no real
network, printer or serial numbers appear). Desktop shots are 1280 px wide; the `ui-mobile-*` shots
are 390 px wide phone renders.

| File | Shows |
|---|---|
| `ui-dashboard.png` | Dashboard: fan gauge, manual override, temperatures, print job, connections |
| `ui-curve.png` | Fan curve page: canvas editor, presets, point table |
| `ui-filament.png` | The Filament card on the Fan curve page, with every section expanded |
| `ui-printer.png` | Printer page: connection form and the live status card |
| `ui-network.png` | Network page as seen from the LAN |
| `ui-setup-ap.png` | Network page in setup mode, with the access-point banner |
| `ui-integrations.png` | Integrations page: broker settings, MQTT topics, REST API reference |
| `ui-system.png` | System page: device info, OTA, backup/restore, web access, maintenance, log |
| `ui-mobile-dashboard.png` | Dashboard on a 390 px phone screen |
| `ui-mobile-curve.png` | Curve editor on a 390 px phone screen |
| `smartflow-module.png` | Composite illustration of the fan module (shell + blades), used by the guide and the site |
| `smartflow_shell.png`, `smartflow_blades.png` | Original renders of the fan module hardware (historical; white line-art on a transparent background, so they are invisible on a light page) |

To retake a screenshot, run `python3 tools/mock_server.py` (add `--ap` for the setup-mode shot) and
capture the page at the same width. `ui-filament.png` is a clip of `#filCard` with its `<details>`
sections forced open; `POST /mock/tray {"now":"0"}` selects which tray it shows.
