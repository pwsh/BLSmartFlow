#include "ha_mqtt.h"

#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <math.h>

#include "app.h"
#include "config.h"
#include "cooldown.h"
#include "curve.h"
#include "fan_control.h"
#include "log.h"
#include "state.h"
#include "status.h"
#include "version.h"
#include "wifi_manager.h"

namespace blsf {

namespace {

// Reconnect cadence: 10 s, backing off to 60 s while the broker stays away, so
// an unreachable broker costs one blocking connect() per minute instead of six.
const uint32_t kReconnectMinMs = 10000;
const uint32_t kReconnectMaxMs = 60000;
// A DNS answer can go stale (broker moved, DHCP lease changed); re-resolve after
// this many consecutive failures rather than retrying a dead address forever.
const uint8_t  kReresolveAfterFailures = 5;
// Publish failures are usually a burst (broker gone); log one per window.
const uint32_t kPublishWarnIntervalMs = 30000;

NetworkClient g_net;
PubSubClient  g_mqtt(g_net);
bool          g_enabled = false;
bool          g_discoveryPublished = false;
volatile bool g_reconfigure = false;
uint32_t      g_nextAttemptMs = 0;
uint32_t      g_reconnectMs = kReconnectMinMs;
uint8_t       g_failures = 0;
uint32_t      g_lastPublishMs = 0;
uint32_t      g_lastPublishWarnMs = 0;
int           g_lastOutput = -1;
IPAddress     g_hostIp;
String        g_base;
String        g_availability;
char          g_clientId[24] = {0};

String topic(const char* suffix) { return g_base + "/" + suffix; }

// Every publish is checked: PubSubClient returns false for a payload that does
// not fit its buffer or a socket that has gone away, and silently dropping the
// state topic is exactly the kind of failure that looks like a firmware bug.
bool publishChecked(const String& t, const char* payload, bool retained)
{
    if (g_mqtt.publish(t.c_str(), payload, retained)) return true;
    const uint32_t now = millis();
    if (g_lastPublishWarnMs == 0 || (now - g_lastPublishWarnMs) >= kPublishWarnIntervalMs) {
        g_lastPublishWarnMs = now;
        LOGW("ha: publish to '%s' failed (state %d)", t.c_str(), g_mqtt.state());
    }
    return false;
}

// --- Home Assistant discovery ---------------------------------------------

String uniquePrefix() { return String("blsmartflow_") + chipId(); }

void addDeviceBlock(JsonObject doc)
{
    JsonObject dev = doc["device"].to<JsonObject>();
    JsonArray ids = dev["identifiers"].to<JsonArray>();
    ids.add(uniquePrefix());
    dev["name"] = String("BLSmartFlow ") + chipId();
    dev["manufacturer"] = "DutchDeveloper";
    dev["model"] = FW_NAME;
    dev["sw_version"] = FW_VERSION;
    dev["configuration_url"] = String("http://") + wifiIp().toString() + "/";
}

// Publishes one discovery document, or an empty payload to delete the entity.
void publishDiscovery(const char* component, const char* objectId, JsonDocument& doc, bool remove)
{
    char prefix[sizeof(cfg().mqtt.haPrefix)];
    {
        ConfigGuard guard;
        strlcpy(prefix, cfg().mqtt.haPrefix, sizeof(prefix));
    }
    String t = String(prefix) + "/" + component + "/" + uniquePrefix() + "/" + objectId + "/config";
    if (remove) {
        g_mqtt.publish(t.c_str(), "", true);
        return;
    }
    JsonObject o = doc.as<JsonObject>();
    o["unique_id"] = uniquePrefix() + "_" + objectId;
    o["object_id"] = uniquePrefix() + "_" + objectId;
    o["availability_topic"] = g_availability;
    addDeviceBlock(o);

    String payload;
    serializeJson(doc, payload);
    if (!g_mqtt.publish(t.c_str(), payload.c_str(), true)) {
        LOGW("ha: discovery publish failed for %s (%u bytes)", objectId, (unsigned)payload.length());
    }
}

// A numeric field that can be null must not reach Home Assistant as an empty
// string - HA logs a parse error for every such update. Emitting the literal
// "unknown" makes it show an unknown state instead.
String nullableTemplate(const char* path)
{
    return String("{% set v = ") + path + " %}{{ 'unknown' if v is none else v }}";
}

// A sensor whose extra attributes come from the same retained `state` document.
// Home Assistant needs a separate attributes topic even when it is the same
// topic, hence json_attributes_topic + json_attributes_template.
void discoverSensorWithAttrs(const char* objectId, const char* name, const char* valueTemplate,
                             const char* attrTemplate, const char* icon, bool remove)
{
    JsonDocument doc;
    if (!remove) {
        doc["name"] = name;
        doc["state_topic"] = topic("state");
        doc["value_template"] = valueTemplate;
        doc["json_attributes_topic"] = topic("state");
        doc["json_attributes_template"] = attrTemplate;
        if (icon) doc["icon"] = icon;
    }
    publishDiscovery("sensor", objectId, doc, remove);
}

// A sensor that reads one field out of the retained `state` document.
void discoverSensor(const char* objectId, const char* name, const char* valueTemplate,
                    const char* unit, const char* deviceClass, const char* stateClass,
                    const char* icon, bool remove)
{
    JsonDocument doc;
    if (!remove) {
        doc["name"] = name;
        doc["state_topic"] = topic("state");
        doc["value_template"] = valueTemplate;
        if (unit) doc["unit_of_measurement"] = unit;
        if (deviceClass) doc["device_class"] = deviceClass;
        if (stateClass) doc["state_class"] = stateClass;
        if (icon) doc["icon"] = icon;
    }
    publishDiscovery("sensor", objectId, doc, remove);
}

void discoverBinary(const char* objectId, const char* name, const char* valueTemplate,
                    const char* deviceClass, bool remove)
{
    JsonDocument doc;
    if (!remove) {
        doc["name"] = name;
        doc["state_topic"] = topic("state");
        doc["value_template"] = valueTemplate;
        doc["payload_on"] = "ON";
        doc["payload_off"] = "OFF";
        if (deviceClass) doc["device_class"] = deviceClass;
    }
    publishDiscovery("binary_sensor", objectId, doc, remove);
}

// A writable set point. HA number entities need a command topic of their own,
// so each target gets `<base>/<id>/set` alongside the combined `target/set`.
void discoverNumber(const char* objectId, const char* name, const char* commandSuffix,
                    const char* valueTemplate, int min, int max, const char* icon, bool remove)
{
    JsonDocument doc;
    if (!remove) {
        doc["name"] = name;
        doc["command_topic"] = topic(commandSuffix);
        doc["state_topic"] = topic("state");
        doc["value_template"] = valueTemplate;
        doc["min"] = min;
        doc["max"] = max;
        doc["step"] = 1;
        doc["mode"] = "box";
        doc["unit_of_measurement"] = "\u00b0C";
        doc["device_class"] = "temperature";
        if (icon) doc["icon"] = icon;
    }
    publishDiscovery("number", objectId, doc, remove);
}

// A switch whose state is read out of the retained `state` document. Used for
// the cool-down session, which is a thing you turn on and off rather than a
// setting you store.
void discoverSwitch(const char* objectId, const char* name, const char* commandSuffix,
                    const char* valueTemplate, const char* icon, bool remove)
{
    JsonDocument doc;
    if (!remove) {
        doc["name"] = name;
        doc["command_topic"] = topic(commandSuffix);
        doc["state_topic"] = topic("state");
        doc["value_template"] = valueTemplate;
        doc["payload_on"] = "ON";
        doc["payload_off"] = "OFF";
        if (icon) doc["icon"] = icon;
    }
    publishDiscovery("switch", objectId, doc, remove);
}

void publishDiscoveryAll(bool remove)
{
    {
        JsonDocument doc;
        if (!remove) {
            doc["name"] = "Fan";
            doc["command_topic"] = topic("fan/on");
            doc["state_topic"] = topic("fan/on_state");
            doc["percentage_command_topic"] = topic("fan/set");
            doc["percentage_state_topic"] = topic("fan/speed");
            doc["speed_range_min"] = 1;
            doc["speed_range_max"] = 100;
            doc["payload_on"] = "ON";
            doc["payload_off"] = "OFF";
        }
        publishDiscovery("fan", "fan", doc, remove);
    }
    {
        JsonDocument doc;
        if (!remove) {
            doc["name"] = "Mode";
            doc["command_topic"] = topic("mode/set");
            doc["state_topic"] = topic("mode");
            JsonArray opts = doc["options"].to<JsonArray>();
            opts.add("auto");
            opts.add("chamber");
            opts.add("manual");
            opts.add("off");
        }
        publishDiscovery("select", "mode", doc, remove);
    }
    {
        JsonDocument doc;
        if (!remove) {
            doc["name"] = "Restart";
            doc["command_topic"] = topic("restart");
            doc["payload_press"] = "PRESS";
            doc["device_class"] = "restart";
        }
        publishDiscovery("button", "restart", doc, remove);
    }

    const String nozzleT   = nullableTemplate("value_json.printer.temps.nozzle");
    const String bedT       = nullableTemplate("value_json.printer.temps.bed");
    const String chamberT   = nullableTemplate("value_json.printer.temps.chamber");
    const String progressT  = nullableTemplate("value_json.printer.progress");
    const String remainingT = nullableTemplate("value_json.printer.remainingMin");

    discoverSensor("nozzle_temp",   "Nozzle temperature",  nozzleT.c_str(),   "°C", "temperature", "measurement", nullptr, remove);
    discoverSensor("bed_temp",      "Bed temperature",     bedT.c_str(),      "°C", "temperature", "measurement", nullptr, remove);
    discoverSensor("chamber_temp",  "Chamber temperature", chamberT.c_str(),  "°C", "temperature", "measurement", nullptr, remove);
    discoverSensor("fan_output",    "Fan output",          "{{ value_json.fan.output }}",            "%",   nullptr,       "measurement", "mdi:fan", remove);
    discoverSensor("printer_state", "Printer state",       "{{ value_json.printer.state }}",         nullptr, nullptr,     nullptr,       "mdi:printer-3d", remove);
    discoverSensor("printer_stage", "Printer stage",       "{{ value_json.printer.stageText }}",     nullptr, nullptr,     nullptr,       "mdi:printer-3d-nozzle", remove);
    discoverSensor("print_progress","Print progress",      progressT.c_str(),  "%",   nullptr,       "measurement", "mdi:progress-clock", remove);
    discoverSensor("remaining_time","Remaining time",      remainingT.c_str(), "min", "duration",    nullptr,       nullptr, remove);
    discoverSensor("printer_wifi",  "Printer WiFi",        "{{ value_json.printer.wifiSignal }}",    nullptr, nullptr,     nullptr,       "mdi:wifi", remove);
    discoverSensor("device_rssi",   "Device RSSI",         "{{ value_json.wifi.rssi }}",             "dBm", "signal_strength", "measurement", nullptr, remove);
    discoverSensor("uptime",        "Uptime",              "{{ value_json.device.uptimeSec }}",      "s",   "duration",    "total_increasing", nullptr, remove);

    const String coolingT = nullableTemplate("value_json.thermal.rateCPerMin");
    discoverSensor("phase",        "Print phase",  "{{ value_json.printer.phase }}", nullptr, nullptr, nullptr, "mdi:state-machine", remove);
    discoverSensor("cooling_rate", "Cooling rate", coolingT.c_str(), "\u00b0C/min", nullptr, "measurement", "mdi:thermometer-chevron-down", remove);

    // Writable set points. The state comes out of the retained `state` document,
    // which carries the configured values under fan.chamberTarget / cooldownTarget.
    discoverNumber("chamber_target",  "Chamber target",   "chamber_target/set",
                   "{{ value_json.fan.chamberTarget }}", 20, 80, "mdi:home-thermometer", remove);
    discoverNumber("cooldown_target", "Cool-down target", "cooldown_target/set",
                   "{{ value_json.fan.cooldownTarget }}", 15, 60, "mdi:snowflake-thermometer", remove);

    // Filament (REWORK-SPEC 16.4). The state is the guide's display name, which is
    // "unknown" until the printer has told us what is in the active tray.
    const String filamentT = nullableTemplate("value_json.filament.name");
    const String filamentChamberT = nullableTemplate("value_json.filament.effective.chamberTarget");
    // Attributes are built explicitly rather than dumping the whole block: an
    // attribute set that changes shape between updates is what makes an HA
    // history graph unusable.
    static const char kFilamentAttrs[] =
        "{{ {'type': value_json.filament.tray.type if value_json.filament.tray else none,"
        " 'idx': value_json.filament.tray.idx if value_json.filament.tray else none,"
        " 'id': value_json.filament.id,"
        " 'family': value_json.filament.family,"
        " 'source': value_json.filament.source,"
        " 'vent': value_json.filament.profile.vent if value_json.filament.profile else none,"
        " 'chamberTarget': value_json.filament.effective.chamberTarget,"
        " 'ventFloor': value_json.filament.effective.ventFloor,"
        " 'postPrintCooling': value_json.filament.effective.postPrintCooling} | tojson }}";
    discoverSensorWithAttrs("filament", "Filament", filamentT.c_str(), kFilamentAttrs,
                            "mdi:printer-3d-nozzle", remove);
    discoverSensor("filament_chamber_target", "Filament chamber target", filamentChamberT.c_str(),
                   "\u00b0C", "temperature", nullptr, "mdi:home-thermometer", remove);

    // Post-print cool-down (REWORK-SPEC 17.3). The switch is the session, not the
    // setting: turning it off ends the session, it does not disable the feature.
    discoverSwitch("cooldown", "Cool-down", "cooldown/set",
                   "{{ 'ON' if value_json.cooldown.active else 'OFF' }}",
                   "mdi:snowflake", remove);
    // Minutes left before the hard stop, so an automation can say "tell me when
    // the chamber is cool" without subscribing to the whole state document.
    static const char kCooldownRemaining[] =
        "{% set c = value_json.cooldown %}"
        "{{ ((c.maxSec - c.elapsedSec) / 60) | round(0, 'ceil') if c.active else 0 }}";
    discoverSensor("cooldown_remaining", "Cool-down remaining", kCooldownRemaining,
                   "min", "duration", nullptr, "mdi:timer-sand", remove);
    static const char kCooldownReason[] =
        "{% set v = value_json.cooldown.reason %}{{ 'none' if v is none else v }}";
    discoverSensor("cooldown_reason", "Cool-down result", kCooldownReason,
                   nullptr, nullptr, nullptr, "mdi:information-outline", remove);

    discoverBinary("printer_online", "Printer online", "{{ 'ON' if value_json.printer.online else 'OFF' }}", "connectivity", remove);
    // doorOpen is null until the printer has proved its door switch reports.
    // "None" is the payload Home Assistant maps to an unknown binary state; the
    // naive `'ON' if ... else 'OFF'` template would report a stuck door as shut.
    discoverBinary("door",           "Door",
                   "{% set v = value_json.printer.doorOpen %}"
                   "{{ 'None' if v is none else ('ON' if v else 'OFF') }}", "opening", remove);
    discoverBinary("printing",       "Printing",       "{{ 'ON' if value_json.printer.printing else 'OFF' }}", "running", remove);
}

// --- publishing ------------------------------------------------------------

void publishState()
{
    JsonDocument doc;
    {
        ConfigGuard guard;
        buildStatus(doc.to<JsonObject>());
    }
    String payload;
    serializeJson(doc, payload);
    // The status document can exceed the default 256-byte buffer by a lot.
    if (payload.length() + g_base.length() + 16 > g_mqtt.getBufferSize()) {
        g_mqtt.setBufferSize(payload.length() + g_base.length() + 64);
    }
    publishChecked(topic("state"), payload.c_str(), true);

    const FanState f = fanSnapshot();
    const int out = (int)lroundf(f.output);
    char buf[8];
    snprintf(buf, sizeof(buf), "%d", out);
    publishChecked(topic("fan/speed"), buf, true);
    publishChecked(topic("fan/on_state"), out > 0 ? "ON" : "OFF", true);

    char mode[sizeof(cfg().fan.mode)];
    {
        ConfigGuard guard;
        strlcpy(mode, cfg().fan.mode, sizeof(mode));
    }
    publishChecked(topic("mode"), mode, true);

    g_lastOutput = out;
    g_lastPublishMs = millis();
}

// --- commands --------------------------------------------------------------

// Fan and mode commands arrive as fast as a Home Assistant slider can move them.
// Writing LittleFS on each one would wear the flash out, so the config is only
// marked dirty and loop() persists it at most every 10 s.
void applyAndPersist()
{
    {
        ConfigGuard guard;
        configValidate(cfg());
    }
    configMarkDirty();
    fanControlReconfigure();
}

// The largest command body we will look at. curve/set is the only one that can
// be long; anything past this is not a curve we would accept anyway.
const size_t kMaxCommandBytes = 2048;

void onCommand(char* rawTopic, byte* payload, unsigned int length)
{
    // PubSubClient payloads are not NUL-terminated. Scalar commands are tiny, so
    // a small stack copy covers them; curve/set is parsed straight from the
    // payload below rather than being truncated into this buffer.
    char body[24];
    const size_t n = length < sizeof(body) - 1 ? length : sizeof(body) - 1;
    if (payload && n) memcpy(body, payload, n);
    body[n] = '\0';

    const String t(rawTopic);
    const String suffix = t.startsWith(g_base + "/") ? t.substring(g_base.length() + 1) : t;

    bool persist = true;
    if (suffix == "fan/set") {
        const int pct = atoi(body);
        fanApplyMode("manual", pct, 0, persist);
        applyAndPersist();
    } else if (suffix == "fan/on") {
        if (strcasecmp(body, "OFF") == 0) {
            fanApplyMode("off", -1, 0, persist);
        } else {
            // HA turns a fan on before setting a percentage; make sure "on" is
            // never a silent 0 %.
            int speed;
            {
                ConfigGuard guard;
                speed = cfg().fan.manualSpeed > 0 ? -1 : 1;
            }
            fanApplyMode("manual", speed, 0, persist);
        }
        applyAndPersist();
    } else if (suffix == "mode/set") {
        if (!fanApplyMode(body, -1, 0, persist)) {
            LOGW("ha: unknown mode '%s'", body);
            return;
        }
        applyAndPersist();
    } else if (suffix == "curve/set") {
        // Parse the payload where it lies: a curve document is far longer than
        // the scalar-command buffer, and copying it there would truncate every
        // curve into a syntax error.
        if (!payload || length == 0 || length > kMaxCommandBytes) {
            LOGW("ha: curve/set payload missing or too large (%u bytes)", (unsigned)length);
            return;
        }
        JsonDocument doc;
        if (deserializeJson(doc, (const char*)payload, (size_t)length)) {
            LOGW("ha: curve/set payload is not valid json");
            return;
        }
        FanCurve tmp{};
        tmp.count = 0;
        for (JsonObjectConst o : doc["points"].as<JsonArrayConst>()) {
            if (tmp.count >= CURVE_MAX_POINTS) break;
            tmp.pts[tmp.count].temp = o["temp"] | 0.0f;
            int sp = o["speed"] | 0;
            tmp.pts[tmp.count].speed = (uint8_t)(sp < 0 ? 0 : (sp > 100 ? 100 : sp));
            tmp.count++;
        }
        if (!curveValidate(tmp)) {
            LOGW("ha: curve/set rejected (need >= 2 valid points)");
            return;
        }
        {
            ConfigGuard guard;
            cfg().fan.curve = tmp;
            configValidate(cfg());
        }
        configSave();          // an explicit curve change is worth a real write
        fanControlReconfigure();
    } else if (suffix == "chamber_target" || suffix == "cooldown_target" ||
               suffix == "chamber_target/set" || suffix == "cooldown_target/set" ||
               suffix == "target/set") {
        // Two shapes, because Home Assistant number entities want one topic per
        // value while a script would rather send both at once.
        int chamber = -1, cooldown = -1;
        if (suffix == "target/set") {
            if (!payload || length == 0 || length > kMaxCommandBytes) {
                LOGW("ha: target/set payload missing or too large (%u bytes)", (unsigned)length);
                return;
            }
            JsonDocument doc;
            if (deserializeJson(doc, (const char*)payload, (size_t)length)) {
                LOGW("ha: target/set payload is not valid json");
                return;
            }
            if (doc["chamberTarget"].is<float>())  chamber = (int)doc["chamberTarget"].as<double>();
            if (doc["cooldownTarget"].is<float>()) cooldown = (int)doc["cooldownTarget"].as<double>();
        } else if (suffix.startsWith("chamber_target")) {
            chamber = (int)strtof(body, nullptr);
        } else {
            cooldown = (int)strtof(body, nullptr);
        }
        if (chamber < 0 && cooldown < 0) {
            LOGW("ha: target command carried no usable value");
            return;
        }
        {
            ConfigGuard guard;
            // configValidate() clamps to 20..80 / 15..60, so a slider that
            // overshoots is corrected rather than rejected.
            if (chamber >= 0)  cfg().fan.chamberTarget = (uint8_t)(chamber > 255 ? 255 : chamber);
            if (cooldown >= 0) cfg().fan.cooldownTarget = (uint8_t)(cooldown > 255 ? 255 : cooldown);
        }
        applyAndPersist();
    } else if (suffix == "cooldown/set") {
        const bool on = strcasecmp(body, "OFF") != 0 && strcmp(body, "0") != 0;
        const char* err = "printer is busy";
        if (!cooldownRequest(on, &err)) {
            LOGW("ha: cooldown/set refused (%s)", err);
            return;
        }
    } else if (suffix == "restart") {
        LOGW("ha: restart requested over mqtt");
        appRequestRestart(500);
        return;
    } else {
        return;
    }

    publishState();
}

void subscribeAll()
{
    g_mqtt.subscribe(topic("fan/set").c_str());
    g_mqtt.subscribe(topic("fan/on").c_str());
    g_mqtt.subscribe(topic("mode/set").c_str());
    g_mqtt.subscribe(topic("curve/set").c_str());
    g_mqtt.subscribe(topic("chamber_target/set").c_str());
    g_mqtt.subscribe(topic("cooldown_target/set").c_str());
    g_mqtt.subscribe(topic("target/set").c_str());
    g_mqtt.subscribe(topic("cooldown/set").c_str());
    g_mqtt.subscribe(topic("restart").c_str());
}

// Resolves mqtt.host to an address and points the client at it. PubSubClient's
// setServer(const char*) resolves inside connect(), which means a DNS timeout
// blocks the loop on every attempt; doing it here keeps the failure path to one
// lookup per (re)configure.
bool resolveHost()
{
    char host[sizeof(cfg().mqtt.host)];
    uint16_t port;
    {
        ConfigGuard guard;
        strlcpy(host, cfg().mqtt.host, sizeof(host));
        port = cfg().mqtt.port;
    }
    if (host[0] == '\0') return false;

    IPAddress ip;
    if (!ip.fromString(host)) {           // not a literal - ask DNS
        if (!WiFi.hostByName(host, ip) || ip == IPAddress((uint32_t)0)) {
            LOGW("ha: cannot resolve '%s'", host);
            return false;
        }
        LOGI("ha: '%s' resolved to %s", host, ip.toString().c_str());
    }
    g_hostIp = ip;
    g_mqtt.setServer(g_hostIp, port);
    return true;
}

void tryConnect()
{
    char user[sizeof(cfg().mqtt.user)];
    char password[sizeof(cfg().mqtt.password)];
    uint16_t port;
    {
        ConfigGuard guard;
        strlcpy(user, cfg().mqtt.user, sizeof(user));
        strlcpy(password, cfg().mqtt.password, sizeof(password));
        port = cfg().mqtt.port;
    }

    g_nextAttemptMs = millis() + g_reconnectMs;

    // A stale address survives a broker that moved; re-resolve after a run of
    // failures rather than retrying a dead IP until the next reboot.
    if (g_hostIp == IPAddress((uint32_t)0) ||
        (g_failures > 0 && g_failures % kReresolveAfterFailures == 0)) {
        if (!resolveHost()) {
            g_failures++;
            g_reconnectMs = g_reconnectMs < kReconnectMaxMs ? g_reconnectMs * 2 : kReconnectMaxMs;
            if (g_reconnectMs > kReconnectMaxMs) g_reconnectMs = kReconnectMaxMs;
            return;
        }
    }

    const bool ok = user[0]
        ? g_mqtt.connect(g_clientId, user, password, g_availability.c_str(), 0, true, "offline")
        : g_mqtt.connect(g_clientId, g_availability.c_str(), 0, true, "offline");
    if (!ok) {
        g_failures++;
        LOGW("ha: broker connect failed (state %d), retry in %u s",
             g_mqtt.state(), (unsigned)(g_reconnectMs / 1000));
        g_reconnectMs = g_reconnectMs < kReconnectMaxMs ? g_reconnectMs * 2 : kReconnectMaxMs;
        if (g_reconnectMs > kReconnectMaxMs) g_reconnectMs = kReconnectMaxMs;
        return;
    }
    g_failures = 0;
    g_reconnectMs = kReconnectMinMs;
    LOGI("ha: connected to %s:%u as %s", g_hostIp.toString().c_str(), (unsigned)port, g_base.c_str());
    publishChecked(g_availability, "online", true);
    subscribeAll();
}

// Tears the client down and re-reads the mqtt section. Runs on the loop task
// only - see haMqttReconfigure().
void applyReconfigure()
{
    if (g_mqtt.connected()) {
        g_mqtt.publish(g_availability.c_str(), "offline", true);
        g_mqtt.disconnect();
    }
    g_net.stop();
    g_discoveryPublished = false;
    g_hostIp = IPAddress((uint32_t)0);
    g_failures = 0;
    g_reconnectMs = kReconnectMinMs;

    {
        ConfigGuard guard;
        const Config& c = cfg();
        g_enabled = c.mqtt.enabled && c.mqtt.host[0] != '\0';
        g_base = mqttBaseTopic();
    }
    g_availability = topic("availability");
    if (!g_enabled) {
        LOGI("ha: external mqtt disabled");
        return;
    }
    // Only worth a DNS query once there is a network to ask over; otherwise
    // tryConnect() picks it up when the station comes up.
    if (wifiConnected()) resolveHost();
    g_nextAttemptMs = millis();     // connect on the next loop pass
    LOGI("ha: external mqtt enabled, base topic '%s'", g_base.c_str());
}

}  // namespace

void haMqttSetup()
{
    snprintf(g_clientId, sizeof(g_clientId), "BLSF-%s", chipId());
    g_mqtt.setCallback(onCommand);
    // Keep broker I/O from stalling the loop; the reconnect cadence does the rest.
    // NetworkClient::setTimeout() is milliseconds on Arduino-ESP32 3.x (TCP connect
    // select), PubSubClient::setSocketTimeout() is seconds (MQTT CONNACK wait).
    // setConnectionTimeout() bounds the TCP connect in milliseconds; keep it
    // short, because it runs on the loop task and an unreachable broker would
    // otherwise stall fan control and the UI for its whole duration.
    // setSocketTimeout() is PubSubClient's CONNACK/read wait, in seconds.
    g_net.setConnectionTimeout(500);
    g_mqtt.setSocketTimeout(1);
    g_mqtt.setKeepAlive(30);
    g_mqtt.setBufferSize(2048);
    haMqttReconfigure();
}

void haMqttReconfigure()
{
    // Called from the web handler, the serial reader and the MQTT callback -
    // none of which may tear a socket down under the loop task that is using it.
    g_reconfigure = true;
}

void haMqttLoop()
{
    if (g_reconfigure) {
        g_reconfigure = false;
        applyReconfigure();
    }
    if (!g_enabled || !wifiConnected()) return;

    const uint32_t now = millis();
    if (!g_mqtt.connected()) {
        if ((int32_t)(now - g_nextAttemptMs) < 0) return;
        tryConnect();
        return;
    }

    g_mqtt.loop();

    bool haDiscovery;
    uint16_t publishIntervalSec;
    {
        ConfigGuard guard;
        haDiscovery = cfg().mqtt.haDiscovery;
        publishIntervalSec = cfg().mqtt.publishIntervalSec;
    }

    if (haDiscovery && !g_discoveryPublished) {
        publishDiscoveryAll(false);
        g_discoveryPublished = true;
        publishState();
        return;
    }
    if (!haDiscovery && g_discoveryPublished) {
        publishDiscoveryAll(true);      // empty payloads remove the entities
        g_discoveryPublished = false;
    }

    const FanState f = fanSnapshot();
    const int out = (int)lroundf(f.output);
    const bool due = (now - g_lastPublishMs) >= (uint32_t)publishIntervalSec * 1000UL;
    if (due || out != g_lastOutput) publishState();
}

bool haMqttConnected() { return g_enabled && g_mqtt.connected(); }

}  // namespace blsf
