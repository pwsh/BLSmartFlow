# Architecture

BLSmartFlow is an Arduino-ESP32 sketch, not an ESP-IDF application, but it is organised the way a
firmware project should be: a small number of modules with explicit ownership, pure logic separated
from anything that touches the network, and exactly one thing allowed to block.

## Modules

| Path | Responsibility |
|---|---|
| `src/main.cpp` | Boot order, the non-blocking loop, deferred restart / factory reset (`app.h`) |
| `src/blflow/version.h` | `FW_VERSION` (from `custom_version` via `-DSTRVERSION`), `FW_BUILD`, `FW_NAME` |
| `src/blflow/log.*` | `LOGI/LOGW/LOGE` → Serial (gated on `debug.serial`) plus a 64-line × 120-byte ring buffer, spinlock-guarded, with a monotonic sequence counter for SSE |
| `src/blflow/config.*` | The `Config` POD, defaults, `configValidate()`, LittleFS persistence, legacy migration, masked JSON, deep merge |
| `src/blflow/curve.h` | Header-only, Arduino-free curve model: `curveValidate()`, `curveInterpolate()`, `curveDefaults()` |
| `src/blflow/state.*` | `PrinterState` / `FanState` snapshots under `portMUX` spinlocks; the PubSubClient state table |
| `src/blflow/printer_parse.h` | Header-only, Arduino-free Bambu report parser (filter + field extraction), the full `stg_cur` name table, the pure `reportPhase()` |
| `src/blflow/thermostat.h` | Header-only, Arduino-free PI step for the chamber thermostat |
| `src/blflow/filament_db.h` | **Generated**: the Filament Field Guide as a PROGMEM `FilamentInfo[]` plus the Bambu `tray_info_idx` → type table |
| `src/blflow/filament_match.h` | Header-only, Arduino-free: tray → guide-id matching, the tray encodings, the pure effective-profile rules |
| `src/blflow/filament.*` | Device-side glue: resolve the active tray from a `PrinterReport`, the status block, `GET /api/filaments` |
| `src/blflow/thermal_math.h` | Header-only, Arduino-free cooling-window arithmetic (buckets, EMA, Newton fit) |
| `src/blflow/thermal.*` | Passive cooling-rate learning: sampling, persistence, status block |
| `src/blflow/printer_link.*` | TLS MQTT client to the printer, in its own FreeRTOS task |
| `src/blflow/ha_mqtt.*` | External broker client, Home Assistant discovery, command topics |
| `src/blflow/fan_control.*` | LEDC setup and the control state machine |
| `src/blflow/web_server.*` | ESPAsyncWebServer: UI, REST, SSE, OTA, legacy routes, captive portal, basic auth |
| `src/blflow/status.*` | `buildStatus()` — the single status document used by REST, SSE and MQTT |
| `src/blflow/wifi_manager.*` | STA state machine, AP + DNS captive portal, async scan, mDNS |
| `src/blflow/indicator.*` | Non-blocking LED patterns |
| `src/blflow/serial_provision.*` | Line-delimited JSON provisioning over USB |
| `src/blflow/ssdp.*` | Optional UPnP/SSDP advertisement (`-DBLSF_SSDP`) |
| `src/blflow/AutoGrowBufferStream.h` | Growing RX buffer for PubSubClient (`size_t` lengths, 64 KB cap, checked `realloc`) |
| `src/www/index.html` | The entire UI: one file, inline CSS and JS, no external requests |

!!! tip "The header-only modules are the interesting ones"
    `curve.h`, `printer_parse.h`, `thermostat.h`, `filament_match.h` and `thermal_math.h` contain no
    Arduino headers at all. That is what makes them
    [testable on the host](building-and-testing.md#tests) — six Unity suites run against them in CI
    with no hardware and no emulator.

## Tasks and threads

| Task | Core | What runs there |
|---|---|---|
| Arduino `loop` task | 1 (Arduino default) | `fanControlLoop`, `indicatorLoop`, `wifiLoop`, `serialProvisionLoop`, `webServerLoop`, `haMqttLoop`, `ssdpLoop`, `configLoopSave`, deferred restart |
| `printer` task | pinned to core 1, 20 KB stack, priority 1 | TLS handshake, `PubSubClient::loop()`, report parsing, `pushall` |
| AsyncTCP task | library-managed, 8 KB stack (`-DCONFIG_ASYNC_TCP_STACK_SIZE=8192`) | Every HTTP handler, SSE fan-out, OTA chunk writes |

**The printer's TLS session is the only thing that can block for seconds**, which is why it lives in
its own task. An unreachable printer must not stall fan control or the web UI — in the 1.x firmware
it did exactly that, and the UI froze whenever the printer was switched off.

!!! note "Why 8 KB for AsyncTCP"
    The 4 KB default is too tight once a handler saves the config (LittleFS) or serialises the status
    document. It manifests as a stack-overflow panic under load, not as a clean error.

## Locking model

Two mechanisms, chosen by what they protect.

### `Config` — a recursive mutex

`configLock()` / `configUnlock()`, with an RAII `ConfigGuard`. `cfg()` is written from the loop task,
the AsyncTCP task, the serial reader and the external-MQTT callback.

It is **recursive** because handlers naturally do *take lock → merge → `configSave()`*, and
`configSave()` takes the lock again.

### `PrinterState` / `FanState` — spinlocks with whole-struct copies

Readers call `printerSnapshot()` / `fanSnapshot()` and get a **private copy** taken inside a very
short `portMUX` critical section.

This is only sound because both structs are **POD**: fixed `char` arrays, no `String`, no
`std::vector`. Keeping them POD is a hard constraint, not a style preference — the whole design rests
on `memcpy` being a valid way to snapshot them.

`printer_link`'s `LinkCfg` is its own POD guarded by a second spinlock, so the printer task never
reads the live `Config` while a web handler is mutating it.

### Never work under a lock

Modules copy what they need (`FanConfig fc = cfg().fan;`) and work off the copy. Cross-task requests
that must not run on the calling task are **flags picked up by the loop task**:
`fanControlReconfigure()`, `haMqttReconfigure()`, `ssdpReconfigure()`, `appRequestRestart()`,
`appRequestFactoryReset()`.

## Boot sequence

`setup()`:

1. `Serial.begin(115200)`, `logInit()`
2. `stateInit()` — every field to its "nothing known yet" sentinel
3. `configLoad()` — mount LittleFS (format on first failure), migrate `/blledconfig.json`, read
   `/config.json`, validate
4. `logSetSerialEnabled(cfg().debug.serial)`
5. `indicatorSetup()`, `fanControlSetup()` (LEDC attach + write 0), `thermalSetup()`,
   `serialProvisionSetup()`
6. **One `fanControlLoop()` before the network comes up** — a hot printer should not wait for DHCP
7. `wifiSetup()` → `webServerSetup()` → `printerLinkStart()` → `haMqttSetup()`

## The main loop

Everything below is non-blocking, in this order:

```text
fanControlLoop()      // first, so its timing is never pushed around
indicatorLoop()
wifiLoop()            // DNS pump when the AP is up, STA state machine, scan reaping
serialProvisionLoop() // bounded to 256 bytes per pass
webServerLoop()       // 1 Hz SSE status push + new log lines (max 8 per pass)
haMqttLoop()
ssdpLoop()
configLoopSave()      // writes a dirty config at most every 10 s
<start SSDP / rebuild the HA configuration_url on the first STA connect>
<execute a pending restart / factory reset>
delay(1)              // feed the idle task
```

`fanControlLoop()` runs first deliberately: it recomputes at most every 100 ms, and putting it last
would let a slow web handler jitter the control period.

## One status document

`buildStatus()` in `status.cpp` produces **one** JSON object. `GET /api/status` returns it, the SSE
stream pushes it every second, and the external MQTT client publishes it retained to `<base>/state`.
There is no second serialiser to drift out of sync.

→ [The status object](rest-api.md#status-object)

!!! warning "ArduinoJson stores `const char*` by pointer"
    Only `char*` (non-const), `String` and `std::string` are *copied* into the document. Assigning a
    field of a **local** `const` struct — a `PrinterState` snapshot, say — and serialising after the
    function has returned produces garbage. `status.cpp` and `config.cpp` wrap every char-array field
    in `String()` for that reason. This was a real bug, fixed in 2.0.1.

## Deferred saves

`configMarkDirty()` marks the config dirty; `configLoopSave()` writes it at most **every 10 seconds**.
`POST /api/fan` and the MQTT fan/mode commands use this path, because a slider can produce dozens of
writes a second and flash has a finite number of them.

Explicit config, curve, WiFi and restore saves are written **inline** — an explicit save is deliberate
and should be durable the moment it returns.
