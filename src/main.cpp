// BLSmartFlow 2.0 - firmware entry point.
//
// setup() brings the modules up in dependency order; loop() is a set of
// non-blocking pumps. The only thing that ever blocks for long - the printer's
// TLS MQTT session - lives in its own FreeRTOS task (see printer_link.cpp), so
// an unreachable printer can never stall fan control or the web UI.

#include <Arduino.h>
#include <LittleFS.h>

#include "blflow/app.h"
#include "blflow/config.h"
#include "blflow/fan_control.h"
#include "blflow/ha_mqtt.h"
#include "blflow/indicator.h"
#include "blflow/log.h"
#include "blflow/printer_link.h"
#include "blflow/serial_provision.h"
#include "blflow/ssdp.h"
#include "blflow/state.h"
#include "blflow/version.h"
#include "blflow/web_server.h"
#include "blflow/wifi_manager.h"

using namespace blsf;

namespace {

// Deferred actions requested from async handlers / other tasks.
// A separate flag rather than "g_restartAtMs != 0" as the sentinel: millis()
// really can be 0 at the moment a request is made, and a restart scheduled then
// would simply never fire.
volatile bool     g_restartPending = false;
volatile uint32_t g_restartAtMs = 0;
volatile bool     g_factoryReset = false;

bool g_wifiWasConnected = false;
bool g_servicesStarted = false;

}  // namespace

namespace blsf {

void appRequestRestart(uint32_t delayMs)
{
    const uint32_t at = millis() + delayMs;
    // Keep the earliest pending deadline so a later, longer request cannot
    // postpone a restart that is already imminent.
    if (!g_restartPending || (int32_t)(at - g_restartAtMs) < 0) g_restartAtMs = at;
    g_restartPending = true;
}

void appRequestFactoryReset()
{
    g_factoryReset = true;
    appRequestRestart(750);
}

void appApplyConfig(bool printerChanged, bool mqttChanged, bool fanChanged, bool ssdpChanged)
{
    logSetSerialEnabled(cfg().debug.serial);
    if (fanChanged) fanControlReconfigure();
    // The printer task also needs to see debug.mqttDump and fan.staleSec, both of
    // which it snapshots in reconfigure().
    printerLinkReconfigure();
    (void)printerChanged;
    if (mqttChanged) haMqttReconfigure();
    if (ssdpChanged) ssdpReconfigure();
}

}  // namespace blsf

void setup()
{
    Serial.begin(115200);
    logInit();

    stateInit();
    configLoad();
    logSetSerialEnabled(cfg().debug.serial);

    LOGI("%s %s (built %s)", FW_NAME, FW_VERSION, FW_BUILD);
    LOGI("chip %s, heap %u bytes", chipId(), (unsigned)ESP.getFreeHeap());

    indicatorSetup();
    fanControlSetup();
    serialProvisionSetup();

    // Fan control must be live before the network comes up: a printer that is
    // already hot should not wait for DHCP.
    fanControlLoop();

    wifiSetup();
    webServerSetup();
    printerLinkStart();
    haMqttSetup();
}

void loop()
{
    // Order matters only in that fan control runs first, so its timing is never
    // pushed around by slower housekeeping.
    fanControlLoop();
    indicatorLoop();
    wifiLoop();
    serialProvisionLoop();
    webServerLoop();
    haMqttLoop();
    ssdpLoop();
    // Persists a config that a fan/mode command marked dirty, at most every 10 s.
    configLoopSave();

    // Services that need an IP are started once the station first comes up.
    const bool up = wifiConnected();
    if (up && !g_wifiWasConnected) {
        if (!g_servicesStarted) {
            ssdpStart();
            g_servicesStarted = true;
        }
        haMqttReconfigure();     // rebuild the configuration_url with the new IP
    }
    g_wifiWasConnected = up;

    if (g_restartPending && (int32_t)(millis() - g_restartAtMs) >= 0) {
        if (g_factoryReset) {
            configWipe();
            LittleFS.end();
        }
        LOGW("restarting");
        Serial.flush();
        ESP.restart();
    }

    // One tick of yield: keeps the idle task fed (watchdog) without adding
    // meaningful latency to anything above.
    delay(1);
}
