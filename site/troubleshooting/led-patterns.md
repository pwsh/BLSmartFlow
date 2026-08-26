# LED patterns

One LED on GPIO 21 reports the **most important problem first**. The priority order is:

```text
setup AP / no credentials  >  WiFi down  >  printer MQTT down  >  data stale  >  manual  >  OK
```

A blink pattern is *N* × 200 ms on/off followed by an **800 ms gap**, so you can count the blinks
between pauses.

| Pattern | Meaning | Where to look |
|---|---|---|
| **Solid on** | Everything is fine | — |
| **Solid, two short dips every 3 s** | [Manual override](../using/manual-override.md) active | Press *Back to auto* on the dashboard |
| **1 blink** | No WiFi credentials, or the [setup network](../getting-started/first-setup.md) is up | [WiFi troubleshooting](wifi.md) |
| **2 blinks** | Credentials stored, but not connected | [WiFi troubleshooting](wifi.md) |
| **3 blinks** | WiFi up, printer MQTT down | [Printer link troubleshooting](printer-link.md) |
| **4 blinks** | Printer connected, but no fresh data for longer than the stale window | [Printer link troubleshooting](printer-link.md) |

## Reading it

**More blinks means further along the chain.**

- **1** — no network at all. The device is waiting for you on `BLSmartFlow-xxxx`.
- **2** — network configured but unreachable. Wrong password, 5 GHz-only SSID, or out of range.
- **3** — your network is fine; the printer is not answering. Wrong IP, wrong access code, LAN Only
  Mode off, or the printer is asleep.
- **4** — the printer answered but has gone quiet. Usually a wrong **serial number**: the MQTT
  session connects, but the device is subscribed to a topic nothing publishes to.

That last distinction is the useful one. **3 blinks is a connection problem; 4 blinks is a topic
problem.**

## Nothing at all

If the LED never lights, the board has no power, or the firmware is not running. Connect USB and watch
the serial port at 115200 baud — the boot banner appears within a few milliseconds of reset.

→ [Recovery](recovery.md)

---

Related: [The status LED](../using/status-led.md) · [Hardware](../technical/hardware.md)
