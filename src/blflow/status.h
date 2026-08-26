// status.h - builds the canonical status document.
//
// Exactly one implementation feeds GET /api/status, the SSE `status` event and
// the external MQTT `<base>/state` topic, so the three can never drift apart.

#ifndef BLSF_STATUS_H
#define BLSF_STATUS_H

#include <ArduinoJson.h>

namespace blsf {

// Fills `out` with the status object described in REWORK-SPEC section 9.
void buildStatus(JsonObject out);

}  // namespace blsf

#endif  // BLSF_STATUS_H
