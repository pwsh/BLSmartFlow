#include "serial_provision.h"

#include <ArduinoJson.h>
#include <WiFi.h>

#include "app.h"
#include "config.h"
#include "log.h"
#include "state.h"
#include "version.h"

namespace blsf {

namespace {

// One line of input. Long enough for a full config document, small enough that
// a runaway sender cannot exhaust the heap.
const size_t kMaxLine = 2048;
String g_line;

void reply(bool ok, const char* msg)
{
    JsonDocument doc;
    doc["ok"] = ok;
    if (msg) doc[ok ? "msg" : "error"] = msg;
    serializeJson(doc, Serial);
    Serial.println();
}

void sendStatus()
{
    const PrinterState p = printerSnapshot();
    const FanState f = fanSnapshot();
    JsonDocument doc;
    doc["fw"] = FW_VERSION;
    doc["chipId"] = chipId();
    {
        ConfigGuard guard;
        doc["hostname"] = cfg().wifi.hostname;
        doc["ssid"] = cfg().wifi.ssid;
    }
    doc["wifi"] = WiFi.status() == WL_CONNECTED;
    doc["ip"] = WiFi.localIP().toString();
    doc["printerConnected"] = p.connected;
    doc["fanOutput"] = (int)lroundf(f.output);
    serializeJson(doc, Serial);
    Serial.println();
}

void handleLegacyKeys(JsonObjectConst doc, bool& changed)
{
    ConfigGuard guard;
    Config& c = cfg();
    // Every read is defaulted, so a message that omits a key leaves it alone.
    const char* ssid = doc["ssid"] | "";
    const char* pass = doc["pass"] | "";
    const char* ip = doc["printerip"] | "";
    const char* code = doc["printercode"] | "";
    const char* ser = doc["printerserial"] | "";

    if (*ssid) { strlcpy(c.wifi.ssid, ssid, sizeof(c.wifi.ssid)); changed = true; }
    if (*pass) { strlcpy(c.wifi.password, pass, sizeof(c.wifi.password)); changed = true; }
    if (*ip)   { strlcpy(c.printer.ip, ip, sizeof(c.printer.ip)); changed = true; }
    if (*code) { strlcpy(c.printer.accessCode, code, sizeof(c.printer.accessCode)); changed = true; }
    if (*ser)  { strlcpy(c.printer.serial, ser, sizeof(c.printer.serial)); changed = true; }
}

void handleLine(const String& line)
{
    if (line.length() == 0) return;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, line);
    if (err) {
        reply(false, "invalid json");
        return;
    }
    JsonObjectConst root = doc.as<JsonObjectConst>();
    if (root.isNull()) {
        reply(false, "expected a json object");
        return;
    }

    const char* cmd = root["cmd"] | (const char*)nullptr;
    if (cmd) {
        if (strcmp(cmd, "status") == 0) { sendStatus(); return; }
        if (strcmp(cmd, "restart") == 0) { reply(true, "restarting"); appRequestRestart(500); return; }
        if (strcmp(cmd, "factoryreset") == 0) { reply(true, "resetting"); appRequestFactoryReset(); return; }
        reply(false, "unknown cmd");
        return;
    }

    bool changed = false;
    if (root["config"].is<JsonObjectConst>()) {
        ConfigChange change;
        {
            ConfigGuard guard;
            configFromJson(root["config"], cfg(), change);
        }
        changed = true;
        appApplyConfig(change.printerChanged, change.mqttChanged, change.fanChanged, change.ssdpChanged);
    } else {
        handleLegacyKeys(root, changed);
        if (changed) {
            ConfigGuard guard;
            configValidate(cfg());
        }
    }

    if (!changed) {
        reply(false, "nothing to apply");
        return;
    }
    if (!configSave()) {
        reply(false, "save failed");
        return;
    }
    reply(true, "saved, restarting");
    // Credentials only take effect on a fresh WiFi bring-up.
    appRequestRestart(1000);
}

}  // namespace

void serialProvisionSetup()
{
    g_line.reserve(128);
}

void serialProvisionLoop()
{
    // Bounded work per pass: never spin here waiting for a terminator.
    int budget = 256;
    while (Serial.available() > 0 && budget-- > 0) {
        const char ch = (char)Serial.read();
        if (ch == '\r') continue;
        if (ch == '\n') {
            String line = g_line;
            g_line = "";
            line.trim();
            handleLine(line);
            continue;
        }
        if (g_line.length() >= kMaxLine) {
            LOGW("serial: line too long, discarded");
            g_line = "";
            continue;
        }
        g_line += ch;
    }
}

}  // namespace blsf
