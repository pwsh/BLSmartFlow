// cooldown_logic.h - the post-print cool-down session, as a pure function.
//
// REWORK-SPEC section 17.2. Arduino-free and side-effect free, so the exact code
// the device runs is what `pio test -e native` (test/test_cooldown) exercises.
// It only depends on printer_parse.h for the Phase enum, which is itself
// host-testable.
//
// The shape is deliberately the same as thermostat.h: the caller owns a
// CooldownState, hands in a snapshot of the world plus the rules, and gets back
// a set of *actions* to perform. Nothing in here talks to MQTT, the fan or the
// clock - which is what makes "does it really stop sending M106 while the
// printer is running?" a question a test can answer.
//
// Why the session exists at all: after a print the printer reports FINISH and
// stops driving its own fans, but the aux and chamber fans are still there and
// are far better placed than an external duct fan. Borrowing them for a few
// minutes empties the chamber quickly - at the price of being the one feature
// that sends commands *to* the printer, hence usePrinterFans defaulting to off
// and the paranoid gating below.

#ifndef BLSF_COOLDOWN_LOGIC_H
#define BLSF_COOLDOWN_LOGIC_H

#include <math.h>
#include <stdint.h>
#include <string.h>

#include "printer_parse.h"

namespace blsf {

// How the device's own fan behaves while a session runs.
enum : uint8_t {
    CD_OWN_THERMOSTAT = 0,   // PI onto the cool-down target (reuses thermostat.h)
    CD_OWN_MAX        = 1,   // flat out
    CD_OWN_CURVE      = 2,   // leave the normal control loop alone
};

inline const char* cooldownOwnFanName(uint8_t v)
{
    switch (v) {
        case CD_OWN_MAX:   return "max";
        case CD_OWN_CURVE: return "curve";
        default:           return "thermostat";
    }
}

inline uint8_t cooldownOwnFanFromName(const char* s, uint8_t fallback = CD_OWN_THERMOSTAT)
{
    if (!s || !*s) return fallback;
    if (strcmp(s, "max") == 0) return CD_OWN_MAX;
    if (strcmp(s, "curve") == 0) return CD_OWN_CURVE;
    if (strcmp(s, "thermostat") == 0) return CD_OWN_THERMOSTAT;
    return fallback;
}

// Why a session ended. `None` means "no session has ended yet".
enum class CooldownReason : uint8_t {
    None = 0,
    Target,     // the chamber reached the target and stayed there
    Timeout,    // maxMinutes elapsed
    NewJob,     // the printer went back to work - it owns its fans again
    Stopped,    // asked to stop (API / MQTT / the UI's Stop button)
    LinkLost,   // no printer for more than 30 s
    Disabled,   // the automatic feature was switched off under a session it started
};

inline const char* cooldownReasonName(CooldownReason r)
{
    switch (r) {
        case CooldownReason::Target:   return "target";
        case CooldownReason::Timeout:  return "timeout";
        case CooldownReason::NewJob:   return "newJob";
        case CooldownReason::Stopped:  return "stopped";
        case CooldownReason::LinkLost: return "linkLost";
        case CooldownReason::Disabled: return "disabled";
        case CooldownReason::None:     break;
    }
    return "";
}

enum class CooldownCommand : uint8_t { None = 0, Start = 1, Stop = 2 };

// The persisted rules (REWORK-SPEC 17.1). Config stores this struct verbatim,
// which is why it lives here rather than in config.h: the pure step and the
// persisted document can then never drift apart.
struct CooldownRules {
    bool     enabled;             // start automatically when a print finishes
    uint8_t  target;              // degC, unless the filament profile overrides it
    bool     usePrinterFans;      // send M106 to the printer (opt-in)
    uint8_t  auxSpeed;            // % for M106 P2
    uint8_t  chamberFanSpeed;     // % for M106 P3
    uint16_t maxMinutes;          // hard stop
    bool     gentleFromFilament;  // honour postPrintCooling == gentle
    uint8_t  ownFan;              // CD_OWN_*
};

struct CooldownInputs {
    Phase       phase;
    const char* gcodeState;    // raw printer state, "" when nothing is known yet
    float       chamber;       // NaN when the printer has no chamber sensor
    bool        linkOnline;    // a fresh report has arrived recently
    bool        gentleMaterial;  // filament effective postPrintCooling == gentle
    uint8_t     chamberTarget;   // filament effective chamber target, for the gentle gate
    uint8_t     target;          // effective cool-down target for a session starting now
    const char* materialId;      // guide id, "" when unknown
    CooldownCommand command;     // a start/stop request latched since the last step
};

struct CooldownState {
    bool     active;
    bool     manual;           // started by hand rather than by the finish edge
    uint32_t startedMs;
    float    startChamber;
    uint8_t  target;           // the target this session is aiming at
    bool     everSent;         // the printer fans were commanded on at least once
    bool     sentOn;           // ... and are believed to be on right now
    uint8_t  sentAux;
    uint8_t  sentChamber;
    uint32_t lastSendMs;
    uint8_t  atTargetCount;    // consecutive samples at or below the target
    uint32_t linkLostMs;       // when the link went away, 0 while it is up
    bool     linkLost;
    Phase    prevPhase;
    bool     prevPhaseValid;
    CooldownReason reason;     // why the last session ended
    char     material[24];
};

// What the caller must do as a result of this step.
struct CooldownActions {
    bool    startedNow;
    bool    stoppedNow;
    CooldownReason reason;     // meaningful when stoppedNow
    bool    sendFans;          // send M106 P2 S<aux> / P3 S<chamber>
    uint8_t auxPct;
    uint8_t chamberPct;
    bool    sendStop;          // send M106 P2 S0 / P3 S0
};

// Re-assert cadence: the printer resets its fans on its own schedule, so a
// command that is never repeated quietly stops being true.
static const uint32_t COOLDOWN_REASSERT_MS = 30000;
// How long the printer may be silent before a running session gives up. Shorter
// than fan.staleSec on purpose: this session is *commanding* the printer, and
// commands sent into a dead link are worse than no session at all.
static const uint32_t COOLDOWN_LINK_GRACE_MS = 30000;
// The gentle rule's margin below the print chamber target (REWORK-SPEC 16.3).
static const float COOLDOWN_GENTLE_MARGIN_C = 10.0f;

inline void cooldownReset(CooldownState& st)
{
    memset(&st, 0, sizeof(st));
    st.startChamber = NAN;
    st.prevPhase = Phase::Offline;
    st.reason = CooldownReason::None;
}

// A print is running (or about to): the printer owns its fans and nothing may be
// sent. Also the answer POST /api/cooldown gives as "printer is busy".
inline bool cooldownPhaseBusy(Phase p) { return phaseIsPrinting(p); }
inline bool cooldownCanStart(Phase p) { return !cooldownPhaseBusy(p); }

// The only two states in which the printer is not driving its own fans.
inline bool cooldownGcodeSafe(const char* s)
{
    if (!s) return false;
    return strcmp(s, "FINISH") == 0 || strcmp(s, "IDLE") == 0;
}

// States that say a job is under way. An *unknown* state is neither: it blocks
// G-code (cooldownGcodeSafe is false) without killing a session the user asked
// for by hand, which matters on a printer that has not reported since boot.
inline bool cooldownGcodeBusy(const char* s)
{
    if (!s || !*s) return false;
    return strcmp(s, "RUNNING") == 0 || strcmp(s, "PAUSE") == 0 ||
           strcmp(s, "PREPARE") == 0 || strcmp(s, "SLICING") == 0;
}

namespace cddetail {

inline void copyStr(char* dst, size_t dstSize, const char* src)
{
    if (dstSize == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    size_t i = 0;
    for (; i + 1 < dstSize && src[i]; i++) dst[i] = src[i];
    dst[i] = '\0';
}

inline uint8_t half(uint8_t v) { return (uint8_t)(v / 2); }

}  // namespace cddetail

// One sample of the session state machine. Call it every COOLDOWN_SAMPLE_MS (5 s
// on the device) and whenever a command arrives; `nowMs` is millis().
//
// The two-sample hold on the target is the whole reason this takes samples
// rather than timestamps: a chamber probe that dips a tenth of a degree below
// the target for one reading should not end a cool-down that has another five
// minutes of real work left.
inline CooldownActions cooldownStep(CooldownState& st, const CooldownInputs& in,
                                    const CooldownRules& cfg, uint32_t nowMs)
{
    CooldownActions a;
    memset(&a, 0, sizeof(a));
    a.reason = CooldownReason::None;

    const Phase prev = st.prevPhaseValid ? st.prevPhase : in.phase;
    st.prevPhase = in.phase;
    st.prevPhaseValid = true;

    const bool busy = cooldownPhaseBusy(in.phase) || cooldownGcodeBusy(in.gcodeState);

    // --- start ---------------------------------------------------------------
    if (!st.active) {
        bool start = false;
        bool manual = false;
        if (in.command == CooldownCommand::Start && !busy) {
            start = true;
            manual = true;
        } else if (cfg.enabled && in.command != CooldownCommand::Stop && !busy) {
            // The edge, not the level: re-entering the loop with the printer
            // already sitting at FINISH must not restart a session the user
            // stopped thirty seconds ago.
            const bool endNow = in.phase == Phase::Finished || in.phase == Phase::Cooling;
            const bool endBefore = prev == Phase::Finished || prev == Phase::Cooling;
            start = endNow && !endBefore;
        }
        if (!start) return a;

        st.active = true;
        st.manual = manual;
        st.startedMs = nowMs;
        st.startChamber = in.chamber;
        st.target = in.target;
        st.everSent = false;
        st.sentOn = false;
        st.sentAux = 0;
        st.sentChamber = 0;
        st.lastSendMs = nowMs;
        st.atTargetCount = 0;
        st.linkLostMs = 0;
        st.linkLost = false;
        st.reason = CooldownReason::None;
        cddetail::copyStr(st.material, sizeof(st.material), in.materialId);
        a.startedNow = true;
        // fall through: the first sample may already command the fans
    }

    // --- stop conditions, most urgent first ----------------------------------
    // A printer that has gone back to work owns its fans again. No stop command
    // is sent: the running print has already set them, and overwriting that with
    // S0 would be exactly the failure mode this feature must never have.
    if (busy) {
        st.active = false;
        st.sentOn = false;
        st.reason = CooldownReason::NewJob;
        a.stoppedNow = true;
        a.reason = st.reason;
        return a;
    }

    if (in.command == CooldownCommand::Stop) {
        st.active = false;
        st.reason = CooldownReason::Stopped;
        a.stoppedNow = true;
        a.reason = st.reason;
        a.sendStop = st.everSent && cooldownGcodeSafe(in.gcodeState);
        st.sentOn = false;
        return a;
    }

    if (!cfg.enabled && !st.manual) {
        st.active = false;
        st.reason = CooldownReason::Disabled;
        a.stoppedNow = true;
        a.reason = st.reason;
        a.sendStop = st.everSent && cooldownGcodeSafe(in.gcodeState);
        st.sentOn = false;
        return a;
    }

    // Link loss: give it a grace period, because a reconnect takes seconds and
    // ending a cool-down for that would be worse than useless. No stop command -
    // there is nothing to send it over.
    if (!in.linkOnline) {
        if (!st.linkLost) {
            st.linkLost = true;
            st.linkLostMs = nowMs;
        }
        if ((uint32_t)(nowMs - st.linkLostMs) > COOLDOWN_LINK_GRACE_MS) {
            st.active = false;
            st.sentOn = false;
            st.reason = CooldownReason::LinkLost;
            a.stoppedNow = true;
            a.reason = st.reason;
            return a;
        }
    } else {
        st.linkLost = false;
        st.linkLostMs = 0;
    }

    if (cfg.maxMinutes > 0 &&
        (uint32_t)(nowMs - st.startedMs) >= (uint32_t)cfg.maxMinutes * 60000UL) {
        st.active = false;
        st.reason = CooldownReason::Timeout;
        a.stoppedNow = true;
        a.reason = st.reason;
        a.sendStop = st.everSent && cooldownGcodeSafe(in.gcodeState);
        st.sentOn = false;
        return a;
    }

    if (!isnan(in.chamber) && in.chamber <= (float)st.target) {
        if (st.atTargetCount < 255) st.atTargetCount++;
    } else {
        st.atTargetCount = 0;
    }
    if (st.atTargetCount >= 2) {
        st.active = false;
        st.reason = CooldownReason::Target;
        a.stoppedNow = true;
        a.reason = st.reason;
        a.sendStop = st.everSent && cooldownGcodeSafe(in.gcodeState);
        st.sentOn = false;
        return a;
    }

    // --- run: what should the printer's fans be doing? -----------------------
    // Safety gate first, and it is absolute: no G-code leaves this device unless
    // the printer says FINISH or IDLE at this very moment.
    bool allowed = cfg.usePrinterFans && in.linkOnline && cooldownGcodeSafe(in.gcodeState);
    bool gentle = false;
    if (allowed && cfg.gentleFromFilament && in.gentleMaterial) {
        // ABS and friends crack when cold air hits them straight off the plate,
        // so the printer's fans stay off until the chamber is well below the
        // print temperature and then only run at half throttle.
        const float gate = (float)in.chamberTarget - COOLDOWN_GENTLE_MARGIN_C;
        if (isnan(in.chamber) || in.chamber >= gate) allowed = false;
        else gentle = true;
    }

    if (allowed) {
        const uint8_t aux = gentle ? cddetail::half(cfg.auxSpeed) : cfg.auxSpeed;
        const uint8_t cha = gentle ? cddetail::half(cfg.chamberFanSpeed) : cfg.chamberFanSpeed;
        const bool changed = !st.sentOn || st.sentAux != aux || st.sentChamber != cha;
        const bool due = st.sentOn && (uint32_t)(nowMs - st.lastSendMs) >= COOLDOWN_REASSERT_MS;
        if (changed || due) {
            a.sendFans = true;
            a.auxPct = aux;
            a.chamberPct = cha;
            st.sentOn = true;
            st.everSent = true;
            st.sentAux = aux;
            st.sentChamber = cha;
            st.lastSendMs = nowMs;
        }
    } else if (st.sentOn) {
        // The gate closed under a running session (gentle rule re-armed, the
        // option switched off, the state left FINISH/IDLE). Put the fans back
        // where we found them rather than leaving them running.
        a.sendStop = cooldownGcodeSafe(in.gcodeState);
        st.sentOn = false;
        st.sentAux = 0;
        st.sentChamber = 0;
    }

    return a;
}

// Seconds left before the hard stop, for the status document and the UI.
inline uint32_t cooldownRemainingSec(const CooldownState& st, const CooldownRules& cfg,
                                     uint32_t nowMs)
{
    if (!st.active) return 0;
    const uint32_t maxMs = (uint32_t)cfg.maxMinutes * 60000UL;
    const uint32_t elapsed = (uint32_t)(nowMs - st.startedMs);
    return elapsed >= maxMs ? 0 : (maxMs - elapsed) / 1000UL;
}

}  // namespace blsf

#endif  // BLSF_COOLDOWN_LOGIC_H
