# Post-print cool-down

!!! info "New in 2.0.3"
    This feature is new. If your device is on 2.0.2, the *Post-print cool-down* card is not there
    yet — [update the firmware](updating.md) first.

When a print finishes, the chamber is still full of heat. Waiting for it to fall on its own can take
half an hour; opening the printer too early on an ABS part is how it cracks. A **cool-down session**
runs the fan deliberately until the chamber reaches a target you set, and then stops.

## What a session does

When a print ends, the device starts a session (if you have left it enabled) and:

- runs **its own fan** towards the target, and
- optionally asks the **printer** to run its auxiliary and chamber fans as well.

It stops as soon as the chamber reaches the target — or on a timeout, or when a new job starts, or
when you press **Stop**.

The dashboard shows a live progress line while a session runs — `Cooling 52 → 35 °C, 12 min, printer
fans on` — with a **Stop** button. When the printer is idle or finished and no session is running,
the same place offers **Cool down now**.

## Settings

The **Post-print cool-down** card sits on the *Fan curve* page.

| Setting | What it does | Default |
|---|---|---|
| **Enabled** | Start a session automatically when a print finishes. | on |
| **Target** | The chamber temperature to cool to, 15–60 °C. With [filament-aware cooling](filament-aware-cooling.md) on, the material's cool-down target wins (and your overrides apply on top). | 35 °C |
| **Own fan** | How the device's own fan behaves during the session: **Thermostat** (PI control towards the target), **Max** (100 %), or **Curve** (business as usual). | Thermostat |
| **Use the printer's fans** | Send commands to the printer to run its aux and chamber fans. **Off by default** — see the warning below. | off |
| **Aux speed** / **Chamber fan speed** | What to ask the printer's fans for, 0–100 %. | 100 % / 100 % |
| **Max minutes** | A hard stop, 1–240 minutes. | 30 |
| **Gentle from filament** | Honour the material's *gentle* post-print rule: the printer fans stay off until the chamber is 10 °C below the print target, and then run at half the configured speed. | on |

## Using the printer's fans

!!! danger "This is the only feature that sends commands to your printer"
    Everything else BLSmartFlow does is read-only: it listens to the printer's reports and drives its
    own fan. Switching **Use the printer's fans** on makes the device send G-code (`M106`) to the
    printer over the local MQTT link.

    It is **off by default** and you have to turn it on deliberately.

The safeguards, so you know what it will and will not do:

- Commands are only ever sent while the printer reports **FINISH** or **IDLE**. If it reports
  RUNNING, PAUSE or PREPARE at the moment of sending, the session ends immediately without sending
  anything — **the print owns the fans**.
- The requested speeds are **re-asserted every 30 seconds**, because the printer may reset its fans
  on its own.
- When the session stops, the fans are explicitly turned **back off** — but only if the device ever
  turned them on in the first place.

## Why it stops

The card and the API report a reason:

| Reason | Meaning |
|---|---|
| `target` | The chamber reached the target (confirmed over two consecutive samples). |
| `timeout` | *Max minutes* elapsed. |
| `newJob` | A print started — preheating, printing or paused. |
| `stopped` | You pressed **Stop**, or sent the command over the API or MQTT. |
| `linkLost` | The printer link was down for more than 30 seconds. |
| `disabled` | The feature was switched off while a session was running. |

## From Home Assistant

Cool-down appears as a **switch** (`switch.cooldown`) plus two sensors: minutes remaining and the
reason the last session ended. Turning the switch on starts a session, provided the printer is not
printing.
→ [Home Assistant](home-assistant.md)

## From a script

```sh
H=http://blsmartflow.local

curl -X POST -d '{"start":true}'  $H/api/cooldown     # start now
curl -X POST -d '{"start":false}' $H/api/cooldown     # stop
curl -s $H/api/status | jq .cooldown                  # watch it
```

A start request while a print is running is refused with `400 {"error":"printer is busy"}`.

---

Technical detail, including the state machine and the G-code path:
[Post-print cool-down](../technical/post-print-cooldown.md).
