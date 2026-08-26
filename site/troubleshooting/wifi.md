# Troubleshooting: WiFi

## `BLSmartFlow-xxxx` does not appear

**Why.** The [setup network](../getting-started/first-setup.md) only starts when **no** WiFi
credentials are stored, or after **90 seconds of continuous failure** to reach the stored network. A
device that is happily on your WiFi will never broadcast it.

**Fix.** Check your router's client list for `blsmartflow` — it is probably already connected. To
force setup mode, [factory reset](recovery.md) from the UI or over serial.

## The setup page does not open by itself

**Why.** The phone's captive-portal probe went out over HTTPS, which the device cannot intercept, or
the OS cached "this network is fine" from a previous session.

**Fix.** Browse to **`http://192.168.4.1/`** by hand — plain `http://`, not `https://`.

## The phone leaves the setup network after a few seconds

**Why.** The setup network has no internet, and phones auto-switch away from such networks.

**Fix.**

- **Android:** choose *Stay connected* on the "no internet access" prompt, and/or turn mobile data
  off for the two minutes the setup takes.
- **iOS:** use the login sheet that appears when you join. Never tap *Cancel* — that tells iOS to
  abandon the network.

## Saved WiFi, but the device never connects

**Why.** Almost always one of two things: a **5 GHz-only network**, or a wrong password.

**Fix.** The ESP32 is **2.4 GHz only** and a 5 GHz-only SSID never even appears in the scan list. If
your router publishes one SSID for both bands, make sure 2.4 GHz is enabled on it. Otherwise re-enter
the password — the setup network comes back 90 s after the failure begins, so you get another go.

Watch the status LED: **2 blinks** means "credentials stored, not connected".

## Slow to come back after a router reboot

**Why.** The device retries with a growing backoff — 5 s, 10 s, 20 s, 30 s, then 60 s — and each
attempt has a 20 s timeout.

**Fix.** Nothing to do; it will reconnect. If it was locked to one access point, the lock is dropped
automatically after **three** failed cycles.

## The open setup network is suddenly broadcasting again

**Why.** The WiFi has been unreachable for 90 seconds or more, so the device raised the setup network
while it keeps retrying in the background.

**Fix.** Fix the WiFi. Once the device rejoins, the setup network closes **5 minutes later** on its
own — the delay exists so you are not cut off mid-page.

!!! warning
    While the setup network is up, **authentication is bypassed on it**. Anyone in radio range can
    read the backup and reflash the device. → [Security](../technical/wifi-and-captive-portal.md#security)

## A mesh network keeps dropping the device

**Why.** Roaming between access points.

**Fix.** On the *Network* page, switch on **Lock to this access point**. That binds the device to one
BSSID and speeds up reconnects. If that access point is later switched off, the lock is dropped
automatically after three failed cycles, so it cannot lock you out permanently.

## Weak signal

*Network → Current connection* shows the RSSI. Better than **−60 dBm** is excellent, **−70 dBm** is
still fine, below **−80 dBm** expect dropouts.

---

Related: [First-time setup](../getting-started/first-setup.md) ·
[WiFi and the captive portal](../technical/wifi-and-captive-portal.md) ·
[Web UI troubleshooting](web-ui.md)
