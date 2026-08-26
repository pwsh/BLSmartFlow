#include "config.h"

#include <LittleFS.h>
#include <WiFi.h>
#include <ctype.h>
#include <limits>

#include <esp_mac.h>

#include "log.h"

namespace blsf {

namespace {

const char* kConfigPath = "/config.json";
const char* kTempPath   = "/config.tmp";
const char* kBadPath    = "/config.bad";
const char* kLegacyPath = "/blledconfig.json";
const char* kMask       = "********";

Config g_cfg;
char   g_chipId[7] = {0};
char   g_baseTopic[80] = {0};

// Created during static init, which on Arduino-ESP32 runs after the scheduler is
// up, so the mutex exists before any task can reach cfg().
SemaphoreHandle_t g_lock = xSemaphoreCreateRecursiveMutex();

// Deferred-save bookkeeping (see configMarkDirty/configLoopSave).
const uint32_t kDeferredSaveIntervalMs = 10000;
volatile bool  g_dirty = false;
uint32_t       g_lastDeferredSaveMs = 0;

// True when every character is '*' (and there is at least one), i.e. the UI sent
// back the masked placeholder and we must keep the stored secret.
bool isMasked(const char* s)
{
    if (!s || !*s) return false;
    for (const char* p = s; *p; ++p) if (*p != '*') return false;
    return true;
}

// Copies in[key] into dst when present, honouring the mask convention for
// secrets. Returns true when dst actually changed.
bool mergeStr(JsonObjectConst in, const char* key, char* dst, size_t dstSize, bool secret = false)
{
    if (!in[key].is<const char*>()) return false;
    const char* v = in[key].as<const char*>();
    if (!v) return false;
    if (secret && isMasked(v)) return false;   // "leave unchanged"
    if (strncmp(dst, v, dstSize) == 0) return false;
    strlcpy(dst, v, dstSize);
    return true;
}

// Accepts any JSON number (ArduinoJson's is<float>() covers ints and floats) and
// saturates it into T's range, so an out-of-range value from a hand-edited
// backup is clamped rather than silently ignored. configValidate() then applies
// the semantic range on top.
template <typename T>
bool mergeNum(JsonObjectConst in, const char* key, T& dst)
{
    JsonVariantConst v = in[key];
    if (!v.is<float>()) return false;
    double d = v.as<double>();
    if (isnan(d) || isinf(d)) return false;
    const double lo = (double)std::numeric_limits<T>::lowest();
    const double hi = (double)std::numeric_limits<T>::max();
    if (d < lo) d = lo;
    if (d > hi) d = hi;
    T nv = (T)d;
    if (nv == dst) return false;
    dst = nv;
    return true;
}

bool mergeBool(JsonObjectConst in, const char* key, bool& dst)
{
    if (!in[key].is<bool>()) return false;
    bool v = in[key].as<bool>();
    if (v == dst) return false;
    dst = v;
    return true;
}

// Constrains a char-array enum to a known set; falls back to `def` otherwise.
void clampEnum(char* dst, size_t dstSize, const char* const* allowed, size_t n, const char* def)
{
    for (size_t i = 0; i < n; i++) {
        if (strcasecmp(dst, allowed[i]) == 0) {
            strlcpy(dst, allowed[i], dstSize);   // normalise case
            return;
        }
    }
    strlcpy(dst, def, dstSize);
}

template <typename T>
T clampVal(T v, T lo, T hi) { return v < lo ? lo : (v > hi ? hi : v); }

void curveToJson(JsonArray arr, const FanCurve& c)
{
    for (uint8_t i = 0; i < c.count; i++) {
        JsonObject o = arr.add<JsonObject>();
        o["temp"] = c.pts[i].temp;
        o["speed"] = c.pts[i].speed;
    }
}

}  // namespace

Config& cfg() { return g_cfg; }

void configLock()
{
    if (g_lock) xSemaphoreTakeRecursive(g_lock, portMAX_DELAY);
}

void configUnlock()
{
    if (g_lock) xSemaphoreGiveRecursive(g_lock);
}

void configMarkDirty() { g_dirty = true; }

void configLoopSave()
{
    if (!g_dirty) return;
    const uint32_t now = millis();
    // The first deferred save goes through immediately; after that the window
    // coalesces a burst of slider updates into one write.
    if (g_lastDeferredSaveMs != 0 && (now - g_lastDeferredSaveMs) < kDeferredSaveIntervalMs) return;
    g_lastDeferredSaveMs = now;
    configSave();          // clears g_dirty
}

const char* chipId()
{
    if (g_chipId[0] == '\0') {
        uint8_t mac[6] = {0};
        // Reads the factory-burned base MAC, so this works before WiFi starts.
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        snprintf(g_chipId, sizeof(g_chipId), "%02x%02x%02x", mac[3], mac[4], mac[5]);
    }
    return g_chipId;
}

const char* mqttBaseTopic()
{
    if (g_cfg.mqtt.baseTopic[0] != '\0') return g_cfg.mqtt.baseTopic;
    if (g_baseTopic[0] == '\0') snprintf(g_baseTopic, sizeof(g_baseTopic), "blsmartflow/%s", chipId());
    return g_baseTopic;
}

void configDefaults(Config& c)
{
    memset(&c, 0, sizeof(c));
    c.version = CONFIG_VERSION;

    c.wifi.lockBssid = false;
    strlcpy(c.wifi.hostname, "blsmartflow", sizeof(c.wifi.hostname));

    strlcpy(c.printer.model, "auto", sizeof(c.printer.model));

    curveDefaults(c.fan.curve);
    strlcpy(c.fan.source, "nozzle", sizeof(c.fan.source));
    strlcpy(c.fan.mode, "auto", sizeof(c.fan.mode));
    c.fan.manualSpeed = 50;
    c.fan.minSpeed = 0;
    c.fan.kickStart = true;
    c.fan.kickMs = 500;
    c.fan.hysteresis = 2.0f;
    c.fan.rampRate = 0;
    c.fan.pwmFreq = 25000;
    c.fan.pwmInvert = false;
    c.fan.output1 = true;
    c.fan.output2 = true;
    c.fan.onlyWhilePrinting = false;
    c.fan.cooldownMin = 10;
    c.fan.staleSec = 120;
    strlcpy(c.fan.staleMode, "off", sizeof(c.fan.staleMode));
    c.fan.staleSpeed = 0;

    c.mqtt.enabled = false;
    c.mqtt.port = 1883;
    c.mqtt.haDiscovery = true;
    strlcpy(c.mqtt.haPrefix, "homeassistant", sizeof(c.mqtt.haPrefix));
    c.mqtt.publishIntervalSec = 10;

    c.web.authEnabled = false;
    strlcpy(c.web.user, "admin", sizeof(c.web.user));

    c.debug.serial = true;
    c.debug.mqttDump = false;

    c.ssdp.enabled = true;
}

void configValidate(Config& c)
{
    c.version = CONFIG_VERSION;

    // --- wifi ---
    if (c.wifi.hostname[0] == '\0') strlcpy(c.wifi.hostname, "blsmartflow", sizeof(c.wifi.hostname));
    // mDNS/DHCP hostnames: letters, digits and '-' only.
    for (char* p = c.wifi.hostname; *p; ++p) {
        if (!isalnum((unsigned char)*p) && *p != '-') *p = '-';
        *p = (char)tolower((unsigned char)*p);
    }
    // A BSSID must parse as six hex octets or it is discarded (and the lock with it).
    if (c.wifi.bssid[0] != '\0') {
        unsigned b[6];
        if (sscanf(c.wifi.bssid, "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) {
            c.wifi.bssid[0] = '\0';
        } else {
            snprintf(c.wifi.bssid, sizeof(c.wifi.bssid), "%02X:%02X:%02X:%02X:%02X:%02X",
                     b[0] & 0xFF, b[1] & 0xFF, b[2] & 0xFF, b[3] & 0xFF, b[4] & 0xFF, b[5] & 0xFF);
        }
    }
    if (c.wifi.bssid[0] == '\0') c.wifi.lockBssid = false;

    // --- printer ---
    for (char* p = c.printer.serial; *p; ++p) *p = (char)toupper((unsigned char)*p);
    // The access code is always 8 characters; anything else is a typo, not a code.
    if (c.printer.accessCode[0] != '\0' && strlen(c.printer.accessCode) != 8) {
        LOGW("access code must be 8 chars, clearing");
        c.printer.accessCode[0] = '\0';
    }
    static const char* kModels[] = {"auto", "x1", "p1", "a1", "h2d"};
    clampEnum(c.printer.model, sizeof(c.printer.model), kModels, 5, "auto");

    // --- fan ---
    if (!curveValidate(c.fan.curve)) {
        LOGW("fan curve invalid, restoring defaults");
        curveDefaults(c.fan.curve);
    }
    static const char* kSources[] = {"nozzle", "bed", "chamber", "max"};
    clampEnum(c.fan.source, sizeof(c.fan.source), kSources, 4, "nozzle");
    static const char* kModes[] = {"auto", "manual", "off"};
    clampEnum(c.fan.mode, sizeof(c.fan.mode), kModes, 3, "auto");
    static const char* kStale[] = {"hold", "off", "fixed"};
    clampEnum(c.fan.staleMode, sizeof(c.fan.staleMode), kStale, 3, "off");

    c.fan.manualSpeed = clampVal<uint8_t>(c.fan.manualSpeed, 0, 100);
    c.fan.minSpeed = clampVal<uint8_t>(c.fan.minSpeed, 0, 100);
    c.fan.staleSpeed = clampVal<uint8_t>(c.fan.staleSpeed, 0, 100);
    c.fan.kickMs = clampVal<uint16_t>(c.fan.kickMs, 0, 5000);
    if (isnan(c.fan.hysteresis) || c.fan.hysteresis < 0.0f) c.fan.hysteresis = 0.0f;
    if (c.fan.hysteresis > 50.0f) c.fan.hysteresis = 50.0f;
    c.fan.rampRate = clampVal<uint16_t>(c.fan.rampRate, 0, 1000);
    c.fan.pwmFreq = clampVal<uint32_t>(c.fan.pwmFreq, 500, 40000);
    c.fan.cooldownMin = clampVal<uint16_t>(c.fan.cooldownMin, 0, 1440);
    // A stale window shorter than a couple of report intervals would flap.
    c.fan.staleSec = clampVal<uint16_t>(c.fan.staleSec, 10, 3600);

    // --- mqtt ---
    if (c.mqtt.port == 0) c.mqtt.port = 1883;
    c.mqtt.publishIntervalSec = clampVal<uint16_t>(c.mqtt.publishIntervalSec, 1, 3600);
    if (c.mqtt.haPrefix[0] == '\0') strlcpy(c.mqtt.haPrefix, "homeassistant", sizeof(c.mqtt.haPrefix));
    if (c.mqtt.host[0] == '\0') c.mqtt.enabled = false;
    // Trailing slashes would produce "base//state".
    size_t bt = strlen(c.mqtt.baseTopic);
    while (bt > 0 && c.mqtt.baseTopic[bt - 1] == '/') c.mqtt.baseTopic[--bt] = '\0';

    // --- web ---
    if (c.web.user[0] == '\0') strlcpy(c.web.user, "admin", sizeof(c.web.user));
    // Auth with no password would lock nothing but confuse everyone.
    if (c.web.password[0] == '\0') c.web.authEnabled = false;
}

// ---------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------

namespace {

// Copies in[key] into dst only when the key really holds a string.
// `strlcpy(dst, in[key] | dst, n)` reads nicely but hands strlcpy the same
// buffer for source and destination whenever the key is absent, and overlapping
// copies are undefined behaviour.
void copyIfString(JsonObjectConst in, const char* key, char* dst, size_t dstSize)
{
    if (!in[key].is<const char*>()) return;
    const char* v = in[key].as<const char*>();
    if (v) strlcpy(dst, v, dstSize);
}

void curveFromJson(JsonArrayConst arr, FanCurve& out)
{
    FanCurve tmp{};
    tmp.count = 0;
    for (JsonObjectConst o : arr) {
        if (tmp.count >= CURVE_MAX_POINTS) break;
        tmp.pts[tmp.count].temp = o["temp"] | 0.0f;
        int sp = o["speed"] | 0;
        tmp.pts[tmp.count].speed = (uint8_t)clampVal<int>(sp, 0, 100);
        tmp.count++;
    }
    if (curveValidate(tmp)) out = tmp;
    // Otherwise `out` keeps whatever it had (defaults or the previous curve).
}

// Reads a config document into `c`, treating every key as optional.
void applyDocument(JsonObjectConst root, Config& c)
{
    JsonObjectConst w = root["wifi"];
    if (!w.isNull()) {
        copyIfString(w, "ssid",     c.wifi.ssid,     sizeof(c.wifi.ssid));
        copyIfString(w, "password", c.wifi.password, sizeof(c.wifi.password));
        copyIfString(w, "bssid",    c.wifi.bssid,    sizeof(c.wifi.bssid));
        copyIfString(w, "hostname", c.wifi.hostname, sizeof(c.wifi.hostname));
        c.wifi.lockBssid = w["lockBssid"] | c.wifi.lockBssid;
    }
    JsonObjectConst p = root["printer"];
    if (!p.isNull()) {
        copyIfString(p, "ip",         c.printer.ip,         sizeof(c.printer.ip));
        copyIfString(p, "accessCode", c.printer.accessCode, sizeof(c.printer.accessCode));
        copyIfString(p, "serial",     c.printer.serial,     sizeof(c.printer.serial));
        copyIfString(p, "model",      c.printer.model,      sizeof(c.printer.model));
    }
    JsonObjectConst f = root["fan"];
    if (!f.isNull()) {
        if (f["curve"].is<JsonArrayConst>()) curveFromJson(f["curve"], c.fan.curve);
        copyIfString(f, "source",    c.fan.source,    sizeof(c.fan.source));
        copyIfString(f, "mode",      c.fan.mode,      sizeof(c.fan.mode));
        copyIfString(f, "staleMode", c.fan.staleMode, sizeof(c.fan.staleMode));
        c.fan.manualSpeed       = f["manualSpeed"]       | c.fan.manualSpeed;
        c.fan.minSpeed          = f["minSpeed"]          | c.fan.minSpeed;
        c.fan.kickStart         = f["kickStart"]         | c.fan.kickStart;
        c.fan.kickMs            = f["kickMs"]            | c.fan.kickMs;
        c.fan.hysteresis        = f["hysteresis"]        | c.fan.hysteresis;
        c.fan.rampRate          = f["rampRate"]          | c.fan.rampRate;
        c.fan.pwmFreq           = f["pwmFreq"]           | c.fan.pwmFreq;
        c.fan.pwmInvert         = f["pwmInvert"]         | c.fan.pwmInvert;
        c.fan.output1           = f["output1"]           | c.fan.output1;
        c.fan.output2           = f["output2"]           | c.fan.output2;
        c.fan.onlyWhilePrinting = f["onlyWhilePrinting"] | c.fan.onlyWhilePrinting;
        c.fan.cooldownMin       = f["cooldownMin"]       | c.fan.cooldownMin;
        c.fan.staleSec          = f["staleSec"]          | c.fan.staleSec;
        c.fan.staleSpeed        = f["staleSpeed"]        | c.fan.staleSpeed;
    }
    JsonObjectConst m = root["mqtt"];
    if (!m.isNull()) {
        c.mqtt.enabled = m["enabled"] | c.mqtt.enabled;
        copyIfString(m, "host",      c.mqtt.host,      sizeof(c.mqtt.host));
        copyIfString(m, "user",      c.mqtt.user,      sizeof(c.mqtt.user));
        copyIfString(m, "password",  c.mqtt.password,  sizeof(c.mqtt.password));
        copyIfString(m, "baseTopic", c.mqtt.baseTopic, sizeof(c.mqtt.baseTopic));
        copyIfString(m, "haPrefix",  c.mqtt.haPrefix,  sizeof(c.mqtt.haPrefix));
        c.mqtt.port = m["port"] | c.mqtt.port;
        c.mqtt.haDiscovery = m["haDiscovery"] | c.mqtt.haDiscovery;
        c.mqtt.publishIntervalSec = m["publishIntervalSec"] | c.mqtt.publishIntervalSec;
    }
    JsonObjectConst wb = root["web"];
    if (!wb.isNull()) {
        c.web.authEnabled = wb["authEnabled"] | c.web.authEnabled;
        copyIfString(wb, "user",     c.web.user,     sizeof(c.web.user));
        copyIfString(wb, "password", c.web.password, sizeof(c.web.password));
    }
    JsonObjectConst d = root["debug"];
    if (!d.isNull()) {
        c.debug.serial   = d["serial"]   | c.debug.serial;
        c.debug.mqttDump = d["mqttDump"] | c.debug.mqttDump;
    }
    JsonObjectConst s = root["ssdp"];
    if (!s.isNull()) c.ssdp.enabled = s["enabled"] | c.ssdp.enabled;
}

// One-time import of the 1.x config file, then delete it.
bool migrateLegacy(Config& c)
{
    if (!LittleFS.exists(kLegacyPath)) return false;
    LOGI("migrating legacy %s", kLegacyPath);
    File f = LittleFS.open(kLegacyPath, "r");
    if (!f) return false;
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) {
        LOGW("legacy config unreadable (%s), discarding", err.c_str());
        LittleFS.remove(kLegacyPath);
        return false;
    }
    strlcpy(c.wifi.ssid,          doc["ssid"]         | "", sizeof(c.wifi.ssid));
    strlcpy(c.wifi.password,      doc["appw"]         | "", sizeof(c.wifi.password));
    strlcpy(c.wifi.bssid,         doc["bssi"]         | "", sizeof(c.wifi.bssid));
    strlcpy(c.printer.ip,         doc["printerIp"]    | "", sizeof(c.printer.ip));
    strlcpy(c.printer.accessCode, doc["accessCode"]   | "", sizeof(c.printer.accessCode));
    strlcpy(c.printer.serial,     doc["serialNumber"] | "", sizeof(c.printer.serial));
    c.debug.serial   = doc["debuging"]  | c.debug.serial;
    c.debug.mqttDump = doc["mqttdebug"] | c.debug.mqttDump;
    if (doc["fanPoints"].is<JsonArrayConst>()) curveFromJson(doc["fanPoints"], c.fan.curve);

    LittleFS.remove(kLegacyPath);
    return true;
}

}  // namespace

bool configLoad()
{
    configDefaults(g_cfg);

    if (!LittleFS.begin(true)) {          // true = format on first mount failure
        LOGE("LittleFS mount failed, running on defaults");
        configValidate(g_cfg);
        return false;
    }

    bool migrated = migrateLegacy(g_cfg);

    if (!LittleFS.exists(kConfigPath)) {
        LOGI("no %s, using defaults", kConfigPath);
        configValidate(g_cfg);
        configSave();                      // so the next boot has a real file
        return migrated;
    }

    File f = LittleFS.open(kConfigPath, "r");
    if (!f) {
        LOGE("cannot open %s", kConfigPath);
        configValidate(g_cfg);
        return false;
    }
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err || !doc.is<JsonObject>()) {
        LOGE("config parse error (%s) - moved to %s", err.c_str(), kBadPath);
        LittleFS.remove(kBadPath);
        LittleFS.rename(kConfigPath, kBadPath);
        configValidate(g_cfg);
        return false;
    }

    applyDocument(doc.as<JsonObjectConst>(), g_cfg);
    configValidate(g_cfg);
    if (migrated) configSave();
    LOGI("config loaded (v%d)", (int)(doc["version"] | 0));
    return true;
}

bool configSave()
{
    ConfigGuard guard;
    g_dirty = false;

    JsonDocument doc;
    JsonObject root = doc.to<JsonObject>();
    configToJson(root, g_cfg, /*masked=*/false);

    LittleFS.remove(kTempPath);
    File f = LittleFS.open(kTempPath, "w");
    if (!f) {
        LOGE("cannot open %s for write", kTempPath);
        return false;
    }
    const size_t written = serializeJson(doc, f);
    f.close();
    if (written == 0) {
        LOGE("config serialise failed");
        LittleFS.remove(kTempPath);
        return false;
    }
    // Rename is the atomic step: a power cut leaves either the old or the new
    // file. Removing the target first would open a window in which neither
    // exists, which is exactly the crash we are protecting against.
    if (!LittleFS.rename(kTempPath, kConfigPath)) {
        LOGE("config rename failed");
        return false;
    }
    LOGI("config saved (%u bytes)", (unsigned)written);
    return true;
}

void configWipe()
{
    LittleFS.remove(kConfigPath);
    LittleFS.remove(kTempPath);
    LittleFS.remove(kBadPath);
    LittleFS.remove(kLegacyPath);
    LOGW("configuration wiped");
}

void configToJson(JsonObject out, const Config& c, bool masked)
{
    // Strings are wrapped in String() so ArduinoJson copies them: a bare
    // `const char*` is stored by pointer and would dangle if `c` were a local.
    out["version"] = c.version;

    JsonObject w = out["wifi"].to<JsonObject>();
    w["ssid"] = String(c.wifi.ssid);
    w["password"] = (masked && c.wifi.password[0]) ? String(kMask) : String(c.wifi.password);
    w["bssid"] = String(c.wifi.bssid);
    w["lockBssid"] = c.wifi.lockBssid;
    w["hostname"] = String(c.wifi.hostname);

    JsonObject p = out["printer"].to<JsonObject>();
    p["ip"] = String(c.printer.ip);
    p["accessCode"] = (masked && c.printer.accessCode[0]) ? String(kMask) : String(c.printer.accessCode);
    p["serial"] = String(c.printer.serial);
    p["model"] = String(c.printer.model);

    JsonObject f = out["fan"].to<JsonObject>();
    curveToJson(f["curve"].to<JsonArray>(), c.fan.curve);
    f["source"] = String(c.fan.source);
    f["mode"] = String(c.fan.mode);
    f["manualSpeed"] = c.fan.manualSpeed;
    f["minSpeed"] = c.fan.minSpeed;
    f["kickStart"] = c.fan.kickStart;
    f["kickMs"] = c.fan.kickMs;
    f["hysteresis"] = c.fan.hysteresis;
    f["rampRate"] = c.fan.rampRate;
    f["pwmFreq"] = c.fan.pwmFreq;
    f["pwmInvert"] = c.fan.pwmInvert;
    f["output1"] = c.fan.output1;
    f["output2"] = c.fan.output2;
    f["onlyWhilePrinting"] = c.fan.onlyWhilePrinting;
    f["cooldownMin"] = c.fan.cooldownMin;
    f["staleSec"] = c.fan.staleSec;
    f["staleMode"] = String(c.fan.staleMode);
    f["staleSpeed"] = c.fan.staleSpeed;

    JsonObject m = out["mqtt"].to<JsonObject>();
    m["enabled"] = c.mqtt.enabled;
    m["host"] = String(c.mqtt.host);
    m["port"] = c.mqtt.port;
    m["user"] = String(c.mqtt.user);
    m["password"] = (masked && c.mqtt.password[0]) ? String(kMask) : String(c.mqtt.password);
    m["baseTopic"] = String(c.mqtt.baseTopic);
    m["haDiscovery"] = c.mqtt.haDiscovery;
    m["haPrefix"] = String(c.mqtt.haPrefix);
    m["publishIntervalSec"] = c.mqtt.publishIntervalSec;

    JsonObject wb = out["web"].to<JsonObject>();
    wb["authEnabled"] = c.web.authEnabled;
    wb["user"] = String(c.web.user);
    wb["password"] = (masked && c.web.password[0]) ? String(kMask) : String(c.web.password);

    JsonObject d = out["debug"].to<JsonObject>();
    d["serial"] = String(c.debug.serial);
    d["mqttDump"] = c.debug.mqttDump;

    JsonObject s = out["ssdp"].to<JsonObject>();
    s["enabled"] = c.ssdp.enabled;
}

void configFromJson(JsonObjectConst in, Config& c, ConfigChange& change)
{
    JsonObjectConst w = in["wifi"];
    if (!w.isNull()) {
        bool ch = false;
        ch |= mergeStr(w, "ssid", c.wifi.ssid, sizeof(c.wifi.ssid));
        ch |= mergeStr(w, "password", c.wifi.password, sizeof(c.wifi.password), true);
        ch |= mergeStr(w, "bssid", c.wifi.bssid, sizeof(c.wifi.bssid));
        ch |= mergeStr(w, "hostname", c.wifi.hostname, sizeof(c.wifi.hostname));
        ch |= mergeBool(w, "lockBssid", c.wifi.lockBssid);
        if (ch) change.restartRequired = true;
    }

    JsonObjectConst p = in["printer"];
    if (!p.isNull()) {
        bool ch = false;
        ch |= mergeStr(p, "ip", c.printer.ip, sizeof(c.printer.ip));
        ch |= mergeStr(p, "accessCode", c.printer.accessCode, sizeof(c.printer.accessCode), true);
        ch |= mergeStr(p, "serial", c.printer.serial, sizeof(c.printer.serial));
        ch |= mergeStr(p, "model", c.printer.model, sizeof(c.printer.model));
        if (ch) change.printerChanged = true;
    }

    JsonObjectConst f = in["fan"];
    if (!f.isNull()) {
        if (f["curve"].is<JsonArrayConst>()) { curveFromJson(f["curve"], c.fan.curve); change.fanChanged = true; }
        bool ch = false;
        ch |= mergeStr(f, "source", c.fan.source, sizeof(c.fan.source));
        ch |= mergeStr(f, "mode", c.fan.mode, sizeof(c.fan.mode));
        ch |= mergeStr(f, "staleMode", c.fan.staleMode, sizeof(c.fan.staleMode));
        ch |= mergeNum<uint8_t>(f, "manualSpeed", c.fan.manualSpeed);
        ch |= mergeNum<uint8_t>(f, "minSpeed", c.fan.minSpeed);
        ch |= mergeBool(f, "kickStart", c.fan.kickStart);
        ch |= mergeNum<uint16_t>(f, "kickMs", c.fan.kickMs);
        ch |= mergeNum<float>(f, "hysteresis", c.fan.hysteresis);
        ch |= mergeNum<uint16_t>(f, "rampRate", c.fan.rampRate);
        ch |= mergeNum<uint32_t>(f, "pwmFreq", c.fan.pwmFreq);
        ch |= mergeBool(f, "pwmInvert", c.fan.pwmInvert);
        ch |= mergeBool(f, "output1", c.fan.output1);
        ch |= mergeBool(f, "output2", c.fan.output2);
        ch |= mergeBool(f, "onlyWhilePrinting", c.fan.onlyWhilePrinting);
        ch |= mergeNum<uint16_t>(f, "cooldownMin", c.fan.cooldownMin);
        ch |= mergeNum<uint16_t>(f, "staleSec", c.fan.staleSec);
        ch |= mergeNum<uint8_t>(f, "staleSpeed", c.fan.staleSpeed);
        if (ch) change.fanChanged = true;
    }

    JsonObjectConst m = in["mqtt"];
    if (!m.isNull()) {
        bool ch = false;
        ch |= mergeBool(m, "enabled", c.mqtt.enabled);
        ch |= mergeStr(m, "host", c.mqtt.host, sizeof(c.mqtt.host));
        ch |= mergeNum<uint16_t>(m, "port", c.mqtt.port);
        ch |= mergeStr(m, "user", c.mqtt.user, sizeof(c.mqtt.user));
        ch |= mergeStr(m, "password", c.mqtt.password, sizeof(c.mqtt.password), true);
        ch |= mergeStr(m, "baseTopic", c.mqtt.baseTopic, sizeof(c.mqtt.baseTopic));
        ch |= mergeBool(m, "haDiscovery", c.mqtt.haDiscovery);
        ch |= mergeStr(m, "haPrefix", c.mqtt.haPrefix, sizeof(c.mqtt.haPrefix));
        ch |= mergeNum<uint16_t>(m, "publishIntervalSec", c.mqtt.publishIntervalSec);
        if (ch) change.mqttChanged = true;
    }

    JsonObjectConst wb = in["web"];
    if (!wb.isNull()) {
        // No restart: authorisation is evaluated per request straight out of
        // cfg(), so a new user/password takes effect on the very next one.
        mergeBool(wb, "authEnabled", c.web.authEnabled);
        mergeStr(wb, "user", c.web.user, sizeof(c.web.user));
        mergeStr(wb, "password", c.web.password, sizeof(c.web.password), true);
    }

    JsonObjectConst d = in["debug"];
    if (!d.isNull()) {
        mergeBool(d, "serial", c.debug.serial);
        mergeBool(d, "mqttDump", c.debug.mqttDump);
    }

    JsonObjectConst s = in["ssdp"];
    if (!s.isNull()) {
        if (mergeBool(s, "enabled", c.ssdp.enabled)) change.ssdpChanged = true;
    }

    configValidate(c);
}

}  // namespace blsf
