// indicator.h - status LED on GPIO 21.
//
// Purely millis()-driven: the old implementation blocked the whole loop with
// delay() while blinking, which froze fan control and the web UI. Here the
// pattern is a small state machine evaluated once per loop pass.
//
// Priority (highest first):
//   1 blink   unprovisioned / running the setup access point
//   2 blinks  WiFi down
//   3 blinks  printer MQTT down
//   4 blinks  printer data stale
//   double-flash every 3 s   manual fan override active
//   solid     everything nominal

#ifndef BLSF_INDICATOR_H
#define BLSF_INDICATOR_H

#include <Arduino.h>

namespace blsf {

static const uint8_t PIN_LED = 21;

void indicatorSetup();
void indicatorLoop();

}  // namespace blsf

#endif  // BLSF_INDICATOR_H
