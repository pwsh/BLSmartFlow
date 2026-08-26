#include "cooldown.h"

#include <Arduino.h>
#include <math.h>

#include "config.h"
#include "filament.h"
#include "log.h"
#include "printer_link.h"
#include "state.h"

namespace blsf {

namespace {

// One sample every five seconds. The chamber of a printer moves in minutes, and
// the two-sample hold on the target (cooldown_logic.h) is only meaningful if the
// samples are spaced far enough apart to be independent measurements.
const uint32_t kSampleMs = 5000;

CooldownState g_st;
uint32_t      g_lastStepMs = 0;
bool          g_init = false;

// The state machine has exactly one owner at a time. Normally that is the loop
// task, but POST /api/cooldown runs the step inline (on the AsyncTCP task) so
// its response can report the session it just started - hence a real mutex
// rather than "only ever touch this from loop()".
SemaphoreHandle_t g_lock = nullptr;

struct StepLock {
    StepLock() { if (g_lock) xSemaphoreTakeRecursive(g_lock, portMAX_DELAY); }
    ~StepLock() { if (g_lock) xSemaphoreGiveRecursive(g_lock); }
    StepLock(const StepLock&) = delete;
    StepLock& operator=(const StepLock&) = delete;
};

// Latched by the API and MQTT tasks, consumed by the loop task on its next step.
volatile uint8_t g_command = (uint8_t)CooldownCommand::None;

// Published copy of everything the status document shows. PrinterState uses the
// same trick: a short critical section around a POD copy beats a lock per field.
struct CooldownPublic {
    bool     active;
    CooldownReason reason;
    uint8_t  target;
    float    startChamber;
    uint32_t startedMs;
    uint32_t maxSec;
    uint8_t  aux;
    uint8_t  chamberFan;
    bool     sent;
    uint8_t  ownFan;
    bool     materialKnown;
    char     material[24];
};

CooldownPublic g_pub{};
portMUX_TYPE   g_pubMux = portMUX_INITIALIZER_UNLOCKED;

CooldownPublic pub()
{
    portENTER_CRITICAL(&g_pubMux);
    const CooldownPublic p = g_pub;
    portEXIT_CRITICAL(&g_pubMux);
    return p;
}

void publish(const CooldownRules& cfg)
{
    CooldownPublic p{};
    p.active = g_st.active;
    p.reason = g_st.reason;
    p.target = g_st.active ? g_st.target : cfg.target;
    p.startChamber = g_st.startChamber;
    p.startedMs = g_st.startedMs;
    p.maxSec = (uint32_t)cfg.maxMinutes * 60UL;
    p.aux = g_st.sentAux;
    p.chamberFan = g_st.sentChamber;
    p.sent = g_st.sentOn;
    p.ownFan = cfg.ownFan;
    p.materialKnown = g_st.active && g_st.material[0] != '\0';
    strlcpy(p.material, g_st.material, sizeof(p.material));
    portENTER_CRITICAL(&g_pubMux);
    g_pub = p;
    portEXIT_CRITICAL(&g_pubMux);
}

// Percent to the printer's 0..255 M106 scale.
uint8_t m106Value(uint8_t pct)
{
    if (pct > 100) pct = 100;
    return (uint8_t)((pct * 255 + 50) / 100);
}

// Both fans in one line, because the printer applies a `gcode_line` param as a
// unit: sending them separately would leave a window where the aux fan is
// running and the chamber fan is not.
void sendFans(uint8_t auxPct, uint8_t chamberPct)
{
    char line[PRINTER_GCODE_MAX];
    snprintf(line, sizeof(line), "M106 P2 S%u\nM106 P3 S%u\n",
             (unsigned)m106Value(auxPct), (unsigned)m106Value(chamberPct));
    if (!printerLinkSendGcode(line)) {
        // Nothing is lost: the session re-asserts on the next sample because the
        // state machine still believes the fans are where it last saw them.
        LOGW("cooldown: could not queue fan command (aux %u%%, chamber %u%%)",
             (unsigned)auxPct, (unsigned)chamberPct);
        return;
    }
    LOGI("cooldown: printer fans aux %u%%, chamber %u%%", (unsigned)auxPct, (unsigned)chamberPct);
}

void sendFansOff()
{
    if (!printerLinkSendGcode("M106 P2 S0\nM106 P3 S0\n")) {
        LOGW("cooldown: could not queue the fan stop command");
        return;
    }
    LOGI("cooldown: printer fans released");
}

// One sample of the state machine, with everything it needs read fresh. The
// filament is resolved here rather than cached for the same reason fan_control
// does it: a tray change during the session must move the gentle rule at once.
void step(CooldownCommand cmd)
{
    const uint32_t now = millis();
    if (!g_init) {
        cooldownReset(g_st);
        g_init = true;
    }

    CooldownRules rules;
    FilamentConfig filcfg;
    FanConfig fan;
    {
        ConfigGuard guard;
        rules = cfg().cooldown;
        filcfg = cfg().filament;
        fan = cfg().fan;
    }

    const PrinterState p = printerSnapshot();
    const FilamentStatus fil = filamentResolve(p, filcfg, fan);

    CooldownInputs in{};
    in.phase = printerPhase(p);
    in.gcodeState = p.gcodeState;
    in.chamber = p.chamber;
    in.linkOnline = printerLinkOnline();
    in.gentleMaterial = fil.eff.gentle;
    in.chamberTarget = fil.eff.chamberTarget;
    // REWORK-SPEC 17.1: with filament-aware cooling on, the material's cool-down
    // target (including any override) wins over the plain cooldown.target.
    in.target = filcfg.autoDetect ? fil.eff.cooldownTarget : rules.target;
    in.materialId = fil.id;
    in.command = cmd;

    const CooldownActions a = cooldownStep(g_st, in, rules, now);

    if (a.startedNow) {
        LOGI("cooldown: started, chamber %.1f -> %u C%s", (double)in.chamber,
             (unsigned)g_st.target, rules.usePrinterFans ? ", printer fans allowed" : "");
    }
    if (a.sendFans) sendFans(a.auxPct, a.chamberPct);
    if (a.sendStop) sendFansOff();
    if (a.stoppedNow) {
        LOGI("cooldown: finished (%s) after %u s", cooldownReasonName(a.reason),
             (unsigned)((now - g_st.startedMs) / 1000UL));
    }

    publish(rules);
}

}  // namespace

void cooldownSetup()
{
    if (!g_lock) g_lock = xSemaphoreCreateRecursiveMutex();
    StepLock lock;
    cooldownReset(g_st);
    g_init = true;
    ConfigGuard guard;
    publish(cfg().cooldown);
}

void cooldownLoop()
{
    // Cheap pre-check outside the lock: the common case is "nothing to do", and
    // taking a mutex a thousand times a second to learn that would be silly.
    const uint32_t now = millis();
    if (g_command == (uint8_t)CooldownCommand::None &&
        g_lastStepMs != 0 && (now - g_lastStepMs) < kSampleMs) {
        return;
    }
    StepLock lock;
    const uint8_t cmd = g_command;
    g_command = (uint8_t)CooldownCommand::None;
    g_lastStepMs = millis();
    step((CooldownCommand)cmd);
}

bool cooldownPrinterBusy()
{
    const PrinterState p = printerSnapshot();
    return cooldownPhaseBusy(printerPhase(p));
}

bool cooldownRequest(bool start, const char** err)
{
    if (start && cooldownPrinterBusy()) {
        if (err) *err = "printer is busy";
        return false;
    }
    // Latched rather than executed here: the state machine has a single owner
    // (the loop task) and two tasks stepping it at once would corrupt it. The
    // loop picks the command up on its very next pass, which is a millisecond
    // away - the handler's own status read below is therefore accurate.
    g_command = (uint8_t)(start ? CooldownCommand::Start : CooldownCommand::Stop);
    cooldownLoop();
    return true;
}

uint32_t cooldownRemainingMin()
{
    const CooldownPublic p = pub();
    if (!p.active) return 0;
    const uint32_t elapsed = (millis() - p.startedMs) / 1000UL;
    if (elapsed >= p.maxSec) return 0;
    return (p.maxSec - elapsed + 59UL) / 60UL;
}

CooldownFanRequest cooldownFanRequest()
{
    const CooldownPublic p = pub();
    CooldownFanRequest r{};
    r.active = p.active;
    r.ownFan = p.ownFan;
    r.target = (float)p.target;
    return r;
}

void cooldownToJson(JsonObject out)
{
    const CooldownPublic p = pub();
    const PrinterState pr = printerSnapshot();

    out["active"] = p.active;
    const char* reason = cooldownReasonName(p.reason);
    if (*reason) out["reason"] = reason;      // string literal, no copy needed
    else out["reason"] = nullptr;
    out["target"] = p.target;
    if (isnan(pr.chamber)) out["chamber"] = nullptr;
    else out["chamber"] = roundf(pr.chamber * 10.0f) / 10.0f;
    if (isnan(p.startChamber)) out["startChamber"] = nullptr;
    else out["startChamber"] = roundf(p.startChamber * 10.0f) / 10.0f;
    out["elapsedSec"] = p.active ? (millis() - p.startedMs) / 1000UL : 0UL;
    out["maxSec"] = p.maxSec;

    JsonObject pf = out["printerFans"].to<JsonObject>();
    pf["aux"] = p.aux;
    pf["chamber"] = p.chamberFan;
    pf["sent"] = p.sent;

    out["ownFan"] = cooldownOwnFanName(p.ownFan);
    if (p.materialKnown) out["material"] = String(p.material);
    else out["material"] = nullptr;
}

}  // namespace blsf
