#include "ssdp.h"

#include "config.h"
#include "log.h"
#include "version.h"
#include "wifi_manager.h"

#ifdef BLSF_SSDP
#include <WiFi.h>
#include <ESP32SSDP.h>
#endif

namespace blsf {

namespace {
bool g_active = false;
volatile bool g_reconfigure = false;

// SSDP.begin() binds a multicast socket, which fails while the interface is
// still settling. Latching that failure meant the device never advertised
// again, so retry on a slow cadence instead.
const uint32_t kRetryIntervalMs = 30000;
uint32_t g_nextTryMs = 0;
}

#ifdef BLSF_SSDP

void ssdpStart()
{
    if (g_active || !cfg().ssdp.enabled) return;
    const uint32_t now = millis();
    if (g_nextTryMs != 0 && (int32_t)(now - g_nextTryMs) < 0) return;
    g_nextTryMs = now + kRetryIntervalMs;

    SSDP.setSchemaURL("description.xml");
    SSDP.setHTTPPort(80);
    SSDP.setDeviceType("urn:schemas-upnp-org:device:Basic:1");
    SSDP.setName(FW_NAME);
    SSDP.setSerialNumber(chipId());
    SSDP.setURL("/");
    SSDP.setModelName(FW_NAME " ESP32");
    SSDP.setModelNumber(FW_VERSION);
    SSDP.setManufacturer("DutchDeveloper");
    SSDP.setManufacturerURL("https://dutchdevelop.com");
    if (!SSDP.begin()) {
        LOGW("SSDP start failed, retrying in %u s", (unsigned)(kRetryIntervalMs / 1000));
        return;
    }
    g_active = true;
    LOGI("SSDP advertising");
}

void ssdpLoop()
{
    bool enabled;
    {
        ConfigGuard guard;
        enabled = cfg().ssdp.enabled;
    }

    if (g_reconfigure) {
        g_reconfigure = false;
        g_nextTryMs = 0;              // a deliberate change gets an immediate try
        if (!enabled && g_active) {
            SSDP.end();
            g_active = false;
            LOGI("SSDP stopped");
            return;
        }
    }
    // Binding the multicast socket only works once the station has an address.
    if (enabled && !g_active && wifiConnected()) ssdpStart();
}

// Called from web handlers and the serial reader; SSDP.begin()/end() touch a
// socket the loop task owns, so only raise a flag here.
void ssdpReconfigure() { g_reconfigure = true; }

#else   // SSDP compiled out

void ssdpStart() {}
void ssdpLoop() {}
void ssdpReconfigure() {}

#endif

bool ssdpActive() { return g_active; }

}  // namespace blsf
