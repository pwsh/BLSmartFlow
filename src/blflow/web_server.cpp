#include "web_server.h"

#include <ArduinoJson.h>
#include <AsyncJson.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <Update.h>
#include <esp_ota_ops.h>
#include <WiFi.h>
#include <math.h>

#include "app.h"
#include "config.h"
#include "curve.h"
#include "fan_control.h"
#include "log.h"
#include "printer_link.h"
#include "state.h"
#include "status.h"
#include "version.h"
#include "wifi_manager.h"
#include "ssdp.h"

// Generated at build time by pre_build.py from everything in src/www/.
#include "../www/www.h"

namespace blsf {

namespace {

AsyncWebServer   g_server(80);
AsyncEventSource g_events("/api/events");

const size_t kMaxJsonBody = 4096;
// Concurrent SSE subscribers. Four covers a phone, a desktop and a spare tab.
const size_t kMaxEventClients = 4;
uint32_t g_lastStatusPushMs = 0;
uint32_t g_logCursor = 0;

// --- helpers ---------------------------------------------------------------

// True when this request came in over the setup access point rather than the
// station interface. The device runs AP+STA while it retries, so "AP mode is up"
// says nothing about where a given request arrived: checking the socket's local
// address is what distinguishes the phone on the open portal from the whole rest
// of the LAN, which must still authenticate.
bool onApInterface(AsyncWebServerRequest* request)
{
    if (!wifiIsApMode()) return false;
    AsyncClient* client = request->client();
    if (!client) return false;
    return client->localIP() == WiFi.softAPIP();
}

// Same check as authorised() but without issuing a challenge - for callbacks
// that fire many times per request (upload chunks).
bool authorisedQuiet(AsyncWebServerRequest* request)
{
    ConfigGuard guard;
    const Config& c = cfg();
    if (!c.web.authEnabled || c.web.password[0] == '\0') return true;
    // Auth is skipped only on the setup AP: the user is there precisely because
    // they cannot reach the device normally, and locking them out would be a
    // dead end. On the station interface auth always applies.
    if (onApInterface(request)) return true;
    return request->authenticate(c.web.user, c.web.password);
}

bool authorised(AsyncWebServerRequest* request)
{
    if (authorisedQuiet(request)) return true;
    request->requestAuthentication(AsyncAuthType::AUTH_BASIC, FW_NAME, nullptr);
    return false;
}

// esp_reset_reason_t as text; the numeric code means nothing to a user reading
// the System page.
const char* resetReasonText(esp_reset_reason_t reason)
{
    switch (reason) {
        case ESP_RST_POWERON:   return "POWERON";
        case ESP_RST_EXT:       return "EXT";
        case ESP_RST_SW:        return "SW";
        case ESP_RST_PANIC:     return "PANIC";
        case ESP_RST_INT_WDT:   return "INT_WDT";
        case ESP_RST_TASK_WDT:  return "TASK_WDT";
        case ESP_RST_WDT:       return "WDT";
        case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
        case ESP_RST_BROWNOUT:  return "BROWNOUT";
        case ESP_RST_SDIO:      return "SDIO";
        case ESP_RST_UNKNOWN:
        default:                return "UNKNOWN";
    }
}

void sendError(AsyncWebServerRequest* request, int code, const char* message)
{
    JsonDocument doc;
    doc["error"] = message;
    String body;
    serializeJson(doc, body);
    request->send(code, "application/json", body);
}

void sendJson(AsyncWebServerRequest* request, JsonDocument& doc, int code = 200)
{
    String body;
    serializeJson(doc, body);
    request->send(code, "application/json", body);
}

void sendOk(AsyncWebServerRequest* request)
{
    request->send(200, "application/json", "{\"ok\":true}");
}

// Applies everything that can change without a reboot and persists the result.
bool saveAndApply(const ConfigChange& change)
{
    appApplyConfig(change.printerChanged, change.mqttChanged, change.fanChanged, change.ssdpChanged);
    return configSave();
}

// --- static UI -------------------------------------------------------------

void handleIndex(AsyncWebServerRequest* request)
{
    if (!authorised(request)) return;
    AsyncWebServerResponse* r =
        request->beginResponse(200, index_html_gz_mime, index_html_gz, index_html_gz_len);
    r->addHeader("Content-Encoding", "gzip");
    // The UI is versioned with the firmware, so never let a proxy pin an old one.
    r->addHeader("Cache-Control", "no-cache");
    request->send(r);
}

// --- /api/status, /api/info, /api/log --------------------------------------

void handleStatus(AsyncWebServerRequest* request)
{
    if (!authorised(request)) return;
    JsonDocument doc;
    buildStatus(doc.to<JsonObject>());
    sendJson(request, doc);
}

void handleInfo(AsyncWebServerRequest* request)
{
    if (!authorised(request)) return;
    JsonDocument doc;
    doc["fw"] = FW_VERSION;
    doc["build"] = FW_BUILD;
    doc["chipId"] = chipId();
    doc["sdk"] = ESP.getSdkVersion();
    doc["flashSize"] = ESP.getFlashChipSize();
    doc["sketchSize"] = ESP.getSketchSize();
    doc["freeSketchSpace"] = ESP.getFreeSketchSpace();
    const esp_partition_t* p = esp_ota_get_running_partition();
    doc["partition"] = p ? p->label : "?";
    doc["resetReason"] = resetReasonText(esp_reset_reason());
    sendJson(request, doc);
}

void handleLog(AsyncWebServerRequest* request)
{
    if (!authorised(request)) return;
    // 64 lines x 120 bytes = 7.5 kB on the stack of the AsyncTCP task, which has
    // CONFIG_ASYNC_TCP_STACK_SIZE (4 kB) - so take it from the heap instead.
    char* buf = (char*)malloc((size_t)LOG_LINES * LOG_LINE_LEN);
    if (!buf) { sendError(request, 500, "out of memory"); return; }
    const uint8_t n = logSnapshot(buf, LOG_LINES);
    JsonDocument doc;
    JsonArray lines = doc["lines"].to<JsonArray>();
    for (uint8_t i = 0; i < n; i++) lines.add((const char*)(buf + (size_t)i * LOG_LINE_LEN));
    String body;
    serializeJson(doc, body);
    free(buf);
    request->send(200, "application/json", body);
}

// --- /api/config -----------------------------------------------------------

void handleGetConfig(AsyncWebServerRequest* request)
{
    if (!authorised(request)) return;
    JsonDocument doc;
    {
        ConfigGuard guard;
        configToJson(doc.to<JsonObject>(), cfg(), /*masked=*/true);
    }
    sendJson(request, doc);
}

// A value made only of '*' is the UI echoing back a masked secret and means
// "leave it alone"; it is never length-checked.
bool isMaskedValue(const char* s)
{
    if (!s || !*s) return false;
    for (const char* p = s; *p; ++p) if (*p != '*') return false;
    return true;
}

void handlePostConfig(AsyncWebServerRequest* request, JsonVariant& json)
{
    if (!authorised(request)) return;
    JsonObjectConst in = json.as<JsonObjectConst>();
    if (in.isNull()) { sendError(request, 400, "expected a json object"); return; }

    // Reject a wrong-length access code instead of letting validate() clear it:
    // silently dropping what the user typed looks like the save did not happen.
    // validate() keeps its clamp for hand-edited files that never come through here.
    JsonObjectConst pr = in["printer"];
    if (!pr.isNull() && pr["accessCode"].is<const char*>()) {
        const char* code = pr["accessCode"].as<const char*>();
        if (code && *code && !isMaskedValue(code) && strlen(code) != 8) {
            sendError(request, 400, "access code must be exactly 8 characters");
            return;
        }
    }

    ConfigChange change;
    JsonDocument doc;
    JsonObject root = doc.to<JsonObject>();
    {
        ConfigGuard guard;
        configFromJson(in, cfg(), change);
        root["ok"] = true;
        root["restartRequired"] = change.restartRequired;
        configToJson(root["config"].to<JsonObject>(), cfg(), /*masked=*/true);
    }
    if (!saveAndApply(change)) { sendError(request, 500, "could not save configuration"); return; }
    sendJson(request, doc);
}

// --- /api/curve ------------------------------------------------------------

void handleGetCurve(AsyncWebServerRequest* request)
{
    if (!authorised(request)) return;
    JsonDocument doc;
    {
        ConfigGuard guard;
        JsonArray pts = doc["points"].to<JsonArray>();
        const FanCurve& c = cfg().fan.curve;
        for (uint8_t i = 0; i < c.count; i++) {
            JsonObject o = pts.add<JsonObject>();
            o["temp"] = c.pts[i].temp;
            o["speed"] = c.pts[i].speed;
        }
        doc["source"] = cfg().fan.source;
    }
    sendJson(request, doc);
}

// Reads {"points":[{"temp":..,"speed":..}, ...]} into `out`. Returns an error
// string on failure, or nullptr on success.
const char* parsePoints(JsonArrayConst arr, FanCurve& out)
{
    if (arr.isNull()) return "missing 'points' array";
    FanCurve tmp{};
    tmp.count = 0;
    for (JsonObjectConst o : arr) {
        if (tmp.count >= CURVE_MAX_POINTS) return "too many points (max 16)";
        if (!o["temp"].is<float>() || !o["speed"].is<float>()) return "each point needs numeric temp and speed";
        tmp.pts[tmp.count].temp = o["temp"].as<float>();
        int sp = (int)o["speed"].as<double>();
        tmp.pts[tmp.count].speed = (uint8_t)(sp < 0 ? 0 : (sp > 100 ? 100 : sp));
        tmp.count++;
    }
    if (!curveValidate(tmp)) return "need at least 2 points with distinct temperatures";
    out = tmp;
    return nullptr;
}

void handlePutCurve(AsyncWebServerRequest* request, JsonVariant& json)
{
    if (!authorised(request)) return;
    FanCurve curve{};
    const char* err = parsePoints(json["points"].as<JsonArrayConst>(), curve);
    if (err) { sendError(request, 400, err); return; }

    JsonDocument doc;
    doc["ok"] = true;
    {
        ConfigGuard guard;
        cfg().fan.curve = curve;
        configValidate(cfg());
        JsonArray pts = doc["points"].to<JsonArray>();
        for (uint8_t i = 0; i < cfg().fan.curve.count; i++) {
            JsonObject o = pts.add<JsonObject>();
            o["temp"] = cfg().fan.curve.pts[i].temp;
            o["speed"] = cfg().fan.curve.pts[i].speed;
        }
    }
    fanControlReconfigure();
    // An explicit curve edit is a deliberate act: save it now, not in 10 s.
    if (!configSave()) { sendError(request, 500, "could not save configuration"); return; }
    sendJson(request, doc);
}

// --- /api/fan --------------------------------------------------------------

void handlePostFan(AsyncWebServerRequest* request, JsonVariant& json)
{
    if (!authorised(request)) return;
    JsonObjectConst in = json.as<JsonObjectConst>();
    if (in.isNull()) { sendError(request, 400, "expected a json object"); return; }

    const char* mode = in["mode"] | (const char*)nullptr;
    int speed = -1;
    if (in["speed"].is<float>()) {
        speed = (int)in["speed"].as<double>();
        // Out of range is a caller bug; clamping it silently would hide that.
        if (speed < 0 || speed > 100) { sendError(request, 400, "speed must be 0..100"); return; }
    }
    const uint32_t duration = in["durationSec"] | 0U;

    bool persist = true;
    if (!fanApplyMode(mode, speed, duration, persist)) {
        sendError(request, 400, "mode must be auto, chamber, manual or off");
        return;
    }
    // Never save inline: the dashboard slider posts here on every drag, and a
    // LittleFS write per step would both stall the handler and wear the flash.
    if (persist) configMarkDirty();

    JsonDocument doc;
    JsonObject root = doc.to<JsonObject>();
    root["ok"] = true;
    JsonDocument st;
    buildStatus(st.to<JsonObject>());
    root["fan"] = st["fan"];
    sendJson(request, doc);
}

// --- /api/restart, /api/factoryreset ---------------------------------------

void handleRestart(AsyncWebServerRequest* request)
{
    if (!authorised(request)) return;
    sendOk(request);
    appRequestRestart(500);
}

void handleFactoryReset(AsyncWebServerRequest* request, JsonVariant& json)
{
    if (!authorised(request)) return;
    if (!(json["confirm"] | false)) { sendError(request, 400, "send {\"confirm\":true}"); return; }
    sendOk(request);
    appRequestFactoryReset();
}

// --- /api/wifi -------------------------------------------------------------

void handleWifiScan(AsyncWebServerRequest* request)
{
    if (!authorised(request)) return;
    // ?force=1 throws the cache away: the user pressed "rescan" because the
    // list was wrong, so handing them the same 20-second-old list is no answer.
    const bool force = request->hasParam("force") &&
                       request->getParam("force")->value() != "0";
    ScanState st = force ? wifiScanStart(true) : wifiScanState();
    if (!force && st == ScanState::Idle) st = wifiScanStart();
    if (st == ScanState::Running) {
        request->send(202, "application/json", "{\"scanning\":true}");
        return;
    }
    JsonDocument doc;
    wifiScanResults(doc["networks"].to<JsonArray>());
    sendJson(request, doc);
}

void handleWifiPost(AsyncWebServerRequest* request, JsonVariant& json)
{
    if (!authorised(request)) return;
    JsonObjectConst in = json.as<JsonObjectConst>();
    if (in.isNull()) { sendError(request, 400, "expected a json object"); return; }
    if (!in["ssid"].is<const char*>() || strlen(in["ssid"].as<const char*>()) == 0) {
        sendError(request, 400, "ssid is required");
        return;
    }
    // Reuse the config merge so masking and validation behave identically.
    JsonDocument wrapper;
    wrapper["wifi"] = in;
    ConfigChange change;
    {
        ConfigGuard guard;
        configFromJson(wrapper.as<JsonObjectConst>(), cfg(), change);
    }
    if (!configSave()) { sendError(request, 500, "could not save configuration"); return; }

    JsonDocument doc;
    doc["ok"] = true;
    doc["restartRequired"] = true;
    sendJson(request, doc);
    // In AP mode the user is waiting for the device to join their network.
    appRequestRestart(wifiIsApMode() ? 1000 : 1500);
}

// --- backup / restore ------------------------------------------------------

void handleBackup(AsyncWebServerRequest* request)
{
    if (!authorised(request)) return;
    JsonDocument doc;
    {
        ConfigGuard guard;
        configToJson(doc.to<JsonObject>(), cfg(), /*masked=*/false);
    }
    String body;
    serializeJson(doc, body);
    AsyncWebServerResponse* r = request->beginResponse(200, "application/json", body);
    char disp[96];
    snprintf(disp, sizeof(disp), "attachment; filename=\"blsmartflow-%s.json\"", chipId());
    r->addHeader("Content-Disposition", disp);
    request->send(r);
}

void handleRestore(AsyncWebServerRequest* request, JsonVariant& json)
{
    if (!authorised(request)) return;
    JsonObjectConst in = json.as<JsonObjectConst>();
    if (in.isNull()) { sendError(request, 400, "expected a json object"); return; }

    ConfigGuard guard;

    // Start from defaults so a partial backup cannot leave stale values behind -
    // but seed the secrets from what is stored. A backup taken from the UI (or
    // hand-edited from GET /api/config) carries "********" for every secret;
    // starting those from blank defaults would silently wipe the WiFi password
    // and the access code, turning a restore into a lockout. configFromJson()
    // treats an all-'*' value as "keep", which now keeps the real value.
    Config restored;
    configDefaults(restored);
    const Config& current = cfg();
    strlcpy(restored.wifi.password, current.wifi.password, sizeof(restored.wifi.password));
    strlcpy(restored.printer.accessCode, current.printer.accessCode, sizeof(restored.printer.accessCode));
    strlcpy(restored.mqtt.password, current.mqtt.password, sizeof(restored.mqtt.password));
    strlcpy(restored.web.password, current.web.password, sizeof(restored.web.password));

    ConfigChange change;
    configFromJson(in, restored, change);

    // Without an SSID the device comes back up in AP mode, which is never what
    // someone restoring a backup wanted. Refuse rather than strand it.
    if (restored.wifi.ssid[0] == '\0') {
        sendError(request, 400, "backup has no wifi.ssid");
        return;
    }

    cfg() = restored;
    if (!configSave()) { sendError(request, 500, "could not save configuration"); return; }
    sendOk(request);
    appRequestRestart(500);
}

// --- OTA -------------------------------------------------------------------

// Set by the upload callback when a chunk fails, so the response can say what
// went wrong even though Update has already been aborted (which clears its own
// error). Only one OTA can be in flight at a time, so a single slot is enough.
char g_otaError[80] = {0};

// Aborts a running update and records why. Aborting is what releases the OTA
// partition; without it a failed upload leaves Update "running" and every
// subsequent attempt fails at begin() until the device is rebooted.
void otaFail(const char* what)
{
    if (g_otaError[0] == '\0') {
        snprintf(g_otaError, sizeof(g_otaError), "%s: %s", what, Update.errorString());
    }
    LOGE("ota: %s", g_otaError);
    if (Update.isRunning()) Update.abort();
}

void handleUpdateDone(AsyncWebServerRequest* request)
{
    if (!authorised(request)) return;
    if (g_otaError[0] != '\0' || Update.hasError()) {
        char msg[112];
        snprintf(msg, sizeof(msg), "update failed: %s",
                 g_otaError[0] ? g_otaError : Update.errorString());
        g_otaError[0] = '\0';
        sendError(request, 500, msg);
        return;
    }
    sendOk(request);
    appRequestRestart(1000);
}

void handleUpdateUpload(AsyncWebServerRequest* request, const String& filename, size_t index,
                        uint8_t* data, size_t len, bool final)
{
    // Silent: the paired POST handler issues the 401 challenge exactly once.
    if (!authorisedQuiet(request)) return;

    if (index == 0) {
        LOGW("ota: receiving %s", filename.c_str());
        g_otaError[0] = '\0';
        // A previous upload that died mid-flight (browser closed, connection
        // dropped) leaves the flash writer armed and begin() would refuse.
        if (Update.isRunning()) {
            LOGW("ota: aborting a previous unfinished update");
            Update.abort();
        }
        // A client that vanishes half-way through must not leave the OTA
        // partition claimed for the rest of this boot.
        request->onDisconnect([]() {
            if (Update.isRunning()) {
                LOGW("ota: client disconnected, update aborted");
                Update.abort();
            }
        });
        // UPDATE_SIZE_UNKNOWN: the browser does not tell us the image size up front.
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
            otaFail("begin failed");
            return;
        }
    }
    // Once a chunk has failed the update is gone; swallow the rest of the upload
    // quietly rather than logging a line per remaining chunk.
    if (g_otaError[0] != '\0') return;

    // A short write means the flash is full or failing; carrying on would build
    // a truncated image and then boot it.
    if (len && Update.write(data, len) != len) {
        otaFail("write failed");
        return;
    }
    if (final) {
        if (Update.end(true)) {
            LOGI("ota: %u bytes written, restarting", (unsigned)(index + len));
        } else {
            otaFail("end failed");
        }
    }
}

// --- legacy 1.x routes -----------------------------------------------------

// Masks all but the last three characters, matching the 1.x UI's expectation.
// The 1.x version leaked a `new char[]` on every request; this one does not.
String obfuscate(const char* s)
{
    String out(s);
    const int len = (int)out.length();
    if (len > 3) for (int i = 0; i < len - 3; i++) out.setCharAt(i, '*');
    return out;
}

void handleLegacyGetOptions(AsyncWebServerRequest* request)
{
    if (!authorised(request)) return;
    ConfigGuard guard;
    const Config& c = cfg();
    JsonDocument doc;
    doc["firmwareversion"] = FW_VERSION;
    doc["ip"] = c.printer.ip;
    doc["code"] = obfuscate(c.printer.accessCode);
    doc["id"] = obfuscate(c.printer.serial);
    doc["staticfans"] = strcmp(c.fan.mode, "manual") == 0;
    doc["staticfanspeed"] = c.fan.manualSpeed;
    doc["debuging"] = c.debug.serial;
    doc["debugingchange"] = false;   // 1.x-only flag, no 2.0 equivalent
    doc["mqttdebug"] = c.debug.mqttDump;
    sendJson(request, doc);
}

String formArg(AsyncWebServerRequest* request, const char* name)
{
    // 1.x posted url-encoded forms; accept the value from either body or query.
    if (request->hasParam(name, true)) return request->getParam(name, true)->value();
    if (request->hasParam(name)) return request->getParam(name)->value();
    return String();
}

void handleLegacySubmitOptions(AsyncWebServerRequest* request)
{
    if (!authorised(request)) return;
    ConfigGuard guard;
    Config& c = cfg();

    const String ip = formArg(request, "ip");
    const String code = formArg(request, "code");
    const String serial = formArg(request, "serial");
    // The 1.x UI echoes back the obfuscated value; '*' means "unchanged".
    if (ip.length() && ip.indexOf('*') < 0) strlcpy(c.printer.ip, ip.c_str(), sizeof(c.printer.ip));
    if (code.length() && code.indexOf('*') < 0) strlcpy(c.printer.accessCode, code.c_str(), sizeof(c.printer.accessCode));
    if (serial.length() && serial.indexOf('*') < 0) strlcpy(c.printer.serial, serial.c_str(), sizeof(c.printer.serial));

    const bool staticFan = formArg(request, "staticfan") == "on";
    strlcpy(c.fan.mode, staticFan ? "manual" : "auto", sizeof(c.fan.mode));
    const String sfs = formArg(request, "staticfanspeed");
    if (sfs.length()) c.fan.manualSpeed = (uint8_t)constrain(sfs.toInt(), 0, 100);

    c.debug.serial = formArg(request, "debuging") == "on" || formArg(request, "debugingchange") == "on";
    c.debug.mqttDump = formArg(request, "mqttdebug") == "on";

    configValidate(c);
    logSetSerialEnabled(c.debug.serial);
    printerLinkReconfigure();
    fanControlReconfigure();
    if (!configSave()) { sendError(request, 500, "could not save configuration"); return; }
    sendOk(request);
}

void handleLegacyGetFanConfig(AsyncWebServerRequest* request)
{
    if (!authorised(request)) return;
    handleGetCurve(request);
}

void handleLegacyUpdateFanConfig(AsyncWebServerRequest* request)
{
    if (!authorised(request)) return;
    const String points = formArg(request, "points");
    if (points.isEmpty()) { sendError(request, 400, "missing 'points' parameter"); return; }

    JsonDocument doc;
    if (deserializeJson(doc, points)) { sendError(request, 400, "'points' is not valid json"); return; }
    // 1.x sent either {"points":[...]} or a bare array.
    JsonArrayConst arr = doc["points"].is<JsonArrayConst>() ? doc["points"].as<JsonArrayConst>()
                                                            : doc.as<JsonArrayConst>();
    FanCurve curve{};
    const char* err = parsePoints(arr, curve);
    if (err) { sendError(request, 400, err); return; }

    {
        ConfigGuard guard;
        cfg().fan.curve = curve;
        configValidate(cfg());
    }
    fanControlReconfigure();
    if (!configSave()) { sendError(request, 500, "could not save configuration"); return; }
    sendOk(request);
}

void handleLegacySensorData(AsyncWebServerRequest* request)
{
    if (!authorised(request)) return;
    // 1.x always reported the nozzle here, whatever the curve was following, and
    // 0 when it did not know - existing dashboards parse it that way.
    const PrinterState p = printerSnapshot();
    const FanState f = fanSnapshot();
    JsonDocument doc;
    doc["temp"] = isnan(p.nozzle) ? 0.0f : roundf(p.nozzle * 100.0f) / 100.0f;
    doc["speed"] = (int)lroundf(f.output);
    sendJson(request, doc);
}

// --- captive portal --------------------------------------------------------

// A captive-portal probe (or any stray request) from a client on the setup AP
// is answered with a redirect to the portal. That redirect is what makes the
// phone/laptop show its "sign in to network" prompt. Only clients on the AP
// interface are redirected: in AP+STA mode (WiFi outage with saved credentials)
// a genuine 404 on the LAN side must stay a 404.
bool wantsPortal(AsyncWebServerRequest* request)
{
    return onApInterface(request);
}

void redirectToPortal(AsyncWebServerRequest* request)
{
    AsyncWebServerResponse* r = request->beginResponse(302, "text/plain", "");
    r->addHeader("Location", "http://" + WiFi.softAPIP().toString() + "/");
    // Probe responses must never be cached, or the OS keeps believing it is
    // (or is not) behind a portal after the state changed.
    r->addHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    r->addHeader("Pragma", "no-cache");
    r->addHeader("Expires", "0");
    request->send(r);
}

void handleNotFound(AsyncWebServerRequest* request)
{
    if (request->method() == HTTP_OPTIONS) { request->send(200); return; }
    if (wantsPortal(request)) { redirectToPortal(request); return; }
    sendError(request, 404, "not found");
}

void registerJsonRoute(const char* uri, WebRequestMethodComposite method, ArJsonRequestHandlerFunction fn)
{
    auto* handler = new AsyncCallbackJsonWebHandler(uri, fn);
    handler->setMethod(method);
    handler->setMaxContentLength(kMaxJsonBody);
    g_server.addHandler(handler);
}

}  // namespace

void webServerSetup()
{
    g_server.on("/", HTTP_GET, handleIndex);
    g_server.on("/index.html", HTTP_GET, handleIndex);
    // UPnP device description advertised by SSDP (Windows "Network", xtouch).
    g_server.on("/description.xml", HTTP_GET, [](AsyncWebServerRequest* request) {
        const char* xml = ssdpSchema();
        if (!xml || !*xml) { sendError(request, 404, "ssdp disabled"); return; }
        request->send(200, "text/xml", xml);
    });

    g_server.on("/api/status", HTTP_GET, handleStatus);
    g_server.on("/api/info", HTTP_GET, handleInfo);
    g_server.on("/api/log", HTTP_GET, handleLog);
    g_server.on("/api/config", HTTP_GET, handleGetConfig);
    g_server.on("/api/curve", HTTP_GET, handleGetCurve);
    g_server.on("/api/wifi/scan", HTTP_GET, handleWifiScan);
    g_server.on("/api/backup", HTTP_GET, handleBackup);
    g_server.on("/api/restart", HTTP_POST, handleRestart);

    registerJsonRoute("/api/config", HTTP_POST, handlePostConfig);
    registerJsonRoute("/api/curve", HTTP_PUT, handlePutCurve);
    registerJsonRoute("/api/fan", HTTP_POST, handlePostFan);
    registerJsonRoute("/api/factoryreset", HTTP_POST, handleFactoryReset);
    registerJsonRoute("/api/wifi", HTTP_POST, handleWifiPost);
    registerJsonRoute("/api/restore", HTTP_POST, handleRestore);

    g_server.on("/api/update", HTTP_POST, handleUpdateDone, handleUpdateUpload);
    g_server.on("/update", HTTP_POST, handleUpdateDone, handleUpdateUpload);

    // Legacy 1.x API, kept so existing tooling and bookmarks keep working.
    g_server.on("/getOptions", HTTP_GET, handleLegacyGetOptions);
    g_server.on("/submitOptions", HTTP_POST, handleLegacySubmitOptions);
    g_server.on("/getFanConfig", HTTP_GET, handleLegacyGetFanConfig);
    g_server.on("/updateFanConfig", HTTP_POST, handleLegacyUpdateFanConfig);
    g_server.on("/sensorData", HTTP_GET, handleLegacySensorData);

    // Captive-portal probes used by iOS, Android, Windows and Linux.
    static const char* kProbes[] = {
        "/generate_204", "/gen_204", "/hotspot-detect.html", "/library/test/success.html",
        "/connecttest.txt", "/ncsi.txt", "/fwlink", "/redirect", "/success.txt", "/canonical.html",
        "/check_network_status.txt", "/chat",   // NetworkManager / GNOME, Samsung
    };
    for (const char* p : kProbes) {
        g_server.on(p, HTTP_GET, [](AsyncWebServerRequest* request) {
            if (wantsPortal(request)) redirectToPortal(request);
            else sendError(request, 404, "not found");
        });
    }

    // SSE bypasses the per-route checks, so guard it explicitly - otherwise the
    // live status stream would be readable with auth enabled.
    g_events.authorizeConnect([](AsyncWebServerRequest* request) {
        return authorisedQuiet(request);
    });
    g_events.onConnect([](AsyncEventSourceClient* client) {
        // Each SSE client holds a socket and a message queue. A page left open
        // in a dozen tabs would eat the heap and the AsyncTCP pool, so cap it -
        // count() already includes this client.
        if (g_events.count() > kMaxEventClients) {
            LOGW("sse: %u clients already connected, refusing another",
                 (unsigned)kMaxEventClients);
            client->close();
            return;
        }
        // Give a fresh client the current state immediately rather than making
        // it wait up to a second for the next tick.
        JsonDocument doc;
        buildStatus(doc.to<JsonObject>());
        String body;
        serializeJson(doc, body);
        client->send(body.c_str(), "status", millis());
    });
    g_server.addHandler(&g_events);

    g_server.onNotFound(handleNotFound);
    g_server.begin();
    LOGI("web server listening on :80");
}

void webServerLoop()
{
    if (g_events.count() == 0) {
        // Nothing is listening: keep the log cursor current so a client that
        // connects later does not receive a burst of history as "new".
        g_logCursor = logSequence();
        return;
    }

    const uint32_t now = millis();
    if ((now - g_lastStatusPushMs) >= 1000) {
        g_lastStatusPushMs = now;
        JsonDocument doc;
        buildStatus(doc.to<JsonObject>());
        String body;
        serializeJson(doc, body);
        g_events.send(body.c_str(), "status", now);
    }

    if (logSequence() != g_logCursor) {
        // Send at most a handful of lines per pass so a burst cannot monopolise
        // the loop or overflow the client's queue.
        char buf[8 * LOG_LINE_LEN];
        const uint8_t n = logSince(&g_logCursor, buf, 8);
        for (uint8_t i = 0; i < n; i++) {
            g_events.send(buf + (size_t)i * LOG_LINE_LEN, "log", now);
        }
    }
}

}  // namespace blsf
