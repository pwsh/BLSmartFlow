#include "indicator.h"

#include <WiFi.h>

#include "config.h"
#include "printer_link.h"
#include "state.h"
#include "wifi_manager.h"

namespace blsf {

namespace {

const uint16_t kBlinkMs = 200;    // on and off time of one blink
const uint16_t kGapMs   = 800;    // pause between pattern repeats
const uint16_t kIdleMs  = 3000;   // manual-mode double flash period

uint32_t g_phaseStartMs = 0;
uint8_t  g_lastPattern = 0xFF;

// 0 = solid, 1..4 = blink count, 5 = manual double-flash.
uint8_t currentPattern()
{
    if (wifiIsApMode() || cfg().wifi.ssid[0] == '\0') return 1;
    if (!wifiConnected()) return 2;

    if (printerLinkConfigured()) {
        const PrinterState p = printerSnapshot();
        if (!p.connected) return 3;
        if (printerDataAgeMs(p) >= (uint32_t)cfg().fan.staleSec * 1000UL) return 4;
    }

    const FanState f = fanSnapshot();
    if (strcmp(f.effectiveMode, "manual") == 0) return 5;
    return 0;
}

}  // namespace

void indicatorSetup()
{
    pinMode(PIN_LED, OUTPUT);
    digitalWrite(PIN_LED, LOW);
    g_phaseStartMs = millis();
}

void indicatorLoop()
{
    const uint32_t now = millis();
    const uint8_t pattern = currentPattern();
    if (pattern != g_lastPattern) {
        g_lastPattern = pattern;
        g_phaseStartMs = now;      // restart the pattern cleanly on a change
    }

    if (pattern == 0) {
        digitalWrite(PIN_LED, HIGH);
        return;
    }

    if (pattern == 5) {
        // Solid with two short dips every 3 s: the device is clearly "on", the
        // dips say "not following the curve".
        const uint32_t t = (now - g_phaseStartMs) % kIdleMs;
        const bool on = !((t < 80) || (t >= 200 && t < 280));
        digitalWrite(PIN_LED, on ? HIGH : LOW);
        return;
    }

    // N blinks then a gap; the whole cycle repeats.
    const uint32_t cycleMs = (uint32_t)pattern * 2 * kBlinkMs + kGapMs;
    const uint32_t t = (now - g_phaseStartMs) % cycleMs;
    const uint32_t blinkWindow = (uint32_t)pattern * 2 * kBlinkMs;
    const bool on = (t < blinkWindow) && ((t / kBlinkMs) % 2 == 0);
    digitalWrite(PIN_LED, on ? HIGH : LOW);
}

}  // namespace blsf
