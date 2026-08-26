# MQTT and Home Assistant

A second, plain-TCP `PubSubClient` running on the loop task, entirely separate from the
[printer link](printer-link.md).

| Property | Value |
|---|---|
| Base topic | `mqtt.baseTopic`, or `blsmartflow/<chipid>` when empty |
| Client id | `BLSF-<chipid>` |
| Transport | Plain TCP — **no TLS** |
| Reconnect | Every 10 s, backing off to 60 s |

The broker hostname is resolved **before** `connect()`, and re-resolved after 5 consecutive failures,
so a DNS timeout cannot stall the loop.

## Topics

| Topic | Direction | Payload |
|---|---|---|
| `<base>/availability` | pub, retained, **LWT** | `online` / `offline` |
| `<base>/state` | pub, retained | The full [status object](rest-api.md#status-object) |
| `<base>/fan/speed` | pub, retained | `0`–`100` (current output) |
| `<base>/fan/on_state` | pub, retained | `ON` when output > 0, else `OFF` |
| `<base>/mode` | pub, retained | `auto` / `chamber` / `manual` / `off` (the configured mode) |
| `<base>/fan/set` | sub | `0`–`100` → manual mode at that speed |
| `<base>/fan/on` | sub | `OFF` → mode `off`; anything else → manual (at least 1 %) |
| `<base>/mode/set` | sub | `auto` / `chamber` / `manual` / `off` |
| `<base>/curve/set` | sub | `{"points":[{"temp":…,"speed":…},…]}`, max 2048 bytes, saved inline |
| `<base>/chamber_target/set` | sub | `20`–`80` → `fan.chamberTarget` |
| `<base>/cooldown_target/set` | sub | `15`–`60` → `fan.cooldownTarget` |
| `<base>/target/set` | sub | `{"chamberTarget":45,"cooldownTarget":35}` — either key may be omitted |
| `<base>/cooldown/set` | sub | `ON` starts a [post-print cool-down](post-print-cooldown.md) session, `OFF` stops it |
| `<base>/restart` | sub | Any payload → restart |

!!! note "Why the set points come in two shapes"
    A Home Assistant `number` entity wants **one topic per value**; a script would rather send both
    at once. Both go through the deferred-save path, so an HA slider cannot wear the flash out.

`state`, `fan/speed`, `fan/on_state` and `mode` are republished every `publishIntervalSec`,
immediately whenever the fan output changes, and right after a command is applied. The client buffer
is grown automatically to fit the status document.

## Home Assistant discovery

Published **retained** on connect, and when `haDiscovery` is switched on. Switching it off publishes
**empty payloads** to the same topics, which removes the entities cleanly.

```text
<haPrefix>/<component>/blsmartflow_<chipid>/<object_id>/config
```

`unique_id` and `object_id` are both `blsmartflow_<chipid>_<object_id>`. Every entity carries
`availability_topic: <base>/availability` and this device block:

```json
{ "identifiers": ["blsmartflow_<chipid>"],
  "name": "BLSmartFlow <chipid>",
  "manufacturer": "DutchDeveloper",
  "model": "BLSmartFlow",
  "sw_version": "<fw>",
  "configuration_url": "http://<ip>/" }
```

### Entities

| Component | `object_id` | Notes |
|---|---|---|
| `fan` | `fan` | `command_topic fan/on`, `state_topic fan/on_state`, `percentage_command_topic fan/set`, `percentage_state_topic fan/speed`, speed range 1–100 |
| `select` | `mode` | Options `auto`, `chamber`, `manual`, `off` |
| `number` | `chamber_target` | 20–80 °C, `command_topic chamber_target/set`, state from `value_json.fan.chamberTarget` |
| `number` | `cooldown_target` | 15–60 °C, `command_topic cooldown_target/set`, state from `value_json.fan.cooldownTarget` |
| `switch` | `cooldown` | `command_topic cooldown/set` (`ON`/`OFF`), state from `value_json.cooldown.active` |
| `button` | `restart` | `payload_press: PRESS`, `device_class: restart` |
| `sensor` | `nozzle_temp`, `bed_temp`, `chamber_temp` | °C, `device_class temperature`, `state_class measurement` |
| `sensor` | `fan_output` | %, `state_class measurement` |
| `sensor` | `printer_state`, `printer_stage`, `phase` | Text. `phase` is the derived print phase the fan rules act on |
| `sensor` | `cooling_rate` | °C/min, `state_class measurement`; `unknown` while nothing is being measured |
| `sensor` | `cooldown_remaining` | min, `device_class duration` |
| `sensor` | `cooldown_reason` | *Cool-down result* — `target｜timeout｜newJob｜stopped｜linkLost｜disabled`, or `none` before the first session ends |
| `sensor` | `print_progress` | %, `state_class measurement` |
| `sensor` | `remaining_time` | min, `device_class duration` |
| `sensor` | `printer_wifi` | The printer's own reported RSSI string |
| `sensor` | `device_rssi` | dBm, `device_class signal_strength` |
| `sensor` | `uptime` | s, `device_class duration`, `state_class total_increasing` |
| `sensor` | `filament` | State = the guide's display name (`unknown` when unmatched). Attributes via `json_attributes_topic` + `json_attributes_template`: `type`, `idx`, `id`, `family`, `source`, `vent`, `chamberTarget`, `ventFloor`, `postPrintCooling` |
| `sensor` | `filament_chamber_target` | °C, `device_class temperature`; the **effective** chamber target for the loaded material |
| `binary_sensor` | `printer_online` | `device_class connectivity` |
| `binary_sensor` | `door` | `device_class opening`. Publishes the literal `None` while `doorKnown` is false, which HA renders as *Unknown* — **a stuck bit must not be reported as a shut door** |
| `binary_sensor` | `printing` | `device_class running` |

!!! note "Why the filament attributes are an explicit dictionary"
    The template builds a fixed set of keys rather than dumping the whole `filament` block. An
    attribute set that changes shape between updates is what makes a Home Assistant history graph
    unusable.

Every sensor reads its value out of the retained `state` document with a `value_template`. Fields
that can be `null` — the three temperatures, progress, remaining time, cooling rate — use

```jinja
{% set v = value_json.… %}{{ 'unknown' if v is none else v }}
```

so Home Assistant shows *Unknown* instead of logging a parse error on an empty string.

## Without Home Assistant

Set `mqtt.haDiscovery` to `false` and use the topic table directly:

```sh
mosquitto_sub -h broker -t 'blsmartflow/a1b2c3/state' -v
mosquitto_pub -h broker -t 'blsmartflow/a1b2c3/fan/set'  -m 45
mosquitto_pub -h broker -t 'blsmartflow/a1b2c3/mode/set' -m auto
mosquitto_pub -h broker -t 'blsmartflow/a1b2c3/curve/set' \
  -m '{"points":[{"temp":0,"speed":0},{"temp":250,"speed":100}]}'
```

Fan and mode commands persist through the deferred-save path (at most one flash write per 10 s);
`curve/set` is saved inline.

---

Related: [Home Assistant in five minutes](../using/home-assistant.md) ·
[Status object](rest-api.md#status-object)
