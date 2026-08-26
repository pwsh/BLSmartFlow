// state.h - shared runtime state, safe to read from any task.
//
// PrinterState is written by the printer MQTT task and read by the loop, the web
// server (AsyncTCP task) and the external MQTT client. Rather than lock around
// individual fields, readers take a whole-struct snapshot inside a very short
// portMUX critical section. That is only sound if the struct is POD - hence
// fixed-size char arrays and no String/std::vector anywhere inside it.
//
// FanState and DeviceState are only touched from the loop task but are exposed
// through the same snapshot API for consistency.

#ifndef BLSF_STATE_H
#define BLSF_STATE_H

#include <Arduino.h>

#include "printer_parse.h"

namespace blsf {

// Fan speeds and temperatures use these sentinels for "not reported yet",
// which the API serialises as JSON null.
static const float TEMP_UNKNOWN = REPORT_TEMP_UNKNOWN;
static const int8_t FAN_UNKNOWN = REPORT_FAN_UNKNOWN;

// PrinterState = the parsed report (printer_parse.h) plus link bookkeeping.
// Inheriting keeps it an aggregate, so it stays trivially copyable and the
// snapshot copy below is a plain memcpy.
struct PrinterState : PrinterReport {
    bool     connected;          // MQTT session up
    int      mqttState;          // PubSubClient state() code
    char     mqttStateText[24];
    uint32_t lastUpdateMs;       // millis() of the last accepted report (0 = never)
    bool     everUpdated;
};


struct FanState {
    float    output;             // percent actually driven (post ramp/clamp)
    float    target;             // percent requested by the active mode
    char     effectiveMode[10];  // off/manual/stale/idle/cooldown/auto
    float    sourceTemp;         // NaN when the configured source is unavailable
    uint16_t pwmDuty;            // 0..255 written to LEDC
    uint32_t manualExpiresAt;    // millis() deadline, 0 = no expiry
    bool     kicking;            // kick-start pulse in progress
};

// Initialises every field to its "nothing known yet" value.
void stateInit();

// --- printer state ---------------------------------------------------------
// Writers prepare a local copy (printerBegin), patch it, then printerCommit().
PrinterState printerSnapshot();
void         printerCommit(const PrinterState& s);
// Read-modify-write helper for the MQTT task: returns the current value so the
// caller can patch only the fields present in a report.
PrinterState printerBegin();

void printerSetMqtt(bool connected, int state, const char* text);
void printerMarkUpdated();

// --- fan state -------------------------------------------------------------
// The control loop owns every field except manualExpiresAt, which is set by the
// API and MQTT tasks. Splitting the writers keeps a temporary override from
// being lost to a snapshot/commit cycle that started before it arrived.
FanState fanSnapshot();
void     fanCommit(const FanState& s);
// Commits the control-loop outputs, leaving manualExpiresAt untouched.
void     fanCommitControl(const FanState& s);
// Patches only the temporary-override deadline (0 = no expiry).
void     fanSetManualExpiry(uint32_t atMs);

// True when the printer is actively working (used by onlyWhilePrinting).
inline bool printerIsPrinting(const PrinterState& s) { return reportIsPrinting(s); }
// Age of the newest accepted report in milliseconds; UINT32_MAX if never.
uint32_t printerDataAgeMs(const PrinterState& s);

// ha-bambulab stg_cur lookup, e.g. 2 -> "heatbed_preheating". Never null.
const char* stageText(int stage);
// PubSubClient state() -> human text. Never null.
const char* mqttStateText(int state);

}  // namespace blsf

#endif  // BLSF_STATE_H
