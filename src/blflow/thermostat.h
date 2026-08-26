// thermostat.h - the chamber thermostat's PI step.
//
// Deliberately free of Arduino/ESP-IDF dependencies so it compiles and is unit
// tested on the host (`pio test -e native`, test/test_thermostat). It is
// header-only and holds no global state: the caller owns a ThermostatState and
// passes the gains and the elapsed time in, which is what makes the behaviour
// reproducible in a test rather than only observable on a warm printer.
//
// The loop is a plain PI controller on the chamber temperature:
//
//     e   = temp - setpoint          (positive = too hot, fan should run)
//     out = kp * e + ki * integral   clamped to 0..100 %
//
// Two things stop the integral running away, which for an exhaust fan is the
// difference between "settles at 45 C" and "sits at 100 % for ten minutes after
// the door was closed":
//
//   * a hard clamp at +-100/ki, so ki*integral alone can never demand more than
//     full scale in either direction;
//   * conditional integration - the integral is frozen whenever the caller says
//     so (an open door: the error is real but the fan cannot fix it) and
//     whenever a step would push an already saturated output further into its
//     rail.

#ifndef BLSF_THERMOSTAT_H
#define BLSF_THERMOSTAT_H

#include <math.h>
#include <stdint.h>

namespace blsf {

struct ThermostatState {
    float integral;   // accumulated error, in degC*s
};

inline void thermostatReset(ThermostatState& st) { st.integral = 0.0f; }

// Advances the controller by `dtSec` and returns the requested output in
// percent (0..100). `temp` and `setpoint` must be finite - the caller decides
// what to do when the chamber temperature is unknown (fan_control falls back to
// the curve). `freeze` suspends integration without suspending the
// proportional term.
inline float thermostatStep(ThermostatState& st, float temp, float setpoint,
                            float kp, float ki, float dtSec, bool freeze)
{
    if (isnan(temp) || isnan(setpoint)) return 0.0f;
    if (isnan(kp) || kp < 0.0f) kp = 0.0f;
    if (isnan(ki) || ki < 0.0f) ki = 0.0f;

    const float e = temp - setpoint;
    float integral = st.integral;

    if (ki > 0.0f && dtSec > 0.0f && !freeze) {
        // Anti-windup clamp: ki*integral is bounded by full scale, so however
        // long the error persists the integral can be unwound in bounded time.
        const float lim = 100.0f / ki;
        float next = integral + e * dtSec;
        if (next > lim) next = lim;
        if (next < -lim) next = -lim;

        // Conditional integration: refuse a step that drives a saturated output
        // deeper into saturation. Steps that bring it back are always allowed.
        const float held  = kp * e + ki * integral;
        const float trial = kp * e + ki * next;
        const bool pushingIntoRail =
            (held >= 100.0f && trial > held) || (held <= 0.0f && trial < held);
        if (!pushingIntoRail) integral = next;
    }
    st.integral = integral;

    float out = kp * e + ki * integral;
    if (isnan(out)) return 0.0f;
    if (out < 0.0f) return 0.0f;
    if (out > 100.0f) return 100.0f;
    return out;
}

}  // namespace blsf

#endif  // BLSF_THERMOSTAT_H
