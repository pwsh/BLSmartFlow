// ssdp.h - UPnP self-advertisement so tools such as xTouch can find the device.
//
// Secondary to mDNS, which is the primary discovery mechanism. Compiled out
// entirely unless -DBLSF_SSDP is set; the functions then become no-ops so the
// call sites need no #ifdefs.

#ifndef BLSF_SSDP_H
#define BLSF_SSDP_H

#include <Arduino.h>

namespace blsf {

// Starts advertising if config.ssdp.enabled. Call once WiFi is up.
void ssdpStart();
// Non-blocking pump: applies a pending reconfigure and retries a failed start.
// Call every loop iteration.
void ssdpLoop();
// UPnP device description XML for GET /description.xml (empty when SSDP is off).
const char* ssdpSchema();
// Requests a re-evaluation of config.ssdp.enabled. Only raises a flag - the
// start/stop happens in ssdpLoop(), on the loop task.
void ssdpReconfigure();
bool ssdpActive();

}  // namespace blsf

#endif  // BLSF_SSDP_H
