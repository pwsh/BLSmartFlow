---
title: Web flasher
---

# Web flasher

Flash the current BLSmartFlow firmware straight from this page — no toolchain, no downloads. The
button below uses [ESP Web Tools](https://esphome.github.io/esp-web-tools/) and the firmware image that
is published together with this site, so it always matches the documentation you are reading.

!!! warning "This is the one step that needs a USB cable"
    The first install of 2.x **cannot** be done over the air from a 2025.x firmware (see
    [Flashing the firmware](flashing.md)). Plug the board into this computer with a **USB data cable**.
    Later updates go [over the air](../using/updating.md) from the device's System page.

## Requirements

* **Chrome or Edge** on a desktop computer (Web Serial is not available in Firefox or Safari, nor on phones).
* A USB *data* cable. If no serial port appears in the list, the cable is the usual culprit.
* On Linux, your user must be allowed to open the port (`sudo usermod -aG dialout $USER`, then log out and in).

## Install

<div class="flasher" markdown>

<script type="module" src="https://unpkg.com/esp-web-tools@10/dist/web/install-button.js?module"></script>
<esp-web-install-button manifest="../../firmware/manifest.json">
  <button slot="activate" class="md-button md-button--primary">Connect and install BLSmartFlow</button>
  <span slot="unsupported">Your browser does not support Web Serial. Use Chrome or Edge on a desktop computer, or the <a href="../flashing/#route-b-esptool-on-the-command-line">esptool route</a>.</span>
  <span slot="not-allowed">This page must be opened over HTTPS for Web Serial to work.</span>
</esp-web-install-button>

</div>

1. Click **Connect and install**, pick the serial port of the board (`CP210x`, `CH340`, `USB Serial`, `wchusbserial…`).
2. Choose **Install BLSmartFlow**. When it offers to **erase the device, say yes** — that removes the old
   2025.x partition layout and settings.
3. Wait for *Installation complete*. Do not unplug the board before that.
4. Continue with [First-time setup](first-setup.md): the board now broadcasts the `BLSmartFlow-xxxx` setup network.

??? info "What the flasher writes"
    The manifest points at the **merged** image (`bootloader + partition table + application`) written at
    offset `0x0`. It is the same file listed on the [Flashing](flashing.md) page. The image published with
    this site is built by the repository's CI from the branch the site is generated from; the version is shown
    in the button dialog.

## Manual download

If you prefer a local tool: the current image is available at
[`firmware/latest.bin`](https://pwsh.github.io/BLSmartFlow/firmware/latest.bin) (merged image for offset `0x0`) and the
[`manifest.json`](https://pwsh.github.io/BLSmartFlow/firmware/manifest.json) describes it. Then follow
[Route B — esptool](flashing.md#route-b-esptool-on-the-command-line).
