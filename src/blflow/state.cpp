#include "state.h"
#include <string.h>

namespace blsf {

namespace {
PrinterState g_printer;
FanState     g_fan;
portMUX_TYPE g_printerMux = portMUX_INITIALIZER_UNLOCKED;
portMUX_TYPE g_fanMux = portMUX_INITIALIZER_UNLOCKED;
}  // namespace

void stateInit()
{
    PrinterState p{};
    printerReportInit(p);
    p.connected = false;
    p.mqttState = -1;
    strlcpy(p.mqttStateText, "disconnected", sizeof(p.mqttStateText));
    p.lastUpdateMs = 0;
    p.everUpdated = false;
    printerCommit(p);

    FanState f{};
    f.output = 0.0f;
    f.target = 0.0f;
    strlcpy(f.effectiveMode, "off", sizeof(f.effectiveMode));
    f.sourceTemp = TEMP_UNKNOWN;
    f.setpoint = TEMP_UNKNOWN;
    f.pwmDuty = 0;
    f.manualExpiresAt = 0;
    f.kicking = false;
    fanCommit(f);
}

PrinterState printerSnapshot()
{
    portENTER_CRITICAL(&g_printerMux);
    PrinterState s = g_printer;
    portEXIT_CRITICAL(&g_printerMux);
    return s;
}

PrinterState printerBegin() { return printerSnapshot(); }

void printerCommit(const PrinterState& s)
{
    portENTER_CRITICAL(&g_printerMux);
    g_printer = s;
    portEXIT_CRITICAL(&g_printerMux);
}

void printerSetMqtt(bool connected, int state, const char* text)
{
    portENTER_CRITICAL(&g_printerMux);
    g_printer.connected = connected;
    g_printer.mqttState = state;
    strlcpy(g_printer.mqttStateText, text ? text : "", sizeof(g_printer.mqttStateText));
    portEXIT_CRITICAL(&g_printerMux);
}

void printerMarkUpdated()
{
    const uint32_t now = millis();
    portENTER_CRITICAL(&g_printerMux);
    g_printer.lastUpdateMs = now;
    g_printer.everUpdated = true;
    portEXIT_CRITICAL(&g_printerMux);
}

FanState fanSnapshot()
{
    portENTER_CRITICAL(&g_fanMux);
    FanState s = g_fan;
    portEXIT_CRITICAL(&g_fanMux);
    return s;
}

void fanCommit(const FanState& s)
{
    portENTER_CRITICAL(&g_fanMux);
    g_fan = s;
    portEXIT_CRITICAL(&g_fanMux);
}

void fanCommitControl(const FanState& s)
{
    portENTER_CRITICAL(&g_fanMux);
    const uint32_t expiry = g_fan.manualExpiresAt;   // owned by the API tasks
    g_fan = s;
    g_fan.manualExpiresAt = expiry;
    portEXIT_CRITICAL(&g_fanMux);
}

void fanSetManualExpiry(uint32_t atMs)
{
    portENTER_CRITICAL(&g_fanMux);
    g_fan.manualExpiresAt = atMs;
    portEXIT_CRITICAL(&g_fanMux);
}

uint32_t printerDataAgeMs(const PrinterState& s)
{
    if (!s.everUpdated) return UINT32_MAX;
    return millis() - s.lastUpdateMs;
}

// The table itself lives in printer_parse.h so the host tests can reach it
// (state.h drags in Arduino.h). Codes 0..77 plus -1/255 (idle) and -2 (offline).
const char* stageText(int stage) { return stageName(stage); }

const char* mqttStateText(int state)
{
    switch (state) {
        case -4: return "timeout";
        case -3: return "connection_lost";
        case -2: return "connect_failed";
        case -1: return "disconnected";
        case  0: return "connected";
        case  1: return "bad_protocol";
        case  2: return "bad_client_id";
        case  3: return "unavailable";
        case  4: return "bad_credentials";
        case  5: return "unauthorized";
        default: return "unknown";
    }
}

}  // namespace blsf
