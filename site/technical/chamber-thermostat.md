# The chamber thermostat

`thermostatStep()` in `thermostat.h` is a pure, Arduino-free PI step, host-tested in
`test/test_thermostat`. It is deliberately small: about thirty lines, no state outside what the
caller passes in.

## The control law

```text
e   = chamber − setpoint                       // positive = too hot, fan should run
out = clamp(kp·e + ki·∫e, 0, 100)
```

Note the sign. The error is positive when the chamber is **hotter** than asked for, and a positive
error asks for **more** fan. This is a cooling-only controller: it can remove heat, never add it, and
`out` is clamped at 0 rather than going negative.

| Gain | Unit | Default | Range |
|---|---|---|---|
| `kp` | % of fan per °C of error | `8.0` | 0–50 |
| `ki` | % of fan per °C·second | `0.02` | 0–1 |
| `thermostatPeriodSec` | s | `5` | 1–60 |

At `kp = 8`, being 5 °C too hot asks for 40 % fan on the proportional term alone. `ki = 0.02` adds
2 % of fan for every 100 °C·s of accumulated error — deliberately slow, because an enclosure responds
in minutes.

## Anti-windup

For an exhaust fan, integral windup is the difference between "settles at 45 °C" and "sits at 100 %
for ten minutes after the door was shut". Two guards:

### 1. A hard clamp at ±100/ki

The integral itself is clamped so that `ki·∫e` alone can never demand more than full scale in either
direction. With the default `ki = 0.02` that is ±5000 °C·s.

### 2. Conditional integration

Integration is **frozen**:

- **while the door is open** — the error is real, but the fan cannot fix it, so accumulating it is
  pure windup; and
- whenever a step would push an **already saturated** output further into its rail.

Steps that bring a saturated output *back* into range are always accepted, so the integral can always
unwind. Without that exception the freeze would be a trap rather than a guard.

!!! note "The door freeze uses the trusted door reading"
    It uses `reportDoorOpen()` — `doorKnown && doorOpen`. On a printer whose door switch has never
    been proved, the freeze never triggers, and the ±100/ki clamp plus the saturation freeze are what
    protect the controller there. See [the door](control-loop.md#the-door).

## Set-point switching

The set point comes from the phase, not from a timer:

| Phase | Set point |
|---|---|
| `preheat`, `printing`, `paused` | `chamberTarget` (or the filament's effective chamber target) |
| `finished`, `cooling`, or `idle` with a recent print | `cooldownTarget` |
| anything else | none — `effectiveMode` becomes `idle` and the output is 0 % |

**Any change of set point resets the integral**, and so does leaving the thermostat for any other
effective mode. Returning from a door event therefore never resumes with a stale integral.

The one exception is [filament-driven](filament-matching.md) set points: the integral is reset only
when the effective set point moves by **more than 5 °C**. An override nudging 50 → 48 keeps minutes
of accumulated correction that would otherwise be thrown away for nothing.

## Timing

The controller steps once every `thermostatPeriodSec`; between steps the last output is **held**, not
recomputed. Running faster gains nothing — an enclosure takes minutes to respond — and only invites
the fan to chase sensor noise on a chamber thermistor that reports in whole tenths.

## No chamber, no thermostat

If the chamber temperature is NaN, the controller does not run. `effectiveMode` falls back to `auto`
and the curve drives the fan.

This is a deliberate fallback rather than an error: a P1 or A1 user who selects *Chamber thermostat*
gets a working fan and a mode chip that says `Automatic`, instead of a fan that sits at 0 % forever
because the error is undefined.

## Testing

`test/test_thermostat` covers:

- proportional response at several errors,
- integral accumulation over repeated steps,
- the ±100/ki clamp,
- the door freeze and the saturation freeze, including the "unwinding is always allowed" case, and
- the printing → cool-down set-point switch and its integral reset.

Because `thermostat.h` has no Arduino dependency, all of that runs on the host in milliseconds.

---

Related: [Control loop](control-loop.md) · [Cooling-rate learning](cooling-rate-learning.md) ·
[Post-print cool-down](post-print-cooldown.md) · [Fan modes](../using/fan-modes.md)
