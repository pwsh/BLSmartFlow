# WiFi and the captive portal

A non-blocking state machine driven from `loop()`. The device owns reconnection itself —
`WiFi.setAutoReconnect(false)` — because the Arduino core's own retry logic is opaque and cannot be
coordinated with raising the setup AP.

## The state machine

```text
IDLE ──beginConnect()──> CONNECTING ──link up──> CONNECTED
  ^                          │                      │
  └────── backoff ───────────┘ 20 s timeout         └── link lost -> IDLE (retry at once)
```

## Timings

| Constant | Value | Meaning |
|---|---|---|
| Connect timeout | 20 s | Per attempt |
| Backoff | 5 → 10 → 20 → 30 → 60 s | Doubles up to 30 s, then 60 s; reset on success |
| Drop BSSID lock | after 3 failed cycles | A locked radio that is gone must not lock the device out |
| Raise setup AP | after **90 s** of *continuous* failure | Cleared by any successful connect, so a link that flaps hourly never ends up broadcasting |
| STA retry while the AP is up | at least every 60 s | The AP does not stop reconnection attempts |
| AP linger | 5 min | After the station connects while the AP is up |
| AP restart backoff | 5 s | After a failed `softAP()` |
| Scan cache TTL | 20 s | See [`GET /api/wifi/scan`](rest-api.md#network) |

Radio setup: `WiFi.persistent(false)`, `setAutoReconnect(false)`, `setSleep(false)`, TX power
19.5 dBm, hostname from `wifi.hostname`.

!!! note "Why 90 seconds, and why *continuous*"
    Raising an open access point is a security event, so it should not happen because the router
    rebooted. Ninety seconds of unbroken failure is long enough to rule that out, and the counter is
    reset by **any** successful connect — a link that drops for a minute every hour never broadcasts.

## The setup AP

| Property | Value |
|---|---|
| SSID | `BLSmartFlow-<chipid>` |
| Security | **Open** |
| Address | `192.168.4.1/24` |
| DNS | `DNSServer` on port 53, answering **every** name with `192.168.4.1` |
| Mode | `WIFI_AP_STA` when credentials exist (so the station keeps retrying), `WIFI_AP` when they do not |

With no credentials at all, the AP starts immediately at boot.

## Portal probes

Operating systems detect a captive portal by fetching a known URL and checking the answer. The device
serves the plaintext ones and answers `302 → http://192.168.4.1/`:

```text
/generate_204  /gen_204  /hotspot-detect.html  /library/test/success.html
/connecttest.txt  /ncsi.txt  /fwlink  /redirect  /success.txt  /canonical.html
/check_network_status.txt  /chat
```

Any other unmatched path gets the same treatment through the not-found handler. Redirects carry
`Cache-Control: no-cache, no-store, must-revalidate`, `Pragma: no-cache` and `Expires: 0` so an OS
cannot cache its portal verdict from a previous session.

!!! warning "HTTPS probes cannot be intercepted"
    Some Android builds probe over TLS. The device can only answer the plaintext probes it serves, so
    the portal sheet sometimes has to be opened by hand at `http://192.168.4.1/`. This is not
    fixable from the device's side.

## Which interface did a request arrive on?

`onApInterface()` compares the request socket's **local IP** with `WiFi.softAPIP()`.

That single comparison is what distinguishes the phone on the open portal from the rest of the LAN
while the device is running AP+STA — and it is what gates both the portal redirects and the
authentication bypass.

## mDNS

`<hostname>.local`, service `_http._tcp` on port 80, TXT records `model=BLSmartFlow` and
`version=<fw>`. It is restarted after a reconnect; a failure is **logged, never fatal**. (The 1.x
firmware hung in `while(1)` when `MDNS.begin()` failed.)

Windows and some Android versions do not resolve `.local` reliably — use the IP address instead.
→ [Web UI troubleshooting](../troubleshooting/web-ui.md)

## SSDP (optional)

Built with `-DBLSF_SSDP` and enabled by `ssdp.enabled`, the device advertises itself over UPnP/SSDP
so it appears in Windows' Network view.

!!! note "Known limitation"
    SSDP advertises `description.xml`, but the web server does not serve that path, so a discovery
    client that fetches the schema URL gets a 404. Discovery is optional and off-path.

## Security

State it plainly.

- **The setup AP is open by design.** The whole point is that a phone can join it with no shared
  secret. It exposes only the local setup UI, and it is only up when the device has no credentials or
  has been unable to reach WiFi for 90 s.
- **Requests arriving on the AP interface skip authentication entirely.** `authorisedQuiet()` returns
  true for them before it ever checks the password. That includes `POST /api/update` (OTA),
  `POST /api/restore`, `POST /api/factoryreset` and `GET /api/backup` — i.e. **anyone within radio
  range of a device that is in setup mode can read the secrets and reflash it.**

    This is a deliberate trade: locking the user out of the one interface they can reach when WiFi is
    broken would be a dead end. Requests on the station interface are always challenged.

- **Basic auth is not encrypted.** There is no TLS on the device's own web server. It keeps casual
  visitors out of a trusted LAN, nothing more.
- Auth is only enforced when `web.authEnabled` **and** a non-empty `web.password` are stored;
  `configValidate()` turns `authEnabled` off when the password is empty, so it cannot be armed with
  no way in.
- **`GET /api/backup` returns every secret in clear text** — WiFi password, printer access code,
  broker password, UI password.
- The [printer link](printer-link.md) uses `setInsecure()`: encrypted but unauthenticated.
- The external broker link is **plain TCP**; broker credentials cross the LAN in clear.
- The web UI loads **no external resources at all** — no CDN, no fonts, no remote images — so it works
  in AP mode and leaks nothing to third parties.

---

Related: [First-time setup](../getting-started/first-setup.md) ·
[WiFi troubleshooting](../troubleshooting/wifi.md) ·
[Backup and security](../using/backup-security.md)
