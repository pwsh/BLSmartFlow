# What you need

Before you flash anything, collect the hardware and the three values the device needs from your
printer. It takes five minutes and saves an hour.

## Hardware

| Item | Notes |
|---|---|
| A **BLSmartFlow module** | An ESP32 with two PWM fan outputs on GPIO 17 and GPIO 16 and a status LED on GPIO 21. See [Hardware](../technical/hardware.md) for the pinout. |
| A **power supply** for the module and the fan(s) | As specified for your SmartFlow hardware. |
| A **USB data cable** | Many cheap USB cables are charge-only. A charge-only cable will not show the board to your computer at all — no port appears, nothing to select. |
| A **2.4 GHz WiFi network** | The ESP32 has no 5 GHz radio. A 5 GHz-only network cannot be used and will not even appear in the device's scan list. |

<div class="grid" markdown>
![Render of the SmartFlow fan module housing](../img/smartflow_shell.png)
![Render of the SmartFlow fan blades](../img/smartflow_blades.png)
</div>

## On the printer: LAN Only Mode

BLSmartFlow talks to the printer over the printer's **own local MQTT broker**. Nothing goes through
Bambu's cloud, and the printer will not accept the connection unless local access is enabled.

On the printer's screen, open **Settings → Network** and switch on:

- **LAN Only Mode**, and
- **Developer Mode**, which recent Bambu firmware additionally requires for local MQTT.

!!! warning "The access code changes"
    Every time LAN Only Mode is switched off and on again, the printer generates a **new** access
    code. If the printer link stops working after you touched the network settings, re-read the code.

## The three values

| Value | Where to find it | Notes |
|---|---|---|
| **IP address** | Printer screen → *Settings → Network* | For example `192.168.1.42`. Give the printer a **fixed DHCP lease** in your router: if the address changes, the printer link stops working. |
| **Access code** | Printer screen → *Settings → Network → LAN Only Mode* | Exactly **8 characters**. This is the MQTT password; the device refuses any other length. |
| **Serial number** | The printer's device-information screen (also shown in Bambu Studio and Handy) | For example `01P00A123456789`. It forms the MQTT topic, so a single wrong character means no data at all. Lower-case letters are upper-cased for you. |

Write all three down now — you will type them on the *Printer* page in a few minutes.

## Supported printers

X1 / X1C / X1E, P1P / P1S, A1 / A1 mini, and **H2D on a best-effort basis**.

!!! info "Chamber temperature"
    Only **X1- and H2D-style** machines report a chamber temperature. On a P1 or an A1 the chamber
    reading stays empty, the [chamber thermostat](../using/fan-modes.md#chamber-thermostat) quietly
    falls back to the curve, and you should use the nozzle or the bed as the curve source.

    Chamber support on the X1 also depends on the printer's firmware: current X1C firmware no longer
    sends the old `chamber_temper` field, and BLSmartFlow reads the packed `device.ctc` block
    instead. See [Printer link](../technical/printer-link.md#field-decoding).

## A word on terms

Throughout this site:

Device
:   The BLSmartFlow module itself — the ESP32 board that drives your fan.

Setup network
:   The open `BLSmartFlow-xxxx` WiFi network the device raises when it has no credentials, or after
    90 seconds of failing to reach your WiFi. It runs a captive portal.

Printer link
:   The TLS MQTT session from the device to your printer. Separate from, and unrelated to, the
    optional [external MQTT broker](../using/home-assistant.md) link.

Effective mode
:   What the fan controller is *actually* doing this second, which can differ from the fan mode you
    selected — a door rule, a preheat rule or the stale-data failsafe all take priority. The
    dashboard and the *Fan mode* card show it as a `now: …` badge.

---

**Next:** [Flashing the firmware](flashing.md)
