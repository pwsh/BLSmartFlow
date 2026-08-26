// config.h - persisted configuration (LittleFS /config.json).
//
// One flat POD struct, defaults for every field, and a validate() that clamps
// everything into range. Nothing in the firmware reads a config value without it
// having passed through configValidate(), so downstream modules never have to
// defend against a nonsense value from a hand-edited file.
//
// Secrets are masked on the way out (GET /api/config) and a value made only of
// '*' on the way in means "keep the stored one", so the UI can round-trip a form
// without ever learning the password.

#ifndef BLSF_CONFIG_H
#define BLSF_CONFIG_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include "cooldown_logic.h"
#include "curve.h"
#include "filament_match.h"

namespace blsf {

static const int CONFIG_VERSION = 2;

struct WifiConfig {
    char ssid[33];
    char password[65];
    char bssid[18];        // "AA:BB:CC:DD:EE:FF"
    bool lockBssid;
    char hostname[33];
};

struct PrinterConfig {
    char ip[64];           // dotted quad or hostname
    char accessCode[9];    // exactly 8 chars, or empty
    char serial[17];
    char model[6];         // auto|x1|p1|a1|h2d
};

struct FanConfig {
    FanCurve curve;
    char     source[8];    // nozzle|bed|chamber|max
    char     mode[8];      // auto|manual|off|chamber
    uint8_t  manualSpeed;
    uint8_t  minSpeed;
    bool     kickStart;
    uint16_t kickMs;
    float    hysteresis;
    uint16_t rampRate;     // %/s, 0 = instant
    uint32_t pwmFreq;
    bool     pwmInvert;
    bool     output1;
    bool     output2;
    bool     onlyWhilePrinting;
    uint16_t cooldownMin;
    uint16_t staleSec;
    char     staleMode[6]; // hold|off|fixed
    uint8_t  staleSpeed;

    // --- thermal states (REWORK-SPEC 15.2) ---
    char     doorMode[8];       // ignore|off|fixed - what to do while the front door is open
    uint8_t  doorSpeed;         // % used by doorMode=fixed
    uint16_t doorResumeSec;     // anti-flap delay after the door closes
    char     preheatMode[8];    // ignore|off|fixed - what to do while the printer heats up
    uint8_t  preheatSpeed;      // % used by preheatMode=fixed
    uint8_t  chamberTarget;     // degC thermostat set point while printing
    uint8_t  cooldownTarget;    // degC the chamber is cooled down to after a print
    float    kp;                // % per degC
    float    ki;                // % per degC*s
    uint8_t  thermostatPeriodSec;
    uint8_t  ambientTemp;       // degC assumed room temperature for the cooling estimate
};

// Filament-aware cooling (REWORK-SPEC 16.3). `overrides` is a small fixed array
// rather than a list: twelve materials is more than anyone tunes by hand, and a
// fixed array keeps Config a POD that can be memcpy'd and validated in place.
//
// FilamentOverrideRule comes from filament_match.h so the pure profile resolver
// can be handed the config array directly, with no conversion step to get wrong.
static const uint8_t FILAMENT_MAX_OVERRIDES = 12;

struct FilamentConfig {
    bool     autoDetect;         // JSON "auto" - `auto` is a keyword in C++
    char     manualId[24];       // guide id forced when the printer reports no tray
    uint8_t  ventFloor[3];       // % minimum output by vent demand: optional/recommended/required
    uint8_t  overrideCount;
    FilamentOverrideRule overrides[FILAMENT_MAX_OVERRIDES];
};

// Learned Newtonian cooling constants (REWORK-SPEC 15.4), in 1/min, indexed by
// fan-output bucket (0/25/50/75/100 %). NaN = never measured. Persisted so a
// power cut does not throw away hours of passive observation.
struct ThermalConfig {
    float    kClosed[5];
    float    kOpen[5];
    uint32_t samples;
};

// Post-print cool-down (REWORK-SPEC 17.1). The struct itself is CooldownRules
// from cooldown_logic.h, so the persisted document and the pure state machine
// can never disagree about what a rule means. `ownFan` is the CD_OWN_* enum in
// memory and a string in JSON - see cooldownOwnFanName().
typedef CooldownRules CooldownConfig;

struct MqttConfig {
    bool     enabled;
    char     host[64];
    uint16_t port;
    char     user[33];
    char     password[65];
    char     baseTopic[64];   // "" -> blsmartflow/<chipid>
    bool     haDiscovery;
    char     haPrefix[32];
    uint16_t publishIntervalSec;
};

struct WebConfig {
    bool authEnabled;
    char user[33];
    char password[65];
};

struct DebugConfig {
    bool serial;
    bool mqttDump;
};

struct SsdpConfig {
    bool enabled;
};

struct Config {
    int           version;
    WifiConfig    wifi;
    PrinterConfig printer;
    FanConfig     fan;
    MqttConfig    mqtt;
    WebConfig     web;
    DebugConfig   debug;
    SsdpConfig    ssdp;
    ThermalConfig thermal;
    FilamentConfig filament;
    CooldownConfig cooldown;
};

// The single live instance. Written from the loop task, the AsyncTCP task (every
// /api/* handler), the serial provisioning reader and the external MQTT command
// callback, so every writer - and every reader that needs a coherent view of
// more than one field - must hold the lock below.
Config& cfg();

// --- concurrency -----------------------------------------------------------
// A recursive FreeRTOS mutex around cfg(). Recursive because the natural way to
// write a handler is "take the lock, merge, save", and configSave() takes it
// again to serialise a consistent document.
void configLock();
void configUnlock();

// RAII wrapper; prefer this over the bare calls so an early return cannot leak
// the lock.
struct ConfigGuard {
    ConfigGuard() { configLock(); }
    ~ConfigGuard() { configUnlock(); }
    ConfigGuard(const ConfigGuard&) = delete;
    ConfigGuard& operator=(const ConfigGuard&) = delete;
};

void configDefaults(Config& c);
// Clamps and normalises in place. Always leaves `c` usable.
void configValidate(Config& c);

// Mounts LittleFS, migrates a legacy /blledconfig.json if present, and loads
// /config.json. Never throws or halts: a corrupt file is moved aside to
// /config.bad and defaults are used. Returns false if defaults had to be used.
bool configLoad();

// Atomic save: writes /config.tmp then renames over /config.json.
bool configSave();

// Requests a deferred save. Fan/mode commands can arrive many times a second
// (an HA slider, the UI's live preview); writing LittleFS on each one would burn
// through the flash, so they mark the config dirty and configLoopSave() persists
// it at most every 10 s. Explicit config/curve/wifi saves still write inline.
void configMarkDirty();
// Call from loop(): saves a dirty config, rate-limited. No-op when clean.
void configLoopSave();

// Wipes the stored configuration (used by factory reset).
void configWipe();

// Serialises into `out`. When `masked`, secrets become "********".
void configToJson(JsonObject out, const Config& c, bool masked);

// Deep-merges a partial config document into `c`. Only keys present are touched.
// `restartRequired` is set when a changed key needs a reboot (wifi/web).
// `printerChanged` / `mqttChanged` / `fanChanged` let the caller re-apply live.
struct ConfigChange {
    bool restartRequired = false;
    bool printerChanged = false;
    bool mqttChanged = false;
    bool fanChanged = false;
    bool ssdpChanged = false;
};
void configFromJson(JsonObjectConst in, Config& c, ConfigChange& change);

// Lowercase hex of the last three MAC bytes, e.g. "a1b2c3". Stable per device.
const char* chipId();
// Effective external-MQTT base topic (config value, or blsmartflow/<chipid>).
const char* mqttBaseTopic();

}  // namespace blsf

#endif  // BLSF_CONFIG_H
