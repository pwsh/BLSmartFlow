# BLSmartFlow 2.0 — User guide

BLSmartFlow is a small ESP32 board that drives one or two fans from your Bambu Lab printer's
temperatures. It reads the printer over the printer's own local MQTT link, looks the temperature up
in a curve you draw in your browser, and sets the fan speed. Everything is configured from a web
page on the device itself — no app, no cloud account.

This guide assumes you have never flashed an ESP32 before. Follow it top to bottom.

| Section | |
|---|---|
| [1. What you need](#1-what-you-need) | hardware, printer settings, the three printer values |
| [2. Flashing the firmware](#2-flashing-the-firmware) | web flasher or `esptool` |
| [3. First-time setup over the setup network](#3-first-time-setup-over-the-setup-network) | captive portal, WiFi credentials |
| [4. Connecting the printer](#4-connecting-the-printer) | IP, access code, serial, connection test |
| [5. Shaping the fan curve](#5-shaping-the-fan-curve) | points, presets, source, behaviour, safety |
| [6. Manual override](#6-manual-override) | take control from the dashboard |
| [7. Home Assistant in five minutes](#7-home-assistant-in-five-minutes) | broker, discovery, entities |
| [8. Updating the firmware](#8-updating-the-firmware) | OTA from the System page |
| [9. Backup, restore, restart, factory reset, login](#9-backup-restore-restart-factory-reset-login) | maintenance |
| [10. The status LED](#10-the-status-led) | what the blinks mean |
| [11. Troubleshooting](#11-troubleshooting) | symptom → cause → fix |
| [12. Serial provisioning (the fallback)](#12-serial-provisioning-the-fallback) | when the browser route fails |

---

## 1. What you need

**Hardware**

* A BLSmartFlow module (ESP32 with two PWM fan outputs on GPIO 17 and GPIO 16, status LED on GPIO 21).
* A power supply for the module and the fan(s) as specified for your SmartFlow hardware.
* A USB **data** cable for the one-off flash. Many cheap USB cables are charge-only and the board
  will simply not appear on your computer.
* A 2.4 GHz WiFi network. The ESP32 has no 5 GHz radio, so a 5 GHz-only network cannot be used and
  will not even show up in the scan list.

<p align="center">
  <img src="img/smartflow_shell.png" alt="Render of the SmartFlow fan module housing" width="45%">
  <img src="img/smartflow_blades.png" alt="Render of the SmartFlow fan blades" width="45%">
</p>

**On the printer**

The printer must allow local MQTT access: enable **LAN Only Mode** on the printer's screen under
*Settings → Network* (recent Bambu firmware additionally requires **Developer Mode** to be switched
on there). Without it the printer refuses the connection. You then need three
values:

| Value | Where to find it | Notes |
|---|---|---|
| **IP address** | Printer screen, *Settings → Network* | e.g. `192.168.1.42`. Give the printer a fixed DHCP lease in your router — if the address changes, the link stops working. |
| **Access code** | Printer screen, *Settings → Network → LAN Only Mode* | Exactly **8 characters**. This is the MQTT password. It changes every time LAN Only Mode is switched off and on again. |
| **Serial number** | Printer's device information screen (also shown in Bambu Studio / Handy) | e.g. `01P00A123456789`. It forms the MQTT topic, so a single wrong character means no data at all. Lower-case letters are upper-cased for you. |

Supported printers: X1 / X1C / X1E, P1P / P1S, A1 / A1 mini, and H2D on a best-effort basis.

---

## 2. Flashing the firmware

You only do this once. Afterwards, updates go over WiFi from the System page.

> **Details — upgrading from the 2025.x firmware.** 2.0 uses a different flash partition layout, so
> a device running the old firmware **must be flashed once over USB (or with the web flasher)**. An
> over-the-air update from 2025.x will not work. This full flash **erases the stored settings**, so
> write down your WiFi and printer details first. After this one flash, OTA updates work normally.

### Option A — the browser web flasher (recommended)

1. Use **Chrome or Edge on a desktop computer**. Firefox and Safari cannot talk to serial ports, and
   phones cannot do this at all.
2. Open the flasher page (the `esphome.html` page in this repository, or the install button on
   <https://www.dutchdevelop.com/blsmartflow>).
3. Plug the module into the computer with the USB data cable.
4. Click **Connect**, pick the serial port that appears (often named `CP210x`, `CH340` or
   `USB Serial`), and confirm.
5. Choose **Install** and let it run. Do not unplug the board until it reports success.

> **Details.** The flasher installs the *merged* image — bootloader, partition table and application
> in one file — written at offset `0x0`. When it offers to erase the device, say yes if you are
> coming from the 2025.x firmware.

### Option B — esptool on the command line

Download the merged image (`BLSmartflow_V<version>.bin`, the same file the flasher uses) from the
release page, then:

```sh
pip install esptool
esptool write_flash 0x0 BLSmartflow_V2.0.0.bin
```

Add `--port /dev/ttyUSB0` (Linux) or `--port COM5` (Windows) if more than one serial device is
attached.

> **Details — which file is which.** `BLSmartflow_V<version>.bin` is the full flash image and always
> goes to offset `0x0`. `BLSmartflow_V<version>.bin.ota` is the plain application image and is only
> for the over-the-air updater on the System page. They are not interchangeable.

---

## 3. First-time setup over the setup network

A device with no WiFi credentials starts its own open network — the **setup network** — and runs a
captive portal on it.

1. Power the module.
2. On a phone or laptop, open the WiFi list and join **`BLSmartFlow-xxxx`** (`xxxx` is the last
   digits of the device's chip ID). It is an **open network**; there is no password.
3. The setup page should open by itself. On Android and Windows the notification says something like
   *"Sign in to network"*; on iOS and macOS a sheet titled with the network name pops up. Tap it if
   it does not open on its own.
4. If nothing appears, open a browser and go to **<http://192.168.4.1/>** — note `http://`, not
   `https://`.

![The Network page in setup mode, with the access-point banner above the WiFi form](img/ui-setup-ap.png)
*Setup mode: the banner explains where you are, "Current connection" shows the access point and
192.168.4.1.*

5. Press **Scan** to list nearby 2.4 GHz networks and pick yours, or type the name by hand for a
   hidden network.
6. Enter the WiFi password, leave the hostname at `blsmartflow` unless you want to change it, and
   press **Save & restart**.
7. The device restarts and joins your network. **Reconnect your phone or laptop to your own WiFi.**
8. Open **<http://blsmartflow.local/>**. If that does not resolve, look the device up in your
   router's client list and use its IP address instead.

> **Callout — phones that drop a network with "no internet".** The setup network has no internet
> access, and modern phones like to leave such networks automatically.
> * **Android:** when the "network has no internet access" prompt appears, choose **Stay connected**
>   (some phones say "Yes"). If the phone keeps hopping away, switch mobile data off for the two
>   minutes the setup takes.
> * **iOS/iPadOS:** use the login sheet that pops up when you join. Do **not** tap *Cancel* — that
>   tells iOS to abandon the network. If you dismissed it, go to *Settings → Wi-Fi*, tap the network
>   again, or open `http://192.168.4.1/` in Safari.
> * **Windows/macOS:** the "sign in" browser window is a stripped-down browser. If it misbehaves,
>   close it and use a normal browser at `http://192.168.4.1/`.

![The Network page as seen from the LAN, showing the WiFi form and the current connection](img/ui-network.png)
*Once the device is on your network, the same page shows the live connection: SSID, IP address,
signal, channel and the mDNS name.*

> **Details — when does the setup network appear?** It comes up immediately when no WiFi credentials
> are stored, and otherwise only after **90 seconds of continuous failure** to reach the stored
> network. While it is up, the device keeps trying your WiFi in the background. Once it succeeds, the
> setup network stays alive for **5 more minutes** (so you are not cut off mid-page) and then closes.

---

## 4. Connecting the printer

1. Open the UI and go to the **Printer** page.
2. Enter the **IP address**, the **access code** (exactly 8 characters) and the **serial number**.
3. Pick the **model**. *Auto detect* is right for most setups; choosing *P1P / P1S* or *A1 / A1 mini*
   makes the device ask the printer for a full status refresh every 5 minutes, because those models
   only send changes.
4. Press **Save & reconnect**. No reboot is needed — the MQTT client restarts immediately.

![The Printer page with the connection form and the live status card](img/ui-printer.png)
*The "Live status" card is the connection test: if the temperatures move, the link works.*

The **Live status** card is the test. Within a few seconds you should see *MQTT state* go to
`connected`, *Last report* count in seconds, and real temperatures appear.

**Not connecting? Work down this list.**

- [ ] Is **LAN Only Mode / developer mode** still enabled on the printer?
- [ ] Is the **access code the current one**? It changes whenever LAN Only Mode is toggled.
- [ ] Is it exactly **8 characters**? Any other length is refused with a clear error.
- [ ] Does the **serial** match exactly, including the leading zeros?
- [ ] Is the **IP** still correct? Check your router; set a fixed lease.
- [ ] Is the printer on the **same network** as the device (not a guest VLAN)?
- [ ] MQTT state `unauthorized` or `bad_credentials` means the printer rejected the access code. The
      device then waits 60 seconds between attempts, so give it a minute after you fix it.

---

## 5. Shaping the fan curve

The **Fan curve** page maps one printer temperature (the *source*) to a fan speed.

![The Fan curve page with the drag-editable canvas and the point table](img/ui-curve.png)
*Drag a point to move it, click the line to insert one, or type exact numbers in the table below.*

### Editing points

* **Drag** a point with the mouse or a finger. Values snap to 1 °C and 1 %.
* **Click or tap the line** to insert a point there, or press **Add point** to split the widest gap.
* **Delete point** (or the <kbd>Delete</kbd> / <kbd>Backspace</kbd> key) removes the selected point.
* The **table** below the graph is the same curve — type exact numbers there if you prefer.
* A curve has **at least 2 and at most 16 points**. Temperatures run 0–400 °C, speeds 0–100 %.
* The dashed marker shows the live source temperature and the output it currently produces.
* The **0-100 °C / 0-400 °C** buttons only zoom the axis; they change nothing on the device.

Nothing is written to the device until you press **Save** in the bar at the bottom. **Revert**
throws your edits away and reloads what is stored.

### Presets

| Preset | What it does |
|---|---|
| **Quiet** | Keeps the fan off until 200 °C and rises gently — the quietest option. |
| **Balanced** | The factory curve: off below 50 °C, 50 % at 180 °C, 80 % at 245 °C, 100 % at 350 °C. Good for PLA/PETG. |
| **Aggressive** | Starts at 35 °C and reaches 100 % by 220 °C — maximum cooling. |
| **Chamber-ABS** | A 0–80 °C curve meant for chamber-temperature control. |

A preset only fills the editor; you still have to save it.

### Choosing the source

| Source | Use it when |
|---|---|
| **Nozzle** | The default. The fan cools something that tracks the hotend — part cooling, a hotend duct, electronics that heat up with the print. |
| **Bed** | The fan reacts to the heated bed. |
| **Chamber** | **Enclosure venting.** Use this if the fan's job is to keep the enclosure at a temperature — e.g. to vent an X1 chamber while printing PLA, or to keep it warm-but-not-too-hot for ABS. Pair it with the **Chamber-ABS** preset, because all the interesting values are below 60 °C. Only X1- and H2D-style printers report a chamber temperature; on other models it stays empty and the curve gets no reading. |
| **Hottest of all** | Takes the maximum of nozzle, bed and chamber. Use it for a general-purpose "something in there is hot" fan. |

### Control behaviour, in plain words

| Setting | What it means |
|---|---|
| **Hysteresis** (°C) | "Do not react to tiny wobbles." The curve is only recalculated once the source temperature has moved by at least this much. 1–3 °C stops the fan hunting up and down around a curve point. 0 turns the filter off. |
| **Ramp rate** (%/s) | "How fast may the speed change." 0 means jump straight to the new value. 20 %/s takes five seconds to go from stopped to full — quieter, and kinder to the bearings. |
| **Minimum speed** (%) | "Below this, just stop." Many fans buzz or stall instead of turning at very low duty. Set this just above the lowest speed at which your fan still spins reliably (often 15–25 % on small fans). Anything below it is output as 0 %. |
| **Kick-start** | "Give it a shove." When the fan starts from standstill it runs at 100 % for a short pulse (default 500 ms, useful range 300–800 ms) so it breaks away instead of sitting there humming. |

> **Details.** The kick pulse is only armed when the fan has genuinely been stopped for a couple of
> seconds, so a curve that dips briefly through zero cannot leave the fan pulsing.

### Safety and gating

| Setting | What it means |
|---|---|
| **Stale after** (s) | If no report arrives from the printer for this long, the temperature is treated as unusable. Default 120 s, range 10–3600 s. |
| **When data is stale** | **Off** stops the fan (safest for part cooling), **Hold** keeps the last speed (good when the fan cools electronics), **Fixed** runs at the speed you set below (good for chamber venting you never want to stop). |
| **Only while printing** | The curve runs only while the printer reports `RUNNING`, `PAUSE`, `PREPARE` or `SLICING`. Outside a print the fan switches off once the cooldown has elapsed. |
| **Cooldown** (min) | How long the curve keeps running after the print finishes, so a hot nozzle or chamber is still cooled. Default 10 minutes, range 0–1440. Only used when *Only while printing* is on. |

### Outputs

Output 1 (GPIO 17) and output 2 (GPIO 16) always carry the same duty cycle; you can disable either
one. **PWM frequency** defaults to 25000 Hz, which is above hearing. **Invert PWM** is for driver
boards that expect an active-low signal — turn it on only if your fan runs full speed when the UI
says 0 %.

---

## 6. Manual override

The **Manual override** card on the dashboard takes the fan away from the curve.

![The dashboard with the fan gauge, manual override card, temperatures and print job](img/ui-dashboard.png)
*The dashboard updates once per second.*

1. Drag **Speed** to the percentage you want.
2. Pick a **Duration**:
   * *Until changed (persisted)* — the manual speed survives a reboot, until you switch back.
   * A timed option — the device returns to the curve on its own when the timer expires. Timed
     overrides are deliberately not persisted, so a reboot ends them.
3. Press **Apply manual**.

**Back to auto** returns to curve control. **Fan off** forces both outputs to 0 % and keeps them
there until you change the mode again.

On a phone the same controls sit in a single column with the navigation as a bottom tab bar:

<p align="center">
  <img src="img/ui-mobile-dashboard.png" alt="The dashboard on a 390 pixel wide phone screen" width="45%">
  <img src="img/ui-mobile-curve.png" alt="The fan curve editor on a 390 pixel wide phone screen" width="45%">
</p>

---

## 7. Home Assistant in five minutes

The device can talk to your own MQTT broker (the Mosquitto add-on, for example) and announce itself
to Home Assistant automatically. This is completely separate from the printer link.

![The Integrations page with the broker form, topic list and REST API reference](img/ui-integrations.png)

1. Go to **Integrations → External MQTT broker**.
2. Switch **Enabled** on and fill in the broker **host** (e.g. `homeassistant.local` or its IP) and
   **port** (`1883`). Add a **username/password** if your broker requires one. This link is plain
   TCP — no TLS.
3. Leave **Base topic** empty unless you want a custom prefix; empty means
   `blsmartflow/<chipid>`, which is unique per device.
4. Leave **Home Assistant discovery** on and the **discovery prefix** at `homeassistant` (change it
   only if you changed it in the HA MQTT integration).
5. Press **Save & connect**. The badge on the card turns green when the broker accepts the
   connection.
6. In Home Assistant, open *Settings → Devices & services → MQTT*. A device called
   **BLSmartFlow &lt;chipid&gt;** appears within a few seconds.

What you get:

| Entity | Type | What it does |
|---|---|---|
| Fan | `fan` | On/off and a percentage — this is the manual override. |
| Mode | `select` | `auto` / `manual` / `off`. |
| Nozzle / Bed / Chamber temperature | `sensor` | °C, from the printer. Shows *Unknown* when the printer does not report it. |
| Fan output | `sensor` | The device's own fan output, in %. |
| Printer state, Printer stage | `sensor` | e.g. `RUNNING`, `heatbed_preheating`. |
| Print progress, Remaining time | `sensor` | % and minutes. |
| Printer WiFi, Device RSSI, Uptime | `sensor` | Diagnostics. |
| Printer online, Door, Printing | `binary_sensor` | Connectivity, door open, print running. |
| Restart | `button` | Reboots the device. |

> **Details.** Turning discovery off again publishes empty discovery messages, which removes the
> entities from Home Assistant cleanly. Setting the fan from Home Assistant puts the device into
> manual mode — remember to set Mode back to `auto` to hand control back to the curve.

You do not need Home Assistant to use MQTT: the same status document is published to
`<base>/state`, and the command topics under `<base>/` work from any MQTT client. The
**MQTT topics** card lists every topic with a copy button, and the full contract is in
[TECHNICAL.md](TECHNICAL.md#mqtt--home-assistant-reference).

---

## 8. Updating the firmware

![The System page with device info, firmware update, backup, web access and maintenance cards](img/ui-system.png)

1. Download the **`.bin.ota`** image for the new version.
2. Open **System → Firmware update**, choose the file and press **Upload**.
3. Watch the progress bar. **Do not power the device off during the update.**
4. The device restarts by itself when the upload succeeds. The page reconnects on its own.

> **Details — which file?** The over-the-air updater wants the plain application image,
> `BLSmartflow_V<version>.bin.ota`. The merged `BLSmartflow_V<version>.bin` contains the bootloader
> and partition table as well and is only for USB / web flashing at offset `0x0`. The new image is
> written to the *inactive* OTA slot, so a failed or interrupted upload leaves the running firmware
> untouched — worst case, the device reboots into the version you already had.

---

## 9. Backup, restore, restart, factory reset, login

All of these live on the **System** page.

**Backup** downloads the whole configuration as a JSON file.

> **Callout.** The backup contains your **WiFi password and printer access code in plain text**.
> Keep the file somewhere safe; do not paste it into a forum post.

**Restore** uploads such a file, replaces the entire configuration with it and restarts. Keys the
file does not contain fall back to their defaults, so restore a complete backup, not a fragment. A
backup that has no WiFi network name in it is refused, because restoring it would strand the device.

**Restart** reboots the device. The fan outputs drop to 0 % for a few seconds while it comes back.

**Factory reset** wipes the configuration and restarts into setup-network mode. Type `RESET` in
capitals to unlock the button. Everything goes: WiFi, printer details, curve, MQTT settings.

**Web access** turns on HTTP basic authentication for the UI and every API route. Set a username
(default `admin`) and a password; it takes effect on the very next request.

> **Details — login safety net.** Basic auth is *not* encrypted; it keeps casual visitors out of a
> trusted LAN, nothing more. Requests that arrive over the **setup network** are never asked for a
> password, so a device you can reach on its own hotspot is a way back in. If you forget the password
> and the device is happily on your WiFi (so the setup network is not running), the way back is the
> serial command `{"cmd":"factoryreset"}` over USB — see [section 12](#12-serial-provisioning-the-fallback).

---

## 10. The status LED

The LED on GPIO 21 shows the most important problem first: WiFi beats the printer link, and the
printer link beats stale data.

| Pattern | Meaning | What to do |
|---|---|---|
| **Solid on** | Everything is fine. | — |
| **Solid with two short dips every 3 s** | Manual override is active; the fan is not following the curve. | Press *Back to auto* when you are done. |
| **1 blink**, pause | No WiFi credentials stored, or the setup network is up. | Join `BLSmartFlow-xxxx` and configure WiFi. |
| **2 blinks**, pause | WiFi credentials exist but the device is not connected. | Check the password, the band (2.4 GHz only) and the signal. |
| **3 blinks**, pause | WiFi is up, but the printer's MQTT session is down. | Check the printer page: IP, access code, serial, LAN mode. |
| **4 blinks**, pause | Connected to the printer, but no fresh data for longer than the stale window. | Check that the printer is awake and reporting. |

---

## 11. Troubleshooting

| Symptom | Likely cause | Fix |
|---|---|---|
| `BLSmartFlow-xxxx` does not appear | The device already has working WiFi credentials — the setup network only starts when none are stored, or after 90 s of continuous failure | Check your router's client list for `blsmartflow`. To force setup mode, factory-reset from the UI or over serial |
| The setup page does not open by itself | The phone's probe went out over HTTPS, or the OS cached "this network is fine" | Browse to `http://192.168.4.1/` manually — plain `http://` |
| The phone leaves the setup network after a few seconds | "No internet" auto-switching | Android: choose *Stay connected* and/or turn mobile data off. iOS: use the login sheet, never *Cancel* |
| Saved WiFi, device never connects | 5 GHz-only network, or a wrong password | The ESP32 is 2.4 GHz only. Re-enter the password; the setup network comes back 90 s after the failure begins |
| Slow to come back after a router reboot | The device retries with a growing back-off (5 s, 10 s, 30 s, then 60 s), 20 s per attempt | Nothing to do; it will reconnect. If it was locked to one access point, the lock is dropped automatically after three failed cycles |
| Was fine, now the open setup network is broadcasting | The WiFi has been unreachable for 90 s or more; the device raises the setup network while it keeps retrying | Fix the WiFi. Once the device rejoins, the setup network closes 5 minutes later on its own |
| `http://blsmartflow.local/` does not load | mDNS is not resolved by every Windows and Android setup | Use the device's IP address from your router. The hostname is also on the Network page under *Current connection* |
| "access code must be exactly 8 characters" | The LAN access code is 8 characters, always | Re-read it from the printer's *LAN Only Mode* screen |
| MQTT state `unauthorized` / `bad_credentials` | Wrong access code, or LAN mode was toggled and the code changed | Enter the current code. After a rejection the device waits **60 s** before trying again, so wait a minute |
| Temperatures show `--`, mode shows **stale** | No printer report for longer than the stale window (printer off, wrong serial, wrong IP) | Check the printer link. Meanwhile the failsafe you configured (off / hold / fixed) is driving the fan |
| Chamber temperature stays empty | Only X1- and H2D-style printers report a chamber temperature | Use *Nozzle*, *Bed* or *Hottest of all* as the curve source |
| The fan never starts | Minimum speed is above what the curve asks for, or kick-start is off and the fan cannot break away | Lower **Minimum speed**, turn **Kick-start** on, or raise the curve |
| The fan whines, buzzes or ticks | PWM carrier frequency does not suit the fan or the driver board | Keep **PWM frequency** at 25000 Hz for good 4-pin and 2-pin fans; some cheap fans and MOSFET boards prefer 1000–8000 Hz |
| The fan runs full speed when the UI says 0 % | The driver board is active-low | Turn **Invert PWM** on |
| The fan stops as soon as a print finishes | *Only while printing* is on and the cooldown has elapsed | Raise **Cooldown**, or turn *Only while printing* off |
| The dashboard stops updating in one tab | The device accepts at most 4 live event streams at once; extra tabs are refused | Close spare tabs. A refused tab falls back to polling every 2 seconds |
| Forgotten UI password | Basic auth applies to everything on your LAN | Send `{"cmd":"factoryreset"}` over USB serial (section 12), or reach the device on the setup network, where no password is asked |

---

## 12. Serial provisioning (the fallback)

If the browser route fails — no WiFi at all, a forgotten password, or a device you want to
pre-configure on the bench — you can talk to the module over USB.

**With the WebSerial page.** Open `docs/wifiSetup.html` from this repository in **Chrome or Edge**,
click *Select BLSmartFlow*, pick the serial port, fill in the fields and press *Send Configuration*.
The page listens for the device's `IP_ADDRESS:` line and shows the address once it has joined.

**By hand.** Open any serial terminal at **115200 baud** and send **one line of JSON**:

```json
{"ssid":"MyNetwork","pass":"MyPassword","printerip":"192.168.1.42","printercode":"12345678","printerserial":"01P00A123456789"}
```

Every key is optional; whatever you send is saved and the device restarts. Three commands are also
accepted, one per line:

| Command | Effect |
|---|---|
| `{"cmd":"status"}` | Prints firmware version, chip ID, hostname, SSID, IP, printer link and fan output as JSON |
| `{"cmd":"restart"}` | Reboots the device |
| `{"cmd":"factoryreset"}` | Wipes the configuration and restarts into setup-network mode |

> **Details.** If you turned *Serial log* off on the System page, the device stops printing log lines
> but still accepts these commands. Every command is answered with a small JSON object, so
> `{"ok":true,...}` means it was understood.

---

Developers and integrators: the REST API, the MQTT contract and the internals are documented in
[TECHNICAL.md](TECHNICAL.md). The design record is [REWORK-SPEC.md](REWORK-SPEC.md).
