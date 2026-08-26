#include "status.h"

#include <WiFi.h>
#include <math.h>

#include "config.h"
#include "fan_control.h"
#include "ha_mqtt.h"
#include "printer_link.h"
#include "state.h"
#include "thermal.h"
#include "version.h"
#include "wifi_manager.h"

namespace blsf {

namespace {

// Unknown readings are JSON null rather than 0, so the UI can show "--" instead
// of a plausible-looking wrong number.
void setTemp(JsonObject o, const char* key, float v)
{
    if (isnan(v)) o[key] = nullptr;
    else o[key] = roundf(v * 10.0f) / 10.0f;
}

void setFan(JsonObject o, const char* key, int8_t v)
{
    if (v < 0) o[key] = nullptr;
    else o[key] = (int)v;
}

// The parser stores "not reported yet" as a negative sentinel. Serialising that
// as -1 would have the UI drawing a progress bar at minus one percent, so the
// whole family goes out as null.
void setCount(JsonObject o, const char* key, int v)
{
    if (v < 0) o[key] = nullptr;
    else o[key] = v;
}

}  // namespace

void buildStatus(JsonObject out)
{
    // Readers need a coherent view too: without the lock a config write on the
    // AsyncTCP task can change fan.mode between two lines of this function.
    ConfigGuard guard;
    const Config& c = cfg();
    const PrinterState p = printerSnapshot();
    const FanState f = fanSnapshot();
    // NOTE: p and f are locals. ArduinoJson stores a `const char*` by POINTER
    // (it assumes string literals) and only deep-copies `char*`/String, so every
    // char-array field below is wrapped in String() - the document outlives this
    // function and a bare pointer into p/f would serialise as garbage.

    JsonObject d = out["device"].to<JsonObject>();
    d["fw"] = FW_VERSION;
    d["uptimeSec"] = (uint32_t)(millis() / 1000);
    d["heapFree"] = ESP.getFreeHeap();
    d["heapMin"] = ESP.getMinFreeHeap();
    d["chipId"] = chipId();
    d["hostname"] = String(c.wifi.hostname);
    d["ip"] = wifiIp().toString();
    d["apMode"] = wifiIsApMode();

    JsonObject w = out["wifi"].to<JsonObject>();
    const bool wifiUp = wifiConnected();
    w["connected"] = wifiUp;
    // Empty rather than the configured SSID: this field says what we are joined
    // to, and "" is the honest answer when we are not joined to anything.
    w["ssid"] = wifiUp ? WiFi.SSID() : String("");
    w["bssid"] = wifiUp ? WiFi.BSSIDstr() : String("");
    w["rssi"] = wifiUp ? WiFi.RSSI() : 0;
    w["channel"] = wifiUp ? WiFi.channel() : 0;

    JsonObject pr = out["printer"].to<JsonObject>();
    pr["configured"] = printerLinkConfigured();
    pr["connected"] = p.connected;
    pr["online"] = printerLinkOnline();
    const uint32_t age = printerDataAgeMs(p);
    if (age == UINT32_MAX) pr["lastUpdateSec"] = nullptr;
    else pr["lastUpdateSec"] = age / 1000;
    pr["mqttState"] = p.mqttState;
    pr["mqttStateText"] = String(p.mqttStateText);
    pr["state"] = String(p.gcodeState);
    const Phase phase = printerPhase(p);
    pr["printing"] = phaseIsPrinting(phase);
    // Derived phase (REWORK-SPEC 15.1): what the fan logic actually reasons
    // about, which "RUNNING" on its own cannot express.
    pr["phase"] = phaseName(phase);
    setCount(pr, "stage", p.stage);
    pr["stageText"] = stageText(p.stage);
    setCount(pr, "progress", p.progress);
    setCount(pr, "remainingMin", p.remainingMin);
    setCount(pr, "layer", p.layer);
    setCount(pr, "totalLayers", p.totalLayers);
    pr["task"] = String(p.task);
    // Null, not false: on some X1C units the door switch never actuates, and the
    // raw bit means nothing until it has been seen to change (REWORK-SPEC 15.1).
    if (p.doorKnown) pr["doorOpen"] = p.doorOpen;
    else             pr["doorOpen"] = nullptr;
    pr["doorKnown"] = p.doorKnown;
    pr["doorEdgeCount"] = p.doorEdgeCount;
    pr["printError"] = p.printError;
    pr["wifiSignal"] = String(p.wifiSignal);

    JsonObject t = pr["temps"].to<JsonObject>();
    setTemp(t, "nozzle", p.nozzle);
    setTemp(t, "nozzleTarget", p.nozzleTarget);
    setTemp(t, "bed", p.bed);
    setTemp(t, "bedTarget", p.bedTarget);
    setTemp(t, "chamber", p.chamber);
    setTemp(t, "chamberTarget", p.chamberTarget);

    JsonObject fans = pr["fans"].to<JsonObject>();
    setFan(fans, "part", p.fanPart);
    setFan(fans, "aux", p.fanAux);
    setFan(fans, "chamber", p.fanChamber);
    setFan(fans, "heatbreak", p.fanHeatbreak);

    JsonObject fo = out["fan"].to<JsonObject>();
    fo["output"] = lroundf(f.output);
    fo["target"] = lroundf(f.target);
    fo["mode"] = String(c.fan.mode);
    fo["effectiveMode"] = String(f.effectiveMode);
    fo["source"] = String(c.fan.source);
    setTemp(fo, "sourceTemp", f.sourceTemp);
    // The thermostat set point in force this instant; null in every other mode.
    setTemp(fo, "setpoint", f.setpoint);
    // Configured set points travel with the status so the Home Assistant number
    // entities (and the UI) have a state to read back.
    fo["chamberTarget"] = c.fan.chamberTarget;
    fo["cooldownTarget"] = c.fan.cooldownTarget;
    fo["manualSpeed"] = c.fan.manualSpeed;
    fo["manualExpiresSec"] = fanManualExpiresInSec();
    fo["pwmDuty"] = f.pwmDuty;
    fo["output1"] = c.fan.output1;
    fo["output2"] = c.fan.output2;

    thermalToJson(out["thermal"].to<JsonObject>());

    JsonObject mx = out["mqttExt"].to<JsonObject>();
    mx["enabled"] = c.mqtt.enabled;
    mx["connected"] = haMqttConnected();
}

}  // namespace blsf
