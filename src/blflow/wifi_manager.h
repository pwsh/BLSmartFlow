// wifi_manager.h - non-blocking WiFi station manager with AP fallback.
//
// The 1.x firmware connected inside a `while` loop with 2 s delays, so a wrong
// password or a missing AP froze the device permanently. Here everything is a
// state machine advanced from loop():
//
//   IDLE -> CONNECTING -> CONNECTED
//   on timeout (20 s) back off (5 s, 10 s, 30 s, 60 s ...) and retry
//   after 3 failed cycles the BSSID lock is dropped (the AP may have moved)
//   after 90 s without a link an open setup AP is raised alongside the STA
//   retries, with a DNS catch-all so any name resolves to the portal
//
// The AP stays up for 5 minutes after a late STA connect so a user who is
// mid-configuration is not cut off.

#ifndef BLSF_WIFI_MANAGER_H
#define BLSF_WIFI_MANAGER_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <IPAddress.h>

namespace blsf {

void wifiSetup();
void wifiLoop();

// Re-reads credentials and restarts the connection attempt from scratch.
void wifiReconfigure();

bool      wifiConnected();
bool      wifiIsApMode();
IPAddress wifiIp();            // STA address, or the AP address in AP-only mode
const char* wifiApSsid();

// --- async scan -------------------------------------------------------------
enum class ScanState { Idle, Running, Done };

// Starts a scan if none is running. `force` discards a cached result first, so
// GET /api/wifi/scan?force=1 always reflects the airwaves right now.
ScanState wifiScanStart(bool force = false);
ScanState wifiScanState();
// Fills `out` with the finished scan, deduplicated by SSID (strongest kept) and
// sorted by RSSI descending. Returns the number of entries written.
size_t wifiScanResults(JsonArray out);

}  // namespace blsf

#endif  // BLSF_WIFI_MANAGER_H
