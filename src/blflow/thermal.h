// thermal.h - passive cooling-rate learning (REWORK-SPEC 15.4).
//
// Purely observational: it never touches the fan. Every 5 s it looks at the
// chamber temperature and, whenever the conditions are quiet enough to be
// informative (steady fan output, unchanged door state, no heater running), it
// fits Newton's cooling law and remembers the constant for that fan setting.
//
// The point of the exercise is a question the UI can then answer honestly:
// "with the door shut and the fan at 50 %, how long until the chamber is cool
// enough to open?". No gain tuning is derived from it.
//
// Runs from the main loop, never blocks, and persists at most once every ten
// minutes through the existing dirty/loop-save mechanism.

#ifndef BLSF_THERMAL_H
#define BLSF_THERMAL_H

#include <Arduino.h>
#include <ArduinoJson.h>

namespace blsf {

void thermalSetup();

// Non-blocking; call every loop iteration.
void thermalLoop();

// Fills `out` with the status block: rateCPerMin, kClosed[5], kOpen[5],
// samples. Never-measured values are JSON null, never NaN.
void thermalToJson(JsonObject out);

// Current chamber slope in degC/min, NaN while no window is running. Exposed so
// the MQTT sensor and the status document agree by construction.
float thermalRateCPerMin();

}  // namespace blsf

#endif  // BLSF_THERMAL_H
