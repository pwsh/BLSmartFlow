# Your first fan curve

The fan is running off the factory curve already. This page gets you to a curve that suits *your*
fan and *your* job, in about five minutes.

## 1. Decide what the fan is for

That single question picks almost everything else.

| Your fan is… | Use | Start from |
|---|---|---|
| Cooling a part, a hotend duct or electronics that heat with the print | **Curve** mode, source **Nozzle** | the *Balanced* preset |
| Venting an enclosure to keep it *cool* (PLA in an X1) | **Chamber thermostat**, or Curve mode with source **Chamber** | *Chamber-ABS* preset |
| Holding an enclosure *warm but not too hot* (ABS/ASA) | **Chamber thermostat** | target 45–50 °C |
| A general "something in there is hot" fan | **Curve** mode, source **Hottest of all** | *Balanced* |

Chamber-based options need a printer that reports a chamber temperature — X1- and H2D-style machines
only.

## 2. Pick the mode

The **Fan mode** card sits at the top of the *Fan curve* page.

![The Fan mode card with Curve, Chamber thermostat, Manual and Off, and a "now: Chamber thermostat" badge](../img/ui-fan-mode.png)
/// caption
The mode applies the moment you click it. The badge on the right is the **effective mode** — what the
controller is actually doing this second.
///

Full detail: [Fan modes](../using/fan-modes.md).

## 3. Shape the curve

![The fan curve editor: a drag-editable graph with five points and the matching table below](../img/ui-curve-editor.png)
/// caption
Drag a point to move it, click the line to insert one, or type exact numbers in the table.
///

- Press **Preset → Balanced** to start from the factory curve (off below 50 °C, 50 % at 180 °C, 80 %
  at 245 °C, 100 % at 350 °C).
- **Drag** points with the mouse or a finger; values snap to 1 °C and 1 %.
- The dashed marker shows the **live source temperature** and the output it currently produces —
  the quickest sanity check there is.
- Press **Save** in the bar at the bottom. Nothing reaches the device until you do.

Full detail: [Fan curve editor](../using/fan-curve-editor.md).

## 4. Make the fan behave

Two settings in the **Control behaviour** card are worth setting before anything else, because they
are about *your fan*, not about the print.

![The Control behaviour card: temperature source, hysteresis, ramp rate, minimum speed and kick-start](../img/ui-control-behaviour.png)

**Minimum speed.** Many small fans buzz or stall instead of turning at very low duty. Set this just
above the lowest speed at which your fan still spins reliably — often 15–25 %. Anything the curve
asks for below it is output as 0 %.

**Kick-start.** When the fan starts from standstill it runs at 100 % for a short pulse (500 ms by
default) so it breaks away instead of humming. Leave it on.

Then, optionally:

- **Hysteresis** 1–3 °C stops the fan hunting up and down around a curve point.
- **Ramp rate** 10–30 %/s makes speed changes gradual — quieter, and kinder to the bearings.

## 5. Add the printer state rules

![The Printer state rules card: door-open behaviour with a resume delay, and the preheat rule](../img/ui-printer-state-rules.png)

Two rules sit *above* the curve and the thermostat:

- **While preheating → Stop the fan** (the default). An exhaust fan running during warm-up is
  fighting the heaters.
- **Door open → Stop the fan.** With the printer open there is nothing to exhaust; the fan just
  pulls room air and dust through the machine.

Full detail: [Printer state rules](../using/printer-state-rules.md).

## 6. Check it on the dashboard

Go back to the **Dashboard** and watch one print through. The things to look at:

- The **fan gauge** and the `target` badge — output should track the curve.
- The **effective mode** badge — `Automatic` normally, `Preheating` while the bed climbs,
  `Stale data – failsafe` if the printer stops reporting.
- The **phase chip** on the *Print job* card — *Preheating*, then *Printing*.

If the fan never starts, or whines, or hunts: [Fan behaviour](../troubleshooting/fan-behaviour.md).

---

**Next:** [The dashboard, field by field](../using/dashboard.md)
