// app.h - the few actions modules need to ask of main().
//
// Restarting from inside an async web handler or the MQTT task would tear down
// the stack that is still executing, so requests are recorded here and carried
// out from loop() once the current work has finished.

#ifndef BLSF_APP_H
#define BLSF_APP_H

#include <Arduino.h>

namespace blsf {

// Schedules ESP.restart() `delayMs` from now. Repeated calls keep the earliest.
void appRequestRestart(uint32_t delayMs);

// Wipes the configuration and restarts into AP mode.
void appRequestFactoryReset();

// Re-applies every live-changeable setting after a config write.
void appApplyConfig(bool printerChanged, bool mqttChanged, bool fanChanged, bool ssdpChanged);

}  // namespace blsf

#endif  // BLSF_APP_H
