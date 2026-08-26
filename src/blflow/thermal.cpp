#include "thermal.h"

#include <math.h>

#include "config.h"
#include "log.h"
#include "state.h"
#include "thermal_math.h"

namespace blsf {

namespace {

// Sampling cadence. Fast enough that a 60 s window has a dozen points, slow
// enough that it costs nothing.
const uint32_t kSampleIntervalMs = 5000;
// A window must last at least this long before its slope means anything.
const float    kMinWindowSec = 60.0f;
// EMA weight for blending a new measurement into a bucket.
const float    kAlpha = 0.3f;
// Flash budget: the learned table is worth persisting, but not every minute.
const uint32_t kPersistIntervalMs = 600000;

ThermalWindow g_window;
uint32_t      g_lastSampleMs = 0;
uint32_t      g_lastPersistMs = 0;
bool          g_pendingPersist = false;

void setNullable(JsonArray arr, float v)
{
    if (isnan(v)) arr.add(nullptr);
    else arr.add(roundf(v * 1000.0f) / 1000.0f);
}

}  // namespace

void thermalSetup()
{
    thermalWindowReset(g_window);
    g_lastSampleMs = 0;
    g_lastPersistMs = 0;
    g_pendingPersist = false;
}

void thermalLoop()
{
    const uint32_t now = millis();
    if (g_lastSampleMs != 0 && (now - g_lastSampleMs) < kSampleIntervalMs) {
        // Still persist a pending result even when no sample is due.
        if (g_pendingPersist && (g_lastPersistMs == 0 || (now - g_lastPersistMs) >= kPersistIntervalMs)) {
            g_pendingPersist = false;
            g_lastPersistMs = now;
            configMarkDirty();
        }
        return;
    }
    g_lastSampleMs = now;

    const PrinterState p = printerSnapshot();
    const FanState f = fanSnapshot();

    float ambient;
    {
        ConfigGuard guard;
        ambient = (float)cfg().fan.ambientTemp;
    }

    // A non-zero bed or nozzle target means the printer is putting heat in, and
    // the slope then says nothing about how well the fan removes it.
    const bool heaterActive = (!isnan(p.bedTarget) && p.bedTarget > 0.0f) ||
                              (!isnan(p.nozzleTarget) && p.nozzleTarget > 0.0f);
    const bool linkUp = p.everUpdated && printerDataAgeMs(p) < 60000UL;

    // The open-door bucket is only ever filled when the door state is genuinely
    // known; an unproven switch counts as closed, exactly as in the control loop.
    const ThermalSample s =
        thermalFeed(g_window, (float)now / 1000.0f, linkUp ? p.chamber : NAN, f.output,
                    printerDoorOpen(p), heaterActive || !linkUp, ambient, kMinWindowSec);

    if (!s.valid) return;

    {
        ConfigGuard guard;
        Config& c = cfg();
        float& slot = s.door ? c.thermal.kOpen[s.bucket] : c.thermal.kClosed[s.bucket];
        slot = thermalBlend(slot, s.k, kAlpha);
        if (c.thermal.samples < UINT32_MAX) c.thermal.samples++;
    }
    LOGI("thermal: k=%.3f /min at %u%% door %s (%.2f degC/min)", (double)s.k,
         (unsigned)thermalBucketPercent(s.bucket), s.door ? "open" : "closed",
         (double)s.rateCPerMin);

    g_pendingPersist = true;
    if (g_lastPersistMs == 0 || (now - g_lastPersistMs) >= kPersistIntervalMs) {
        g_pendingPersist = false;
        g_lastPersistMs = now;
        configMarkDirty();
    }
}

float thermalRateCPerMin() { return g_window.rateCPerMin; }

void thermalToJson(JsonObject out)
{
    const float rate = g_window.rateCPerMin;
    if (isnan(rate)) out["rateCPerMin"] = nullptr;
    else out["rateCPerMin"] = roundf(rate * 100.0f) / 100.0f;

    JsonArray closed = out["kClosed"].to<JsonArray>();
    JsonArray open = out["kOpen"].to<JsonArray>();
    uint32_t samples;
    {
        ConfigGuard guard;
        const Config& c = cfg();
        for (uint8_t i = 0; i < THERMAL_BUCKETS; i++) setNullable(closed, c.thermal.kClosed[i]);
        for (uint8_t i = 0; i < THERMAL_BUCKETS; i++) setNullable(open, c.thermal.kOpen[i]);
        samples = c.thermal.samples;
    }
    out["samples"] = samples;
}

}  // namespace blsf
