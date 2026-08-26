# The printer link

A TLS MQTT client to the printer's own broker, running in its own FreeRTOS task.

## Connection

| Property | Value |
|---|---|
| Transport | `WiFiClientSecure::setInsecure()` — the printer presents a self-signed certificate |
| Port | **8883** |
| User | `bblp` |
| Password | The 8-character LAN access code |
| Client id | `BLSF-<chipid>` |
| Subscribe | `device/<serial>/report` |
| Publish | `device/<serial>/request` |

| Timeout | Value |
|---|---|
| TCP connect | 10 s |
| TLS handshake | 15 s |
| `setKeepAlive` | 30 s |
| `setSocketTimeout` | 10 s |
| TX buffer | 2048 bytes |
| RX buffer | `AutoGrowBufferStream`, 64 KB cap |

The task is created with `xTaskCreatePinnedToCore(printerTask, "printer", 20480, …, 1, core 1)`.

!!! warning "The certificate is not verified"
    `setInsecure()`: the printer's certificate is self-signed and there is no CA to pin. The session
    is **encrypted but unauthenticated**. On a LAN you already trust this is an acceptable trade; it
    is not a substitute for network hygiene.

## Backoff

3 s, doubling to 60 s.

PubSubClient state **4** (bad credentials) or **5** (unauthorized) jumps **straight to 60 s**, because
retrying a rejected password cannot help and hammering the printer's broker is rude. That is why a
corrected access code can take a minute to take effect.

## `pushall`

On connect, after subscribing:

```json
{"pushing":{"sequence_id":"0","command":"pushall","version":1,"push_target":1}}
```

Cadence afterwards, by configured model:

| Model | `pushall` every |
|---|---|
| `p1`, `a1` | 5 minutes |
| `auto` | 10 minutes |
| `x1`, `h2d` | never — they push complete reports themselves |

P1 and A1 firmware sends **only changes**, so without a periodic full refresh a freshly connected
device would wait a long time for a complete picture.

## Reconfiguration

`printerLinkReconfigure()` tears the session down **only** when `ip`, `accessCode` or `serial`
changed. `staleSec` and `debug.mqttDump` are picked up in place, so toggling a debug switch does not
drop the link.

## Report filtering

`deserializeJson` runs with a filter that keeps only:

```text
print.{ command, nozzle_temper, nozzle_target_temper, bed_temper, bed_target_temper,
        chamber_temper, cooling_fan_speed, big_fan1_speed, big_fan2_speed,
        heatbreak_fan_speed, fan_gear, gcode_state, mc_percent, mc_remaining_time,
        layer_num, total_layer_num, subtask_name, stg_cur, print_error, wifi_signal,
        home_flag, lights_report, info.temp, sequence_id, result, reason, err_code }
print.device.{ extruder, bed, ctc, airduct }
print.ams.{ tray_now, ams[*].{ id, tray[*].{ id, tray_type, tray_sub_brands,
                                             tray_info_idx, tray_color } } }
print.vt_tray.{ … }
```

Messages whose `print.command` is one of `gcode_line`, `project_prepare`, `project_file`,
`clean_print_error`, `resume`, `get_accessories`, `prepare` or `extrusion_cali_get` are **ignored**
outright — parsing them would blank out good data with the sparse fields of an acknowledgement.

The one exception is the *result* of a `gcode_line`, which is read before the message is dropped —
see [Command acknowledgements](#command-acknowledgements) below. The four extra keys in the filter
(`sequence_id`, `result`, `reason`, `err_code`) exist for that; none of them appears in a
`push_status` report, so no state parsing is affected.

A message that survives filtering updates `lastUpdateMs`, which is what
[staleness](control-loop.md#effective-modes) is measured against.

## Field decoding

| Field | Rule |
|---|---|
| Temperatures | The classic floats (`nozzle_temper`, `bed_temper`, …) win when present |
| Packed `device.*` | `device.extruder.info[0].temp`, `device.bed.info.temp`, `device.ctc.info.temp`: **current = `v & 0xFFFF`, target = `v >> 16`**. Used wherever the classic key is missing |
| Chamber | **Current X1C firmware no longer sends `chamber_temper`.** The chamber comes from `device.ctc.info.temp`, with `print.info.temp` as a last-resort mirror. The packed block is therefore a fallback for *every* model, not just the H2D |
| Fan speeds | Decimal strings on a 0–15 gear scale → `(gear * 100 + 7) / 15` percent. `cooling_fan_speed` → part, `big_fan1_speed` → aux, `big_fan2_speed` → chamber, `heatbreak_fan_speed` → heatbreak |
| `fan_gear` | Parsed by the filter but **deliberately not used**: on a live X1C it read `0x6400` while `big_fan1_speed` was `"6"` (40 %) |
| `device.airduct.parts[]` | Each part's `state` is already a percentage; indices 0/1/2 map to part/aux/chamber (H2D) |
| `home_flag` | Arrives as a negative int32; read as `uint32_t`, **bit 23 = door open** |
| `stg_cur` | The full ha-bambulab stage table `0..77` in `stageName()` (`0 = printing`, `2 = heatbed_preheating`, `49 = heating_chamber`, `50 = heatbed_cooling`, …), plus `-1`/`255` = `idle` and `-2` = `offline`. Codes 36+ are H2D-era and best-effort |
| `device.ctc.info.temp` high word | The chamber **target**. `0` means "this printer has no chamber heater", so it becomes `NaN` (JSON `null`) rather than a target of 0 °C |
| Unknown values | Temperatures stay `NaN`, fan speeds `-1`, counters `-1`; the status document turns all of those into JSON `null` |

## Online vs connected

```text
printer.connected = the MQTT session is up
printer.online    = connected && data age < staleSec
```

The fan's stale rule uses **data age only**, never the socket state, so a brief reconnect does not
yank the fan to the failsafe while the last reading is still seconds old.

## The RX buffer

PubSubClient's fixed buffer is far too small for a Bambu `pushall`, which can run to tens of
kilobytes. `AutoGrowBufferStream` grows on demand with `size_t` lengths, a checked `realloc` and a
**64 KB cap**.

A report larger than the cap is **dropped with a warning**, not parsed in pieces.

## Sending G-code

The [post-print cool-down](post-print-cooldown.md) feature is the one thing that publishes commands
rather than only listening. It uses a small spinlock-guarded queue,
`printerLinkSendGcode(const char*)`, drained by the printer task — so no other task ever touches the
MQTT client. Publish failures are logged and retried on the next tick.

## Command acknowledgements

Every `gcode_line` request is acknowledged on `device/<serial>/report`. On a healthy printer:

```json
{"print":{"command":"gcode_line","result":"SUCCESS","reason":"SUCCESS",
          "err_code":0,"sequence_id":"5001"}}
```

With **Developer Mode off**, the printer signature-checks writes and answers instead:

```json
{"print":{"command":"gcode_line","result":"failed","reason":"mqtt message verify failed",
          "err_code":84033543,"param":"M106 P3 S255\n","sequence_id":"5002"}}
```

Reports keep arriving either way — only commands are checked — so this ack is the *only* evidence
that a command was refused.

Bambu Studio acknowledges its own `gcode_line` requests on the same topic, so an ack only counts as
ours when its `sequence_id` matches one we published. Our ids start at **5000** and increment, and
the link keeps a ring of the **last four** it sent. A matching ack is recorded in the printer
snapshot as `lastGcodeResult` / `lastGcodeReason` / `lastGcodeErr` / `lastGcodeMs`, surfaced as
`printer.lastCommandError`, and cleared by the next accepted command. Everything else is dropped as
before.

## Debugging

Turn on **Dump printer MQTT** in *System → Diagnostics* (`debug.mqttDump`) to print every *filtered*
report to the log. It is very noisy; use it while diagnosing a missing temperature, then turn it off.

---

Related: [Printer link troubleshooting](../troubleshooting/printer-link.md) ·
[Connecting the printer](../getting-started/connect-printer.md) ·
[Control loop](control-loop.md)
