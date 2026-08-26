# Configuration reference

The configuration is stored as JSON at **`/config.json`** on LittleFS, and read or written through
[`GET`/`POST /api/config`](../technical/rest-api.md#configuration).

- Written **atomically**: serialise to `/config.tmp`, then rename over `/config.json`.
- A file that fails to parse is moved to `/config.bad` and defaults are used.
- `CONFIG_VERSION` is **2**.

!!! note "`configValidate()` clamps, it does not reject"
    Every value is coerced into range rather than refused. The only things it discards outright are an
    access code of the wrong length and an unparsable BSSID. Numeric merges saturate into the target
    type **first** and then get the semantic clamp, so a hand-edited backup with `"pwmFreq": 1e9` ends
    at 40000 rather than wrapping to something absurd.

## Shape

```jsonc
{
  "version": 2,
  "wifi":    { "ssid": "", "password": "", "bssid": "", "lockBssid": false,
               "hostname": "blsmartflow" },
  "printer": { "ip": "", "accessCode": "", "serial": "", "model": "auto" },
  "fan": {
    "curve": [ {"temp":0,"speed":0}, {"temp":50,"speed":0}, {"temp":180,"speed":50},
               {"temp":245,"speed":80}, {"temp":350,"speed":100} ],
    "source": "nozzle", "mode": "auto", "manualSpeed": 50, "minSpeed": 0,
    "kickStart": true, "kickMs": 500, "hysteresis": 2.0, "rampRate": 0,
    "pwmFreq": 25000, "pwmInvert": false, "output1": true, "output2": true,
    "onlyWhilePrinting": false, "cooldownMin": 10,
    "staleSec": 120, "staleMode": "off", "staleSpeed": 0,
    "doorMode": "ignore", "doorSpeed": 0, "doorResumeSec": 5,
    "preheatMode": "off", "preheatSpeed": 0,
    "chamberTarget": 45, "cooldownTarget": 35,
    "kp": 8.0, "ki": 0.02, "thermostatPeriodSec": 5, "ambientTemp": 25
  },
  "filament": { "auto": true, "manualId": "",
                "ventFloor": { "optional": 0, "recommended": 0, "required": 10 },
                "overrides": [] },
  "cooldown": { "enabled": true, "target": 35, "usePrinterFans": false,
                "auxSpeed": 100, "chamberFanSpeed": 100, "maxMinutes": 30,
                "gentleFromFilament": true, "ownFan": "thermostat" },
  "thermal": { "k": [null,null,null,null,null,null,null,null,null,null], "samples": 0 },
  "mqtt": { "enabled": false, "host": "", "port": 1883, "user": "", "password": "",
            "baseTopic": "", "haDiscovery": true, "haPrefix": "homeassistant",
            "publishIntervalSec": 10 },
  "web":   { "authEnabled": false, "user": "admin", "password": "" },
  "debug": { "serial": true, "mqttDump": false },
  "ssdp":  { "enabled": true }
}
```

---

## `version`

| Key | Type | Default | Meaning |
|---|---|---|---|
| `version` | int | `2` | Always rewritten to `CONFIG_VERSION` by `configValidate()` |

## `wifi`

| Key | Type / buffer | Default | Meaning and validation |
|---|---|---|---|
| `wifi.ssid` | `char[33]` | `""` | Empty ⇒ the device starts the setup AP instead of connecting |
| `wifi.password` | `char[65]` | `""` | **Secret** (masked). Empty = open network |
| `wifi.bssid` | `char[18]` | `""` | Must parse as six hex octets; normalised to upper-case `AA:BB:…`, otherwise cleared |
| `wifi.lockBssid` | bool | `false` | Forced to `false` when `bssid` is empty. Dropped automatically after 3 failed connect cycles |
| `wifi.hostname` | `char[33]` | `blsmartflow` | Lower-cased; every character that is not `[a-z0-9-]` becomes `-`; empty ⇒ default. Used for DHCP and mDNS |

## `printer`

| Key | Type / buffer | Default | Meaning and validation |
|---|---|---|---|
| `printer.ip` | `char[64]` | `""` | Dotted quad or hostname |
| `printer.accessCode` | `char[9]` | `""` | **Secret** (masked). **Exactly 8 characters**, or empty. `configValidate()` clears any other length with a warning; `POST /api/config` rejects it with `400` instead |
| `printer.serial` | `char[17]` | `""` | Upper-cased in place |
| `printer.model` | `char[6]` | `auto` | `auto｜x1｜p1｜a1｜h2d`, case-insensitive; anything else ⇒ `auto`. Drives the `pushall` cadence |

## `fan`

### Curve and source

| Key | Type | Default | Range / validation |
|---|---|---|---|
| `fan.curve` | ≤ 16 points | 5-point default | Sorted ascending, duplicates collapsed (last wins), temps 0–400 °C, speeds 0–100 %. Fewer than 2 usable points ⇒ the default curve is restored |
| `fan.source` | `char[8]` | `nozzle` | `nozzle｜bed｜chamber｜max`, else `nozzle` |

### Mode

| Key | Type | Default | Range / validation |
|---|---|---|---|
| `fan.mode` | `char[8]` | `auto` | `auto｜chamber｜manual｜off`, else `auto`. **Persisted** |
| `fan.manualSpeed` | uint8 % | `50` | 0–100 |

### Output shaping

| Key | Type | Default | Range / validation |
|---|---|---|---|
| `fan.minSpeed` | uint8 % | `0` | 0–100. Outputs strictly below it are forced to 0 %; `0` disables the clamp |
| `fan.kickStart` | bool | `true` | Full-duty pulse when leaving standstill |
| `fan.kickMs` | uint16 ms | `500` | 0–5000 |
| `fan.hysteresis` | float °C | `2.0` | 0–50; NaN or negative ⇒ 0 |
| `fan.rampRate` | uint16 %/s | `0` | 0–1000; `0` = instant |

### Hardware

| Key | Type | Default | Range / validation |
|---|---|---|---|
| `fan.pwmFreq` | uint32 Hz | `25000` | 500–40000. A change re-attaches LEDC on both pins |
| `fan.pwmInvert` | bool | `false` | Inverts the byte written to the pin |
| `fan.output1` / `fan.output2` | bool | `true` | A disabled output is driven to 0 % (or `255` when inverted) |

### Gating and failsafe

| Key | Type | Default | Range / validation |
|---|---|---|---|
| `fan.onlyWhilePrinting` | bool | `false` | Gate the curve on `printing` = phase ∈ {preheat, printing, paused} |
| `fan.cooldownMin` | uint16 min | `10` | 0–1440 |
| `fan.staleSec` | uint16 s | `120` | 10–3600 |
| `fan.staleMode` | `char[6]` | `off` | `hold｜off｜fixed`, else `off` |
| `fan.staleSpeed` | uint8 % | `0` | 0–100, used by `staleMode: "fixed"` |

### Printer state rules

| Key | Type | Default | Range / validation |
|---|---|---|---|
| `fan.doorMode` | `char[8]` | `ignore` | `ignore｜off｜fixed`, else `ignore`. **Inert until `doorKnown`** |
| `fan.doorSpeed` | uint8 % | `0` | 0–100, used by `doorMode: "fixed"` |
| `fan.doorResumeSec` | uint16 s | `5` | 0–300. The door rule stays armed this long after the door closes (anti-flap) |
| `fan.preheatMode` | `char[8]` | `off` | `ignore｜off｜fixed`, else `off`. Applies while `phase == preheat` |
| `fan.preheatSpeed` | uint8 % | `0` | 0–100, used by `preheatMode: "fixed"` |

### Chamber thermostat

| Key | Type | Default | Range / validation |
|---|---|---|---|
| `fan.chamberTarget` | uint8 °C | `45` | 20–80. Set point while printing |
| `fan.cooldownTarget` | uint8 °C | `35` | 15–60. Set point after a print; **also** ends the `auto` cool-down window early |
| `fan.kp` | float %/°C | `8.0` | 0–50; NaN or negative ⇒ 0 |
| `fan.ki` | float %/°C·s | `0.02` | 0–1; NaN or negative ⇒ 0 |
| `fan.thermostatPeriodSec` | uint8 s | `5` | 1–60 |
| `fan.ambientTemp` | uint8 °C | `25` | 0–40. Assumed room temperature; used **only** by the cooling-rate estimate, never by the control loop |

## `filament`

| Key | Type | Default | Range / validation |
|---|---|---|---|
| `filament.auto` | bool | `true` | Let the loaded material set the chamber target, the cool-down style and the vent floor. `false` ⇒ the plain `fan.*` values are used and **no override is applied** |
| `filament.manualId` | `char[24]` | `""` | Guide id used when the printer reports no usable tray. An id that is not in the table is cleared by `configValidate()` |
| `filament.ventFloor.optional` | uint8 % | `0` | 0–100; `0` disables the floor |
| `filament.ventFloor.recommended` | uint8 % | `0` | 0–100 |
| `filament.ventFloor.required` | uint8 % | `10` | 0–100 |
| `filament.overrides` | ≤ 12 rules | `[]` | See below |

An override rule is
`{"id", "chamberTarget", "cooldownTarget", "ventFloor", "postPrintCooling"}`:

- `id` is a guide id or `"*"` (the catch-all). Rules with an empty `id` are dropped.
- Every other field is optional; `null` means "keep the guide's figure".
- The array is **replaced wholesale** by a `POST /api/config` — never merged element-wise.
- A material's own rule always beats `"*"`.

## `cooldown`

!!! info "New in 2.0.3"
    → [Post-print cool-down](../technical/post-print-cooldown.md)

| Key | Type | Default | Range / validation |
|---|---|---|---|
| `cooldown.enabled` | bool | `true` | Start a session automatically when a print finishes |
| `cooldown.target` | uint8 °C | `35` | 15–60. When `filament.auto` is on, the filament's effective cool-down target wins (and overrides apply on top) |
| `cooldown.usePrinterFans` | bool | `false` | **Opt-in.** Send `M106` to the printer — the only feature that commands the printer |
| `cooldown.auxSpeed` | uint8 % | `100` | 0–100 — `M106 P2` |
| `cooldown.chamberFanSpeed` | uint8 % | `100` | 0–100 — `M106 P3` |
| `cooldown.maxMinutes` | uint16 min | `30` | 1–240. Hard stop |
| `cooldown.gentleFromFilament` | bool | `true` | Materials whose `postPrintCooling` is `gentle`: the printer fans stay off until the chamber is 10 °C below the chamber target, then run at half the configured percentages |
| `cooldown.ownFan` | `char[12]` | `thermostat` | `thermostat｜max｜curve` |

## `thermal`

Learned, not configured — but it survives a backup/restore round trip.

| Key | Type | Default | Meaning |
|---|---|---|---|
| `thermal.k` | float[10] | 10 × `null` | Newtonian cooling constants in 1/min: five **closed-door** buckets (0/25/50/75/100 % fan) then five **open-door** ones. `null`/NaN = never measured; anything outside `(0, 5]` is reset to `null` |
| `thermal.samples` | uint32 | `0` | Number of windows blended in so far |

→ [Cooling-rate learning](../technical/cooling-rate-learning.md)

## `mqtt`

| Key | Type | Default | Range / validation |
|---|---|---|---|
| `mqtt.enabled` | bool | `false` | Forced to `false` when `host` is empty |
| `mqtt.host` | `char[64]` | `""` | Hostname or IP. Plain TCP, **no TLS** |
| `mqtt.port` | uint16 | `1883` | Port `0` ⇒ `1883` |
| `mqtt.user` | `char[33]` | `""` | Empty user ⇒ anonymous connect |
| `mqtt.password` | `char[65]` | `""` | **Secret** (masked) |
| `mqtt.baseTopic` | `char[64]` | `""` | Trailing `/` stripped. Empty ⇒ `blsmartflow/<chipid>` |
| `mqtt.haDiscovery` | bool | `true` | Toggling off publishes empty discovery payloads, removing the entities |
| `mqtt.haPrefix` | `char[32]` | `homeassistant` | Empty ⇒ default |
| `mqtt.publishIntervalSec` | uint16 s | `10` | 1–3600 |

## `web`

| Key | Type | Default | Range / validation |
|---|---|---|---|
| `web.authEnabled` | bool | `false` | Forced to `false` when `web.password` is empty, so auth cannot be armed with no way in |
| `web.user` | `char[33]` | `admin` | Empty ⇒ `admin` |
| `web.password` | `char[65]` | `""` | **Secret** (masked) |

## `debug`

| Key | Type | Default | Meaning |
|---|---|---|---|
| `debug.serial` | bool | `true` | Log lines to USB serial as well as to the ring buffer |
| `debug.mqttDump` | bool | `false` | Print every *filtered* printer report to the log. Very noisy |

## `ssdp`

| Key | Type | Default | Meaning |
|---|---|---|---|
| `ssdp.enabled` | bool | `true` | Only meaningful when built with `-DBLSF_SSDP` |

---

## Masked secrets

`wifi.password`, `printer.accessCode`, `mqtt.password` and `web.password` serialise as `"********"`
whenever `masked=true` — that is, everywhere except `GET /api/backup` and the on-disk file.

**On input, a value consisting only of `*` means "leave unchanged"** and is never length-checked. That
is what lets the UI round-trip a form without ever learning the stored secret.

## Merge and save semantics

- `configFromJson()` is a **deep merge**: only keys present in the document are touched, and each
  section reports whether it changed (`restartRequired`, `printerChanged`, `mqttChanged`,
  `fanChanged`, `ssdpChanged`). It ends by calling `configValidate()`.
- **Deferred save:** `configMarkDirty()` marks the config dirty and `configLoopSave()` writes it at
  most **every 10 seconds**. `POST /api/fan` and the MQTT fan/mode commands use this, because a slider
  can produce dozens of writes a second.
- Explicit config, curve, WiFi and restore saves are written **inline**.

## Legacy migration

A 1.x `/blledconfig.json` is imported once at boot and then deleted:

| Legacy key | New key |
|---|---|
| `ssid` | `wifi.ssid` |
| `appw` | `wifi.password` |
| `bssi` | `wifi.bssid` |
| `printerIp` | `printer.ip` |
| `accessCode` | `printer.accessCode` |
| `serialNumber` | `printer.serial` |
| `debuging` | `debug.serial` |
| `mqttdebug` | `debug.mqttDump` |
| `fanPoints[]` | `fan.curve` |

An unreadable legacy file is discarded rather than fatal.
