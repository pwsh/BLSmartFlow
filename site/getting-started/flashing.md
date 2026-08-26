# Flashing the firmware

!!! danger "The first install of 2.x must go over a USB cable"
    A device running the old **2025.x** firmware **cannot** be updated to 2.x over the air. Two
    things stand in the way: the flash **partition layout changed** (an OTA image cannot rewrite the
    partition table), and the 2025.x firmware never served a working update page in the first place.

    So the first time, you plug the board into a computer with a USB cable and write the whole flash.
    **This erases the stored settings** — write your WiFi password and printer details down first.
    After this one flash, every later update is [over the air](../using/updating.md) from the System
    page.

## What you need

- The board, and a **USB data cable**. Charge-only cables carry power but no data: the board simply
  never appears as a serial port. If no port shows up, suspect the cable before anything else.
- A desktop computer. Phones cannot flash an ESP32.
- The firmware image for your version:

| File | What it is | Where it goes |
|---|---|---|
| `BLSmartflow_2.0.2.bin` | **Merged** image: bootloader + partition table + application | USB / web flasher, at offset **`0x0`** |
| `BLSmartflow_2.0.2.bin.ota` | Plain application image | The [OTA updater](../using/updating.md) on the System page only |

Substitute the version you actually downloaded for `2.0.2` throughout this page — the file names
follow `custom_version` in `platformio.ini`.

They are **not** interchangeable. Writing the `.ota` file at offset `0x0` produces a board that does
not boot; uploading the merged file to the OTA page is rejected or wastes the slot.

## Route A — the browser web flasher (recommended)

1. Use **Chrome or Edge on a desktop computer**. Firefox and Safari cannot talk to serial ports at
   all, so the *Connect* button will do nothing there.
2. Open the **[Web flasher](web-flasher.md)** page of this documentation — it carries the firmware image that matches these docs. Any generic browser flasher such as [esptool.spacehuhn.com](https://esptool.spacehuhn.com/) also works: download `latest.bin` from the same page, erase, and program it at offset `0x0`. (The `esphome.html` file in the repository is the same button for local use; the DutchDevelop Universal-Flasher only lists the upstream 2025.x firmware.)
3. Plug the board in with the USB **data** cable.
4. Click **Connect** and pick the serial port that appears. It is usually named something like
   `CP210x`, `CH340`, `USB Serial` or `wchusbserial`.
5. Choose **Install**, and when it offers to **erase the device, say yes** — that is what clears the
   old 2025.x partition layout and settings.
6. Let it run to the end. **Do not unplug the board until it reports success.**

The flasher writes the merged image at offset `0x0` for you; you do not have to enter an address.

## Route B — `esptool` on the command line

Download `BLSmartflow_2.0.2.bin` (the merged image) from the release page, then:

```sh
pip install esptool
esptool --chip esp32 --port <port> --baud 921600 write-flash 0x0 BLSmartflow_2.0.2.bin
```

To wipe the old settings and partition table first — recommended when coming from 2025.x:

```sh
esptool --chip esp32 --port <port> erase-flash
esptool --chip esp32 --port <port> --baud 921600 write-flash 0x0 BLSmartflow_2.0.2.bin
```

=== "Linux"

    The port is usually `/dev/ttyUSB0` (CP210x/CH340) or `/dev/ttyACM0`.

    ```sh
    esptool --chip esp32 --port /dev/ttyUSB0 --baud 921600 write-flash 0x0 BLSmartflow_2.0.2.bin
    ```

    `Permission denied: '/dev/ttyUSB0'` means your user is not in the serial group:

    ```sh
    sudo usermod -a -G dialout $USER      # then log out and back in
    ```

    (On Arch and openSUSE the group is `uucp`.)

=== "Windows"

    The port is a `COM` number. Find it in *Device Manager → Ports (COM & LPT)*; the board shows up
    as *Silicon Labs CP210x* or *USB-SERIAL CH340*.

    ```powershell
    esptool --chip esp32 --port COM5 --baud 921600 write-flash 0x0 BLSmartflow_2.0.2.bin
    ```

    If no COM port appears at all, install the CP210x or CH340 USB-serial driver.

=== "macOS"

    The port is under `/dev/cu.*` — list them with `ls /dev/cu.*`. It is typically
    `/dev/cu.usbserial-0001` or `/dev/cu.SLAB_USBtoUART`.

    ```sh
    esptool --chip esp32 --port /dev/cu.usbserial-0001 --baud 921600 write-flash 0x0 BLSmartflow_2.0.2.bin
    ```

!!! tip "If the board will not enter the bootloader"
    Some boards need a nudge: hold **BOOT**, tap **RESET**, release **BOOT**, then start the flash.
    If `921600` baud fails partway through, drop to `--baud 460800` or `115200`.

## What you should see

The board reboots into the new firmware by itself. Open a serial monitor at **115200 baud**
(`pio device monitor`, `screen /dev/ttyUSB0 115200`, or the flasher's own *Logs* button) and the
first seconds look like this:

```text
[      6] [I] BLSmartFlow 2.0.2 (built 2026-08-26 12:00:00)
[      9] [I] chip a1b2c3, heap 243912 bytes
[     14] [I] no /config.json, using defaults
[     31] [W] wifi: no credentials, starting setup AP
[     52] [W] setup AP 'BLSmartFlow-a1b2c3' at 192.168.4.1
[     73] [I] web server listening on :80
```

The number in brackets is the uptime in milliseconds; `I` / `W` / `E` is the level. Two lines are the
ones that matter: `no /config.json, using defaults` (the erase worked) and `setup AP … at
192.168.4.1` (the device is waiting for you).

**The status LED** now blinks **once, pause, once, pause** — that is the "no WiFi credentials / setup
network is up" pattern. See [the status LED](../using/status-led.md) for the full table.

## After the flash

The device is now broadcasting the open `BLSmartFlow-xxxx` network and has no settings at all.

**Next:** [First-time setup over the setup network](first-setup.md)

---

## Later updates are over the air

Once 2.x is running, you never need the cable again for updates. Download the **`.bin.ota`** image,
open **System → Firmware update** in the browser and upload it — see
[Updating the firmware](../using/updating.md). The new image goes into the inactive OTA slot, so a
failed upload leaves the running firmware untouched.

The USB route remains the way back if anything goes badly wrong; see
[Recovery](../troubleshooting/recovery.md).
