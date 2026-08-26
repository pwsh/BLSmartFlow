#include "fan_control.h"

#include <math.h>

#include "config.h"
#include "log.h"
#include "printer_link.h"
#include "state.h"
#include "thermostat.h"

namespace blsf {

namespace {

const uint32_t kRecomputeIntervalMs = 100;

bool     g_attached = false;
uint32_t g_attachedFreq = 0;
uint32_t g_lastComputeMs = 0;
uint16_t g_lastDuty1 = 0xFFFF;      // impossible value forces the first write
uint16_t g_lastDuty2 = 0xFFFF;

// Reattaching LEDC from the AsyncTCP or MQTT task would race with the loop task
// that is writing duty cycles, so the request is only a flag; the work happens
// at the top of fanControlLoop(). Same for "recompute now".
volatile bool g_reconfigure = false;
volatile bool g_recomputeNow = false;

// Hysteresis anchor: the source temperature the current curve target was
// computed from. The target only moves once the source drifts past it.
float    g_heldSourceTemp = NAN;
float    g_curveTarget = 0.0f;

// When the print ended (millis()), for the onlyWhilePrinting cooldown.
uint32_t g_printEndedMs = 0;
bool     g_wasPrinting = false;

// Chamber thermostat. The PI step itself is pure (thermostat.h); everything
// stateful lives here so the controller can be reset the moment its set point
// or its mode changes rather than dragging an old integral into a new regime.
ThermostatState g_pi = {0.0f};
uint32_t g_lastThermostatMs = 0;
float    g_thermostatOut = 0.0f;
float    g_lastSetpoint = NAN;

// Door anti-flap: the door rule stays armed for doorResumeSec after the
// door closes, so pulling a part out in three goes does not make the fan stutter.

// Kick-start pulse bookkeeping. g_zeroSinceMs is when the output last reached
// 0 %: a fan that is merely dipping through zero must not re-arm a kick on every
// pass, or minSpeed plus a jittery curve turns into a continuous 100 % pulse.
uint32_t g_kickUntilMs = 0;
uint32_t g_zeroSinceMs = 0;
const uint32_t kKickRearmAfterMs = 2000;

// Ramp accumulator, kept separate from the published output: if the clamped
// output were fed back into the slew, minSpeed would swallow every small step
// and the fan could never leave 0 %.
float    g_slew = 0.0f;

void attachOutputs(uint32_t freq)
{
    if (g_attached) {
        ledcDetach(PIN_FAN1);
        ledcDetach(PIN_FAN2);
        g_attached = false;
    }
    // Arduino-ESP32 3.x picks the LEDC channel/timer itself.
    const bool ok1 = ledcAttach(PIN_FAN1, freq, PWM_RESOLUTION_BITS);
    const bool ok2 = ledcAttach(PIN_FAN2, freq, PWM_RESOLUTION_BITS);
    if (!ok1 || !ok2) {
        LOGE("fan: ledcAttach failed at %u Hz", (unsigned)freq);
        return;
    }
    g_attached = true;
    g_attachedFreq = freq;
    g_lastDuty1 = g_lastDuty2 = 0xFFFF;
    LOGI("fan: PWM on GPIO %u/%u at %u Hz", PIN_FAN1, PIN_FAN2, (unsigned)freq);
}

float selectSourceTemp(const PrinterState& p, const char* source)
{
    if (strcmp(source, "bed") == 0) return p.bed;
    if (strcmp(source, "chamber") == 0) return p.chamber;
    if (strcmp(source, "max") == 0) {
        float best = NAN;
        const float candidates[3] = {p.nozzle, p.bed, p.chamber};
        for (float c : candidates) {
            if (isnan(c)) continue;
            if (isnan(best) || c > best) best = c;
        }
        return best;
    }
    return p.nozzle;
}

void writeDuty(uint16_t duty, const FanConfig& fc)
{
    if (!g_attached) return;
    const uint16_t maxDuty = (1u << PWM_RESOLUTION_BITS) - 1;
    if (duty > maxDuty) duty = maxDuty;
    // Some driver boards are active-low; invert once here so everything upstream
    // can reason in plain "percent of full speed".
    const uint16_t d1 = fc.output1 ? (fc.pwmInvert ? (uint16_t)(maxDuty - duty) : duty)
                                   : (fc.pwmInvert ? maxDuty : 0);
    const uint16_t d2 = fc.output2 ? (fc.pwmInvert ? (uint16_t)(maxDuty - duty) : duty)
                                   : (fc.pwmInvert ? maxDuty : 0);
    if (d1 != g_lastDuty1) { ledcWrite(PIN_FAN1, d1); g_lastDuty1 = d1; }
    if (d2 != g_lastDuty2) { ledcWrite(PIN_FAN2, d2); g_lastDuty2 = d2; }
}

// The duty byte that actually reaches an enabled pin, which is what the API
// reports as fan.pwmDuty: with pwmInvert an output of 0 % is a duty of 255.
uint16_t drivenDuty(uint16_t duty, const FanConfig& fc)
{
    const uint16_t maxDuty = (1u << PWM_RESOLUTION_BITS) - 1;
    if (duty > maxDuty) duty = maxDuty;
    return fc.pwmInvert ? (uint16_t)(maxDuty - duty) : duty;
}

// Runs on the loop task once fanControlReconfigure() has raised the flag.
void applyReconfigure()
{
    uint32_t freq;
    {
        ConfigGuard guard;
        freq = cfg().fan.pwmFreq;
    }
    if (freq != g_attachedFreq) attachOutputs(freq);
    // Force a re-write so inversion / output-enable changes take effect at once.
    g_lastDuty1 = g_lastDuty2 = 0xFFFF;
    g_heldSourceTemp = NAN;
    g_recomputeNow = true;
}

}  // namespace

void fanControlSetup()
{
    attachOutputs(cfg().fan.pwmFreq);
    writeDuty(0, cfg().fan);
}

void fanControlReconfigure() { g_reconfigure = true; }

uint32_t fanManualExpiresInSec()
{
    const FanState f = fanSnapshot();
    if (f.manualExpiresAt == 0) return 0;
    const int32_t remain = (int32_t)(f.manualExpiresAt - millis());
    return remain > 0 ? (uint32_t)(remain / 1000) + 1 : 0;
}

bool fanApplyMode(const char* mode, int speed, uint32_t durationSec, bool& persist)
{
    if (mode && *mode &&
        strcmp(mode, "auto") != 0 && strcmp(mode, "manual") != 0 &&
        strcmp(mode, "off") != 0 && strcmp(mode, "chamber") != 0) {
        return false;
    }
    // A day is longer than any plausible override and keeps the deadline well
    // inside a millis() half-period.
    if (durationSec > 86400UL) durationSec = 86400UL;

    bool manual;
    {
        ConfigGuard guard;
        Config& c = cfg();
        if (mode && *mode) strlcpy(c.fan.mode, mode, sizeof(c.fan.mode));
        if (speed >= 0) c.fan.manualSpeed = (uint8_t)(speed > 100 ? 100 : speed);
        configValidate(c);
        manual = strcmp(c.fan.mode, "manual") == 0;
    }

    // A duration only makes sense for a manual override; anything else is
    // permanent and therefore worth persisting. Patch just the deadline: a full
    // snapshot/commit here would overwrite whatever the control loop published
    // while this handler was running.
    if (durationSec > 0 && manual) {
        fanSetManualExpiry(millis() + durationSec * 1000UL);
        persist = false;
    } else {
        fanSetManualExpiry(0);
        persist = true;
    }
    g_recomputeNow = true;
    return true;
}

void fanControlLoop()
{
    if (g_reconfigure) {
        g_reconfigure = false;
        applyReconfigure();
    }

    const uint32_t now = millis();
    const bool forced = g_recomputeNow;
    if (forced) g_recomputeNow = false;
    if (!forced && g_lastComputeMs != 0 && (now - g_lastComputeMs) < kRecomputeIntervalMs) return;
    const uint32_t dtMs = g_lastComputeMs == 0 ? kRecomputeIntervalMs : (now - g_lastComputeMs);
    g_lastComputeMs = now;

    // Work off a copy taken under the lock: a /api/config write on the AsyncTCP
    // task can rewrite the curve or the mode part-way through this tick, and a
    // half-old, half-new view of the config is how fans end up at the wrong speed.
    FanConfig fc;
    {
        ConfigGuard guard;
        fc = cfg().fan;
    }
    const PrinterState p = printerSnapshot();
    FanState f = fanSnapshot();

    // --- print phase (REWORK-SPEC 15.1) and the cooldown window ---
    const Phase phase = printerPhase(p);
    const bool printing = phaseIsPrinting(phase);
    if (g_wasPrinting && !printing) g_printEndedMs = now;
    g_wasPrinting = printing;

    // --- expire a temporary manual override ---
    // The stored mode was never changed for a timed override (it is not
    // persisted), so returning to auto needs no save.
    if (f.manualExpiresAt != 0 && (int32_t)(now - f.manualExpiresAt) >= 0) {
        LOGI("fan: manual override expired, back to auto");
        fanSetManualExpiry(0);
        f.manualExpiresAt = 0;
        {
            ConfigGuard guard;
            strlcpy(cfg().fan.mode, "auto", sizeof(cfg().fan.mode));
        }
        strlcpy(fc.mode, "auto", sizeof(fc.mode));
    }

    // --- source temperature (NaN = unavailable) ---
    const float src = selectSourceTemp(p, fc.source);
    f.sourceTemp = src;

    // --- effective mode ---
    // Staleness is judged by data age only: a brief MQTT reconnect must not
    // yank the fan to the failsafe speed while the last reading is seconds old.
    // A dropped link simply ages out after staleSec (never-seen = UINT32_MAX).
    const bool stale = printerDataAgeMs(p) >= (uint32_t)fc.staleSec * 1000UL;

    // A print that ended recently enough that the chamber is still worth
    // emptying. In auto mode the window also ends early once the chamber has
    // actually reached cooldownTarget - there is no point running for the full
    // ten minutes when it is already cold (REWORK-SPEC 15.3 step 6).
    const bool recentPrint = !printing && g_printEndedMs != 0 &&
                             (now - g_printEndedMs) < (uint32_t)fc.cooldownMin * 60000UL;
    const bool chamberCool = !isnan(p.chamber) && p.chamber <= (float)fc.cooldownTarget;
    const bool cooling = fc.onlyWhilePrinting && recentPrint && !chamberCool;

    // --- door rule (step 3) --------------------------------------------------
    // While the door is open there is nothing to exhaust - the fan would just
    // pull room air and dust straight through the printer. It stays armed for
    // doorResumeSec after the door closes so three quick openings do not make
    // the fan stutter. During a cool-down an open door is *helping*, so the rule
    // deliberately does not apply there.
    //
    // Everything here is gated on doorKnown: on some X1C units the closed door
    // never actuates the switch and the bit reads "open" from boot to power-off,
    // so the rule stays inert until the printer has been seen to report a change.
    const bool doorOpen = printerDoorOpen(p);
    const bool doorRecentlyClosed = p.doorKnown && !doorOpen && p.lastDoorCloseMs != 0 &&
                                    (now - p.lastDoorCloseMs) < (uint32_t)fc.doorResumeSec * 1000UL;
    const bool doorPhaseRelevant = phase != Phase::Finished && phase != Phase::Cooling &&
                                   phase != Phase::Idle;
    const bool doorRule = strcmp(fc.doorMode, "ignore") != 0 && p.doorKnown &&
                          doorPhaseRelevant && (doorOpen || doorRecentlyClosed);

    // --- preheat rule (step 4) ----------------------------------------------
    // An exhaust fan during warm-up is fighting the heaters: it costs time and
    // energy and, on an enclosed printer, can stop the chamber reaching target.
    const bool preheatRule = strcmp(fc.preheatMode, "ignore") != 0 && phase == Phase::Preheat;

    const bool chamberMode = strcmp(fc.mode, "chamber") == 0;
    // The thermostat needs a chamber reading; without one it has nothing to
    // control and the curve is the honest fallback.
    const bool thermostatUsable = chamberMode && !isnan(p.chamber);
    const bool thermostatCooldown = phase == Phase::Finished || phase == Phase::Cooling ||
                                    (phase == Phase::Idle && recentPrint);
    float setpoint = NAN;
    if (thermostatUsable) {
        if (phaseIsPrinting(phase))    setpoint = (float)fc.chamberTarget;
        else if (thermostatCooldown)   setpoint = (float)fc.cooldownTarget;
    }

    const char* eff;
    if (strcmp(fc.mode, "off") == 0)          eff = "off";
    else if (strcmp(fc.mode, "manual") == 0)  eff = "manual";
    else if (stale)                           eff = "stale";
    else if (doorRule)                        eff = "door";
    else if (preheatRule)                     eff = "preheat";
    else if (thermostatUsable && isnan(setpoint))           eff = "idle";
    else if (thermostatUsable && thermostatCooldown)        eff = "cooldown";
    else if (thermostatUsable)                              eff = "chamber";
    else if (fc.onlyWhilePrinting && !printing && !cooling) eff = "idle";
    else if (fc.onlyWhilePrinting && !printing)             eff = "cooldown";
    else                                      eff = "auto";
    strlcpy(f.effectiveMode, eff, sizeof(f.effectiveMode));

    // --- target ---
    float target;
    if (strcmp(eff, "off") == 0) {
        target = 0.0f;
    } else if (strcmp(eff, "manual") == 0) {
        target = (float)fc.manualSpeed;
    } else if (strcmp(eff, "idle") == 0) {
        target = 0.0f;
    } else if (strcmp(eff, "door") == 0) {
        target = strcmp(fc.doorMode, "fixed") == 0 ? (float)fc.doorSpeed : 0.0f;
    } else if (strcmp(eff, "preheat") == 0) {
        target = strcmp(fc.preheatMode, "fixed") == 0 ? (float)fc.preheatSpeed : 0.0f;
    } else if (strcmp(eff, "stale") == 0) {
        if (strcmp(fc.staleMode, "hold") == 0)       target = g_slew;
        else if (strcmp(fc.staleMode, "fixed") == 0) target = (float)fc.staleSpeed;
        else                                         target = 0.0f;
    } else if (thermostatUsable) {
        // Chamber thermostat (step 5). It only steps every thermostatPeriodSec -
        // a PI loop on an enclosure that takes minutes to respond has nothing to
        // gain from running ten times a second, and a long dt is what makes the
        // integral term readable in percent per degree-second.
        if (isnan(g_lastSetpoint) || fabsf(setpoint - g_lastSetpoint) > 0.01f) {
            // Switching between print and cool-down set points is a new regime;
            // carrying the old integral over would dump a step into the output.
            thermostatReset(g_pi);
            g_lastSetpoint = setpoint;
            g_lastThermostatMs = 0;
        }
        const uint32_t periodMs = (uint32_t)fc.thermostatPeriodSec * 1000UL;
        if (g_lastThermostatMs == 0 || (now - g_lastThermostatMs) >= periodMs) {
            const float dtSec = g_lastThermostatMs == 0
                                    ? (float)fc.thermostatPeriodSec
                                    : (float)(now - g_lastThermostatMs) / 1000.0f;
            g_lastThermostatMs = now;
            g_thermostatOut = thermostatStep(g_pi, p.chamber, setpoint, fc.kp, fc.ki, dtSec,
                                             /*freeze=*/doorOpen);
        }
        target = g_thermostatOut;
    } else {
        // auto / cooldown: follow the curve, with hysteresis on the source so a
        // sensor jittering by a fraction of a degree does not modulate the fan.
        if (isnan(src)) {
            g_heldSourceTemp = NAN;
            g_curveTarget = 0.0f;
        } else if (isnan(g_heldSourceTemp) || fabsf(src - g_heldSourceTemp) >= fc.hysteresis) {
            g_heldSourceTemp = src;
            g_curveTarget = curveInterpolate(fc.curve, src);
        }
        target = g_curveTarget;
    }
    // Anything that is not the thermostat leaves it with a clean slate, so
    // coming back from a door event or a manual override does not resume with a
    // stale integral.
    const bool thermostatRan = thermostatUsable && !isnan(setpoint) &&
                               strcmp(eff, "door") != 0 && strcmp(eff, "preheat") != 0 &&
                               strcmp(eff, "stale") != 0 && strcmp(eff, "off") != 0 &&
                               strcmp(eff, "manual") != 0;
    if (!thermostatRan) {
        thermostatReset(g_pi);
        g_lastThermostatMs = 0;
        g_thermostatOut = 0.0f;
        g_lastSetpoint = NAN;
    }
    f.setpoint = thermostatRan ? setpoint : NAN;
    if (target < 0.0f) target = 0.0f;
    if (target > 100.0f) target = 100.0f;
    f.target = target;

    // --- slew towards the target ---
    if (fc.rampRate == 0) {
        g_slew = target;
    } else {
        const float step = (float)fc.rampRate * (float)dtMs / 1000.0f;
        if (target > g_slew) g_slew = fminf(target, g_slew + step);
        else                 g_slew = fmaxf(target, g_slew - step);
    }
    float output = g_slew;

    // --- minimum-speed cutoff: below it the fan would stall or whine ---
    if (fc.minSpeed > 0 && output > 0.0f && output < (float)fc.minSpeed) output = 0.0f;

    // --- kick start: a stalled fan needs full duty for a moment to break away ---
    // Only a fan that has genuinely been stopped for a couple of seconds gets a
    // kick; re-arming on every brief dip to 0 % would leave it pulsing. The flag
    // is read before the tracker is updated, so it describes the zero run that
    // this tick may be ending.
    const bool wasStopped = g_zeroSinceMs != 0 && (now - g_zeroSinceMs) >= kKickRearmAfterMs;
    if (output <= 0.0f) {
        if (g_zeroSinceMs == 0) g_zeroSinceMs = now;
    } else {
        g_zeroSinceMs = 0;
    }

    if (fc.kickStart && fc.kickMs > 0) {
        if (f.output <= 0.0f && output > 0.0f && g_kickUntilMs == 0 && wasStopped) {
            g_kickUntilMs = now + fc.kickMs;
        }
        if (g_kickUntilMs != 0) {
            if ((int32_t)(now - g_kickUntilMs) >= 0 || output <= 0.0f) {
                g_kickUntilMs = 0;
                f.kicking = false;
            } else {
                f.kicking = true;
            }
        }
    } else {
        g_kickUntilMs = 0;
        f.kicking = false;
    }

    const float driven = f.kicking ? 100.0f : output;
    const uint16_t duty = (uint16_t)lroundf(driven * 255.0f / 100.0f);

    f.output = output;
    // Reported post-inversion: this is the byte the pin actually sees.
    f.pwmDuty = drivenDuty(duty, fc);
    fanCommitControl(f);

    writeDuty(duty, fc);
}

}  // namespace blsf
