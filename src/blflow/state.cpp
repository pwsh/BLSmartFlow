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

const char* stageText(int stage)
{
    // Table from ha-bambulab's CURRENT_STAGE_IDS. 255 (and -1 internally) is the
    // printer's "nothing in progress" value.
    switch (stage) {
        case 0:  return "printing";
        case 1:  return "auto_bed_leveling";
        case 2:  return "heatbed_preheating";
        case 3:  return "sweeping_xy_mech_mode";
        case 4:  return "changing_filament";
        case 5:  return "m400_pause";
        case 6:  return "paused_filament_runout";
        case 7:  return "heating_hotend";
        case 8:  return "calibrating_extrusion";
        case 9:  return "scanning_bed_surface";
        case 10: return "inspecting_first_layer";
        case 11: return "identifying_build_plate_type";
        case 12: return "calibrating_micro_lidar";
        case 13: return "homing_toolhead";
        case 14: return "cleaning_nozzle_tip";
        case 15: return "checking_extruder_temperature";
        case 16: return "paused_user";
        case 17: return "paused_front_cover_falling";
        case 18: return "calibrating_lidar";
        case 19: return "calibrating_extrusion_flow";
        case 20: return "paused_nozzle_temperature_malfunction";
        case 21: return "paused_heat_bed_temperature_malfunction";
        case -1:
        case 255: return "idle";
        default: return "unknown";
    }
}

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
