# Updating the firmware

Once 2.x is running, updates go **over the air** from the browser. No cable, no lost settings.

![The System page: device info, firmware update, backup and restore, web access, diagnostics and maintenance](../img/ui-system.png)

## The OTA update

1. Download the **`.bin.ota`** image for the new version — for example
   `BLSmartflow_2.0.2.bin.ota`.
2. Open **System → Firmware update**, choose the file and press **Upload & flash**.
3. Watch the progress bar. **Do not power the device off during the update.**
4. The device restarts by itself when the upload succeeds, and the page reconnects on its own.

!!! danger "Which file?"
    The over-the-air updater wants the **plain application image**, `BLSmartflow_<version>.bin.ota`.

    The merged `BLSmartflow_<version>.bin` contains the bootloader and partition table as well and is
    only for USB or web flashing at offset `0x0`. They are not interchangeable.

!!! success "Measured on hardware"
    A 2.0.x OTA is about **1.36 MB and takes roughly 13 seconds** over WiFi, after which the device
    reboots into the other app slot. If yours takes very much longer, the WiFi signal is the usual
    reason.

## Why it is safe

The new image is written to the **inactive OTA slot**, not over the running firmware. So:

- A failed or interrupted upload leaves the running firmware untouched — worst case, the device
  reboots into the version you already had.
- A client that disconnects mid-upload also aborts the update, which releases the partition
  immediately rather than at the next boot.
- **Your settings are kept.** The configuration lives on a separate LittleFS partition that an OTA
  never touches.

*System → Device* reports `Free OTA space` and the running `Partition` (`app0` or `app1`), which is
the quickest way to check how much headroom a slot still has.

## Coming from 2025.x

!!! warning "The very first 2.x install cannot be an OTA"
    The flash partition layout changed, and an OTA image cannot rewrite the partition table. A device
    running the 2025.x firmware **must be flashed once over USB** — and that flash erases the stored
    settings.
    → [Flashing the firmware](../getting-started/flashing.md)

    After that one flash, every later update is an OTA like the one above.

## From the command line

```sh
curl -F firmware=@BLSmartflow_2.0.2.bin.ota http://blsmartflow.local/api/update
```

Add `-u user:password` if you have turned on [web access](backup-security.md#a-password-for-the-ui).
A failure comes back as `500 {"error":"update failed: …"}` with the reason.

## Before you update

Take a **backup** from *System → Backup & restore* first. It costs nothing and it is the only copy of
your curve, your overrides and your learned cooling rates that lives off the device.
→ [Backup and security](backup-security.md)

---

Technical detail: [Partitions and OTA](../technical/partitions-and-ota.md)
