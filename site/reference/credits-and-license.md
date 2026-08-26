# Credits and licence

## Firmware and documentation

**Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International**
([CC BY-NC-SA 4.0](https://creativecommons.org/licenses/by-nc-sa/4.0/legalcode)).

You are free to:

- **Share** — copy and redistribute the material in any medium or format;
- **Adapt** — remix, transform and build upon the material.

Under these terms:

- **Attribution** — give appropriate credit, link to the licence, and indicate if changes were made.
- **Non-commercial** — you may not use the material for commercial purposes.
- **Device and DIY restriction** — the material may only be used for the "bambulanb controller"
  device or for non-commercial DIY projects.
- **Share-alike** — distribute your contributions under the same licence as the original.
- **No additional restrictions.**

The full text is in [`LICENSE`](https://github.com/pwsh/BLSmartFlow/blob/main/LICENSE) in the
repository. It is based on CC BY-NC-SA 4.0, whose terms apply to the extent they do not conflict.

## Material data — the Filament Field Guide

The per-material data behind [filament-aware cooling](../using/filament-aware-cooling.md) —
recommended ambient bands, part-cooling figures, ventilation demand, emission levels and enclosure
flags — comes from the **[Filament Field Guide](https://github.com/pwsh/filament-field-guide)** by
pwsh.

It is used under the **[Creative Commons Attribution 4.0 International licence
(CC BY 4.0)](https://creativecommons.org/licenses/by/4.0/)**.

It is compiled into `src/blflow/filament_db.h` by `tools/gen_filament_db.py`; the generated header
carries the source URL, the fetch date, the record counts and the attribution. Every place the UI
shows one of its numbers carries the credit and a link back, and
[`GET /api/filaments`](../technical/rest-api.md#status-and-diagnostics) returns
`"licence": "CC BY 4.0"` alongside the data.

## Lineage

BLSmartFlow descends from the **BLLEDController** firmware and the SmartFlow fan module.

The 2.0 rework is a ground-up rewrite of the 1.x / 2025.x firmware — the review that motivated it
lists around thirty findings, several of them crashes or silent data loss — but the hardware, the
purpose and much of the original thinking come from the projects and people below.

## Credits

- **[DutchDeveloper](https://dutchdevelop.com/)** — original author and lead programmer.
- **[xps3riments](https://github.com/xps3riments)** — inspiration for the foundation of the code.
- **[longrackslabs](https://github.com/longrackslabs)** — build process, documentation, community
  support.
- **sschwetz** — chamber-temperature source and dependency fixes (upstream PRs #4 and #5), and the
  LED indicator work (PRs #2 and #3).
- **[Filament Field Guide](https://github.com/pwsh/filament-field-guide)** by **pwsh** — the
  per-material data, under CC BY 4.0.

## Third-party libraries

| Library | Licence | Used for |
|---|---|---|
| [ArduinoJson 7](https://arduinojson.org/) | MIT | Every JSON document, including the filtered printer-report parse |
| [PubSubClient 2.8](https://github.com/knolleary/pubsubclient) | MIT | Both MQTT clients |
| [ESPAsyncWebServer 3.12](https://github.com/ESP32Async/ESPAsyncWebServer) + [AsyncTCP 3.5](https://github.com/ESP32Async/AsyncTCP) | LGPL-3.0 | The web server, SSE and OTA |
| [ESP32SSDP 2.0.3](https://github.com/luc-github/ESP32SSDP) | LGPL-2.1 | Optional UPnP/SSDP discovery |
| [pioarduino platform-espressif32](https://github.com/pioarduino/platform-espressif32) | Apache-2.0 | Arduino-ESP32 3.3.x toolchain |

The **stage-name table** (`stg_cur` 0–77) follows the naming used by the
[ha-bambulab](https://github.com/greghesp/ha-bambulab) Home Assistant integration, so that
`printer.stageText` matches what other tooling in the ecosystem publishes.

## The web UI

`src/www/index.html` is one file with inline CSS and JavaScript and **no external requests at all** —
no CDN, no web fonts, no remote images. That is deliberate: it has to work on the setup network with
no internet, and it should not leak anything to third parties.

## This documentation

Built with [MkDocs](https://www.mkdocs.org/) and
[Material for MkDocs](https://squidfunk.github.io/mkdocs-material/) (MIT). The screenshots are
rendered from `tools/mock_server.py`, so they show the real UI driven by a simulated printer rather
than anyone's actual network.

The in-repository reference documents remain the source of truth for contributors:

| Document | What is in it |
|---|---|
| `docs/USER-GUIDE.md` | The complete user guide |
| `docs/TECHNICAL.md` | Architecture, config schema, REST API, MQTT contract, build and test |
| `docs/REWORK-SPEC.md` | The 2.0 design record and the firmware ↔ UI contract |
| `docs/CODE-REVIEW.md` | What was wrong with the 1.x firmware and why 2.0 exists |
