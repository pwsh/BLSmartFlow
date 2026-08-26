// curve.h - fan curve model and interpolation.
//
// This module is deliberately free of Arduino/ESP-IDF dependencies so it can be
// compiled and unit-tested on the host (`pio test -e native`). It is header-only
// and has no global state: a FanCurve is a plain value that can be copied into a
// critical section or a snapshot without allocating.
//
// A curve is a list of (temperature, speed%) points. Between points the speed is
// linearly interpolated; outside the first/last point it is clamped.

#ifndef BLSF_CURVE_H
#define BLSF_CURVE_H

#include <stdint.h>
#include <math.h>

namespace blsf {

// Upper bound chosen so a FanCurve stays small enough to copy freely (16 * 8 B).
static const uint8_t CURVE_MAX_POINTS = 16;
static const float CURVE_MIN_TEMP = 0.0f;
static const float CURVE_MAX_TEMP = 400.0f;

struct CurvePoint {
    float   temp;   // degrees C, 0..400
    uint8_t speed;  // percent, 0..100
};

struct FanCurve {
    CurvePoint pts[CURVE_MAX_POINTS];
    uint8_t    count;
};

// Normalises a curve in place:
//   * points with a non-finite temperature are dropped
//   * temperatures are clamped to 0..400, speeds to 0..100
//   * points are sorted by ascending temperature (stable insertion sort)
//   * duplicate temperatures collapse to a single point, keeping the last one
//     given (so the caller's later entry wins, matching the editor's semantics)
//   * excess points beyond CURVE_MAX_POINTS are discarded
// Returns false when fewer than two usable points remain; the caller is then
// expected to fall back to the default curve rather than use the result.
inline bool curveValidate(FanCurve& c)
{
    if (c.count > CURVE_MAX_POINTS) c.count = CURVE_MAX_POINTS;

    // Drop invalid entries and clamp the survivors.
    uint8_t w = 0;
    for (uint8_t i = 0; i < c.count; i++) {
        float t = c.pts[i].temp;
        if (isnan(t) || isinf(t)) continue;   // NaN would poison the sort
        if (t < CURVE_MIN_TEMP) t = CURVE_MIN_TEMP;
        if (t > CURVE_MAX_TEMP) t = CURVE_MAX_TEMP;
        uint8_t s = c.pts[i].speed;
        if (s > 100) s = 100;
        c.pts[w].temp = t;
        c.pts[w].speed = s;
        w++;
    }
    c.count = w;

    // Insertion sort: n <= 16, and stability is what makes "last duplicate wins"
    // below deterministic.
    for (uint8_t i = 1; i < c.count; i++) {
        CurvePoint key = c.pts[i];
        int8_t j = (int8_t)i - 1;
        while (j >= 0 && c.pts[j].temp > key.temp) {
            c.pts[j + 1] = c.pts[j];
            j--;
        }
        c.pts[j + 1] = key;
    }

    // Collapse equal temperatures, keeping the last of each run.
    w = 0;
    for (uint8_t i = 0; i < c.count; i++) {
        if (w > 0 && c.pts[w - 1].temp == c.pts[i].temp) {
            c.pts[w - 1] = c.pts[i];
        } else {
            c.pts[w++] = c.pts[i];
        }
    }
    c.count = w;

    return c.count >= 2;
}

// Returns the fan speed in percent (0..100) for `temp`.
// An empty curve yields 0; a single-point curve yields that point's speed;
// temperatures outside the curve clamp to the first/last speed. A NaN input
// means "temperature unknown" and yields 0 so the fan does not run blind.
inline float curveInterpolate(const FanCurve& c, float temp)
{
    if (c.count == 0) return 0.0f;
    if (isnan(temp)) return 0.0f;
    if (c.count == 1) return (float)c.pts[0].speed;

    if (temp <= c.pts[0].temp) return (float)c.pts[0].speed;
    if (temp >= c.pts[c.count - 1].temp) return (float)c.pts[c.count - 1].speed;

    for (uint8_t i = 1; i < c.count; i++) {
        if (temp <= c.pts[i].temp) {
            const float t1 = c.pts[i - 1].temp;
            const float t2 = c.pts[i].temp;
            const float s1 = (float)c.pts[i - 1].speed;
            const float s2 = (float)c.pts[i].speed;
            const float span = t2 - t1;
            // Guard against a zero span even though curveValidate() dedups: a
            // curve may reach here straight from a caller that skipped validation.
            if (span <= 0.0f) return s2;
            return s1 + (temp - t1) * (s2 - s1) / span;
        }
    }
    return (float)c.pts[c.count - 1].speed;
}

// The factory curve, also used whenever a stored curve fails validation.
inline void curveDefaults(FanCurve& c)
{
    static const CurvePoint kDefault[] = {
        {   0.0f,   0 },
        {  50.0f,   0 },
        { 180.0f,  50 },
        { 245.0f,  80 },
        { 350.0f, 100 },
    };
    c.count = (uint8_t)(sizeof(kDefault) / sizeof(kDefault[0]));
    for (uint8_t i = 0; i < c.count; i++) c.pts[i] = kDefault[i];
}

}  // namespace blsf

#endif  // BLSF_CURVE_H
