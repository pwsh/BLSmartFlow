# The control loop

`fanControlLoop()` runs on every pass of the main loop but recomputes at most every **100 ms** — or
immediately when something has raised the "recompute now" flag. It works off a `FanConfig` copy taken
under the config lock and a `PrinterState` snapshot taken under a spinlock, so it never holds a lock
while it thinks.

## Print phase

The fan logic reasons about a derived **phase**, not about `gcode_state`, because `RUNNING` alone
cannot tell a chamber that is still heating from one that is at temperature.

`reportPhase(const PrinterReport&)` lives in `printer_parse.h` (Arduino-free, host-tested);
`state.h` wraps it as `printerPhase(const PrinterState&)`, which additionally reports `offline` when
no report has ever arrived. **First rule that matches wins:**

| Phase | Rule |
|---|---|
| `offline` | `gcode_state` empty or `OFFLINE`, or `stg_cur == -2`, or no report ever received |
| `paused` | `gcode_state == PAUSE`, or `stg_cur ∈ {5,6,16,17,20,21,23,26,27,28,30,32,33,34,35}` |
| `preheat` | `stg_cur ∈ {2,7,49,54,58,63,64}`, or `RUNNING` **and** (`bedTarget > 0 && bed < bedTarget − 3`) or (`chamberTarget > 0 && chamber < chamberTarget − 2`) |
| `cooling` | `stg_cur ∈ {29,50,69}` |
| `printing` | `gcode_state ∈ {RUNNING, PREPARE, SLICING}` |
| `finished` | `gcode_state == FINISH` |
| `failed` | `gcode_state == FAILED` |
| `idle` | otherwise |

**`printing` = phase ∈ {`preheat`, `printing`, `paused`}**, and that is what `onlyWhilePrinting`,
`printer.printing` and the Home Assistant `printing` binary sensor all use.

`stageName()` covers the whole ha-bambulab table `0..77` plus `-1`/`255` → `idle` and `-2` →
`offline`, in the snake_case spelling `printer.stageText` has always published.

## The door

`home_flag` **bit 23** is the front-door plunger switch. **The top lid has no sensor at all**, so
lifting it changes nothing.

On some X1C units the closed door does not actuate that switch, so the bit sits at `1` from boot to
power-off (pressing the switch by hand flips it). A raw bit is therefore not evidence of anything
until it has been seen to *change*:

| Field | Meaning |
|---|---|
| `doorOpen` | The raw bit — recorded, but meaningless on its own |
| `doorRawSeen` | A report has carried `home_flag` at least once |
| `doorKnown` | **An edge has been observed** — this printer's switch really reports |
| `doorEdgeCount`, `lastDoorOpenMs`, `lastDoorCloseMs` | Edge bookkeeping |

`reportDoorOpen()` is `doorKnown && doorOpen` and is the **only** door reading the control loop, the
thermostat freeze and the cooling-rate learner ever use. A printer with a stuck bit therefore behaves
exactly as it did before the feature existed.

!!! note "The first report is state, not an edge"
    The first report that carries `home_flag` establishes the raw state; it does not set `doorKnown`.
    Otherwise every MQTT reconnect would look like someone opening the printer.

`printer.doorOpen` is serialised as `null` while `doorKnown` is false, and `printer.doorKnown` says
which case you are in.

## Effective modes

Definitions used below:

```text
printing    = phase ∈ {preheat, printing, paused}
stale       = age of the newest accepted report ≥ staleSec   (never received = infinitely old)
recentPrint = !printing && a print has ended && now − printEnd < cooldownMin
cooling     = onlyWhilePrinting && recentPrint && chamber > cooldownTarget
```

The first rule that matches decides `effectiveMode` and the target:

| Order | `effectiveMode` | Condition | Target |
|---|---|---|---|
| 1 | `off` | `fan.mode == "off"` | 0 % |
| 2 | `manual` | `fan.mode == "manual"` | `manualSpeed` |
| 3 | `stale` | not off/manual and `stale` | `hold` → the current ramp value · `off` → 0 % · `fixed` → `staleSpeed` |
| 4 | `door` | `doorMode != ignore`, **`doorKnown`**, the door is open (or closed less than `doorResumeSec` ago), and phase ∉ {`finished`, `cooling`, `idle`} | `off` → 0 % · `fixed` → `doorSpeed` |
| 5 | `preheat` | `preheatMode != ignore` and phase == `preheat` | `off` → 0 % · `fixed` → `preheatSpeed` |
| 6 | `idle` | `mode == chamber`, chamber known, and the phase has no set point | 0 % |
| 7 | `cooldown` | `mode == chamber`, phase ∈ {`finished`, `cooling`} or (`idle` and `recentPrint`) | thermostat towards `cooldownTarget` |
| 8 | `chamber` | `mode == chamber`, phase ∈ {`preheat`, `printing`, `paused`} | thermostat towards `chamberTarget` |
| 9 | `idle` | `onlyWhilePrinting`, not printing, cool-down finished | 0 % |
| 10 | `cooldown` | `onlyWhilePrinting`, not printing, cool-down running | curve |
| 11 | `auto` | otherwise — **including `mode == chamber` with an unknown chamber temperature** | curve |

Three things worth calling out:

- **Staleness is judged by data age only**, not by the MQTT socket state, so a brief reconnect does
  not yank the fan to the failsafe while the last reading is seconds old.
- **Rule 4 skips the cool-down phases.** During a cool-down an open door is *helping*.
- **In `auto` mode the cool-down window ends at `cooldownTarget` or `cooldownMin`, whichever comes
  first**, so the fan does not run out a ten-minute timer on a chamber that is already cold.

## Order of operations

Once a rule has produced a target, the rest is unconditional:

```text
sourceTemp = select(fan.source, printer temps)        // NaN when unavailable
             nozzle | bed | chamber | max(nozzle, bed, chamber ignoring NaN)

0. rules       off / manual / stale / door / preheat / thermostat, in the order above
1. curve       target = curveInterpolate(curve, sourceTemp)      (auto / cooldown only)
2. hysteresis  the curve is only re-evaluated once |sourceTemp − heldTemp| >= hysteresis;
               a NaN source resets the anchor and yields 0 %
3. clamp       target = clamp(target, 0, 100)
4. ramp        rampRate == 0 ? slew = target
                             : slew moves towards target by rampRate * dt
5. minSpeed    output = (minSpeed > 0 && 0 < slew < minSpeed) ? 0 : slew
6. kick        if kickStart && kickMs > 0 && the output leaves 0 % after >= 2 s at 0 %,
               drive 100 % until kickMs has elapsed (or the output returns to 0 %)
7. duty        duty = round(driven * 255 / 100)
8. invert      pin value = pwmInvert ? 255 − duty : duty; a disabled output gets
               0 (or 255 when inverted)
```

!!! warning "The ramp accumulator is separate from the published output"
    If the minimum-speed clamp fed back into the slew, a fan with `minSpeed` set could never climb
    away from 0 % — the clamp would keep zeroing the accumulator the ramp is trying to grow. Keeping
    the two apart is the whole reason step 4 and step 5 are distinct.

The kick pulse is only armed after the output has genuinely been at 0 % for **two seconds**, so a
curve that dips briefly through zero cannot leave the fan pulsing.

## Where the filament comes in

`fanControlLoop()` resolves the [filament profile](filament-matching.md) **every tick** from the
snapshot it already holds — 90 string compares, microseconds — so there is no cache to invalidate and
a tool change mid-print takes effect on the next 100 ms pass. It feeds four things:

| Where | Effect |
|---|---|
| Thermostat set point | `chamberTarget` while printing, `cooldownTarget` afterwards. The integral is reset only when the set point moves by **more than 5 °C**, so an override nudging 50 → 48 keeps minutes of accumulated correction |
| Cool-down window | `chamber <= cooldownTarget` ends the `auto` cool-down early, against the *effective* target |
| Gentle cool-down | While `effectiveMode == "cooldown"` and the material is `gentle`: output 0 % until the chamber is 10 °C below the chamber target, then 50 % at most |
| Ventilation floor | A minimum target while `printer.printing`, applied **after** the mode has produced a target and **before** the ramp. Skipped in `off`/`manual` mode and under the `door`, `preheat` and `stale` rules — those deliberately want the fan stopped |

## What `FanState` publishes

`output`, `target`, `effectiveMode`, `sourceTemp`, `setpoint`, `pwmDuty`, `manualExpiresAt` and
`kicking`.

- `setpoint` is the thermostat set point in force this instant, and is NaN (JSON `null`) in every
  other mode.
- **`pwmDuty` is post-inversion** — it is the byte the pin actually sees, so with `pwmInvert` an
  output of 0 % reports 255.

## Manual overrides

`fanApplyMode(mode, speed, durationSec, persist)` is shared by [`POST /api/fan`](rest-api.md#fan-control)
and the MQTT command topics; it accepts `auto`, `chamber`, `manual` and `off`. `durationSec` is
clamped to 86400.

A **timed** override sets a deadline and is *not* persisted, so a reboot ends it. A duration of `0`
persists mode and speed through the deferred-save path. When the deadline passes, the control loop
logs it, clears the deadline and writes `fan.mode = "auto"` back into the config.
