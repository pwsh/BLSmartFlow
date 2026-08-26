#include "printer_link.h"

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <math.h>

#include "AutoGrowBufferStream.h"
#include "config.h"
#include "printer_parse.h"
#include "log.h"
#include "state.h"

namespace blsf {

namespace {

// A snapshot of the config fields the task needs, so the task never reads the
// live Config while the web server is mutating it.
struct LinkCfg {
    char     ip[64];
    char     accessCode[9];
    char     serial[17];
    char     model[6];
    uint16_t staleSec;
    bool     dump;
};

LinkCfg      g_link{};
portMUX_TYPE g_linkMux = portMUX_INITIALIZER_UNLOCKED;
volatile bool g_dirty = true;

WiFiClientSecure*    g_tls = nullptr;
PubSubClient*        g_mqtt = nullptr;
AutoGrowBufferStream g_stream;

TaskHandle_t g_task = nullptr;
char         g_clientId[24] = {0};
String       g_reportTopic;
String       g_requestTopic;
// PubSubClient::setServer(const char*, port) stores the pointer, it does not
// copy the string - so the host has to outlive every call into the client.
char         g_host[64] = {0};

// Reconnect backoff: 3 s doubling to 60 s; 60 s flat for bad credentials so we
// do not hammer the printer with a password it has already rejected.
const uint32_t kBackoffMinMs = 3000;
const uint32_t kBackoffMaxMs = 60000;
// P1/A1 push incremental diffs and need a periodic full refresh; on "auto" we do
// not know the model, so refresh half as often as a confirmed diff-pusher.
const uint32_t kPushallP1IntervalMs   = 5UL * 60UL * 1000UL;
const uint32_t kPushallAutoIntervalMs = 10UL * 60UL * 1000UL;

// --- outgoing G-code queue (REWORK-SPEC 17.2) ------------------------------
// The cool-down session runs on the loop task but MQTT belongs to this one, so
// commands are handed over through a tiny ring rather than by sharing the
// client. Four slots is deliberate: the only producer sends at most two lines
// every five seconds, and a queue deeper than that would only let a backlog of
// stale fan commands build up behind a dead link.
const uint8_t kGcodeSlots = 4;

struct GcodeSlot {
    char text[PRINTER_GCODE_MAX];
};

GcodeSlot     g_gcodeQ[kGcodeSlots];
uint8_t       g_gcodeHead = 0;      // next free slot
uint8_t       g_gcodeTail = 0;      // oldest unpublished slot
portMUX_TYPE  g_gcodeMux = portMUX_INITIALIZER_UNLOCKED;

// Sequence ids for our own requests. They start well clear of the small numbers
// Bambu Studio and the printer's own housekeeping use, so an ack that carries
// one of ours is unambiguous.
const uint32_t kGcodeSeqBase = 5000;
uint32_t      g_gcodeSeq = kGcodeSeqBase;

// The last four ids we published. `gcode_line` acks from Bambu Studio arrive on
// the same report topic and must not be mistaken for replies to us; four is one
// per queue slot, which is as many commands as can be in flight.
const uint8_t kSentIdSlots = 4;
uint32_t      g_sentIds[kSentIdSlots] = {0};
uint8_t       g_sentIdNext = 0;

void rememberSentId(uint32_t id)
{
    portENTER_CRITICAL(&g_gcodeMux);
    g_sentIds[g_sentIdNext] = id;
    g_sentIdNext = (uint8_t)((g_sentIdNext + 1) % kSentIdSlots);
    portEXIT_CRITICAL(&g_gcodeMux);
}

// True when `id` is one we published recently. Id 0 is never ours - it is the
// empty ring slot, and also the id the printer's own pushall replies carry.
bool isOurSequenceId(uint32_t id)
{
    if (id == 0) return false;
    bool ours = false;
    portENTER_CRITICAL(&g_gcodeMux);
    for (uint8_t i = 0; i < kSentIdSlots; i++) {
        if (g_sentIds[i] == id) { ours = true; break; }
    }
    portEXIT_CRITICAL(&g_gcodeMux);
    return ours;
}

uint32_t g_backoffMs = kBackoffMinMs;
uint32_t g_nextAttemptMs = 0;
uint32_t g_lastPushallMs = 0;

LinkCfg linkCfg()
{
    portENTER_CRITICAL(&g_linkMux);
    LinkCfg c = g_link;
    portEXIT_CRITICAL(&g_linkMux);
    return c;
}

bool configured(const LinkCfg& c)
{
    return c.ip[0] && c.accessCode[0] && c.serial[0];
}

void parseReport(const char* payload, size_t len, const LinkCfg& lc)
{
    JsonDocument filter;
    buildPrinterFilter(filter);

    JsonDocument doc;
    DeserializationError err =
        deserializeJson(doc, payload, len, DeserializationOption::Filter(filter));
    if (err) {
        LOGW("printer report parse error: %s", err.c_str());
        return;
    }

    // A `gcode_line` acknowledgement carries no state, so it never reaches the
    // report parser - but the *result* is the only way to learn that a printer
    // with Developer Mode off is silently refusing our M106 lines. Only acks for
    // ids we published are ours; Bambu Studio's are ignored exactly as before.
    GcodeAck ack;
    if (parseGcodeAck(doc.as<JsonVariantConst>(), ack)) {
        if (isOurSequenceId(ack.sequenceId)) {
            PrinterState st = printerBegin();
            applyGcodeAck(st, ack, millis());
            printerCommit(st);
            if (!ack.ok) {
                LOGW("printer -> refused our command: %s (err %u)",
                     ack.reason[0] ? ack.reason : "no reason given", (unsigned)ack.errCode);
            }
        }
        return;
    }

    // Read-modify-write: a report only carries the fields that changed, so the
    // previous snapshot is the starting point.
    PrinterState s = printerBegin();
    // millis() timestamps the door edges, which fan_control's resume delay needs.
    if (!parsePrinterReport(doc.as<JsonVariantConst>(), s, millis())) return;

    if (lc.dump) {
        Serial.print(F("[mqtt] "));
        serializeJson(doc, Serial);
        Serial.println();
    }

    s.lastUpdateMs = millis();
    s.everUpdated = true;
    printerCommit(s);
}

void onMessage(char* /*topic*/, byte* /*payload*/, unsigned int /*length*/)
{
    // PubSubClient was given a Stream, so the real payload is in g_stream and
    // the payload argument is empty.
    const LinkCfg lc = linkCfg();
    if (g_stream.overflowed()) {
        LOGW("printer report too large (%u bytes), dropped", (unsigned)g_stream.current_length());
    } else if (g_stream.current_length() > 0) {
        parseReport(g_stream.get_buffer(), g_stream.current_length(), lc);
    }
    g_stream.reset();
}

void publishPushall()
{
    if (!g_mqtt || !g_mqtt->connected()) return;
    static const char kPushall[] =
        "{\"pushing\":{\"sequence_id\":\"0\",\"command\":\"pushall\",\"version\":1,\"push_target\":1}}";
    if (!g_mqtt->publish(g_requestTopic.c_str(), kPushall)) {
        LOGW("pushall publish failed");
    }
    g_lastPushallMs = millis();
}

// How often to re-request a full report. X1/H2D push complete reports on their
// own, so they get none at all.
uint32_t pushallIntervalMs(const LinkCfg& c)
{
    if (strcmp(c.model, "p1") == 0 || strcmp(c.model, "a1") == 0) return kPushallP1IntervalMs;
    if (strcmp(c.model, "auto") == 0) return kPushallAutoIntervalMs;
    return 0;
}

// Publishes the oldest queued line, leaving it in place if the publish fails so
// the next tick retries it (REWORK-SPEC 17.2). Returns false when there is
// nothing to do or the publish failed.
bool publishOneGcode()
{
    char line[PRINTER_GCODE_MAX];
    portENTER_CRITICAL(&g_gcodeMux);
    const bool empty = g_gcodeHead == g_gcodeTail;
    if (!empty) memcpy(line, g_gcodeQ[g_gcodeTail].text, sizeof(line));
    portEXIT_CRITICAL(&g_gcodeMux);
    if (empty) return false;

    // Built with ArduinoJson rather than sprintf: the payload carries embedded
    // newlines, and a hand-rolled escape is exactly the kind of thing that turns
    // "M106 P2 S255" into a printer-side parse error.
    JsonDocument doc;
    JsonObject pr = doc["print"].to<JsonObject>();
    pr["sequence_id"] = String(g_gcodeSeq);
    pr["command"] = "gcode_line";
    pr["param"] = line;              // char* -> ArduinoJson copies it
    String payload;
    serializeJson(doc, payload);

    if (!g_mqtt->publish(g_requestTopic.c_str(), payload.c_str())) {
        LOGW("printer: gcode publish failed (state %d), will retry", g_mqtt->state());
        return false;
    }
    // Logged on one line: the payload carries real newlines, and a log entry
    // that breaks in the middle is unreadable in the UI's log box.
    char disp[PRINTER_GCODE_MAX];
    for (size_t i = 0; i < sizeof(disp); i++) {
        disp[i] = (line[i] == '\n' || line[i] == '\r') ? ' ' : line[i];
        if (!line[i]) break;
    }
    disp[sizeof(disp) - 1] = '\0';
    size_t end = strlen(disp);
    while (end > 0 && disp[end - 1] == ' ') disp[--end] = '\0';
    LOGI("printer <- %s", disp);
    rememberSentId(g_gcodeSeq);
    g_gcodeSeq++;

    portENTER_CRITICAL(&g_gcodeMux);
    // Only advance once the bytes are actually on the wire; the producer may
    // have appended more in the meantime, which is why tail is recomputed here.
    if (g_gcodeHead != g_gcodeTail) g_gcodeTail = (uint8_t)((g_gcodeTail + 1) % kGcodeSlots);
    portEXIT_CRITICAL(&g_gcodeMux);
    return true;
}

// Anything still queued when the link goes down is a fan command for a printer
// state we can no longer vouch for. Dropping it is the safe choice: the session
// re-asserts every 30 s anyway.
void dropQueuedGcode()
{
    portENTER_CRITICAL(&g_gcodeMux);
    g_gcodeTail = g_gcodeHead;
    portEXIT_CRITICAL(&g_gcodeMux);
}

void teardown()
{
    dropQueuedGcode();
    if (g_mqtt && g_mqtt->connected()) g_mqtt->disconnect();
    if (g_tls) g_tls->stop();
    // Hand the RX high-water buffer (up to 64 KB) back while the link is down.
    g_stream.release();
    printerSetMqtt(false, -1, mqttStateText(-1));
}

void applyConfig(const LinkCfg& c)
{
    if (!g_tls) g_tls = new WiFiClientSecure();
    if (!g_mqtt) {
        g_mqtt = new PubSubClient(*g_tls);
        g_mqtt->setStream(g_stream);
        g_mqtt->setCallback(onMessage);
    }
    // The printer presents a self-signed certificate; there is no CA to pin to.
    g_tls->setInsecure();
    // setConnectionTimeout() is the TCP connect budget in milliseconds.
    // Stream::setTimeout() - which this used to call - only bounds the blocking
    // read helpers and does nothing for connect().
    g_tls->setConnectionTimeout(10000);
    g_tls->setHandshakeTimeout(15);   // seconds
    strlcpy(g_host, c.ip, sizeof(g_host));
    g_mqtt->setServer(g_host, 8883);
    g_mqtt->setKeepAlive(30);
    g_mqtt->setSocketTimeout(10);
    g_mqtt->setBufferSize(2048);          // TX only; RX goes through g_stream

    g_reportTopic  = String("device/") + c.serial + "/report";
    g_requestTopic = String("device/") + c.serial + "/request";
    snprintf(g_clientId, sizeof(g_clientId), "BLSF-%s", chipId());
}

void tryConnect(const LinkCfg& c)
{
    const uint32_t now = millis();
    if ((int32_t)(now - g_nextAttemptMs) < 0) return;

    LOGI("printer: connecting to %s", c.ip);
    if (g_mqtt->connect(g_clientId, "bblp", c.accessCode)) {
        LOGI("printer: connected, subscribing");
        g_mqtt->subscribe(g_reportTopic.c_str());
        publishPushall();
        printerSetMqtt(true, 0, mqttStateText(0));
        g_backoffMs = kBackoffMinMs;
        return;
    }

    const int st = g_mqtt->state();
    printerSetMqtt(false, st, mqttStateText(st));
    // Wrong access code: retrying quickly cannot help and some firmwares
    // rate-limit, so go straight to the maximum backoff.
    if (st == 5 || st == 4) {
        g_backoffMs = kBackoffMaxMs;
        LOGE("printer: rejected credentials (state %d)", st);
    } else {
        LOGW("printer: connect failed (state %d, %s)", st, mqttStateText(st));
        g_backoffMs = g_backoffMs * 2;
        if (g_backoffMs > kBackoffMaxMs) g_backoffMs = kBackoffMaxMs;
    }
    g_nextAttemptMs = millis() + g_backoffMs;
    // Free the socket right away rather than leaving a half-open TLS session.
    g_tls->stop();
}

void printerTask(void*)
{
    for (;;) {
        if (g_dirty) {
            g_dirty = false;
            const LinkCfg c = linkCfg();
            teardown();
            if (configured(c)) {
                applyConfig(c);
                g_backoffMs = kBackoffMinMs;
                g_nextAttemptMs = millis();   // reconnect immediately
                LOGI("printer: reconfigured (%s / %s)", c.ip, c.serial);
            } else {
                printerSetMqtt(false, -1, "unconfigured");
            }
        }

        const LinkCfg c = linkCfg();
        if (!configured(c) || WiFi.status() != WL_CONNECTED) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        if (!g_mqtt->connected()) {
            tryConnect(c);
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        g_mqtt->loop();

        // One line per pass: a burst of publishes would starve the socket that
        // has to carry the printer's 16 KB reports.
        publishOneGcode();

        const uint32_t pushallMs = pushallIntervalMs(c);
        if (pushallMs != 0 && (millis() - g_lastPushallMs) >= pushallMs) {
            publishPushall();
        }

        // 10 ms keeps the socket responsive while leaving the core to others.
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

}  // namespace

void printerLinkReconfigure()
{
    LinkCfg next{};
    {
        ConfigGuard guard;
        const Config& c = cfg();
        strlcpy(next.ip, c.printer.ip, sizeof(next.ip));
        strlcpy(next.accessCode, c.printer.accessCode, sizeof(next.accessCode));
        strlcpy(next.serial, c.printer.serial, sizeof(next.serial));
        strlcpy(next.model, c.printer.model, sizeof(next.model));
        next.staleSec = c.fan.staleSec;
        next.dump = c.debug.mqttDump;
    }

    // Only the connection-defining fields warrant tearing down a working
    // session; staleSec and the dump flag are picked up in place, so toggling a
    // debug switch does not drop the printer link. The comparison belongs inside
    // the spinlock: the printer task reads g_link concurrently, and reading it
    // outside could see a half-written value.
    portENTER_CRITICAL(&g_linkMux);
    const bool reconnect = strcmp(g_link.ip, next.ip) != 0 ||
                           strcmp(g_link.accessCode, next.accessCode) != 0 ||
                           strcmp(g_link.serial, next.serial) != 0;
    g_link = next;
    portEXIT_CRITICAL(&g_linkMux);
    if (reconnect) g_dirty = true;
}

void printerLinkStart()
{
    if (g_task) return;
    printerLinkReconfigure();
    g_dirty = true;            // first run always configures the client
    // 20 KB: the mbedTLS handshake runs on this stack, followed by a filtered
    // ArduinoJson parse of a ~16 KB report. BLLED uses the same size in the field.
    xTaskCreatePinnedToCore(printerTask, "printer", 20480, nullptr, 1, &g_task, 1);
    if (!g_task) LOGE("printer: task creation failed");
}

bool printerLinkSendGcode(const char* gcode)
{
    if (!gcode || !*gcode) return false;
    const size_t len = strlen(gcode);
    if (len >= PRINTER_GCODE_MAX) {
        LOGW("printer: gcode line too long (%u bytes)", (unsigned)len);
        return false;
    }
    if (!printerLinkConfigured()) return false;

    portENTER_CRITICAL(&g_gcodeMux);
    const uint8_t next = (uint8_t)((g_gcodeHead + 1) % kGcodeSlots);
    const bool full = next == g_gcodeTail;
    if (!full) {
        memcpy(g_gcodeQ[g_gcodeHead].text, gcode, len + 1);
        g_gcodeHead = next;
    }
    portEXIT_CRITICAL(&g_gcodeMux);

    if (full) {
        LOGW("printer: gcode queue full, dropped '%s'", gcode);
        return false;
    }
    return true;
}

uint8_t printerLinkGcodePending()
{
    portENTER_CRITICAL(&g_gcodeMux);
    const uint8_t n = (uint8_t)((g_gcodeHead + kGcodeSlots - g_gcodeTail) % kGcodeSlots);
    portEXIT_CRITICAL(&g_gcodeMux);
    return n;
}

bool printerLinkConfigured() { return configured(linkCfg()); }

bool printerLinkOnline()
{
    const PrinterState s = printerSnapshot();
    if (!s.connected) return false;
    const LinkCfg c = linkCfg();
    return printerDataAgeMs(s) < (uint32_t)c.staleSec * 1000UL;
}

}  // namespace blsf
