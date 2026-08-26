# BLSmartFlow 2.0 — Technical reference

For developers and integrators. It documents what the firmware in this repository actually does:
module layout, threading, the configuration schema, the fan control law, the printer link, and the
full HTTP / MQTT / serial contracts.

* The design record — why 2.0 looks like this — is [REWORK-SPEC.md](REWORK-SPEC.md).
* The review of the 1.x firmware that motivated the rework is [CODE-REVIEW.md](CODE-REVIEW.md).
* End-user instructions are in [USER-GUIDE.md](USER-GUIDE.md).

---

## Contents

1. [Architecture overview](#architecture-overview)
2. [Boot and main-loop sequence](#boot-and-main-loop-sequence)
3. [Configuration schema](#configuration-schema)
4. [Fan control state machine](#fan-control-state-machine)
5. [Printer link](#printer-link)
6. [REST API reference](#rest-api-reference)
7. [MQTT / Home Assistant reference](#mqtt--home-assistant-reference)
8. [Serial provisioning protocol](#serial-provisioning-protocol)
9. [WiFi, AP and captive portal](#wifi-ap-and-captive-portal)
10. [Security notes](#security-notes)
11. [Building and testing](#building-and-testing)
12. [Partitions and OTA](#partitions-and-ota)
13. [Hardware](#hardware)
14. [Known limitations](#known-limitations)

---

## Architecture overview

### Modules

| Path | Responsibility |
|---|---|
| `src/main.cpp` | Boot order, the non-blocking loop, deferred restart / factory reset (`app.h`) |
| `src/blflow/version.h` | `FW_VERSION` (from `custom_version` via `-DSTRVERSION`), `FW_BUILD`, `FW_NAME` |
| `src/blflow/log.h/.cpp` | `LOGI/LOGW/LOGE` → Serial (gated on `debug.serial`) plus a 64-line × 120-byte ring buffer, spinlock-guarded, with a monotonic sequence counter for SSE |
| `src/blflow/config.h/.cpp` | The `Config` POD, defaults, `configValidate()`, LittleFS persistence, legacy migration, masked JSON, deep merge |
| `src/blflow/curve.h` | Header-only, Arduino-free curve model: `curveValidate()`, `curveInterpolate()`, `curveDefaults()` |
| `src/blflow/state.h/.cpp` | `PrinterState` / `FanState` snapshots under `portMUX` spinlocks; the PubSubClient state table |
| `src/blflow/printer_parse.h` | Header-only, Arduino-free Bambu report parser (filter + field extraction), the full `stg_cur` name table and the pure `reportPhase()` |
| `src/blflow/thermostat.h` | Header-only, Arduino-free PI step for the chamber thermostat (anti-windup + freeze flags) |
| `src/blflow/thermal_math.h` | Header-only, Arduino-free cooling-window arithmetic (buckets, EMA, Newton fit) |
| `src/blflow/thermal.h/.cpp` | Passive cooling-rate learning: sampling, persistence, status block |
| `src/blflow/printer_link.h/.cpp` | TLS MQTT client to the printer, in its own FreeRTOS task |
| `src/blflow/ha_mqtt.h/.cpp` | External broker client, Home Assistant discovery, command topics |
| `src/blflow/fan_control.h/.cpp` | LEDC setup and the control state machine |
| `src/blflow/web_server.h/.cpp` | ESPAsyncWebServer: UI, REST, SSE, OTA, legacy routes, captive portal, basic auth |
| `src/blflow/status.h/.cpp` | `buildStatus()` — the single status document used by REST, SSE and MQTT |
| `src/blflow/wifi_manager.h/.cpp` | STA state machine, AP + DNS captive portal, async scan, mDNS |
| `src/blflow/indicator.h/.cpp` | Non-blocking LED patterns |
| `src/blflow/serial_provision.h/.cpp` | Line-delimited JSON provisioning over USB |
| `src/blflow/ssdp.h/.cpp` | Optional UPnP/SSDP advertisement (`-DBLSF_SSDP`) |
| `src/blflow/AutoGrowBufferStream.h` | Growing RX buffer for PubSubClient (`size_t` lengths, 64 KB cap, checked `realloc`) |
| `src/www/index.html` | The entire UI: one file, inline CSS/JS, no external requests |

### Tasks and threads

| Task | Core | What runs there |
|---|---|---|
| Arduino `loop` task | 1 (Arduino default) | `fanControlLoop`, `indicatorLoop`, `wifiLoop`, `serialProvisionLoop`, `webServerLoop`, `haMqttLoop`, `ssdpLoop`, `configLoopSave`, deferred restart |
| `printer` task | pinned to core 1, 20 KB stack, priority 1 | TLS handshake, `PubSubClient::loop()`, report parsing, `pushall` |
| AsyncTCP task | library-managed, 8 KB stack (`-DCONFIG_ASYNC_TCP_STACK_SIZE=8192`) | Every HTTP handler, SSE fan-out, OTA chunk writes |

The printer's TLS session is the only thing that can block for seconds, which is why it lives in its
own task: an unreachable printer cannot stall fan control or the web UI.

### Locking model

Two different mechanisms, chosen by what they protect:

* **`Config` — recursive mutex** (`configLock()` / `configUnlock()`, RAII `ConfigGuard`). `cfg()` is
  written from the loop task, the AsyncTCP task, the serial reader and the external-MQTT callback.
  Recursive because handlers naturally do "take lock → merge → `configSave()`", and `configSave()`
  takes it again.
* **`PrinterState` / `FanState` — `portMUX` spinlocks with whole-struct copies.** Readers call
  `printerSnapshot()` / `fanSnapshot()` and get a private copy taken inside a very short critical
  section. This is only sound because both structs are POD (fixed char arrays, no `String`, no
  `std::vector`).
* **`printer_link`'s `LinkCfg`** is its own POD guarded by a second spinlock, so the printer task
  never reads the live `Config` while a web handler is mutating it.
* Long operations are never done under a lock: modules copy what they need
  (`FanConfig fc = cfg().fan;`) and work off the copy.

Cross-task requests that must not run on the calling task are **flags picked up by the loop task**:
`fanControlReconfigure()`, `haMqttReconfigure()`, `ssdpReconfigure()`, `appRequestRestart()`,
`appRequestFactoryReset()`.

---

## Boot and main-loop sequence

`setup()`:

1. `Serial.begin(115200)`, `logInit()`
2. `stateInit()` — every field to its "nothing known yet" sentinel
3. `configLoad()` — mount LittleFS (format on first failure), migrate `/blledconfig.json`, read
   `/config.json`, validate
4. `logSetSerialEnabled(cfg().debug.serial)`
5. `indicatorSetup()`, `fanControlSetup()` (LEDC attach + write 0), `serialProvisionSetup()`
6. **One `fanControlLoop()` before the network comes up** — a hot printer should not wait for DHCP
7. `wifiSetup()` → `webServerSetup()` → `printerLinkStart()` → `haMqttSetup()`

`loop()`, in order, all non-blocking:

```
fanControlLoop()      // first, so its timing is never pushed around
indicatorLoop()
wifiLoop()            // DNS pump when the AP is up, STA state machine, scan reaping
serialProvisionLoop() // bounded to 256 bytes per pass
webServerLoop()       // 1 Hz SSE status push + new log lines (max 8 per pass)
haMqttLoop()
ssdpLoop()
configLoopSave()      // writes a dirty config at most every 10 s
<start SSDP / rebuild HA configuration_url on the first STA connect>
<execute a pending restart / factory reset>
delay(1)              // feed the idle task
```

---

## Configuration schema

Stored as JSON at **`/config.json`** on LittleFS. Written atomically: serialise to `/config.tmp`,
then rename over `/config.json`. A file that fails to parse is moved to `/config.bad` and defaults
are used. `CONFIG_VERSION` is `2`.

```jsonc
{
  "version": 2,
  "wifi":    { "ssid": "", "password": "", "bssid": "", "lockBssid": false, "hostname": "blsmartflow" },
  "printer": { "ip": "", "accessCode": "", "serial": "", "model": "auto" },
  "fan": {
    "curve": [ {"temp":0,"speed":0}, {"temp":50,"speed":0}, {"temp":180,"speed":50},
               {"temp":245,"speed":80}, {"temp":350,"speed":100} ],
    "source": "nozzle", "mode": "auto", "manualSpeed": 50, "minSpeed": 0,
    "kickStart": true, "kickMs": 500, "hysteresis": 2.0, "rampRate": 0,
    "pwmFreq": 25000, "pwmInvert": false, "output1": true, "output2": true,
    "onlyWhilePrinting": false, "cooldownMin": 10,
    "staleSec": 120, "staleMode": "off", "staleSpeed": 0
  },
  "mqtt": { "enabled": false, "host": "", "port": 1883, "user": "", "password": "",
            "baseTopic": "", "haDiscovery": true, "haPrefix": "homeassistant",
            "publishIntervalSec": 10 },
  "web":   { "authEnabled": false, "user": "admin", "password": "" },
  "debug": { "serial": true, "mqttDump": false },
  "ssdp":  { "enabled": true }
}
```

### Keys

| Key | Type / buffer | Default | Meaning and validation |
|---|---|---|---|
| `version` | int | `2` | Always rewritten to `CONFIG_VERSION` by `configValidate()` |
| `wifi.ssid` | `char[33]` | `""` | Empty ⇒ the device starts the setup AP instead of connecting |
| `wifi.password` | `char[65]` | `""` | Secret (masked). Empty = open network |
| `wifi.bssid` | `char[18]` | `""` | Must parse as six hex octets; normalised to upper-case `AA:BB:…`, otherwise cleared |
| `wifi.lockBssid` | bool | `false` | Forced to `false` when `bssid` is empty |
| `wifi.hostname` | `char[33]` | `blsmartflow` | Lower-cased; every character that is not `[a-z0-9-]` becomes `-`; empty ⇒ default. Used for DHCP and mDNS |
| `printer.ip` | `char[64]` | `""` | Dotted quad or hostname |
| `printer.accessCode` | `char[9]` | `""` | **Exactly 8 characters**, or empty. `configValidate()` clears any other length (with a warning); `POST /api/config` rejects it with 400 instead |
| `printer.serial` | `char[17]` | `""` | Upper-cased in place |
| `printer.model` | `char[6]` | `auto` | `auto｜x1｜p1｜a1｜h2d`, case-insensitive, anything else ⇒ `auto`. Drives the `pushall` cadence |
| `fan.curve` | ≤ 16 points | 5-point default | Sorted ascending, duplicates collapsed (last wins), temps 0–400 °C, speeds 0–100 %. Fewer than 2 usable points ⇒ the default curve is restored |
| `fan.source` | `char[8]` | `nozzle` | `nozzle｜bed｜chamber｜max`, else `nozzle` |
| `fan.mode` | `char[8]` | `auto` | `auto｜chamber｜manual｜off`, else `auto`. Persisted. `chamber` = PI thermostat on the chamber temperature |
| `fan.manualSpeed` | uint8 % | `50` | 0–100 |
| `fan.minSpeed` | uint8 % | `0` | 0–100. Outputs strictly below it are forced to 0 %; `0` disables the clamp |
| `fan.kickStart` | bool | `true` | Full-duty pulse when leaving standstill |
| `fan.kickMs` | uint16 ms | `500` | 0–5000 |
| `fan.hysteresis` | float °C | `2.0` | 0–50; NaN or negative ⇒ 0 |
| `fan.rampRate` | uint16 %/s | `0` | 0–1000; `0` = instant |
| `fan.pwmFreq` | uint32 Hz | `25000` | 500–40000. A change re-attaches LEDC |
| `fan.pwmInvert` | bool | `false` | Inverts the byte written to the pin |
| `fan.output1` / `output2` | bool | `true` | A disabled output is driven to 0 % (or `255` when inverted) |
| `fan.onlyWhilePrinting` | bool | `false` | Gate the curve on `printing` |
| `fan.cooldownMin` | uint16 min | `10` | 0–1440 |
| `fan.staleSec` | uint16 s | `120` | 10–3600 |
| `fan.staleMode` | `char[6]` | `off` | `hold｜off｜fixed`, else `off` |
| `fan.staleSpeed` | uint8 % | `0` | 0–100, used by `staleMode: "fixed"` |
| `fan.doorMode` | `char[8]` | `ignore` | `ignore｜off｜fixed`, else `ignore`. What the fan does while the front door is open. Inert until `doorKnown` |
| `fan.doorSpeed` | uint8 % | `0` | 0–100, used by `doorMode: "fixed"` |
| `fan.doorResumeSec` | uint16 s | `5` | 0–300. The door rule stays armed this long after the door closes (anti-flap) |
| `fan.preheatMode` | `char[8]` | `off` | `ignore｜off｜fixed`, else `off`. What the fan does while `phase == preheat` |
| `fan.preheatSpeed` | uint8 % | `0` | 0–100, used by `preheatMode: "fixed"` |
| `fan.chamberTarget` | uint8 °C | `45` | 20–80. Thermostat set point while printing |
| `fan.cooldownTarget` | uint8 °C | `35` | 15–60. Thermostat set point after a print; **also** ends the `auto` cool-down window early |
| `fan.kp` | float %/°C | `8.0` | 0–50; NaN or negative ⇒ 0 |
| `fan.ki` | float %/°C·s | `0.02` | 0–1; NaN or negative ⇒ 0 |
| `fan.thermostatPeriodSec` | uint8 s | `5` | 1–60 |
| `fan.ambientTemp` | uint8 °C | `25` | 0–40. Assumed room temperature; used **only** by the cooling-rate estimate, never by the control loop |
| `thermal.k` | float[10] | 10 × `null` | Learned Newtonian cooling constants in 1/min: five closed-door buckets (0/25/50/75/100 % fan) then five open-door ones. `null`/NaN = never measured; anything outside `(0, 5]` is reset to `null` |
| `thermal.samples` | uint32 | `0` | Number of windows blended in so far. Learned, not configured — but it survives a backup/restore round trip |
| `mqtt.enabled` | bool | `false` | Forced to `false` when `host` is empty |
| `mqtt.host` / `port` | `char[64]` / uint16 | `""` / `1883` | Port `0` ⇒ `1883`. Plain TCP, no TLS |
| `mqtt.user` / `password` | `char[33]` / `char[65]` | `""` | Password is a secret (masked). Empty user ⇒ anonymous connect |
| `mqtt.baseTopic` | `char[64]` | `""` | Trailing `/` stripped. Empty ⇒ `blsmartflow/<chipid>` |
| `mqtt.haDiscovery` | bool | `true` | Toggling off publishes empty discovery payloads (removes the entities) |
| `mqtt.haPrefix` | `char[32]` | `homeassistant` | Empty ⇒ default |
| `mqtt.publishIntervalSec` | uint16 s | `10` | 1–3600 |
| `web.authEnabled` | bool | `false` | Forced to `false` when `web.password` is empty |
| `web.user` | `char[33]` | `admin` | Empty ⇒ `admin` |
| `web.password` | `char[65]` | `""` | Secret (masked) |
| `debug.serial` | bool | `true` | Log lines to USB serial as well as to the ring buffer |
| `debug.mqttDump` | bool | `false` | Print every *filtered* printer report to the log |
| `ssdp.enabled` | bool | `true` | Only meaningful when built with `-DBLSF_SSDP` |

### Masked secrets

`wifi.password`, `printer.accessCode`, `mqtt.password` and `web.password` are serialised as
`"********"` whenever `masked=true` (that is, everywhere except `GET /api/backup` and the on-disk
file). On input, **a value consisting only of `*` means "leave unchanged"** and is never
length-checked. This lets the UI round-trip a form without ever learning the stored secret.

### Merge, validation and saving

* `configFromJson()` is a **deep merge**: only keys present in the document are touched, and each
  section reports whether it changed (`restartRequired`, `printerChanged`, `mqttChanged`,
  `fanChanged`, `ssdpChanged`). It ends by calling `configValidate()`.
* `configValidate()` **clamps, never rejects**. The only value it discards outright is an access code
  of the wrong length and an unparsable BSSID.
* Numeric merges saturate into the target type first, then get the semantic clamp, so a hand-edited
  backup with `"pwmFreq": 1e9` ends at 40000 rather than wrapping.
* **Deferred save**: `configMarkDirty()` marks the config dirty and `configLoopSave()` writes it at
  most every 10 s. `POST /api/fan` and the MQTT fan/mode commands use this, because a slider can
  produce dozens of writes a second. Explicit config, curve, WiFi and restore saves are written
  inline.

### Legacy migration

A 1.x `/blledconfig.json` is imported once at boot and then deleted:

| Legacy key | New key |
|---|---|
| `ssid` | `wifi.ssid` |
| `appw` | `wifi.password` |
| `bssi` | `wifi.bssid` |
| `printerIp` | `printer.ip` |
| `accessCode` | `printer.accessCode` |
| `serialNumber` | `printer.serial` |
| `debuging` / `mqttdebug` | `debug.serial` / `debug.mqttDump` |
| `fanPoints[]` | `fan.curve` |

An unreadable legacy file is discarded rather than fatal.

---

## Fan control state machine

`fanControlLoop()` runs every loop pass but recomputes at most every **100 ms** (or immediately when
something raised the "recompute now" flag). It works off a `FanConfig` copy taken under the config
lock and a `PrinterState` snapshot.

### Print phase

The fan logic reasons about a derived **phase**, not about `gcode_state`, because `RUNNING` alone
cannot tell a chamber that is still heating from one that is at temperature.
`reportPhase(const PrinterReport&)` lives in `printer_parse.h` (Arduino-free, host-tested); `state.h`
wraps it as `printerPhase(const PrinterState&)`, which additionally reports `offline` when no report
has ever arrived. First rule that matches wins:

| Phase | Rule |
|---|---|
| `offline` | `gcode_state` empty or `OFFLINE`, or `stg_cur == -2`, or no report ever received |
| `paused` | `gcode_state == PAUSE`, or `stg_cur ∈ {5,6,16,17,20,21,23,26,27,28,30,32,33,34,35}` |
| `preheat` | `stg_cur ∈ {2,7,49,54,58,63,64}`, or `RUNNING` **and** (`bedTarget > 0 && bed < bedTarget − 3`) or (`chamberTarget > 0 && chamber < chamberTarget − 2`) |
| `cooling` | `stg_cur ∈ {29,50,69}` |
| `printing` | `gcode_state ∈ {RUNNING, PREPARE, SLICING}` |
| `finished` | `gcode_state == FINISH` |
| `failed` | `gcode_state == FAILED` |
| `idle` | otherwise |

**`printing` = phase ∈ {`preheat`, `printing`, `paused`}**, and that is what `onlyWhilePrinting`,
`printer.printing` and the HA `printing` binary sensor all use. `stageName()` (also in
`printer_parse.h`) covers the whole ha-bambulab table `0..77` plus `-1`/`255` → `idle` and `-2` →
`offline`, in the snake_case spelling `printer.stageText` has always published.

### Door

`home_flag` bit 23 is the **front-door plunger switch**. The top lid has **no sensor at all**, so
lifting it changes nothing.

On some X1C units the closed door does not actuate that switch, so the bit sits at `1` from boot to
power-off (pressing the switch by hand flips it). A raw bit is therefore not evidence of anything
until it has been seen to *change*:

| Field | Meaning |
|---|---|
| `doorOpen` | The raw bit — recorded, but meaningless on its own |
| `doorRawSeen` | A report has carried `home_flag` at least once |
| `doorKnown` | **An edge has been observed**, i.e. this printer's switch really reports |
| `doorEdgeCount`, `lastDoorOpenMs`, `lastDoorCloseMs` | Edge bookkeeping |

`reportDoorOpen()` (in `printer_parse.h`, wrapped as `printerDoorOpen()` in `state.h`) is
`doorKnown && doorOpen` and is the **only** door reading the control loop, the thermostat freeze and
the cooling-rate learner ever use — a printer with a stuck bit therefore behaves exactly as it did
before the feature existed. **The first report that carries `home_flag` establishes the raw state, is
not an edge and does not set `doorKnown`**, or every MQTT reconnect would look like someone opening
the printer.

`printer.doorOpen` is serialised as `null` while `doorKnown` is false, and `printer.doorKnown` says
which case you are in.

### Effective modes

`printing = phase ∈ {preheat, printing, paused}` (see above)
`stale = age of the newest accepted report ≥ staleSec` (never received counts as infinitely old)
`recentPrint = !printing && a print has ended && now − printEnd < cooldownMin`
`cooling = onlyWhilePrinting && recentPrint && chamber > cooldownTarget`

| Order | `effectiveMode` | Condition | Target |
|---|---|---|---|
| 1 | `off` | `fan.mode == "off"` | 0 % |
| 2 | `manual` | `fan.mode == "manual"` | `manualSpeed` |
| 3 | `stale` | not off/manual and `stale` | `hold` → the current ramp value; `off` → 0 %; `fixed` → `staleSpeed` |
| 4 | `door` | `doorMode != ignore`, **`doorKnown`**, the door is open (or closed less than `doorResumeSec` ago), and phase ∉ {`finished`, `cooling`, `idle`} | `off` → 0 %; `fixed` → `doorSpeed` |
| 5 | `preheat` | `preheatMode != ignore` and phase == `preheat` | `off` → 0 %; `fixed` → `preheatSpeed` |
| 6 | `idle` | `mode == chamber`, chamber known, and the phase has no set point | 0 % |
| 7 | `cooldown` | `mode == chamber`, phase ∈ {`finished`, `cooling`} or (`idle` and `recentPrint`) | thermostat towards `cooldownTarget` |
| 8 | `chamber` | `mode == chamber`, phase ∈ {`preheat`, `printing`, `paused`} | thermostat towards `chamberTarget` |
| 9 | `idle` | `onlyWhilePrinting`, not printing, cool-down finished | 0 % |
| 10 | `cooldown` | `onlyWhilePrinting`, not printing, cool-down running | curve |
| 11 | `auto` | otherwise (including `mode == chamber` with an unknown chamber temperature) | curve |

Note that staleness is judged **by data age only**, not by the MQTT socket state, so a brief
reconnect does not yank the fan to the failsafe while the last reading is seconds old.

During a cool-down an open door is *helping*, which is why rule 4 skips the `finished`, `cooling` and
`idle` phases. And in `auto` mode the cool-down window now ends at `cooldownTarget` **or**
`cooldownMin`, whichever comes first, so the fan does not run out a ten-minute timer on a chamber
that is already cold.

### Chamber thermostat

`thermostatStep()` in `thermostat.h` is a pure, Arduino-free PI step (host-tested in
`test/test_thermostat`):

```
e   = chamber − setpoint                       // positive = too hot, fan should run
out = clamp(kp·e + ki·∫e, 0, 100)
```

Two guards keep the integral from running away, which for an exhaust fan is the difference between
"settles at 45 °C" and "sits at 100 % for ten minutes after the door was shut":

* a hard clamp at ±`100/ki`, so `ki·∫e` alone can never demand more than full scale either way;
* **conditional integration** — integration is frozen while the door is open (the error is real but
  the fan cannot fix it) and whenever a step would push an already saturated output further into its
  rail. Steps that bring a saturated output back into range are always accepted, so the integral can
  always unwind.

The controller steps once every `thermostatPeriodSec`; between steps the last output is held. Any
change of set point (print → cool-down) resets the integral, and so does leaving the thermostat for
any other effective mode, so returning from a door event never resumes with a stale integral.

### Cooling-rate learning

`thermal.h/.cpp` samples the chamber every 5 s and never touches the fan. A **window** is a run of
≥ 60 s in which the fan output stayed within ±5 %, the door state did not change, and no heater was
active (`bedTarget == 0 && nozzleTarget == 0`). For each usable window it fits Newton's law of
cooling

```
dT/dt = −k · (T − ambientTemp)        →        k = −(dT/dt) / (T − ambientTemp)      [1/min]
```

and blends `k` into `thermal.k[bucket][door]` with an EMA (α = 0.3), bucketing by nearest fan output
(0/25/50/75/100 %) and by door state. A window is refused when the chamber moved less than 0.5 °C,
when it sits less than 3 °C above ambient (dividing by `T − ambient` would amplify sensor noise into
nonsense), or when the chamber was warming rather than cooling. Windows are harvested as soon as they
are long enough and then restarted, so a long cool-down contributes a run of samples rather than one
average. The learned table is persisted through the existing dirty/loop-save mechanism at most once
every **10 minutes**. The arithmetic lives in `thermal_math.h` and is host-tested in
`test/test_thermal`.

### Order of operations

```
sourceTemp = select(fan.source, printer temps)        // NaN when unavailable
             nozzle | bed | chamber | max(nozzle,bed,chamber ignoring NaN)

0. rules       off / manual / stale / door / preheat / thermostat, in the order above; the
               steps below apply to whatever target that produced
1. curve       target = curveInterpolate(curve, sourceTemp)      (auto/cooldown only)
2. hysteresis  the curve is only re-evaluated once |sourceTemp − heldTemp| >= hysteresis;
               a NaN source resets the anchor and yields 0 %
3. clamp       target = clamp(target, 0, 100)
4. ramp        rampRate == 0 ? slew = target
                             : slew moves towards target by rampRate * dt
5. minSpeed    output = (minSpeed > 0 && 0 < slew < minSpeed) ? 0 : slew
6. kick        if kickStart && kickMs > 0 && the output leaves 0 % after >= 2 s at 0 %,
               drive 100 % until kickMs has elapsed (or the output returns to 0 %)
7. duty        duty = round(driven * 255 / 100)
8. invert      pin value = pwmInvert ? 255 − duty : duty; a disabled output gets
               0 (or 255 when inverted)
```

The ramp accumulator is kept **separate from the published output**: if the min-speed clamp fed back
into the slew, a fan with `minSpeed` set could never climb away from 0 %.

`FanState` publishes `output`, `target`, `effectiveMode`, `sourceTemp`, `setpoint`, `pwmDuty`,
`manualExpiresAt` and `kicking`. `setpoint` is the thermostat set point in force this instant and is
NaN (JSON `null`) in every other mode. **`pwmDuty` is post-inversion** — it is the byte the pin actually
sees, so with `pwmInvert` an output of 0 % reports 255.

### Manual overrides

`fanApplyMode(mode, speed, durationSec, persist)` is shared by `POST /api/fan` and the MQTT command
topics; it accepts `auto`, `chamber`, `manual` and `off`. `durationSec` is clamped to 86400. A **timed** override sets a deadline and is *not*
persisted (so a reboot ends it); a duration of 0 persists mode and speed through the deferred-save
path. When the deadline passes, the control loop logs it, clears the deadline and writes
`fan.mode = "auto"` back into the config.

---

## Printer link

* `WiFiClientSecure::setInsecure()` (the printer presents a self-signed certificate), port **8883**,
  user **`bblp`**, password = the access code, client id `BLSF-<chipid>`.
* Timeouts: TCP connect 10 s, TLS handshake 15 s, `setKeepAlive(30)`, `setSocketTimeout(10)`,
  `setBufferSize(2048)` for TX. RX goes through `AutoGrowBufferStream` (64 KB cap).
* Runs in `xTaskCreatePinnedToCore(printerTask, "printer", 20480, …, 1, core 1)`.
* **Backoff**: 3 s doubling to 60 s. PubSubClient state `4` (bad credentials) or `5` (unauthorized)
  jumps straight to 60 s, because retrying a rejected password cannot help.
* On connect: subscribe `device/<serial>/report`, publish
  `{"pushing":{"sequence_id":"0","command":"pushall","version":1,"push_target":1}}` to
  `device/<serial>/request`.
* **`pushall` cadence**: `p1`/`a1` → every 5 minutes; `auto` → every 10 minutes; `x1`/`h2d` → never
  (they push complete reports themselves).
* `printerLinkReconfigure()` tears the session down **only** when `ip`, `accessCode` or `serial`
  changed; `staleSec` and `debug.mqttDump` are picked up in place, so toggling a debug switch does
  not drop the link.

### Report filtering

`deserializeJson` runs with a filter that keeps only:

`print.{command, nozzle_temper, nozzle_target_temper, bed_temper, bed_target_temper, chamber_temper,
cooling_fan_speed, big_fan1_speed, big_fan2_speed, heatbreak_fan_speed, fan_gear, gcode_state,
mc_percent, mc_remaining_time, layer_num, total_layer_num, subtask_name, stg_cur, print_error,
wifi_signal, home_flag, lights_report, info.temp}` plus
`print.device.{extruder, bed, ctc, airduct}`.

Messages whose `print.command` is one of `gcode_line, project_prepare, project_file,
clean_print_error, resume, get_accessories, prepare, extrusion_cali_get` are **ignored** — parsing
them would blank out good data. A message that survives filtering updates `lastUpdateMs`.

### Field decoding

| Field | Rule |
|---|---|
| Temperatures | The classic floats (`nozzle_temper`, `bed_temper`, …) win when present |
| Packed `device.*` | `device.extruder.info[0].temp`, `device.bed.info.temp`, `device.ctc.info.temp`: **current = `v & 0xFFFF`, target = `v >> 16`**. Used wherever the classic key is missing |
| Chamber | **Current X1C firmware no longer sends `chamber_temper`.** The chamber comes from `device.ctc.info.temp`, with `print.info.temp` as a last-resort mirror. The packed block is therefore a fallback for *every* model, not just H2D |
| Fan speeds | Decimal strings on a 0–15 gear scale → `(gear * 100 + 7) / 15` percent. `cooling_fan_speed` → part, `big_fan1_speed` → aux, `big_fan2_speed` → chamber, `heatbreak_fan_speed` → heatbreak |
| `fan_gear` | Parsed by the filter but **deliberately not used**: on a live X1C it read `0x6400` while `big_fan1_speed` was `"6"` (40 %) |
| `device.airduct.parts[]` | Each part's `state` is already a percentage; indices 0/1/2 map to part/aux/chamber (H2D) |
| `home_flag` | Arrives as a negative int32; read as `uint32_t`, **bit 23 = door open** |
| `stg_cur` | Mapped through the full ha-bambulab stage table `0..77` in `stageName()` (`0 = printing`, `2 = heatbed_preheating`, `49 = heating_chamber`, `50 = heatbed_cooling`, …), plus `-1`/`255` = `idle` and `-2` = `offline`. Codes 36+ are H2D-era and best-effort |
| `device.ctc.info.temp` high word | Chamber **target**; `0` means "this printer has no chamber heater", so it becomes `NaN` (JSON `null`) rather than a target of 0 °C |
| Unknown values | Temperatures stay `NaN`, fan speeds `-1`, counters `-1`; the status document turns all of those into JSON `null` |

`printer.online` is `connected && data age < staleSec`.

---

## REST API reference

ESPAsyncWebServer on port **80**. All API responses are `application/json`; errors are
`{"error":"…"}`. JSON request bodies go through `AsyncCallbackJsonWebHandler` with a **4096-byte**
limit; the legacy routes take form-encoded input.

**Authentication.** When `web.authEnabled` is on *and* `web.password` is non-empty, every route
requires HTTP Basic auth — **except requests that arrive on the setup-AP interface**, which are never
challenged (see [Security notes](#security-notes)). A failed check returns `401` with a
`WWW-Authenticate: Basic realm="BLSmartFlow"` challenge.

Examples below assume `H=http://blsmartflow.local`.

### Status and diagnostics

| Method | Path | Body | Response |
|---|---|---|---|
| `GET` | `/` , `/index.html` | — | The gzipped UI, `Cache-Control: no-cache` |
| `GET` | `/api/status` | — | The [status object](#status-object) |
| `GET` | `/api/events` | — | `text/event-stream`; `event: status` every second, `event: log` per new log line |
| `GET` | `/api/info` | — | `{"fw","build","chipId","sdk","flashSize","sketchSize","freeSketchSpace","partition","resetReason"}` |
| `GET` | `/api/log` | — | `{"lines":[…]}`, up to 64 lines |

```sh
curl $H/api/status
curl -N $H/api/events
curl $H/api/info
```

`resetReason` is a string: `UNKNOWN, POWERON, EXT, SW, PANIC, INT_WDT, TASK_WDT, WDT, DEEPSLEEP,
BROWNOUT, SDIO`. A log line looks like `[   1234] [E] mqtt: connect failed` — 7-column uptime in
milliseconds, then `I`/`W`/`E`, then the message.

**SSE.** At most **4 concurrent clients**; a fifth connection is closed immediately. A new client is
sent the current status document at once rather than waiting for the next tick. Log lines are pushed
at most 8 per loop pass. The stream is authorised with the same rules as the rest of the API.

### Configuration

| Method | Path | Body | Response |
|---|---|---|---|
| `GET` | `/api/config` | — | Full config, secrets masked |
| `POST` | `/api/config` | Partial config (deep merge) | `{"ok":true,"restartRequired":bool,"config":{…masked}}` |

```sh
curl $H/api/config
curl -X POST -d '{"fan":{"minSpeed":20,"kickStart":true}}' $H/api/config
```

* Applies live: `fan`, `mqtt`, `debug`, `ssdp` and `web` take effect immediately; `printer` triggers
  `printerLinkReconfigure()`. **Only a changed `wifi.*` key sets `restartRequired`.**
* `400 {"error":"access code must be exactly 8 characters"}` for a non-masked
  `printer.accessCode` of any other length.
* `400 {"error":"expected a json object"}` for a non-object body.
* `500 {"error":"could not save configuration"}` when the LittleFS write fails.
* Everything else is clamped by `configValidate()`, never rejected.

### Fan curve

| Method | Path | Body | Response |
|---|---|---|---|
| `GET` | `/api/curve` | — | `{"points":[{"temp","speed"},…],"source":"nozzle"}` |
| `PUT` | `/api/curve` | `{"points":[…]}` | `{"ok":true,"points":[…]}` (the stored, normalised curve) |

```sh
curl -X PUT -d '{"points":[{"temp":0,"speed":0},{"temp":250,"speed":100}]}' $H/api/curve
```

Saved inline (an explicit curve edit is deliberate). Temperatures are **clamped** to 0–400 and speeds
to 0–100, so out-of-range input returns `200` with adjusted points. `400` only for:

| Error | Cause |
|---|---|
| `missing 'points' array` | No `points` array in the body |
| `too many points (max 16)` | More than 16 points |
| `each point needs numeric temp and speed` | A non-numeric `temp` or `speed` |
| `need at least 2 points with distinct temperatures` | Fewer than 2 usable points after normalisation |

### Fan control

| Method | Path | Body | Response |
|---|---|---|---|
| `POST` | `/api/fan` | `{"mode":"auto"｜"manual"｜"off","speed":0..100,"durationSec":0}` | `{"ok":true,"fan":{…}}` (the `fan` section of the status object) |

```sh
curl -X POST -d '{"mode":"manual","speed":60,"durationSec":600}' $H/api/fan
curl -X POST -d '{"mode":"auto"}' $H/api/fan
```

* Every field is optional; omitting `mode` keeps the current one.
* `speed` outside 0–100 is **not** clamped: `400 {"error":"speed must be 0..100"}`.
* An unknown `mode`: `400 {"error":"mode must be auto, manual or off"}`.
* `durationSec > 0` with `mode: "manual"` creates a temporary override (not persisted, clamped to
  86400 s). `durationSec == 0` persists mode and speed through the **deferred** save.

### Network

| Method | Path | Body | Response |
|---|---|---|---|
| `GET` | `/api/wifi/scan` | — | `202 {"scanning":true}` while a scan runs, else `{"networks":[{"ssid","bssid","rssi","channel","secure"},…]}` |
| `POST` | `/api/wifi` | `{"ssid","password","bssid","lockBssid","hostname"}` | `{"ok":true,"restartRequired":true}`, then restart |

```sh
curl "$H/api/wifi/scan?force=1"     # start a fresh scan -> 202
curl $H/api/wifi/scan               # poll -> 202 or the list
curl -X POST -d '{"ssid":"Home","password":"secret"}' $H/api/wifi
```

* Results are sorted by RSSI descending, deduped by SSID (strongest BSSID wins), hidden networks
  dropped, BSSIDs upper-case, 2.4 GHz only (the radio has no other band).
* A result younger than **20 s** is served from cache. `?force=1` (any value but `0`) discards the
  cache and starts a new scan, so it always answers `202`. The UI sends `force=1` on an explicit
  *Scan* click and then polls without it.
* `POST /api/wifi` requires a non-empty `ssid` (`400 {"error":"ssid is required"}`), reuses the
  config merge (so `"********"` keeps the stored password) and saves inline. It then restarts after
  **1 s in AP mode**, **1.5 s** otherwise.

### Backup, restore, maintenance

| Method | Path | Body | Response |
|---|---|---|---|
| `GET` | `/api/backup` | — | Full config **with secrets**, `Content-Disposition: attachment; filename="blsmartflow-<chipid>.json"` |
| `POST` | `/api/restore` | Full config | `{"ok":true}`, then restart after 500 ms |
| `POST` | `/api/restart` | — | `{"ok":true}`, then restart after 500 ms |
| `POST` | `/api/factoryreset` | `{"confirm":true}` | `{"ok":true}`, wipes the config, restarts after 750 ms |

```sh
curl -OJ $H/api/backup
curl -X POST --data-binary @blsmartflow-a1b2c3.json $H/api/restore
curl -X POST -d '{"confirm":true}' $H/api/factoryreset
```

**Restore semantics.** The document is merged onto **defaults**, not onto the current config, so keys
it omits fall back to their defaults. The four secrets are seeded from the running config first, so a
backup taken from the UI (which carries `"********"`) keeps the stored WiFi password and access code
instead of wiping them. A restore whose `wifi.ssid` ends up empty is refused with
`400 {"error":"backup has no wifi.ssid"}` rather than stranding the device. `/api/factoryreset`
without `{"confirm":true}` returns `400 {"error":"send {\"confirm\":true}"}`.

### OTA

| Method | Path | Body | Response |
|---|---|---|---|
| `POST` | `/api/update` (alias `/update`) | `multipart/form-data`, any file field name | `{"ok":true}` then restart after 1 s, or `500 {"error":"update failed: …"}` |

```sh
curl -F firmware=@BLSmartflow_V2.0.0.bin.ota $H/api/update
```

The image is written to the inactive OTA slot with `Update.begin(UPDATE_SIZE_UNKNOWN)`. A failed
chunk aborts the update (which releases the partition) and the reason is reported in the response; a
client that disconnects mid-upload also triggers an abort, so a dead upload cannot claim the OTA
partition for the rest of the boot.

### Legacy 1.x endpoints

Kept so 1.x tooling and bookmarks keep working.

| Method | Path | Body | Response |
|---|---|---|---|
| `GET` | `/getOptions` | — | `{"firmwareversion","ip","code","id","staticfans","staticfanspeed","debuging","debugingchange","mqttdebug"}` |
| `POST` | `/submitOptions` | form: `ip, code, serial, staticfan, staticfanspeed, debuging, mqttdebug` | `{"ok":true}` |
| `GET` | `/getFanConfig` | — | Same payload as `GET /api/curve` |
| `POST` | `/updateFanConfig` | form field `points`: `{"points":[…]}` or a bare `[…]` | `{"ok":true}` |
| `GET` | `/sensorData` | — | `{"temp":<nozzle, 2 decimals, 0 when unknown>,"speed":<fan output %>}` |
| `POST` | `/update` | multipart | Alias of `POST /api/update` |

* `code` and `id` are **obfuscated**: every character but the last three becomes `*` (values of three
  characters or fewer are returned unchanged). On the way back in, a value containing a `*` is
  treated as "unchanged".
* `staticfans` = `fan.mode == "manual"`; `staticfanspeed` = `fan.manualSpeed`;
  `debuging` = `debug.serial`; `mqttdebug` = `debug.mqttDump`.
* `staticfan=on` sets `fan.mode = "manual"`, anything else sets `"auto"`. Values are validated and
  **saved** (the 1.x bug where `/submitOptions` never persisted is fixed).
* `/updateFanConfig` validates exactly like `PUT /api/curve`.
* `/sensorData` always reports the **nozzle**, whatever the curve source is, and `0` when unknown —
  existing dashboards parse it that way.

### Captive-portal probes

These paths answer `302 → http://192.168.4.1/` **only for requests that arrive on the AP
interface**; on the station interface they are a normal `404`.

```
/generate_204  /gen_204  /hotspot-detect.html  /library/test/success.html
/connecttest.txt  /ncsi.txt  /fwlink  /redirect  /success.txt  /canonical.html
/check_network_status.txt  /chat
```

Any other unmatched path gets the same treatment through the not-found handler: portal redirect on
the AP interface, `404 {"error":"not found"}` elsewhere. `HTTP_OPTIONS` requests answer `200`.
Redirects carry `Cache-Control: no-cache, no-store, must-revalidate`, `Pragma: no-cache` and
`Expires: 0` so an OS cannot cache its portal verdict.

### Status object

The same document is returned by `GET /api/status`, pushed as the SSE `status` event and published
retained to `<base>/state`.

```jsonc
{
  "device":  { "fw":"2.0.0", "uptimeSec":123, "heapFree":123456, "heapMin":100000,
               "chipId":"a1b2c3", "hostname":"blsmartflow", "ip":"10.0.1.5", "apMode":false },
  "wifi":    { "connected":true, "ssid":"…", "bssid":"…", "rssi":-61, "channel":6 },
  "printer": { "configured":true, "connected":true, "online":true, "lastUpdateSec":2,
               "mqttState":0, "mqttStateText":"connected", "state":"RUNNING", "printing":true,
               "stage":0, "stageText":"printing", "progress":42, "remainingMin":87,
               "layer":12, "totalLayers":210, "task":"Benchy.3mf", "phase":"printing",
               "doorOpen":false, "doorEdgeCount":2,
               "printError":0, "wifiSignal":"-45dBm",
               "temps": { "nozzle":220.4, "nozzleTarget":220, "bed":60.1, "bedTarget":60,
                          "chamber":38.0, "chamberTarget":45.0 },
               "fans":  { "part":100, "aux":0, "chamber":40, "heatbreak":100 } },
  "fan":     { "output":55, "target":55, "mode":"auto", "effectiveMode":"auto", "source":"nozzle",
               "sourceTemp":220.4, "setpoint":null, "chamberTarget":45, "cooldownTarget":35,
               "manualSpeed":50, "manualExpiresSec":0, "pwmDuty":140,
               "output1":true, "output2":true },
  "thermal": { "rateCPerMin":-0.42, "kClosed":[0.31,null,null,null,null],
               "kOpen":[null,null,null,null,null], "samples":7 },
  "mqttExt": { "enabled":true, "connected":true }
}
```

| Field | Meaning |
|---|---|
| `device.heapFree` / `heapMin` | Current and all-time-minimum free heap, in bytes |
| `device.ip` | STA address, or `192.168.4.1` in AP-only mode, or `0.0.0.0` |
| `device.apMode` | The setup AP is currently up (it can be up while the station is also connected) |
| `wifi.*` | The **station** link. `ssid`/`bssid` are `""` and `rssi`/`channel` are `0` when not associated |
| `printer.configured` | IP, access code and serial are all set |
| `printer.connected` | MQTT session up · `printer.online` | session up **and** data younger than `staleSec` |
| `printer.mqttState` / `mqttStateText` | PubSubClient state code and its text (`connected`, `unauthorized`, `bad_credentials`, `connection_lost`, …) |
| `printer.state` | Raw `gcode_state` |
| `printer.phase` | Derived phase: `offline｜paused｜preheat｜cooling｜printing｜finished｜failed｜idle` (see [Print phase](#print-phase)). `printer.printing` is true for `preheat`/`printing`/`paused` |
| `printer.stageText` | ha-bambulab stage name for `stg_cur` |
| `printer.doorOpen` | Front-door switch — `true`/`false`, or **`null`** while `doorKnown` is false. The top lid has no sensor |
| `printer.doorKnown` | An open/close edge has been observed, so the bit can be trusted. Everything door-driven is inert until then |
| `printer.doorEdgeCount` | Transitions seen since boot; the first report is state, not an edge |
| `printer.temps.chamberTarget` | The printer's own chamber set point, `null` on machines without a chamber heater |
| `fan.output` / `target` | Rounded percent actually driven / requested by the active mode |
| `fan.effectiveMode` | `off｜manual｜stale｜door｜preheat｜idle｜cooldown｜chamber｜auto` |
| `fan.setpoint` | Thermostat set point in force right now, `null` outside `chamber`/`cooldown` |
| `fan.chamberTarget` / `cooldownTarget` | The **configured** set points, so the HA number entities have a state to read back |
| `thermal.rateCPerMin` | Current chamber slope in °C/min (negative = cooling), `null` until the fan output and door have been steady for ~20 s |
| `thermal.kClosed` / `kOpen` | Learned cooling constants in 1/min for fan buckets 0/25/50/75/100 %; `null` where nothing has been measured — **never** NaN |
| `thermal.samples` | Number of windows blended into the table so far |
| `fan.manualExpiresSec` | Seconds left on a timed override, `0` when there is none |
| `fan.pwmDuty` | The 0–255 byte written to the pin, **already inverted** when `pwmInvert` is on |
| `mqttExt` | External broker: configured-and-enabled, and currently connected |

**Null rules.** Unknown temperatures are `null`, and so is every unmeasured entry in `thermal`
(NaN must never reach the JSON — `serializeJson` would emit a bare `null` for a float NaN, but the
status builder makes it explicit). `printer.stage`, `progress`, `remainingMin`,
`layer` and `totalLayers` are `null` until the printer reports them — never `-1` or `0`. Printer fan
percentages are `null` when unknown. `printer.lastUpdateSec` is `null` until the very first report
ever arrives (the UI renders that as "never").

---

## MQTT / Home Assistant reference

A second, plain-TCP `PubSubClient` running on the loop task. `base = mqtt.baseTopic` or
`blsmartflow/<chipid>`. Client id `BLSF-<chipid>`. Reconnect every 10 s, backing off to 60 s; the
broker hostname is resolved before `connect()` (and re-resolved after 5 consecutive failures) so a
DNS timeout cannot stall the loop.

| Topic | Direction | Payload |
|---|---|---|
| `<base>/availability` | pub, retained, **LWT** | `online` / `offline` |
| `<base>/state` | pub, retained | The full status object |
| `<base>/fan/speed` | pub, retained | `0`–`100` (current output) |
| `<base>/fan/on_state` | pub, retained | `ON` when output > 0, else `OFF` |
| `<base>/mode` | pub, retained | `auto` / `chamber` / `manual` / `off` (the configured mode) |
| `<base>/fan/set` | sub | `0`–`100` → manual mode at that speed |
| `<base>/fan/on` | sub | `OFF` → mode `off`; anything else → manual (at least 1 %) |
| `<base>/mode/set` | sub | `auto` / `chamber` / `manual` / `off` |
| `<base>/curve/set` | sub | `{"points":[{"temp":…,"speed":…},…]}`, max 2048 bytes, saved inline |
| `<base>/chamber_target/set` | sub | `20`–`80` → `fan.chamberTarget` (clamped by `validate()`) |
| `<base>/cooldown_target/set` | sub | `15`–`60` → `fan.cooldownTarget` |
| `<base>/target/set` | sub | `{"chamberTarget":45,"cooldownTarget":35}` — either key may be omitted |
| `<base>/restart` | sub | Any payload → restart |

The set points come in two shapes because a Home Assistant `number` entity wants one topic per
value, while a script would rather send both at once. Both go through the deferred-save path, so an
HA slider cannot wear the flash out.

`state`, `fan/speed`, `fan/on_state` and `mode` are republished every `publishIntervalSec`,
immediately whenever the fan output changes, and right after a command is applied. The client buffer
is grown automatically to fit the status document.

### Home Assistant discovery

Published **retained** on connect (and when `haDiscovery` is switched on); switching it off publishes
empty payloads to the same topics, which removes the entities.

Discovery topic: `<haPrefix>/<component>/blsmartflow_<chipid>/<object_id>/config`.
`unique_id` and `object_id` are both `blsmartflow_<chipid>_<object_id>`. Every entity carries
`availability_topic: <base>/availability` and this device block:

```json
{ "identifiers": ["blsmartflow_<chipid>"], "name": "BLSmartFlow <chipid>",
  "manufacturer": "DutchDeveloper", "model": "BLSmartFlow",
  "sw_version": "<fw>", "configuration_url": "http://<ip>/" }
```

| Component | `object_id` | Notes |
|---|---|---|
| `fan` | `fan` | `command_topic fan/on`, `state_topic fan/on_state`, `percentage_command_topic fan/set`, `percentage_state_topic fan/speed`, speed range 1–100 |
| `select` | `mode` | Options `auto`, `chamber`, `manual`, `off` |
| `number` | `chamber_target` | 20–80 °C, `command_topic chamber_target/set`, state from `value_json.fan.chamberTarget` |
| `number` | `cooldown_target` | 15–60 °C, `command_topic cooldown_target/set`, state from `value_json.fan.cooldownTarget` |
| `button` | `restart` | `payload_press: PRESS`, `device_class: restart` |
| `sensor` | `nozzle_temp`, `bed_temp`, `chamber_temp` | °C, `device_class temperature`, `state_class measurement` |
| `sensor` | `fan_output` | %, `state_class measurement` |
| `sensor` | `printer_state`, `printer_stage`, `phase` | Text. `phase` is the derived print phase the fan rules act on |
| `sensor` | `cooling_rate` | °C/min, `state_class measurement`; `unknown` while nothing is being measured |
| `sensor` | `print_progress` | %, `state_class measurement` |
| `sensor` | `remaining_time` | min, `device_class duration` |
| `sensor` | `printer_wifi` | The printer's own reported RSSI string |
| `sensor` | `device_rssi` | dBm, `device_class signal_strength` |
| `sensor` | `uptime` | s, `device_class duration`, `state_class total_increasing` |
| `binary_sensor` | `printer_online` | `device_class connectivity` |
| `binary_sensor` | `door` | `device_class opening`. Publishes the literal `None` while `doorKnown` is false, which Home Assistant renders as *Unknown* — a stuck bit must not be reported as a shut door |
| `binary_sensor` | `printing` | `device_class running` |

Every sensor reads its value out of the retained `state` document with a `value_template`. Fields
that can be `null` (the three temperatures, progress, remaining time, cooling rate) use
`{% set v = value_json.… %}{{ 'unknown' if v is none else v }}` so Home Assistant shows *Unknown*
instead of logging a parse error on an empty string.

### Using it without Home Assistant

Set `mqtt.haDiscovery` to `false` and use the topic table directly:

```sh
mosquitto_sub -h broker -t 'blsmartflow/a1b2c3/state' -v
mosquitto_pub -h broker -t 'blsmartflow/a1b2c3/fan/set' -m 45
mosquitto_pub -h broker -t 'blsmartflow/a1b2c3/mode/set' -m auto
mosquitto_pub -h broker -t 'blsmartflow/a1b2c3/curve/set' \
  -m '{"points":[{"temp":0,"speed":0},{"temp":250,"speed":100}]}'
```

Fan and mode commands persist through the deferred-save path (at most one flash write per 10 s);
`curve/set` is saved inline.

---

## Serial provisioning protocol

USB CDC / UART at **115200 baud**, one JSON object per line (`\n`, `\r` ignored). Lines longer than
2048 bytes are discarded with a warning; at most 256 bytes are consumed per loop pass. Every line is
answered with `{"ok":true,"msg":"…"}` or `{"ok":false,"error":"…"}`.

**Legacy provisioning keys** (all optional; an empty or missing value leaves the field alone):

```json
{"ssid":"…","pass":"…","printerip":"…","printercode":"…","printerserial":"…"}
```

**Full config document** — the same schema and merge rules as `POST /api/config`:

```json
{"config":{"fan":{"minSpeed":20},"mqtt":{"enabled":true,"host":"10.0.1.2"}}}
```

Either form validates, saves and then restarts after 1 s (credentials only take effect on a fresh
WiFi bring-up). A document with neither form answers `{"ok":false,"error":"nothing to apply"}`.

**Commands:**

| Line | Effect |
|---|---|
| `{"cmd":"status"}` | Replies `{"fw","chipId","hostname","ssid","wifi","ip","printerConnected","fanOutput"}` |
| `{"cmd":"restart"}` | Restart after 500 ms |
| `{"cmd":"factoryreset"}` | `configWipe()` and restart after 750 ms |

Unknown verbs answer `{"ok":false,"error":"unknown cmd"}`; malformed lines answer
`{"ok":false,"error":"invalid json"}`.

On every successful station connect the firmware prints `IP_ADDRESS:<ip>` on Serial. The WebSerial
page `docs/wifiSetup.html` parses exactly that prefix, so do not change it.

---

## WiFi, AP and captive portal

Non-blocking state machine driven from `loop()`:

```
IDLE ──beginConnect()──> CONNECTING ──link up──> CONNECTED
  ^                          │                      │
  └────── backoff ───────────┘ 20 s timeout         └── link lost -> IDLE (retry at once)
```

| Constant | Value | Meaning |
|---|---|---|
| Connect timeout | 20 s | Per attempt |
| Backoff | 5 s → 10 s → 20 s → 30 s → 60 s | Doubles up to 30 s, then 60 s; reset on success |
| Drop BSSID lock | after 3 failed cycles | A locked radio that is gone must not lock the device out |
| Raise setup AP | after **90 s** of *continuous* failure | Cleared by any successful connect, so a link that flaps hourly never ends up broadcasting |
| STA retry while AP is up | at least every 60 s | The AP does not stop reconnection attempts |
| AP linger | 5 min | After the station connects while the AP is up |
| AP restart backoff | 5 s | After a failed `softAP()` |
| Scan cache TTL | 20 s | See `GET /api/wifi/scan` |

Radio setup: `WiFi.persistent(false)`, `setAutoReconnect(false)` (the state machine owns
reconnection), `setSleep(false)`, TX power 19.5 dBm, hostname from `wifi.hostname`.

**AP**: SSID `BLSmartFlow-<chipid>`, **open**, `192.168.4.1/24`, `DNSServer` on port 53 answering
every name with `192.168.4.1`. Mode is `WIFI_AP_STA` when credentials exist (so the station keeps
retrying) and `WIFI_AP` when they do not. With no credentials at all, the AP starts immediately at
boot.

**mDNS**: `<hostname>.local`, service `_http._tcp` on port 80 with TXT records `model=BLSmartFlow`
and `version=<fw>`. Restarted after a reconnect; a failure is logged, never fatal.

**Portal behaviour**: `onApInterface()` compares the request socket's **local IP** with
`WiFi.softAPIP()`. That is what distinguishes the phone on the open portal from the rest of the LAN
while the device runs AP+STA — only AP-side requests get portal redirects and the auth bypass.

---

## Security notes

State it plainly:

* **The setup AP is open by design.** The whole point is that a phone can join it with no shared
  secret. It exposes only the local setup UI, and it is only up when the device has no credentials or
  has been unable to reach WiFi for 90 s.
* **Requests arriving on the AP interface skip authentication entirely.** `authorisedQuiet()` returns
  true for them before it ever checks the password. That includes `POST /api/update` (OTA),
  `POST /api/restore`, `POST /api/factoryreset` and `GET /api/backup` — i.e. **anyone within radio
  range of a device that is in setup mode can read the secrets and flash it**. This is a deliberate
  trade: locking the user out of the one interface they can reach when WiFi is broken would be a dead
  end. Requests on the station interface are always challenged.
* **Basic auth is not encrypted.** There is no TLS on the device's own web server. It keeps casual
  visitors out of a trusted LAN, nothing more.
* Auth is only enforced when `web.authEnabled` **and** a non-empty `web.password` are stored;
  `configValidate()` turns `authEnabled` off when the password is empty, so it cannot be armed with
  no way in.
* **`GET /api/backup` returns every secret in clear text** — WiFi password, printer access code,
  broker password, UI password. Treat the file accordingly.
* The printer link uses `setInsecure()`: the printer's certificate is self-signed and there is no CA
  to pin, so the TLS session is encrypted but unauthenticated.
* The external broker link is plain TCP; broker credentials cross the LAN in clear.
* The web UI loads no external resources at all (no CDN, no fonts, no remote images), so it works in
  AP mode and leaks nothing to third parties.

---

## Building and testing

Requirements: PlatformIO Core ≥ 6.1 and Python 3.

```sh
pio run                        # build esp32dev (default env)
pio run -t upload              # flash over USB
pio test -e native             # host unit tests
python3 tools/mock_server.py   # API mock at http://localhost:8080
```

### Environments

| Env | Purpose |
|---|---|
| `esp32dev` | The firmware. pioarduino platform `55.03.311` (Arduino-ESP32 3.3.x), board `esp32dev`, LittleFS, `min_spiffs.csv`, `custom_version = 2.0.0`, `custom_project_name = BLSmartflow` |
| `native` | Host build of the tests only (`test_build_src = no`, `build_src_filter = -<*>`, `-I src/blflow`, C++17). Only ArduinoJson is pulled in |

Build flags: `-DSTRVERSION`, `-DCORE_DEBUG_LEVEL=0`, `-DCONFIG_ASYNC_TCP_STACK_SIZE=8192` (4 KB is
too tight once a handler writes LittleFS or serialises the status document), `-DBLSF_SSDP`.
Libraries: ArduinoJson 7, PubSubClient 2.8, ESPAsyncWebServer 3.12 + AsyncTCP 3.5 (ESP32Async),
ESP32SSDP pinned to tag `2.0.3` (the registry copy only builds against core 2.x).

> **Note.** `[platformio] build_dir` is deliberately **not** overridden: the pioarduino `elf2image`
> step fails when `$BUILD_DIR` is moved out of `.pio/build`.

### Asset pipeline — `pre_build.py`

Runs as a `pre:` script. It gzips every `*.html`, `*.js`, `*.css`, `*.svg`, `*.png` under `src/www/`
into `src/www/www.h` as `PROGMEM` arrays plus `_len` and `_mime` symbols (`index_html_gz`,
`index_html_gz_len`, `index_html_gz_mime`). `www.h` is generated, not committed.

### Packaging — `merge_firmware.py`

Runs as a post-action on the app binary and produces:

| Output | What it is |
|---|---|
| `.firmware/BLSmartflow_V<version>.bin.ota` | Plain application image — **the OTA upload** |
| `.firmware/BLSmartflow_V<version>.bin` | Merged bootloader + partitions + app, **flash at `0x0`** |
| `firmware/esp32dev/BLSmartflow_<version>.bin` | Copy of the merged image that the web flasher serves |

It then rewrites `firmware/manifest.json`: `version` from `custom_version`, and
`builds[0].parts[0]` to `{ "path": "esp32dev/BLSmartflow_<version>.bin", "offset": 0 }`. Set
`ENABLE_MERGE_BIN = False` to skip the merged image (the OTA image is always produced). If
`merge-bin` fails, the manifest and the release copy are left untouched.

### Tests

`pio test -e native` runs five Unity suites against the Arduino-free headers:

| Suite | Covers |
|---|---|
| `test/test_curve` | `curve.h`: interpolation at / between / outside points, equal temperatures, unsorted input, clamping, `curveValidate()` rules |
| `test/test_parse` | `printer_parse.h` against the captured fixtures, plus door-edge semantics, the packed chamber target, every `reportPhase()` rule and the `stg_cur` name table |
| `test/test_buffer` | `AutoGrowBufferStream` (with local `Arduino.h` / `Stream.h` shims) |
| `test/test_thermostat` | `thermostat.h`: proportional response, integral accumulation, the ±100/ki anti-windup clamp, the door and saturation freezes, and the printing → cool-down set-point switch |
| `test/test_thermal` | `thermal_math.h`: bucketing, the EMA blend, recovering a known cooling constant from a synthetic cool-down, and every reason a window is refused |

Fixtures in `test/fixtures/` were captured from a live X1C and sanitised:
`x1c_push_status.json` (a full `print.push_status`, notably **without** `chamber_temper`, with packed
`device.*` temperatures and gear-scale fan strings) and `x1c_gcode_line.json` (an acknowledgement
that the parser must ignore). See `test/fixtures/README.md`.

### UI mock server

`tools/mock_server.py` implements the whole API in Python (stdlib only) with a simulated printer and
fan controller, so `src/www/index.html` can be developed without hardware.

| Flag | Effect |
|---|---|
| `--port N` | Listen port (default `8080`) |
| `--host ADDR` | Bind address (default `0.0.0.0`) |
| `--ap` | Simulate AP / provisioning mode (`device.apMode = true`, no station) |
| `--offline` | The printer never connects: temps and counters `null`, `lastUpdateSec` `null`, `effectiveMode` `stale` |
| `--auth USER:PASS` | Require HTTP basic auth on every route, as `web.authEnabled` does |
| `--door` | Start with the door reported open (`doorKnown` still false until the first toggle) |

The simulated printer walks a whole job — idle → preheat → printing → finished/cooling → idle —
driving `stg_cur`, the temperature targets and therefore `printer.phase`, and it runs a Newtonian
chamber model (`dT/dt = heatIn − k·(T − ambient)`, with `k` raised by the fan output and by an open
door) so the thermostat and the cooling-rate learning have something real to chew on. It implements
the same evaluation order, the same PI step and the same window logic as the firmware.

`POST /mock/door` is the one route that is **not** part of the device API: `{"open":true}`,
`{"open":false}` or `{"toggle":true}` stands in for someone opening the printer, so the door rules
can be demonstrated. It answers `{"ok":true,"changed":…,"doorOpen":…,"doorEdgeCount":…}`.

### CI

`.github/workflows/build.yml` runs on every branch push, pull request, `v*` tag and manual dispatch:
checkout (`actions/checkout@v7`) → Python 3.12 (`actions/setup-python@v7`) → PlatformIO cache
(`actions/cache@v6`) → `pip install --upgrade platformio` → `pio test -e native` → `pio run -e
esp32dev` → upload `.firmware/*` (`actions/upload-artifact@v7`, `include-hidden-files: true`,
`if-no-files-found: error`). On a tag, `softprops/action-gh-release@v3` publishes the same files as
release assets.

---

## Partitions and OTA

* Partition table: **`min_spiffs.csv`** — two ~1.875 MB OTA app slots and ~128 KB LittleFS on a 4 MB
  flash. The configuration file is well under 4 KB, so the small filesystem is ample.
* **Upgrading from the 2025.x firmware needs one full flash** (USB or web flasher, offset `0x0`)
  because the partition layout changed; an OTA image cannot rewrite the partition table. That flash
  erases the stored settings. Every later update can be OTA.
* `GET /api/info` reports `sketchSize`, `freeSketchSpace` and the running `partition` label, which is
  the quickest way to check how much headroom an app slot still has.
* OTA writes into the inactive slot; a failure leaves the running image intact, and an aborted upload
  releases the partition immediately rather than at the next boot.

---

## Hardware

| Function | GPIO | Details |
|---|---|---|
| Fan output 1 | 17 | LEDC PWM, `pwmFreq` (default 25 kHz), 8-bit resolution |
| Fan output 2 | 16 | Same duty as output 1; can be disabled independently |
| Status LED | 21 | Non-blocking `millis()` patterns |

* Arduino-ESP32 3.x API: `ledcAttach(pin, freq, resolution)` / `ledcWrite(pin, duty)` — the channel
  and timer are chosen by the core. A `pwmFreq` change detaches and re-attaches both pins.
* Duty is 8-bit: `duty = round(output% * 255 / 100)`, inverted at the pin when `pwmInvert` is set. A
  duty write is skipped when the value has not changed.

**LED patterns** (highest priority first): 1 blink = no credentials / setup AP · 2 = WiFi down ·
3 = printer MQTT down · 4 = printer data stale · solid with two short dips every 3 s = manual mode ·
solid = OK. A blink pattern is *N* × 200 ms on/off followed by an 800 ms gap.

---

### Pitfalls learned on hardware

* **ArduinoJson stores `const char*` by pointer.** Only `char*` (non-const), `String` and `std::string` are
  copied into the document. Assigning a field of a *local* `const` struct (for example a `PrinterState`
  snapshot) and serialising after the function returned produces garbage. `status.cpp` and
  `config.cpp` wrap every char-array field in `String()` for that reason — keep doing so.
* **SSE needs the `Accept: text/event-stream` header.** `AsyncEventSource` answers 404 to any other
  client, so `curl /api/events` without the header is not a valid test; browsers send it automatically.
* **Captive-portal mini browsers may open an EventSource that never delivers.** The UI therefore polls in
  AP mode, keeps polling until the stream delivers its first event, and falls back to polling after 5 s
  of silence.
* **`__DATE__`/`__TIME__` only refresh when their translation unit is recompiled.** `pre_build.py`
  writes `src/blflow/build_stamp.h` on every build so `/api/info.build` is always current.

## Known limitations

* **H2D support is best effort.** The `device.airduct.parts[]` → part/aux/chamber mapping is inferred
  from index order and has not been verified against a real H2D.
* **No 5 GHz.** The ESP32 radio is 2.4 GHz only; 5 GHz-only networks never appear in a scan.
* **HTTPS captive-portal probes cannot be intercepted.** Some Android builds probe over TLS; the
  device can only answer the plaintext probes it serves, so the portal sheet sometimes has to be
  opened by hand at `http://192.168.4.1/`.
* **At most 4 concurrent SSE clients**; further connections are closed and those pages fall back to
  2 s polling.
* **JSON request bodies are capped at 4096 bytes**, and `curve/set` over MQTT at 2048 bytes.
* **The log ring buffer holds 64 lines of 120 bytes**; longer messages are truncated and older lines
  are lost.
* **Basic auth is bypassed on the setup AP** — see [Security notes](#security-notes).
* **The printer's TLS certificate is not verified** (`setInsecure()`).
* **SSDP advertises `description.xml`, but the web server does not serve that path**, so a discovery
  client that fetches the schema URL gets a 404. Discovery is optional (`-DBLSF_SSDP`, `ssdp.enabled`).
* **A printer report larger than the 64 KB RX cap is dropped** with a warning rather than parsed in
  pieces.
* **`fan.mode` is persisted**, so a device that was left in `manual` comes back in `manual` after a
  power cut; timed overrides deliberately do not survive a reboot.
* **The door bit is best effort, and the top lid is not sensed at all.** On some X1C units the closed
  door never actuates the switch, so `home_flag` bit 23 stays at 1 for the whole session. Those
  machines never reach `doorKnown`, so `printer.doorOpen` stays `null`, `doorEdgeCount` stays 0 and
  every door rule stays inert — which is the intended failure mode, not a bug.
* **The thermostat's integral freeze uses the same door reading**, so on a printer with an unproven
  switch it never freezes — the ±100/ki clamp and the saturation freeze are what protect it there.
* **Chamber thermostat mode needs a chamber temperature.** On a printer that reports none it falls
  back to the curve (`effectiveMode: "auto"`) rather than running blind.
* **The cooling-rate table is descriptive, not prescriptive.** Nothing auto-tunes `kp`/`ki` from it;
  it exists so the UI and Home Assistant can say how fast the chamber actually cools.
* **`fan.ambientTemp` is a typed-in number, not a measurement.** The device has no room sensor, so a
  wrong value shifts every learned `k` even though the fan behaviour is unaffected.
