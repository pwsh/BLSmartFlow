# Recovery

Ways back in, from least to most drastic.

## 1. Forgotten UI password

Basic auth applies to the UI and every API route — but **not to requests arriving on the setup
network**.

- If the device **cannot reach your WiFi**, the setup network is up: join `BLSmartFlow-xxxx` and open
  `http://192.168.4.1/`. No password is asked, and you can turn *Require login* off from there.
- If the device **is happily on your WiFi**, the setup network is not running, so use the serial
  route below.

## 2. Factory reset from the UI

*System → Maintenance → Factory reset*. Type `RESET` in capitals to unlock the button.

It wipes the configuration — WiFi, printer details, curve, filament overrides, MQTT settings, learned
cooling rates — and restarts into setup-network mode.

## 3. Serial provisioning

If the browser route fails entirely — no WiFi at all, a forgotten password, or a device you want to
pre-configure on the bench — you can talk to the module over USB at **115200 baud**.

### With the WebSerial page

Open `docs/wifiSetup.html` from the repository in **Chrome or Edge**, click *Select BLSmartFlow*, pick
the serial port, fill in the fields and press *Send Configuration*. The page listens for the device's
`IP_ADDRESS:` line and shows the address once it has joined.

### By hand

Open any serial terminal at 115200 baud and send **one line of JSON**:

```json
{"ssid":"Workshop-WiFi","pass":"MyPassword","printerip":"192.168.1.42","printercode":"12345678","printerserial":"01P00A123456789"}
```

Every key is optional; whatever you send is saved and the device restarts after 1 s.

You can also send a **full configuration document**, with the same schema and merge rules as
`POST /api/config`:

```json
{"config":{"fan":{"minSpeed":20},"mqtt":{"enabled":true,"host":"192.168.1.10"}}}
```

### Commands

| Line | Effect |
|---|---|
| `{"cmd":"status"}` | Replies `{"fw","chipId","hostname","ssid","wifi","ip","printerConnected","fanOutput"}` |
| `{"cmd":"restart"}` | Restart after 500 ms |
| `{"cmd":"factoryreset"}` | Wipes the configuration and restarts into setup-network mode |

Every line is answered with `{"ok":true,"msg":"…"}` or `{"ok":false,"error":"…"}`, so
`{"ok":true,…}` means it was understood. Unknown verbs answer `unknown cmd`; malformed lines answer
`invalid json`.

!!! note
    If you turned *Serial log* off on the System page, the device stops printing log lines but still
    accepts these commands. Lines longer than 2048 bytes are discarded with a warning, and at most
    256 bytes are consumed per loop pass.

## 4. Full reflash over USB

The last resort, and the only cure for a device that will not boot.

```sh
esptool --chip esp32 --port <port> erase-flash
esptool --chip esp32 --port <port> --baud 921600 write-flash 0x0 BLSmartflow_2.0.2.bin
```

This erases everything, including the configuration partition, and leaves the device broadcasting
`BLSmartFlow-xxxx` again.

→ [Flashing the firmware](../getting-started/flashing.md)

!!! tip "Bootloader mode"
    If the board will not respond, hold **BOOT**, tap **RESET**, release **BOOT**, then start the
    flash. If `921600` baud fails partway through, drop to `460800` or `115200`.

## Restoring your settings afterwards

If you took a [backup](../using/backup-security.md), *System → Backup & restore → Restore from file*
puts everything back in one step — curve, printer details, filament overrides, learned cooling rates.

Restore replaces the **whole** configuration and merges onto **defaults**, so restore a complete
backup rather than a fragment. A backup with no `wifi.ssid` is refused, because restoring it would
strand the device.

---

Related: [Backup and security](../using/backup-security.md) ·
[Web UI troubleshooting](web-ui.md)
