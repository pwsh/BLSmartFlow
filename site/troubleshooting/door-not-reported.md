# Troubleshooting: "Door: not reported"

The dashboard shows a muted **Door: not reported** chip, and the door rule does nothing.

![The Printer state rules card, including the explanation of the lid and the stuck-switch problem](../img/ui-printer-state-rules.png)

## What it means

BLSmartFlow reads the front-door switch from `home_flag` bit 23 in the printer's reports. But **on
some X1C units the closed door never quite presses that switch**, so the bit reads "open" from the
moment the printer powers on and never changes. A fan rule built on that would sit switched off for
every single print.

So the device does not believe the bit until it has seen it **change**. Until then:

- the chip says *Door: not reported*,
- the door is treated as **closed** for every purpose, and
- the door rule is completely inert.

The status document reports `printer.doorOpen: null` and `printer.doorKnown: false`, and Home
Assistant renders the door binary sensor as **Unknown** rather than as a shut door.

## The fix — prove the switch once

**Open the front door and close it again** while the device is connected to the printer.

- If the chip turns into **Door open** / **Door closed**, your printer reports properly. The rule is
  live from then on, and stays live until the device reboots.
- If it stays **not reported**, yours is one of the affected units. Nothing here will work, and there
  is nothing to be done about it from this end.

You can check whether the switch is the problem by pressing the door plunger by hand: if the chip
moves while your finger is on it but not when the door is shut normally, the door is not reaching the
switch.

## The top lid is never sensed

!!! danger "There is no lid sensor at all"
    Bambu printers have **no sensor on the top lid**. Lifting it is invisible to the printer's own
    reports, and therefore to BLSmartFlow, to Home Assistant and to the API.

    If your workflow is "lift the lid to vent", the door rule cannot help. Use a
    [manual override](../using/manual-override.md), or a
    [chamber thermostat](../using/fan-modes.md#chamber-thermostat) that will notice the temperature
    change on its own.

## Why the first report is not counted as a change

After every MQTT reconnect the first report carries `home_flag`. If that counted as a change, every
reconnect would look like someone opening the printer — and on a flaky link the fan would flap. So
the first report only establishes the state; `doorKnown` needs a genuine transition.

That also means `doorKnown` resets on a device reboot. Open and close the door once after a firmware
update if you want the rule live immediately.

## Knock-on effects

While `doorKnown` is false:

| Feature | Behaviour |
|---|---|
| Door rule | Inert — the fan follows the curve or the thermostat as if the rule did not exist |
| Thermostat integral freeze | Never triggers. The ±100/ki clamp and the saturation freeze protect the controller instead |
| Cooling-rate learning | Every window is recorded as **door closed** |
| HA `binary_sensor.door` | *Unknown* |

This is the **intended failure mode**, not a bug: a printer with a stuck bit behaves exactly as it did
before the feature existed.

---

Related: [Printer state rules](../using/printer-state-rules.md) ·
[The door in the control loop](../technical/control-loop.md#the-door)
