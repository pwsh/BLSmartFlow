# Hardware

## Pinout

| Function | GPIO | Details |
|---|---|---|
| Fan output 1 | **17** | LEDC PWM, `pwmFreq` (default 25 kHz), 8-bit resolution |
| Fan output 2 | **16** | Same duty as output 1; can be disabled independently |
| Status LED | **21** | Non-blocking `millis()` patterns |

Both fan outputs always carry the **same duty cycle**. Output 2 exists so a second fan can be driven
without a splitter, not so it can run at a different speed.

## PWM

The Arduino-ESP32 3.x API is used: `ledcAttach(pin, freq, resolution)` and `ledcWrite(pin, duty)` —
the channel and the timer are chosen by the core rather than pinned by hand.

- **Duty is 8-bit:** `duty = round(output% * 255 / 100)`.
- Inverted at the pin when `pwmInvert` is set: `255 − duty`. A disabled output gets `0`, or `255` when
  inverted.
- A duty write is **skipped when the value has not changed**.
- Changing `pwmFreq` detaches and re-attaches **both** pins.

| `pwmFreq` | When |
|---|---|
| **25000 Hz** (default) | Above hearing. Silent on good 4-pin and quality 2-pin fans. |
| 1000–8000 Hz | Some cheap fans and MOSFET driver boards prefer this. Audible, but it may be the only thing that works. |
| Range | 500–40000 Hz |

!!! tip "Invert PWM"
    Only for driver boards that expect an **active-low** signal. The symptom is unmistakable: the fan
    runs at full speed when the UI says 0 %.

Note that `fan.pwmDuty` in the [status object](rest-api.md#status-object) is **post-inversion** — it
is the byte the pin actually sees, so with `pwmInvert` an output of 0 % reports `255`.

## Status LED

Patterns, **highest priority first**:

| Pattern | Meaning |
|---|---|
| 1 blink | No WiFi credentials, or the setup AP is up |
| 2 blinks | WiFi down |
| 3 blinks | Printer MQTT down |
| 4 blinks | Printer data stale |
| Solid with two short dips every 3 s | Manual mode |
| Solid | OK |

A blink pattern is *N* × 200 ms on/off followed by an 800 ms gap. Everything is driven from `millis()`
in `indicatorLoop()`; nothing blocks. (The 1.x firmware used `delay()` here and could spend two
seconds per loop iteration in an error state.)

→ [The status LED](../using/status-led.md) · [LED patterns](../troubleshooting/led-patterns.md)

## Board and flash

| Property | Value |
|---|---|
| Board | `esp32dev` |
| Radio | **2.4 GHz only** — the ESP32 has no 5 GHz radio |
| Flash | 4 MB |
| Partitions | `min_spiffs.csv` — two ~1.875 MB OTA app slots, ~128 KB LittleFS |
| Filesystem | LittleFS, holding `/config.json` (well under 4 KB) |
| Serial | 115200 baud |

→ [Partitions and OTA](partitions-and-ota.md)
