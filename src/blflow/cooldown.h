// cooldown.h - the post-print cool-down session (REWORK-SPEC 17).
//
// After a print the printer reports FINISH and stops driving its own fans. This
// module runs a short session that empties the chamber down to a target: the
// device's own fan follows `cooldown.ownFan`, and - only when the user has opted
// in - the printer's auxiliary and chamber fans are borrowed by sending
// `M106 P2/P3` over the printer link.
//
// The decisions live in cooldown_logic.h (pure, host-tested); everything here is
// the plumbing: the 5-second sample tick on the loop task, the config and
// filament lookups, the command latch shared with the API/MQTT tasks, and the
// snapshot the status document reads.

#ifndef BLSF_COOLDOWN_H
#define BLSF_COOLDOWN_H

#include <ArduinoJson.h>

#include "cooldown_logic.h"

namespace blsf {

// What the fan controller needs to know about a running session. Copied out
// under a spinlock, so it is safe to call from the loop task every 100 ms.
struct CooldownFanRequest {
    bool    active;
    uint8_t ownFan;     // CD_OWN_*
    float   target;     // degC set point for CD_OWN_THERMOSTAT
};

void cooldownSetup();

// Non-blocking; call every loop iteration. Samples every 5 s, plus immediately
// whenever a command has been latched.
void cooldownLoop();

// Starts or stops a session from the API / MQTT. Returns false and leaves
// `err` pointing at a message when the printer is busy printing.
bool cooldownRequest(bool start, const char** err);

// True while a print is running, i.e. while a manual start must be refused.
bool cooldownPrinterBusy();

// The `cooldown` block of the status document (REWORK-SPEC 17.3).
void cooldownToJson(JsonObject out);

// Minutes left before the hard stop; 0 when no session is running.
uint32_t cooldownRemainingMin();

CooldownFanRequest cooldownFanRequest();

}  // namespace blsf

#endif  // BLSF_COOLDOWN_H
