# Partitions and OTA

## The partition table

**`min_spiffs.csv`** on a 4 MB flash:

| Region | Size | Contents |
|---|---|---|
| `app0` | ~1.875 MB | OTA slot A |
| `app1` | ~1.875 MB | OTA slot B |
| `spiffs` | ~128 KB | LittleFS — `/config.json` |
| nvs, otadata, phy_init | small | Bootloader bookkeeping |

The configuration file is well under 4 KB, so the small filesystem is ample. `GET /api/info` reports
`sketchSize`, `freeSketchSpace` and the running `partition` label, which is the quickest way to check
how much headroom an app slot still has.

## The one full flash

!!! danger "Upgrading from 2025.x needs one USB flash"
    The partition layout changed, and **an OTA image cannot rewrite the partition table**. A device
    running the 2025.x firmware must be flashed once over USB or with the web flasher, at offset
    `0x0`. That flash **erases the stored settings**.

    Every later update can be an OTA.

→ [Flashing the firmware](../getting-started/flashing.md)

## The two images

| File | What it is | Written to |
|---|---|---|
| `BLSmartflow_<version>.bin` | **Merged**: bootloader + partition table + application | Offset `0x0`, over USB |
| `BLSmartflow_<version>.bin.ota` | Plain application image | The inactive OTA slot, over HTTP |

Both are produced by `merge_firmware.py` at build time.
→ [Packaging](building-and-testing.md#packaging-merge_firmwarepy)

## How an OTA runs

1. `POST /api/update` (alias `/update`) takes `multipart/form-data` with any file field name.
2. The image is written with `Update.begin(UPDATE_SIZE_UNKNOWN)` into the **inactive** slot.
3. On success the device answers `{"ok":true}` and restarts after 1 s; the bootloader then boots the
   slot that was just written.

Failure handling:

- A failed chunk **aborts** the update, which releases the partition, and the reason is returned as
  `500 {"error":"update failed: …"}`.
- A client that **disconnects mid-upload** also triggers an abort — so a dead upload cannot claim the
  OTA partition for the rest of the boot.
- Either way the running firmware is untouched. Worst case, the device reboots into the version it
  already had.

!!! success "Measured on hardware"
    A 2.0.x OTA is about **1.36 MB and takes roughly 13 seconds** over WiFi, after which the device
    reboots into the other app slot.

## Configuration survives

`/config.json` lives on the LittleFS partition, which an OTA never touches. Curve, printer details,
filament overrides and the learned cooling-rate table all come back after an update.

They do **not** survive a full USB flash with erase. Take a
[backup](../using/backup-security.md) first.

## Writing the configuration safely

`/config.json` is written **atomically**: serialise to `/config.tmp`, then rename over
`/config.json`. A file that fails to parse at boot is moved to `/config.bad` and defaults are used —
so a power cut during a save costs you your settings, never your ability to boot.

---

Related: [Updating the firmware](../using/updating.md) · [REST API — OTA](rest-api.md#ota)
