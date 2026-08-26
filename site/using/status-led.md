# The status LED

One LED on GPIO 21 tells you the most important problem first: **WiFi beats the printer link, and the
printer link beats stale data.**

| Pattern | Meaning | What to do |
|---|---|---|
| **Solid on** | Everything is fine. | — |
| **Solid, with two short dips every 3 s** | A [manual override](manual-override.md) is active; the fan is not following the curve. | Press *Back to auto* when you are done. |
| **1 blink**, pause | No WiFi credentials stored, or the [setup network](../getting-started/first-setup.md) is up. | Join `BLSmartFlow-xxxx` and configure WiFi. |
| **2 blinks**, pause | WiFi credentials exist but the device is not connected. | Check the password, the band (2.4 GHz only) and the signal. → [WiFi troubleshooting](../troubleshooting/wifi.md) |
| **3 blinks**, pause | WiFi is up, but the printer's MQTT session is down. | Check the *Printer* page: IP, access code, serial, LAN Only Mode. → [Printer link troubleshooting](../troubleshooting/printer-link.md) |
| **4 blinks**, pause | Connected to the printer, but no fresh data for longer than the stale window. | Check that the printer is awake and reporting. |

A blink pattern is *N* × 200 ms on/off followed by an 800 ms gap, so you can count the blinks between
pauses. The patterns are driven from `millis()` and never block anything else.

!!! tip "Reading it in one glance"
    Count the blinks. **More blinks = further along the chain**: 1 is "no network at all", 2 is
    "network configured but unreachable", 3 is "network fine, printer unreachable", 4 is "printer
    reachable but silent".

More detail: [LED patterns](../troubleshooting/led-patterns.md) ·
[Hardware](../technical/hardware.md)
