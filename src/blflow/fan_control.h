// fan_control.h - PWM outputs and the fan control state machine.
//
// Two LEDC channels (GPIO 17 and 16) driven from one duty value. The control
// loop runs from the Arduino loop task and never blocks: it recomputes at most
// every 100 ms and writes the outputs only when the duty actually changes.
//
// See REWORK-SPEC section 6 for the mode table this implements.

#ifndef BLSF_FAN_CONTROL_H
#define BLSF_FAN_CONTROL_H

#include <Arduino.h>

namespace blsf {

static const uint8_t PIN_FAN1 = 17;
static const uint8_t PIN_FAN2 = 16;
static const uint8_t PWM_RESOLUTION_BITS = 8;

// Attaches the LEDC channels using the configured frequency. Call after config load.
void fanControlSetup();

// Re-applies PWM frequency/inversion/output enables after a config change.
void fanControlReconfigure();

// Non-blocking; call every loop iteration.
void fanControlLoop();

// Applies a mode change from the API or MQTT.
//   mode        - "auto" | "manual" | "off" (nullptr keeps the current mode)
//   speed       - manual speed 0..100, or <0 to keep the current one
//   durationSec - 0 makes the change persistent, >0 reverts to auto afterwards
// Returns false if `mode` is not a known mode.
bool fanApplyMode(const char* mode, int speed, uint32_t durationSec, bool& persist);

// Seconds until a temporary manual override expires; 0 when none is running.
uint32_t fanManualExpiresInSec();

}  // namespace blsf

#endif  // BLSF_FAN_CONTROL_H
