// thermal_math.h - the arithmetic behind the passive cooling-rate learning.
//
// Arduino-free and header-only (host-tested in test/test_thermal). thermal.cpp
// owns the timing, the config and the status object; everything that can be got
// wrong numerically lives here.
//
// The model is Newtonian cooling: with the heaters off, an enclosure loses heat
// at a rate proportional to how far above the room it is,
//
//     dT/dt = -k * (T - ambient)      ->      k = -(dT/dt) / (T - ambient)
//
// k has the unit 1/min and depends almost entirely on how much air the fan is
// moving and whether the door is open, which is exactly what we bucket it by.
// Nothing here drives the fan: the numbers are shown to the user (and published
// to Home Assistant) so a cool-down time can be predicted.
//
// A *window* is a stretch of at least `minWindowSec` in which
//   * the fan output stayed within +-`outputTolSec` percent of where it started,
//   * the door state did not change, and
//   * no heater was active (bed and nozzle targets both zero),
// because only then is the fan the sole thing explaining the slope.

#ifndef BLSF_THERMAL_MATH_H
#define BLSF_THERMAL_MATH_H

#include <math.h>
#include <stdint.h>

namespace blsf {

// Fan-output buckets: 0 / 25 / 50 / 75 / 100 %, nearest wins.
static const uint8_t THERMAL_BUCKETS = 5;

inline uint8_t thermalBucket(float outputPct)
{
    if (isnan(outputPct) || outputPct <= 0.0f) return 0;
    if (outputPct >= 100.0f) return 4;
    int b = (int)((outputPct + 12.5f) / 25.0f);
    if (b < 0) b = 0;
    if (b > 4) b = 4;
    return (uint8_t)b;
}

inline float thermalBucketPercent(uint8_t bucket) { return (float)(bucket * 25); }

// Exponential moving average; an unseen bucket (NaN) simply takes the sample.
inline float thermalBlend(float previous, float sample, float alpha)
{
    if (isnan(sample)) return previous;
    if (isnan(previous)) return sample;
    return previous + alpha * (sample - previous);
}

struct ThermalWindow {
    bool  active;
    float startSec, startTemp;
    float lastSec, lastTemp;
    float outputRef;
    bool  doorRef;
    float rateCPerMin;    // slope of the window so far, NaN until it is long enough
};

inline void thermalWindowReset(ThermalWindow& w)
{
    w.active = false;
    w.startSec = w.startTemp = w.lastSec = w.lastTemp = 0.0f;
    w.outputRef = 0.0f;
    w.doorRef = false;
    w.rateCPerMin = NAN;
}

struct ThermalSample {
    bool    valid;        // a usable window just closed
    float   k;            // 1/min
    float   rateCPerMin;  // negative while cooling
    uint8_t bucket;
    bool    door;
};

// Enough of a slope to be worth learning from: below this the window is mostly
// sensor quantisation (the chamber sensor reports whole degrees on some models).
static const float THERMAL_MIN_DELTA_C = 0.5f;
// The closer the chamber sits to the room, the more the division by
// (T - ambient) amplifies noise; refuse the sample instead of learning garbage.
static const float THERMAL_MIN_LIFT_C = 3.0f;
// Below this the window has not yet produced a slope worth showing.
static const float THERMAL_MIN_RATE_SEC = 20.0f;
// A window that has not become usable after this many times the minimum length
// never will; start over rather than averaging over an ever longer stretch.
static const float THERMAL_MAX_WINDOW_FACTOR = 5.0f;

// Evaluates the window that is currently open, without touching it.
inline ThermalSample thermalEvaluate(const ThermalWindow& w, float ambient,
                                     float minWindowSec)
{
    ThermalSample s{false, NAN, NAN, 0, false};
    if (!w.active) return s;
    const float span = w.lastSec - w.startSec;
    if (!(span >= minWindowSec)) return s;
    const float dT = w.lastTemp - w.startTemp;
    if (!(fabsf(dT) >= THERMAL_MIN_DELTA_C)) return s;

    const float rate = dT / span * 60.0f;                 // degC per minute
    const float lift = (w.startTemp + w.lastTemp) * 0.5f - ambient;
    if (!(lift >= THERMAL_MIN_LIFT_C)) return s;

    const float k = -rate / lift;
    if (isnan(k) || isinf(k) || k <= 0.0f) return s;       // warming up, not cooling

    s.valid = true;
    s.k = k;
    s.rateCPerMin = rate;
    s.bucket = thermalBucket(w.outputRef);
    s.door = w.doorRef;
    return s;
}

// Feeds one chamber sample. Returns the result of the window this sample ended,
// if any; the window is then restarted from the new conditions.
//
// `nowSec` is a monotonic clock in seconds, `heaterActive` is true whenever a
// bed or nozzle target is non-zero (a print in progress explains the slope far
// better than the fan does).
inline ThermalSample thermalFeed(ThermalWindow& w, float nowSec, float chamber,
                                 float output, bool doorOpen, bool heaterActive,
                                 float ambient, float minWindowSec = 60.0f,
                                 float outputTol = 5.0f)
{
    ThermalSample closed{false, NAN, NAN, 0, false};

    if (isnan(chamber) || heaterActive) {          // nothing learnable right now
        closed = thermalEvaluate(w, ambient, minWindowSec);
        thermalWindowReset(w);
        return closed;
    }

    // A clock that went backwards (millis() wrapping after 49 days) would leave
    // a window with a negative span that can never close again.
    const bool broken = w.active && (fabsf(output - w.outputRef) > outputTol ||
                                     doorOpen != w.doorRef || nowSec < w.lastSec);
    if (broken) closed = thermalEvaluate(w, ambient, minWindowSec);

    if (!w.active || broken) {
        w.active = true;
        w.startSec = w.lastSec = nowSec;
        w.startTemp = w.lastTemp = chamber;
        w.outputRef = output;
        w.doorRef = doorOpen;
        w.rateCPerMin = NAN;
        return closed;
    }

    w.lastSec = nowSec;
    w.lastTemp = chamber;
    const float span = w.lastSec - w.startSec;
    w.rateCPerMin = span >= THERMAL_MIN_RATE_SEC
                        ? (w.lastTemp - w.startTemp) / span * 60.0f
                        : NAN;

    // Harvest as soon as the window is long enough, then start a fresh one from
    // here: an hour-long cool-down should contribute a run of samples, not one.
    // A window that stays unusable (a chamber already at room temperature, a
    // coarse sensor that has not moved yet) is given a few multiples of the
    // minimum before it is abandoned.
    if (span >= minWindowSec) {
        const ThermalSample done = thermalEvaluate(w, ambient, minWindowSec);
        if (done.valid || span >= minWindowSec * THERMAL_MAX_WINDOW_FACTOR) {
            if (done.valid) closed = done;
            w.startSec = w.lastSec;
            w.startTemp = w.lastTemp;
            w.outputRef = output;
            w.doorRef = doorOpen;
            w.rateCPerMin = NAN;
        }
    }
    return closed;
}

}  // namespace blsf

#endif  // BLSF_THERMAL_MATH_H
