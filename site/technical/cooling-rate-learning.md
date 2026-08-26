# Cooling-rate learning

`thermal.h` / `thermal.cpp` measures how fast your chamber sheds heat. It is **passive**: it never
touches the fan, and nothing auto-tunes from it. It exists so the UI and Home Assistant can answer
"if I set the fan to 50 % and shut the door, how long until I can open the printer?".

## The physics

A warm box loses heat at a rate proportional to how much warmer it is than the room. That is Newton's
law of cooling, and it has exactly one unknown:

```text
dT/dt = −k · (T − ambientTemp)     →     k = −(dT/dt) / (T − ambientTemp)     [1/min]
```

So `k` is the whole story of how well your setup sheds heat, and it depends almost entirely on two
things: **how hard the fan is blowing**, and **whether the printer is open**.

Hence the table shape: one number per fan bucket (0 / 25 / 50 / 75 / 100 %), twice — door closed and
door open. Ten floats.

## Windows

The chamber is sampled every **5 seconds**. A **window** is a run of at least **60 seconds** in which:

- the fan output stayed within **±5 %**,
- the **door state did not change**, and
- **no heater was active** — `bedTarget == 0 && nozzleTarget == 0`.

!!! note "Why the heater condition matters most"
    While the printer is heating, the chamber's slope says far more about the bed than about your
    fan. Excluding heated periods is what makes the measurement mean anything. In practice the
    numbers therefore come from the minutes *after* a print — which is also exactly when you care
    about them.

A window is **refused** when:

| Condition | Why |
|---|---|
| The chamber moved less than **0.5 °C** | Noise, not a measurement |
| The chamber sits less than **3 °C above ambient** | The arithmetic divides by `T − ambient`; near zero it magnifies sensor noise into nonsense |
| The chamber was **warming** rather than cooling | `k` would come out negative, which is not a cooling constant |

Windows are harvested as soon as they are long enough and then **restarted**, so a long cool-down
contributes a run of samples rather than one average. A window that runs five times the minimum
without producing a usable fit is discarded.

## Bucketing and blending

Each usable window produces one `k`, which is:

1. bucketed by **nearest fan output** — 0, 25, 50, 75 or 100 % — and by **door state**, then
2. blended into that cell with an **exponential moving average, α = 0.3**.

```text
k[bucket][door] = k[bucket][door] + 0.3 · (k_new − k[bucket][door])
```

So the figures settle down over a few prints rather than jumping around after every measurement. An
empty cell takes the first measurement outright.

## Persistence

The table lives in the configuration as `thermal.k` (10 floats) and `thermal.samples`, written
through the ordinary dirty/loop-save mechanism at most **once every 10 minutes** — flash has a finite
number of writes and this is a background measurement, not a user action.

It therefore survives reboots and firmware updates, and it travels with a
[backup](../using/backup-security.md).

## Reading the numbers

The dashboard's **Thermal** card shows the live rate in °C/min, and behind *Learned cooling rates* the
table itself.

- **Bigger `k` means faster cooling.**
- **A dash** means that combination has simply not happened yet — run a print with the fan at that
  speed and it will fill in.
- The **count** next to the heading is how many windows have gone in so far.

!!! warning "Check the room temperature"
    *Room temperature* on the *Chamber thermostat* card is a number **you type**. The device has no
    room sensor. It has no effect on the fan whatsoever, but a wrong value skews every `k` in the
    table, because it is the denominator of the fit.

## In the status document and MQTT

```json
"thermal": { "rateCPerMin": -0.42,
             "kClosed": [0.31, null, null, null, null],
             "kOpen":   [null, null, null, null, null],
             "samples": 7 }
```

- `rateCPerMin` is `null` until the fan output and the door have been steady for about **20 seconds**.
- Unmeasured cells are `null`, **never NaN** — a bare NaN in JSON is not valid and the status builder
  makes the absence explicit.
- Anything outside `(0, 5]` on load is reset to `null`, so a hand-edited backup cannot inject nonsense.

Home Assistant gets `sensor.cooling_rate` in °C/min, which reads *Unknown* while nothing is being
measured.

## Testing

`thermal_math.h` is Arduino-free and host-tested in `test/test_thermal`: bucketing, the EMA blend,
recovering a **known** cooling constant from a synthetic cool-down curve, and every reason a window is
refused.

---

Related: [Chamber thermostat](chamber-thermostat.md) · [Dashboard](../using/dashboard.md)
