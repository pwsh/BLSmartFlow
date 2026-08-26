// web_server.h - ESPAsyncWebServer: UI, REST API, SSE, OTA and captive portal.
//
// Everything runs on the AsyncTCP task, so handlers must be short. Saving the
// config (a few kB to LittleFS, single-digit milliseconds) is acceptable;
// restarts are never performed inline - they are deferred to loop() through
// app.h so the response can be flushed first.
//
// See REWORK-SPEC section 9 for the route table.

#ifndef BLSF_WEB_SERVER_H
#define BLSF_WEB_SERVER_H

#include <Arduino.h>

namespace blsf {

void webServerSetup();

// Pushes SSE status/log events. Non-blocking; call every loop iteration.
void webServerLoop();

}  // namespace blsf

#endif  // BLSF_WEB_SERVER_H
