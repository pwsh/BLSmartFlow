# Post-print cool-down

!!! info "New in 2.0.3"
    Specified in `docs/REWORK-SPEC.md` §17.

A **cool-down session** runs after a print to bring the chamber down to a target temperature, using
the device's own fan and — if you opt in — the printer's auxiliary and chamber fans.

This is the **only** feature that sends commands to the printer. Everything else BLSmartFlow does is
read-only.

## The G-code path

After a print the printer reports `gcode_state == FINISH` and stops issuing commands of its own, so
the device may safely drive the fans:

| Command | Fan |
|---|---|
| `M106 P2 S<0-255>` | Auxiliary |
| `M106 P3 S<0-255>` | Chamber / exhaust |

Sent on `device/<serial>/request` as:

```json
{"print":{"sequence_id":"<n>","command":"gcode_line",
          "param":"M106 P2 S255\nM106 P3 S255\n"}}
```

The command path is a small **spinlock-guarded queue**, `printerLinkSendGcode(const char*)`, drained
by the [printer task](printer-link.md) — no other task ever touches the MQTT client. Publish failures
are logged and retried on the next tick.

## Configuration

```jsonc
"cooldown": {
  "enabled": true,            // start a session automatically when a print finishes
  "target": 35,               // °C chamber; the filament cooldownTarget wins when filament.auto is on
  "usePrinterFans": false,    // send M106 to the printer (opt-in)
  "auxSpeed": 100,            // % for M106 P2 while the session runs
  "chamberFanSpeed": 100,     // % for M106 P3
  "maxMinutes": 30,           // hard stop
  "gentleFromFilament": true, // honour the material's gentle post-print rule
  "ownFan": "thermostat"      // thermostat | max | curve
}
```

Validation: `target` 15–60, speeds 0–100, `maxMinutes` 1–240, `ownFan` an enum.

→ [Configuration reference](../reference/configuration.md#cooldown)

## The session state machine

`cooldown.h` / `cooldown.cpp`, on the loop task.

### Start

- On the **phase edge** into `finished` (or `cooling`) when `enabled`, **or**
- on `POST /api/cooldown {"start":true}` / MQTT `cooldown/set` `ON`, at any time the printer is not
  printing — phase ∉ {`preheat`, `printing`, `paused`}.

It records `startedMs`, `startChamber`, the effective target and the material.

### Run — every 5 s

- If `usePrinterFans` **and** `gcode_state ∈ {FINISH, IDLE}` **and** the gentle rule allows,
  send or re-assert `M106 P2/P3`. **Re-asserted every 30 s**, because the printer may reset its fans
  on its own; also sent immediately whenever the requested value changes.
- The device's own fan follows `ownFan`:

    | `ownFan` | Behaviour |
    |---|---|
    | `thermostat` | PI control towards the target, reusing the [thermostat](chamber-thermostat.md) code with the set point = target |
    | `max` | 100 % |
    | `curve` | The normal curve |

### Stop

| Reason | Condition |
|---|---|
| `target` | `chamber ≤ target`, held for **two consecutive samples** |
| `timeout` | `elapsed ≥ maxMinutes` |
| `newJob` | Phase ∈ {`preheat`, `printing`, `paused`}, or the printer leaves FINISH/IDLE |
| `linkLost` | The printer link has been down for more than 30 s |
| `stopped` | `POST /api/cooldown {"start":false}` or MQTT `OFF` |
| `disabled` | The feature was switched off mid-session |

On stop, `M106 P2 S0\nM106 P3 S0` is sent **once** — and only if the device ever turned the fans on
in the first place.

### Safety

!!! danger "The print owns the fans"
    G-code is **never** sent unless `gcode_state ∈ {FINISH, IDLE}` **at the moment of sending**. If
    the printer reports RUNNING, PAUSE or PREPARE, the session ends immediately **without sending a
    stop command** — the running print's own fan settings must not be overwritten on the way out.

## Gentle cool-down

When `gentleFromFilament` is on and the loaded (or [last remembered](filament-matching.md#remembering-the-last-material))
material's `postPrintCooling` is `gentle`, the printer fans stay off until the chamber is **10 °C
below the chamber target**, and then run at **half** the configured percentages.

Blasting cold air at a hot ABS part is how it splits.

## Status, API and MQTT

```json
"cooldown": { "active": true, "reason": null, "target": 35, "chamber": 41.2,
              "startChamber": 52, "elapsedSec": 120, "maxSec": 1800,
              "printerFans": { "aux": 100, "chamber": 100, "sent": true },
              "ownFan": "thermostat", "material": "abs" }
```

| Endpoint / topic | Payload |
|---|---|
| `POST /api/cooldown` | `{"start":true｜false}` → `{"ok":true,"cooldown":{…}}`; `400 {"error":"printer is busy"}` while a print runs |
| `<base>/cooldown/set` | `ON` / `OFF` |

Home Assistant gets `switch.cooldown` (state from `cooldown.active`) plus
`sensor.cooldown_remaining` (minutes) and `sensor.cooldown_reason`.

## Testing

The decision logic lives in a pure `cooldown_logic.h` with its own native suite: start and stop
conditions, gentle gating, re-assert timing and every reason code. The mock server runs sessions
against its chamber model and logs the `M106` commands it would have sent.

---

Related: [Post-print cool-down (user guide)](../using/post-print-cooldown.md) ·
[Chamber thermostat](chamber-thermostat.md) · [Printer link](printer-link.md)
