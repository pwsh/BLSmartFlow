# BLSmartFlow

**BLSmartFlow is a small ESP32 board that drives one or two fans from your Bambu Lab printer's own
temperatures.** It reads the printer over the printer's local MQTT link, looks the temperature up in
a curve you draw in your browser, and sets the fan speed. It can also work the other way round —
hold the enclosure at a temperature you pick and adjust the fan until it gets there — and it knows to
leave the fan alone while the printer is warming up or the door is open.

Everything is configured from a web page served by the device itself. No app, no cloud account, no
internet connection.

![The BLSmartFlow dashboard during a print: fan gauge at 68 %, manual override, live temperatures and print job](img/ui-dashboard-printing.png)
/// caption
The dashboard during a print. It updates once a second over a server-sent event stream.
///

## What it does

- **Temperature-driven fan curve** — 2 to 16 points, drag-editable in the browser, with presets. The
  source can be the nozzle, the bed, the chamber, or the hottest of the three.
- **Chamber thermostat** — instead of guessing a curve, hold the enclosure at a set temperature with
  a PI controller that measures the result and corrects itself.
- **Filament-aware cooling** — the device reads what is loaded in the AMS or on the external spool
  and moves the chamber target, the cool-down style and the ventilation floor to suit the material.
- **Printer state rules** — stop the fan while the printer is preheating (it would be fighting the
  heaters) and while the front door is open (there is nothing to exhaust).
- **Safe by default** — a stale-data failsafe when the printer stops reporting, an "only while
  printing" gate with a post-print cool-down, and manual overrides that expire on their own.
- **Home Assistant and MQTT** — optional link to your own broker with auto-discovery: a fan entity,
  a mode select, temperature and print sensors, command topics.
- **Everything the UI does, over REST** — `/api/*` JSON endpoints plus a live event stream, so you
  can script it from anything.
- **Setup without a computer** — first boot raises a `BLSmartFlow-xxxx` setup network with a captive
  portal; later firmware updates go over the air from the System page.

## Get started in 5 steps

1. **Flash over USB — once.** The first install of 2.x *must* go over a USB data cable; the 2025.x
   firmware cannot update to it over the air. Use the browser web flasher or `esptool`, write the
   merged `BLSmartflow_2.0.2.bin` at offset `0x0`, and tick *erase*.
   → [Flashing the firmware](getting-started/flashing.md)
2. **Join the setup network.** The device raises the open `BLSmartFlow-xxxx` network. The setup page
   opens by itself; if it does not, browse to <http://192.168.4.1/>. Enter your 2.4 GHz WiFi and
   save. → [First-time setup](getting-started/first-setup.md)
3. **Reconnect and open the UI.** Put your phone or laptop back on your own network and open
   <http://blsmartflow.local/> (or the IP your router gave the device).
4. **Add the printer.** On the *Printer* page enter the printer's IP address, its 8-character LAN
   access code and its serial number, with **LAN Only Mode** (and Developer Mode) enabled on the
   printer. → [Connecting the printer](getting-started/connect-printer.md)
5. **Shape the curve** — or switch to the chamber thermostat — and save.
   → [Your first fan curve](getting-started/first-fan-curve.md)

!!! tip "The whole install, in order"
    Flash over USB → join the setup network → save WiFi → reopen at `blsmartflow.local` → enter the
    printer details → check the dashboard → pick a fan mode. The
    [first-time setup page](getting-started/first-setup.md) ends with a checklist covering all of it.

## Where to go next

<div class="grid cards" markdown>

- :material-rocket-launch: **New here?**

    Start with [what you need](getting-started/what-you-need.md), then flash and set up.

- :material-tune: **Already running?**

    Learn the [fan modes](using/fan-modes.md), the [printer state rules](using/printer-state-rules.md)
    and [filament-aware cooling](using/filament-aware-cooling.md).

- :material-code-braces: **Integrating?**

    The [REST API](technical/rest-api.md), the [MQTT contract](technical/mqtt-and-home-assistant.md)
    and the [configuration schema](reference/configuration.md).

- :material-help-circle: **Something wrong?**

    [Troubleshooting](troubleshooting/wifi.md) is organised by symptom: WiFi, printer link, door,
    fan behaviour, web UI, recovery.

</div>

## Supported printers

X1 / X1C / X1E, P1P / P1S, A1 / A1 mini, and H2D on a best-effort basis — all in **LAN Only Mode**.
Only X1- and H2D-style machines report a chamber temperature, which the chamber thermostat needs.

---

*This site documents BLSmartFlow 2.0.x. Firmware and documentation under
[CC BY-NC-SA 4.0](reference/credits-and-license.md); material data from the
[Filament Field Guide](https://github.com/pwsh/filament-field-guide) under CC BY 4.0.*
