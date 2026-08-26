# Changelog

Versions are set in one place — `custom_version` in `platformio.ini` — and flow into the firmware
banner, `/api/info`, the merged image name and `firmware/manifest.json`.

---

## 2.0.4 — printer command rejection, narrow-screen header

!!! info "In development"

- **The printer can now say no, and you get told.** A Bambu printer with **Developer Mode** switched
  off keeps reporting normally but signature-checks every write command, refusing the cool-down's
  `M106` with `mqtt message verify failed`. That acknowledgement used to be dropped as noise. The
  link now matches it to the `sequence_id` it sent (ours start at 5000; Bambu Studio's acks are still
  ignored) and records it.
- The cool-down reports `cooldown.printerFans.error` and forces `printerFans.sent` to `false`, logs
  the refusal **once per session**, and retries once every **5 minutes** instead of every 30 seconds
  — so switching Developer Mode on mid-session is still picked up.
- New status field `printer.lastCommandError`, cleared by the next accepted command. The Home
  Assistant *Cool-down result* sensor carries it as an `error` attribute.
- The dashboard and the cool-down card show a banner explaining exactly which printer setting to
  switch on.
- **Fixed: the header forced a ~429 px minimum page width**, so cards were clipped on the right on a
  390 px phone. The header now wraps — badges move under the brand below 430 px and a long SSID
  truncates — and card headers wrap their badges instead of overflowing.
- `tools/mock_server.py --reject-gcode` reproduces the refusal without the hardware.

→ [Post-print cool-down](../using/post-print-cooldown.md) ·
[Troubleshooting: the printer link](../troubleshooting/printer-link.md#the-cool-down-says-the-printer-rejected-the-fan-command)

---

## 2.0.3 — post-print cool-down

!!! info "In development"
    Specified in `docs/REWORK-SPEC.md` §17.

- **Post-print cool-down sessions.** When a print finishes, the device runs a deliberate cool-down to
  a target chamber temperature and stops when it gets there — or on a timeout, a new job, or a manual
  stop.
- **Optionally drives the printer's own fans** (`M106 P2` aux, `M106 P3` chamber) over the local MQTT
  link, re-asserting every 30 s. **Off by default**: this is the only feature that sends commands to
  the printer, and it never sends anything unless the printer reports `FINISH` or `IDLE`.
- The device's own fan during a session follows `cooldown.ownFan` — thermostat, max or curve.
- Honours the material's **gentle** post-print rule, so an ABS part is not hit with cold air the
  moment the print ends.
- New: `cooldown.*` configuration, `POST /api/cooldown`, a `cooldown` block in the status object,
  the `<base>/cooldown/set` topic, and Home Assistant `switch.cooldown` plus
  `sensor.cooldown_remaining` and `sensor.cooldown_reason`.
- Dashboard: a live progress line with **Stop**, and a **Cool down now** button when the printer is
  idle or finished.
- **Filament memory.** When the AMS unloads at the end of a print, `tray_now` reports nothing. The
  device now remembers the material of the print that just ended through the *finished*, *cooling*
  and *idle* phases — `filament.source` becomes `"last"` and the UI shows a *last print* badge — so
  the cool-down rule still follows the right material. Cleared when a new tray is loaded or a new job
  starts without one.

→ [Post-print cool-down](../using/post-print-cooldown.md)

---

## 2.0.2 — filament-aware cooling

*Released 2026-08-26 · `545a061`*

The printer already knows what is loaded; the Filament Field Guide already knows what each material
wants from an enclosure. This release joins the two.

- **The loaded filament sets the targets.** The chamber set point, the post-print cool-down style and
  a ventilation floor now follow the material in the active tray, from an embedded copy of the
  [Filament Field Guide](https://github.com/pwsh/filament-field-guide) — about 90 materials in ~8 KB
  of flash, with no network access.
- **Tray matching** covers `tray_type`, `tray_sub_brands` and `tray_info_idx`, with CF/GF fallbacks to
  the unfilled polymer, support-material pairing, the Bambu id prefix rules, the `tray_now`
  encoding including the external holder and AMS-HT unit ids, and the H2D `device.extruder.snow`
  encoding for two tool heads.
- **Per-material overrides** — up to twelve rules, plus a `"*"` catch-all — and a global
  ventilation-floor table.
- **A ventilation floor that is honest about the trade-off.** Defaults are 0 % except for
  *ventilation required*, which gets 10 %: Bambu deliberately keeps the exhaust off for warm-chamber
  materials, and an exhaust fan throws away the heat that stops ABS splitting.
- **Gentle post-print cooling** for materials the guide prints with the part fan off.
- New: `filament.*` configuration, a `filament` block in the status object, `GET /api/filaments`,
  Home Assistant `sensor.filament` and `sensor.filament_chamber_target`, a *Filament* card on the Fan
  curve page and a filament chip on the dashboard.
- Tooling: `tools/gen_filament_db.py` with `--src` and `--check`; `tools/mock_server.py` gains
  `--filament TYPE` and `POST /mock/tray`; a new `test/test_filament` suite that resolves **every row**
  of `tools/bambu_filament_ids.csv`.

→ [Filament-aware cooling](../using/filament-aware-cooling.md) ·
[Filament matching](../technical/filament-matching.md)

---

## 2.0.1 — print phases, door rules, chamber thermostat, cooling-rate learning

*Released 2026-08-26 · `5e73e78`*

An exhaust fan that follows a temperature curve alone fights the printer during warm-up and ignores
the single biggest disturbance — an open door. This release gives the controller a model of what the
printer is *doing*.

- **Print phases.** `stg_cur` and `gcode_state` are turned into one derived phase — `offline`,
  `paused`, `preheat`, `cooling`, `printing`, `finished`, `failed`, `idle`. Preheat is detected even
  while the printer calls itself *running*, if the bed is more than 3 °C or the chamber more than 2 °C
  below target. The full ha-bambulab stage table (0–77) is included.
- **Door feed-forward, with the `doorKnown` rule.** `home_flag` bit 23 is the front-door switch — but
  on some X1C units a closed door never presses it, so the bit reads "open" forever. The device
  therefore does not trust the bit until it has seen it **change**; until then the door is treated as
  closed, `printer.doorOpen` is `null` and the rule is inert. **The top lid has no sensor at all.**
- **Chamber thermostat mode.** A PI controller on the chamber temperature, with a hard ±100/ki
  anti-windup clamp and conditional integration frozen while the door is open or the output is
  saturated. Falls back to the curve on printers with no chamber sensor.
- **Passive cooling-rate learning.** Newton's law of cooling fitted over ≥ 60 s windows of steady fan
  output, unchanged door and no active heater, blended into a table of `k` values per fan bucket and
  door state.
- **A cool-down that ends when it should.** In curve mode the post-print window now ends at the
  cool-down target **or** the timer, whichever comes first.
- New: `fan.doorMode/doorSpeed/doorResumeSec`, `fan.preheatMode/preheatSpeed`,
  `fan.chamberTarget/cooldownTarget/kp/ki/thermostatPeriodSec/ambientTemp`, `thermal.*`;
  `printer.phase`, `printer.doorKnown`, `fan.setpoint` and a `thermal` block in the status object;
  Home Assistant `phase`, `cooling_rate`, `number.chamber_target` and `number.cooldown_target`.
- Tests: new `test_thermostat` and `test_thermal` suites; `tools/mock_server.py` gains `--door` and
  `POST /mock/door`, plus a Newtonian chamber model.

→ [Printer state rules](../using/printer-state-rules.md) ·
[Chamber thermostat](../technical/chamber-thermostat.md) ·
[Cooling-rate learning](../technical/cooling-rate-learning.md)

---

## 2.0.0 — the rework

*Released 2026-08-25 · `f0f5bf0`*

A ground-up rewrite of the 1.x / 2025.x firmware. The review that motivated it — 30-odd findings,
several of them crashes or silent data loss — is in `docs/CODE-REVIEW.md`.

**Architecture**

- The printer's TLS MQTT session moved into **its own FreeRTOS task**. In 1.x an unreachable printer
  blocked the main loop for seconds at a time and froze both the fan output and the web UI.
- POD state snapshots under spinlocks, a recursive mutex for the configuration, and long operations
  never performed under a lock.
- Arduino-free, host-testable modules for the curve, the report parser, the thermostat, the filament
  matcher and the thermal maths.

**Fan control**

- 25 kHz LEDC PWM instead of `analogWrite`'s audible ~1 kHz, configurable 500–40000 Hz, with an invert
  option for active-low driver boards.
- Hysteresis, ramp rate, minimum-speed clamp and kick-start.
- A stale-data failsafe — hold, off or a fixed speed — where 1.x simply held the last speed forever.
- Curve validation that cannot divide by zero, cannot be emptied and cannot be unsorted.

**Configuration**

- A validated JSON schema on LittleFS, written atomically, with masked secrets, deep merge, clamping
  rather than rejection, and one-time migration from the 1.x `/blledconfig.json`. The 1.x bug where
  `/submitOptions` never persisted anything is fixed.

**Networking and UI**

- A non-blocking WiFi state machine with backoff, a BSSID lock that drops itself, an **open setup
  network with a captive portal** after 90 s of failure, mDNS, and optional SSDP.
- A new single-file responsive web UI with **no external requests at all** — no CDN, no fonts, no
  remote images — so it works in AP mode and leaks nothing.
- A full JSON REST API, a server-sent event stream, and the 1.x endpoints kept for compatibility.
- External MQTT broker support with Home Assistant auto-discovery.
- OTA update from the browser, backup and restore, factory reset, optional HTTP basic auth, and an
  on-device log viewer.

**Toolchain**

- pioarduino platform `55.03.311` (Arduino-ESP32 3.3.x), pinned dependencies, `min_spiffs.csv`
  partitions, `pre_build.py` asset gzipping, `merge_firmware.py` packaging, and CI that builds and
  tests every push.

!!! danger "Upgrading from 2025.x needs one USB flash"
    The partition layout changed, so an OTA from 2025.x is not possible. Flash the merged image once
    at offset `0x0` — this erases the stored settings — and every later update can be an OTA.

**Follow-ups in the same series**

- `5001da5` — user guide, technical reference and README, with UI screenshots; captive-portal
  refinements.
- `aaca68b` — fixed garbage strings in `/api/status` (ArduinoJson stores `const char*` by pointer,
  not by value), captive-portal UI robustness, icon clipping on Safari.
- `e8cd581` — CI: upload artifacts from the hidden `.firmware` directory.

---

## Before 2.0

The 1.x / 2025.x firmware by [DutchDeveloper](https://dutchdevelop.com/), with contributions from
xps3riments, longrackslabs and sschwetz — including the chamber-temperature source and dependency
fixes in upstream PRs #4 and #5, and the LED indicator work in PRs #2 and #3.

→ [Credits and licence](credits-and-license.md)
