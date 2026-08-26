# REST API

ESPAsyncWebServer on port **80**. All `/api/*` responses are `application/json`; errors are
`{"error":"…"}` with a 4xx or 5xx status.

JSON request bodies go through `AsyncCallbackJsonWebHandler` with a **4096-byte limit**. The legacy
1.x routes take form-encoded input instead.

!!! info "Authentication"
    When `web.authEnabled` is on **and** `web.password` is non-empty, every route requires HTTP Basic
    auth — **except requests that arrive on the setup-AP interface**, which are never challenged.

    A failed check returns `401` with `WWW-Authenticate: Basic realm="BLSmartFlow"`. Add
    `-u user:password` to the curl examples below.

    The AP bypass is deliberate and has real consequences —
    see [security](wifi-and-captive-portal.md#security).

Examples below assume:

```sh
H=http://blsmartflow.local
```

---

## Status and diagnostics

| Method | Path | Body | Response |
|---|---|---|---|
| `GET` | `/` , `/index.html` | — | The gzipped UI, `Cache-Control: no-cache` |
| `GET` | `/api/status` | — | The [status object](#status-object) |
| `GET` | `/api/events` | — | `text/event-stream` — see [SSE](#server-sent-events) |
| `GET` | `/api/info` | — | Build and partition information |
| `GET` | `/api/log` | — | `{"lines":[…]}`, up to 64 lines |
| `GET` | `/api/filaments` | — | The embedded guide table, `Cache-Control: public, max-age=86400` |

```sh
curl $H/api/status
curl -N -H 'Accept: text/event-stream' $H/api/events
curl $H/api/info
curl $H/api/log
```

**`GET /api/info`** returns
`{"fw","build","chipId","sdk","flashSize","sketchSize","freeSketchSpace","partition","resetReason"}`.
`resetReason` is one of `UNKNOWN, POWERON, EXT, SW, PANIC, INT_WDT, TASK_WDT, WDT, DEEPSLEEP,
BROWNOUT, SDIO`.

**`GET /api/log`** lines look like `[   1234] [E] mqtt: connect failed` — a 7-column uptime in
milliseconds, then `I`/`W`/`E`, then the message.

**`GET /api/filaments`** returns the whole [Filament Field Guide](filament-matching.md) table the
firmware carries — about 16 KB, fetched once by the UI to populate the material pickers:

```json
{ "count": 90,
  "source": "https://pwsh.github.io/filament-field-guide",
  "licence": "CC BY 4.0",
  "filaments": [
    { "id": "abs", "name": "ABS", "cls": "styrenic",
      "cMin": 40, "cRec": 50, "cMax": 60, "cool": 0,
      "vent": "required", "flags": 1, "voc": "high", "part": "high" }
  ] }
```

`cMin` / `cRec` / `cMax` are the ambient band in °C and `cool` the recommended part-cooling
percentage; all four are `null` where the guide has no figure. `flags` is the bit field from
`FilamentInfo` (1 enclosure recommended, 2 heated chamber required, 4 enclosure open for cooling,
8 hardened nozzle).

---

## Configuration

| Method | Path | Body | Response |
|---|---|---|---|
| `GET` | `/api/config` | — | Full config, secrets masked |
| `POST` | `/api/config` | Partial config (deep merge) | `{"ok":true,"restartRequired":bool,"config":{…masked}}` |

```sh
curl $H/api/config
curl -X POST -d '{"fan":{"minSpeed":20,"kickStart":true}}' $H/api/config
```

- Applies live: `fan`, `mqtt`, `debug`, `ssdp`, `web` and `cooldown` take effect immediately;
  `printer` triggers `printerLinkReconfigure()`.
- **Only a changed `wifi.*` key sets `restartRequired`.**
- Everything else is **clamped** by `configValidate()`, never rejected.

| Status | Error |
|---|---|
| `400` | `access code must be exactly 8 characters` — a non-masked `printer.accessCode` of any other length |
| `400` | `expected a json object` — the body is not an object |
| `500` | `could not save configuration` — the LittleFS write failed |

!!! note "Masked secrets round-trip"
    `wifi.password`, `printer.accessCode`, `mqtt.password` and `web.password` serialise as
    `"********"` everywhere except `GET /api/backup` and the on-disk file. On input, **a value
    consisting only of `*` means "leave unchanged"** and is never length-checked. That lets the UI
    round-trip a form without ever learning the stored secret.

Full key list: [configuration reference](../reference/configuration.md).

---

## Fan curve

| Method | Path | Body | Response |
|---|---|---|---|
| `GET` | `/api/curve` | — | `{"points":[{"temp","speed"},…],"source":"nozzle"}` |
| `PUT` | `/api/curve` | `{"points":[…]}` | `{"ok":true,"points":[…]}` — the stored, normalised curve |

```sh
curl $H/api/curve
curl -X PUT -d '{"points":[{"temp":0,"speed":0},{"temp":250,"speed":100}]}' $H/api/curve
```

Saved **inline** — an explicit curve edit is deliberate. Temperatures are clamped to 0–400 and speeds
to 0–100, so out-of-range input returns `200` with adjusted points. `400` only for:

| Error | Cause |
|---|---|
| `missing 'points' array` | No `points` array in the body |
| `too many points (max 16)` | More than 16 points |
| `each point needs numeric temp and speed` | A non-numeric `temp` or `speed` |
| `need at least 2 points with distinct temperatures` | Fewer than 2 usable points after normalisation |

---

## Fan control

| Method | Path | Body | Response |
|---|---|---|---|
| `POST` | `/api/fan` | `{"mode":"auto"｜"chamber"｜"manual"｜"off","speed":0..100,"durationSec":0}` | `{"ok":true,"fan":{…}}` — the `fan` section of the status object |

```sh
curl -X POST -d '{"mode":"manual","speed":60,"durationSec":600}' $H/api/fan
curl -X POST -d '{"mode":"auto"}' $H/api/fan
```

- Every field is optional; omitting `mode` keeps the current one.
- `speed` outside 0–100 is **not** clamped: `400 {"error":"speed must be 0..100"}`.
- An unknown `mode`: `400 {"error":"mode must be auto, manual or off"}`.
- `durationSec > 0` with `mode: "manual"` creates a temporary override — **not persisted**, clamped
  to 86400 s. `durationSec == 0` persists mode and speed through the **deferred** save.

---

## Post-print cool-down

| Method | Path | Body | Response |
|---|---|---|---|
| `POST` | `/api/cooldown` | `{"start":true｜false}` | `{"ok":true,"cooldown":{…}}` |

```sh
curl -X POST -d '{"start":true}'  $H/api/cooldown
curl -X POST -d '{"start":false}' $H/api/cooldown
curl -s $H/api/status | jq .cooldown
```

`400 {"error":"printer is busy"}` when a print is running — that is, phase ∈ {`preheat`, `printing`,
`paused`}. The session settings themselves live under `cooldown.*` in `POST /api/config`.

→ [Post-print cool-down](post-print-cooldown.md)

---

## Network

| Method | Path | Body | Response |
|---|---|---|---|
| `GET` | `/api/wifi/scan` | — | `202 {"scanning":true}` while a scan runs, else `{"networks":[…]}` |
| `POST` | `/api/wifi` | `{"ssid","password","bssid","lockBssid","hostname"}` | `{"ok":true,"restartRequired":true}`, then restart |

```sh
curl "$H/api/wifi/scan?force=1"     # start a fresh scan -> 202
curl $H/api/wifi/scan               # poll -> 202 or the list
curl -X POST -d '{"ssid":"Workshop-WiFi","password":"secret"}' $H/api/wifi
```

A scan result entry is `{"ssid","bssid","rssi","channel","secure"}`.

- Results are sorted by RSSI descending, deduped by SSID (strongest BSSID wins), hidden networks
  dropped, BSSIDs upper-case, **2.4 GHz only** — the radio has no other band.
- A result younger than **20 s** is served from cache. `?force=1` (any value but `0`) discards the
  cache and starts a new scan, so it always answers `202`. The UI sends `force=1` on an explicit
  *Scan* click and then polls without it.
- `POST /api/wifi` requires a non-empty `ssid` (`400 {"error":"ssid is required"}`), reuses the config
  merge (so `"********"` keeps the stored password) and saves inline. It restarts after **1 s in AP
  mode**, **1.5 s** otherwise.

---

## Backup, restore, maintenance

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
instead of wiping them.

A restore whose `wifi.ssid` ends up empty is refused with `400 {"error":"backup has no wifi.ssid"}`
rather than stranding the device. `/api/factoryreset` without `{"confirm":true}` returns
`400 {"error":"send {\"confirm\":true}"}`.

!!! danger
    `GET /api/backup` returns **every secret in clear text** — WiFi password, printer access code,
    broker password, UI password.

---

## OTA

| Method | Path | Body | Response |
|---|---|---|---|
| `POST` | `/api/update` (alias `/update`) | `multipart/form-data`, any file field name | `{"ok":true}` then restart after 1 s, or `500 {"error":"update failed: …"}` |

```sh
curl -F firmware=@BLSmartflow_2.0.2.bin.ota $H/api/update
```

The image is written to the inactive OTA slot with `Update.begin(UPDATE_SIZE_UNKNOWN)`. A failed
chunk aborts the update (which releases the partition) and the reason is reported in the response; a
client that disconnects mid-upload also triggers an abort, so a dead upload cannot claim the OTA
partition for the rest of the boot.

→ [Partitions and OTA](partitions-and-ota.md)

---

## Legacy 1.x endpoints

Kept so 1.x tooling and bookmarks keep working.

| Method | Path | Body | Response |
|---|---|---|---|
| `GET` | `/getOptions` | — | `{"firmwareversion","ip","code","id","staticfans","staticfanspeed","debuging","debugingchange","mqttdebug"}` |
| `POST` | `/submitOptions` | form: `ip, code, serial, staticfan, staticfanspeed, debuging, mqttdebug` | `{"ok":true}` |
| `GET` | `/getFanConfig` | — | Same payload as `GET /api/curve` |
| `POST` | `/updateFanConfig` | form field `points`: `{"points":[…]}` or a bare `[…]` | `{"ok":true}` |
| `GET` | `/sensorData` | — | `{"temp":<nozzle, 2 dp, 0 when unknown>,"speed":<fan output %>}` |
| `POST` | `/update` | multipart | Alias of `POST /api/update` |

- `code` and `id` are **obfuscated**: every character but the last three becomes `*` (values of three
  characters or fewer are returned unchanged). On the way back in, a value containing a `*` is treated
  as "unchanged".
- `staticfans` = `fan.mode == "manual"`; `staticfanspeed` = `fan.manualSpeed`;
  `debuging` = `debug.serial`; `mqttdebug` = `debug.mqttDump`.
- `staticfan=on` sets `fan.mode = "manual"`, anything else sets `"auto"`. Values are validated and
  **saved** — the 1.x bug where `/submitOptions` never persisted is fixed.
- `/sensorData` always reports the **nozzle**, whatever the curve source is, and `0` when unknown:
  existing dashboards parse it that way.

---

## Captive-portal probes

These paths answer `302 → http://192.168.4.1/` **only for requests that arrive on the AP interface**;
on the station interface they are a normal `404`.

```text
/generate_204  /gen_204  /hotspot-detect.html  /library/test/success.html
/connecttest.txt  /ncsi.txt  /fwlink  /redirect  /success.txt  /canonical.html
/check_network_status.txt  /chat
```

Any other unmatched path gets the same treatment through the not-found handler. `HTTP_OPTIONS`
requests answer `200`. Redirects carry `Cache-Control: no-cache, no-store, must-revalidate`,
`Pragma: no-cache` and `Expires: 0` so an OS cannot cache its portal verdict.

---

## Server-sent events

`GET /api/events` is `text/event-stream`:

| Event | Payload | Cadence |
|---|---|---|
| `status` | The full [status object](#status-object) | Every second, plus immediately on connect |
| `log` | One log line | As lines appear, at most 8 per loop pass |

!!! warning "The `Accept` header is mandatory"
    `AsyncEventSource` answers **404** to any client that does not send
    `Accept: text/event-stream`. Browsers send it automatically; `curl /api/events` without
    `-H 'Accept: text/event-stream'` is not a valid test.

At most **4 concurrent clients**. A fifth connection is closed immediately, and that page falls back
to polling `/api/status` every 2 seconds. The stream is authorised with the same rules as the rest of
the API.

The UI also polls in AP mode, keeps polling until the stream delivers its first event, and falls back
to polling after 5 seconds of silence — some captive-portal mini browsers open an `EventSource` that
never delivers anything.

---

## Status object

The same document is returned by `GET /api/status`, pushed as the SSE `status` event, and published
retained to `<base>/state`.

```json
{
  "device":  { "fw":"2.0.2", "uptimeSec":123, "heapFree":123456, "heapMin":100000,
               "chipId":"a1b2c3", "hostname":"blsmartflow", "ip":"192.168.1.50",
               "apMode":false },
  "wifi":    { "connected":true, "ssid":"Workshop-WiFi", "bssid":"02:00:5E:10:20:30",
               "rssi":-61, "channel":6 },
  "printer": { "configured":true, "connected":true, "online":true, "lastUpdateSec":2,
               "mqttState":0, "mqttStateText":"connected", "state":"RUNNING",
               "printing":true, "stage":0, "stageText":"printing", "progress":42,
               "remainingMin":87, "layer":12, "totalLayers":210,
               "task":"Bracket_v3.3mf", "phase":"printing",
               "doorOpen":false, "doorKnown":true, "doorEdgeCount":2,
               "printError":0, "wifiSignal":"-45dBm", "lastCommandError":null,
               "temps": { "nozzle":220.4, "nozzleTarget":220, "bed":60.1, "bedTarget":60,
                          "chamber":38.0, "chamberTarget":45.0 },
               "fans":  { "part":100, "aux":0, "chamber":40, "heatbreak":100 } },
  "fan":     { "output":55, "target":55, "mode":"auto", "effectiveMode":"auto",
               "source":"nozzle", "sourceTemp":220.4, "setpoint":null,
               "chamberTarget":45, "cooldownTarget":35, "manualSpeed":50,
               "manualExpiresSec":0, "pwmDuty":140, "output1":true, "output2":true },
  "filament":{ "source":"ams", "auto":true,
               "tray": { "ams":0, "slot":0, "type":"ABS", "subBrand":null,
                         "idx":"GFB00", "color":"FFFFFFFF" },
               "id":"abs", "name":"ABS", "family":"ABS",
               "profile": { "chamberRec":50, "chamberMax":60, "partCoolRec":0,
                            "vent":"required", "openForCooling":false,
                            "heatedRequired":false },
               "effective": { "chamberTarget":50, "cooldownTarget":35, "ventFloor":10,
                              "postPrintCooling":"gentle", "overridden":false },
               "trays":[ { "ams":0, "slot":0, "type":"ABS", "subBrand":null,
                           "idx":"GFB00", "color":"FFFFFFFF", "id":"abs" },
                         { "ams":-1, "slot":254, "type":"ASA", "idx":"GFB01",
                           "id":"asa" } ] },
  "thermal": { "rateCPerMin":-0.42, "kClosed":[0.31,null,null,null,null],
               "kOpen":[null,null,null,null,null], "samples":7 },
  "cooldown":{ "active":false, "reason":"target", "target":35, "chamber":41.2,
               "startChamber":52, "elapsedSec":0, "maxSec":1800,
               "printerFans": { "aux":0, "chamber":0, "sent":false, "error":null },
               "ownFan":"thermostat", "material":"abs" },
  "mqttExt": { "enabled":true, "connected":true }
}
```

### Field reference

| Field | Meaning |
|---|---|
| `device.heapFree` / `heapMin` | Current and all-time-minimum free heap, in bytes |
| `device.ip` | STA address, or `192.168.4.1` in AP-only mode, or `0.0.0.0` |
| `device.apMode` | The setup AP is currently up. It can be up while the station is also connected |
| `wifi.*` | The **station** link. `ssid`/`bssid` are `""` and `rssi`/`channel` are `0` when not associated |
| `printer.configured` | IP, access code and serial are all set |
| `printer.connected` | The MQTT session is up |
| `printer.online` | Session up **and** data younger than `staleSec` |
| `printer.mqttState` / `mqttStateText` | PubSubClient state code and its text (`connected`, `unauthorized`, `bad_credentials`, `connection_lost`, …) |
| `printer.state` | Raw `gcode_state` |
| `printer.phase` | Derived phase: `offline｜paused｜preheat｜cooling｜printing｜finished｜failed｜idle`. `printer.printing` is true for `preheat`/`printing`/`paused` |
| `printer.stageText` | ha-bambulab stage name for `stg_cur` |
| `printer.doorOpen` | Front-door switch — `true`/`false`, or **`null`** while `doorKnown` is false. The top lid has no sensor |
| `printer.doorKnown` | An open/close edge has been observed, so the bit can be trusted. Everything door-driven is inert until then |
| `printer.doorEdgeCount` | Transitions seen since boot; the first report is state, not an edge |
| `printer.lastCommandError` | Why the printer refused the last command **we** sent, or `null`. `"mqtt message verify failed"` means Developer Mode is off on the printer. Cleared by the next accepted command |
| `cooldown.printerFans.error` | `"rejected: <reason>"` while the printer is refusing the `M106`, else `null` |
| `printer.temps.chamberTarget` | The printer's own chamber set point, `null` on machines without a chamber heater |
| `fan.output` / `target` | Rounded percent actually driven / requested by the active mode |
| `fan.effectiveMode` | `off｜manual｜stale｜door｜preheat｜idle｜cooldown｜chamber｜auto` |
| `fan.setpoint` | The thermostat set point in force right now, `null` outside `chamber`/`cooldown` |
| `fan.chamberTarget` / `cooldownTarget` | The **configured** set points, so the HA number entities have a state to read back |
| `fan.manualExpiresSec` | Seconds left on a timed override, `0` when there is none |
| `fan.pwmDuty` | The 0–255 byte written to the pin, **already inverted** when `pwmInvert` is on |
| `filament.source` | `ams｜external｜manual｜last｜none` — where the active material came from. **`last`** means the tray has been unloaded and the device is still using the material of the print that just ended |
| `filament.auto` | Mirrors `filament.auto` from the config, so the UI can grey the card without a second fetch |
| `filament.tray` | The active tray, or **`null`** when the printer has not described one. `ams` is `-1` and `slot` `254` for the external holder; every string field is `null` when empty |
| `filament.id` / `name` | The matched guide entry, `null` when the guide has no entry for this material |
| `filament.family` | What the printer called it, normalised — `"PA-GF"` even when `id` is `pa`. `null` when nothing is loaded |
| `filament.profile` | The guide's figures, or `null` when unmatched. `chamberRec`/`chamberMax`/`partCoolRec` are `null` individually where the guide has no figure |
| `filament.effective` | What the fan controller is actually using this second. `overridden` is true when a `filament.overrides` rule changed at least one of them |
| `filament.trays` | Every tray the printer has described, empty slots omitted |
| `thermal.rateCPerMin` | Current chamber slope in °C/min (negative = cooling), `null` until the fan output and door have been steady for ~20 s |
| `thermal.kClosed` / `kOpen` | Learned cooling constants in 1/min for fan buckets 0/25/50/75/100 %; `null` where nothing has been measured — **never** NaN |
| `thermal.samples` | Number of windows blended into the table so far |
| `cooldown.active` | A cool-down session is running |
| `cooldown.reason` | Why the last session ended: `target｜timeout｜newJob｜stopped｜linkLost｜disabled`, `null` if none has ended yet |
| `cooldown.target` / `chamber` / `startChamber` | The effective target, the chamber now, and the chamber when the session started |
| `cooldown.elapsedSec` / `maxSec` | Session age and the hard stop |
| `cooldown.printerFans` | `{aux, chamber, sent}` — the percentages requested from the printer, and whether anything was ever sent |
| `cooldown.ownFan` | `thermostat｜max｜curve` |
| `cooldown.material` | The guide id the session is cooling for, `null` when unknown |
| `mqttExt` | External broker: configured-and-enabled, and currently connected |

### Null rules

- **Unknown temperatures are `null`.** So is every unmeasured entry in `thermal`. NaN must never reach
  the JSON — `serializeJson` would emit a bare `null` for a float NaN anyway, but the status builder
  makes it explicit.
- `printer.stage`, `progress`, `remainingMin`, `layer` and `totalLayers` are `null` until the printer
  reports them — **never** `-1` or `0`.
- Printer fan percentages are `null` when unknown.
- `printer.lastUpdateSec` is `null` until the very first report ever arrives. The UI renders that as
  *never*.
- `printer.doorOpen` is `null` until an edge has proved the switch.
