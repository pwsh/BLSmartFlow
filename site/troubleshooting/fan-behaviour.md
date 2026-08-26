# Troubleshooting: fan behaviour

!!! tip "Look at the effective-mode badge first"
    The badge on the dashboard's *Fan output* card names what is actually in charge: `Automatic`,
    `Chamber thermostat`, `Cool-down`, `Manual`, `Off`, `Preheating`, `Door open`,
    `Stale data – failsafe` or `Idle`. Half the problems on this page are answered by that badge
    alone.

## The fan never starts

| Cause | Fix |
|---|---|
| **Minimum speed** is above what the curve is asking for | Lower it, or raise the curve. Anything strictly below the minimum is output as 0 % |
| The fan cannot break away from standstill | Turn **Kick-start** on (500 ms is the default; 300–800 ms is the useful range) |
| The curve is simply at 0 % for the current temperature | Watch the dashed marker on the [curve editor](../using/fan-curve-editor.md) — it shows the live source temperature and the output it produces |
| The output is disabled | Check **Output 1 / Output 2** on the *Outputs* card |
| Fan mode is **Off** | Check the *Fan mode* card |

## The fan whines, buzzes or ticks

**Why.** The PWM carrier frequency does not suit the fan or the driver board.

**Fix.** Keep **PWM frequency** at **25000 Hz** for good 4-pin and quality 2-pin fans — that is above
hearing. Some cheap fans and MOSFET boards prefer **1000–8000 Hz**; it is audible as a hum, but it may
be the only setting they run smoothly at.

If the noise is a *mechanical* buzz at low speed rather than an electrical whine, it is the fan
stalling: raise **Minimum speed** instead.

## The fan runs full speed when the UI says 0 %

**Why.** The driver board expects an **active-low** signal.

**Fix.** Turn **Invert PWM** on, in the *Outputs* card. Note that `fan.pwmDuty` in the API is
post-inversion, so 0 % will then report `duty 255` — that is correct.

## The fan hunts — surges up and down

In **Curve** mode:

- Raise **Hysteresis** to 1–3 °C. The curve is then only re-evaluated once the source temperature has
  actually moved, instead of chasing every tenth of a degree.
- Add a **Ramp rate** of 10–30 %/s so speed changes are gradual.

In **Chamber thermostat** mode:

- **Lower Kp.** At the default 8, being 5 °C too hot asks for 40 % fan; on a small enclosure with a
  strong fan that is too much gain.
- Leave the update period at 5 s. Running it faster makes hunting worse, not better.

→ [Chamber thermostat](../technical/chamber-thermostat.md)

## The fan overshoots badly after I close the door

That is integral windup, and it is guarded against — the integral is **frozen while the door is
open**. If you are seeing it anyway, the most likely reason is that your printer's door switch has
never been proved, so the freeze never triggers.
→ [Door not reported](door-not-reported.md)

Lowering **Ki** also helps.

## The fan stays off during preheat

**Working as designed.** *While preheating → Stop the fan* is the default, because an exhaust fan
during warm-up fights the heaters.

If the printer seems stuck in **Preheating**, the phase rule is why: the device treats a *running*
printer as preheating while the **bed is more than 3 °C below its target** or the **chamber is more
than 2 °C below its own**. A slow bed, or a chamber target the printer cannot reach, keeps it there.

Check the phase chip on the dashboard. To disable the behaviour, set *While preheating* to
*Ignore*.

→ [Printer state rules](../using/printer-state-rules.md)

## The fan stops as soon as a print finishes

**Why.** *Only while printing* is on and the cool-down has elapsed.

**Fix.** Raise **Cooldown after a print**, or turn *Only while printing* off. Remember that the window
also ends early once the chamber reaches the **cool-down target** — whichever comes first.

## The fan will not go below 10 % during an ABS print

**Why.** That is the [ventilation floor](../using/filament-aware-cooling.md#the-ventilation-floor-and-why-it-is-nearly-zero)
for a *ventilation required* material.

**Fix.** If your fan does not vent anywhere useful, set *Required* to 0 in the *Ventilation floor
table*. If it ducts outside or through a filter, leave it — or raise it.

## The chamber target ignores what I typed

**Why.** *Use the loaded filament* is on, and the material's own figure is being used instead.

**Fix.** Add an [override](../using/filament-aware-cooling.md#overriding-a-material) for that
material, or turn *Use the loaded filament* off.

## The fan runs at a speed I did not ask for, and the badge says "Stale data"

No fresh printer report. The failsafe you configured is driving the fan.
→ [Printer link troubleshooting](printer-link.md)

## "Chamber thermostat" is selected but the chip says "Automatic"

The printer reports no chamber temperature, so the thermostat has nothing to control and falls back
to the curve rather than running blind. Only X1- and H2D-style machines have a chamber sensor.

## Learned cooling rates stays empty

It needs a minute of **steady fan output**, an **unchanged door state** and **both heaters off** —
which in practice means after a print. Let one print finish and idle for a few minutes.

Also check that *Room temperature* on the *Chamber thermostat* card is roughly right; it is the
denominator of the fit.
→ [Cooling-rate learning](../technical/cooling-rate-learning.md)

---

Related: [Fan curve editor](../using/fan-curve-editor.md) ·
[Fan modes](../using/fan-modes.md) · [Control loop](../technical/control-loop.md)
