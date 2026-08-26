#include "wifi_manager.h"

#include <WiFi.h>
#include <DNSServer.h>
#include <ESPmDNS.h>

#include "config.h"
#include "log.h"
#include "version.h"

namespace blsf {

namespace {

enum class State { Idle, Connecting, Connected };

const uint32_t kConnectTimeoutMs = 20000;
const uint32_t kApAfterMs        = 90000;   // raise the setup AP after this long
const uint32_t kApRetryMs        = 60000;   // STA retry cadence once the AP is up
const uint32_t kApLingerMs       = 5UL * 60UL * 1000UL;
const uint8_t  kDropBssidAfter   = 3;       // failed cycles before unlocking BSSID

// How long a failed softAP() is left alone before trying again. Retrying every
// pass would flood the log and hammer the driver.
const uint32_t kApRetryBackoffMs = 5000;

State    g_state = State::Idle;
uint32_t g_stateSinceMs = 0;
uint32_t g_nextAttemptMs = 0;
// millis() at which the station last stopped being connected (or first tried).
// The setup AP is raised only after kApAfterMs of *continuous* failure, so a
// link that flaps once an hour never ends up permanently broadcasting an open
// network - onConnected() clears this.
uint32_t g_downSinceMs = 0;
uint32_t g_backoffMs = 5000;
uint8_t  g_failedCycles = 0;
bool     g_bssidDropped = false;
bool     g_apActive = false;
uint32_t g_apNextTryMs = 0;
uint32_t g_apLingerUntilMs = 0;
bool     g_mdnsUp = false;

DNSServer g_dns;
char      g_apSsid[32] = {0};
const IPAddress kApIp(192, 168, 4, 1);

ScanState g_scan = ScanState::Idle;
uint32_t  g_scanDoneMs = 0;
// A cached scan older than this is refreshed on the next request.
const uint32_t kScanTtlMs = 20000;

bool haveCredentials() { return cfg().wifi.ssid[0] != '\0'; }

bool parseBssid(const char* text, uint8_t* out)
{
    unsigned b[6];
    if (sscanf(text, "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) return false;
    for (int i = 0; i < 6; i++) out[i] = (uint8_t)b[i];
    return true;
}

void startMdns()
{
    if (g_mdnsUp) return;
    if (!MDNS.begin(cfg().wifi.hostname)) {
        LOGW("mDNS start failed");
        return;
    }
    MDNS.addService("http", "tcp", 80);
    MDNS.addServiceTxt("http", "tcp", "model", FW_NAME);
    MDNS.addServiceTxt("http", "tcp", "version", FW_VERSION);
    g_mdnsUp = true;
    LOGI("mDNS: http://%s.local/", cfg().wifi.hostname);
}

void startAp()
{
    if (g_apActive) return;
    const uint32_t now = millis();
    if (g_apNextTryMs != 0 && (int32_t)(now - g_apNextTryMs) < 0) return;
    g_apNextTryMs = now + kApRetryBackoffMs;

    snprintf(g_apSsid, sizeof(g_apSsid), "BLSmartFlow-%s", chipId());
    WiFi.mode(haveCredentials() ? WIFI_AP_STA : WIFI_AP);
    WiFi.softAPConfig(kApIp, kApIp, IPAddress(255, 255, 255, 0));
    // Open network: the point of the portal is that a phone can join it with no
    // shared secret. It only exposes the local setup UI.
    if (!WiFi.softAP(g_apSsid)) {
        LOGE("failed to start AP, retrying in %u s", (unsigned)(kApRetryBackoffMs / 1000));
        return;
    }
    g_dns.setErrorReplyCode(DNSReplyCode::NoError);
    g_dns.start(53, "*", kApIp);      // resolve every name to us -> captive portal
    g_apActive = true;
    LOGW("setup AP '%s' at %s", g_apSsid, kApIp.toString().c_str());
}

void stopAp()
{
    if (!g_apActive) return;
    g_dns.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    g_apActive = false;
    g_apNextTryMs = 0;
    LOGI("setup AP closed");
}

void beginConnect()
{
    // Copy the credentials out under the lock: a /api/wifi POST on the AsyncTCP
    // task can rewrite them field by field, and WiFi.begin() with a new SSID and
    // the old password is a connection attempt that can only fail.
    WifiConfig w;
    {
        ConfigGuard guard;
        w = cfg().wifi;
    }
    if (!g_apActive) WiFi.mode(WIFI_STA);

    uint8_t bssid[6];
    const bool useBssid = w.lockBssid && !g_bssidDropped && w.bssid[0] && parseBssid(w.bssid, bssid);

    WiFi.disconnect(false, false);
    if (useBssid) {
        LOGI("wifi: connecting to '%s' locked to %s", w.ssid, w.bssid);
        WiFi.begin(w.ssid, w.password, 0, bssid);
    } else {
        LOGI("wifi: connecting to '%s'", w.ssid);
        WiFi.begin(w.ssid, w.password);
    }
    g_state = State::Connecting;
    g_stateSinceMs = millis();
    if (g_downSinceMs == 0) g_downSinceMs = g_stateSinceMs;
}

void onConnected()
{
    g_state = State::Connected;
    g_stateSinceMs = millis();
    g_backoffMs = 5000;
    g_failedCycles = 0;
    g_bssidDropped = false;
    // The outage is over: the 90 s AP countdown starts from scratch next time.
    g_downSinceMs = 0;

    // This exact prefix is parsed by the WebSerial provisioning page - keep it.
    Serial.print(F("IP_ADDRESS:"));
    Serial.println(WiFi.localIP());

    LOGI("wifi: connected, ip %s rssi %d", WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());
    startMdns();

    if (g_apActive) {
        // Give whoever is on the portal a few minutes to finish before we vanish.
        g_apLingerUntilMs = millis() + kApLingerMs;
        LOGI("wifi: AP will close in %u min", (unsigned)(kApLingerMs / 60000));
    }
}

void onFailedAttempt()
{
    g_failedCycles++;
    if (g_failedCycles == kDropBssidAfter && cfg().wifi.lockBssid && !g_bssidDropped) {
        // Three misses in a row usually means the locked radio is gone, not that
        // the credentials are wrong - let the supplicant pick any AP.
        g_bssidDropped = true;
        LOGW("wifi: dropping BSSID lock after %u failures", (unsigned)g_failedCycles);
    }
    g_state = State::Idle;
    g_nextAttemptMs = millis() + g_backoffMs;
    g_backoffMs = g_backoffMs < 30000 ? g_backoffMs * 2 : 60000;
    if (g_apActive && g_backoffMs < kApRetryMs) g_backoffMs = kApRetryMs;
    LOGW("wifi: attempt failed, retrying in %u s", (unsigned)(g_backoffMs / 1000));
}

}  // namespace

void wifiSetup()
{
    WiFi.persistent(false);
    // We drive reconnection ourselves; the built-in retry fights the state machine.
    WiFi.setAutoReconnect(false);
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);             // modem sleep adds 100+ ms of latency
    WiFi.setTxPower(WIFI_POWER_19_5dBm);
    WiFi.setHostname(cfg().wifi.hostname);

    if (!haveCredentials()) {
        LOGW("wifi: no credentials, starting setup AP");
        startAp();
        return;
    }
    beginConnect();
}

void wifiReconfigure()
{
    g_bssidDropped = false;
    g_failedCycles = 0;
    g_backoffMs = 5000;
    g_downSinceMs = 0;
    g_apNextTryMs = 0;
    g_mdnsUp = false;
    WiFi.setHostname(cfg().wifi.hostname);
    if (haveCredentials()) beginConnect();
    else startAp();
}

void wifiLoop()
{
    if (g_apActive) g_dns.processNextRequest();

    const uint32_t now = millis();
    const bool linkUp = WiFi.status() == WL_CONNECTED;

    switch (g_state) {
        case State::Idle:
            if (!haveCredentials()) {
                if (!g_apActive) startAp();
                break;
            }
            if ((int32_t)(now - g_nextAttemptMs) >= 0) beginConnect();
            break;

        case State::Connecting:
            if (linkUp) { onConnected(); break; }
            if ((now - g_stateSinceMs) >= kConnectTimeoutMs) onFailedAttempt();
            break;

        case State::Connected:
            if (!linkUp) {
                LOGW("wifi: link lost");
                g_state = State::Idle;
                g_nextAttemptMs = now;      // reconnect straight away
                g_backoffMs = 5000;
                g_downSinceMs = now;        // the outage clock starts here
                g_mdnsUp = false;
                MDNS.end();
            }
            break;
    }

    // Raise the setup AP once we have been continuously dark for long enough, so
    // a user with a mistyped password is never left without a way in. A link
    // that connects, even briefly, resets the clock in onConnected().
    if (!g_apActive && g_state != State::Connected && g_downSinceMs != 0 &&
        (now - g_downSinceMs) >= kApAfterMs) {
        startAp();
    }

    // Close the AP after the linger window once the station is up.
    if (g_apActive && g_state == State::Connected && g_apLingerUntilMs != 0 &&
        (int32_t)(now - g_apLingerUntilMs) >= 0) {
        g_apLingerUntilMs = 0;
        stopAp();
    }

    // Reap a finished async scan so scanComplete() does not stay latched.
    if (g_scan == ScanState::Running) {
        const int n = WiFi.scanComplete();
        if (n >= 0) { g_scan = ScanState::Done; g_scanDoneMs = now; }
        else if (n == WIFI_SCAN_FAILED) g_scan = ScanState::Idle;
    }
}

bool wifiConnected() { return WiFi.status() == WL_CONNECTED; }
bool wifiIsApMode() { return g_apActive; }

IPAddress wifiIp()
{
    if (WiFi.status() == WL_CONNECTED) return WiFi.localIP();
    if (g_apActive) return WiFi.softAPIP();
    return IPAddress(0, 0, 0, 0);
}

const char* wifiApSsid() { return g_apSsid; }

ScanState wifiScanStart(bool force)
{
    if (g_scan == ScanState::Running) return g_scan;
    if (force) g_scan = ScanState::Idle;      // discard whatever is cached
    WiFi.scanDelete();
    // Async: a synchronous scan blocks for 2-4 s, which would stall the loop.
    WiFi.scanNetworks(true);
    g_scan = ScanState::Running;
    return g_scan;
}

ScanState wifiScanState()
{
    if (g_scan == ScanState::Running) {
        const int n = WiFi.scanComplete();
        if (n >= 0) { g_scan = ScanState::Done; g_scanDoneMs = millis(); }
        else if (n == WIFI_SCAN_FAILED) g_scan = ScanState::Idle;
    } else if (g_scan == ScanState::Done && (millis() - g_scanDoneMs) > kScanTtlMs) {
        // Serving a minutes-old list would show APs that are no longer there.
        g_scan = ScanState::Idle;
    }
    return g_scan;
}

size_t wifiScanResults(JsonArray out)
{
    const int n = WiFi.scanComplete();
    if (n <= 0) return 0;

    // Keep only the strongest BSSID per SSID, then sort by RSSI descending.
    // n is bounded by the driver (~20-40 APs), so an O(n^2) pass is fine and
    // avoids allocating an index array.
    for (int i = 0; i < n; i++) {
        const String ssid = WiFi.SSID(i);
        if (ssid.isEmpty()) continue;      // hidden network
        bool better = true;
        for (int j = 0; j < n; j++) {
            if (j == i) continue;
            if (WiFi.SSID(j) != ssid) continue;
            // Ties broken by index so exactly one entry survives.
            if (WiFi.RSSI(j) > WiFi.RSSI(i) || (WiFi.RSSI(j) == WiFi.RSSI(i) && j < i)) {
                better = false;
                break;
            }
        }
        if (!better) continue;
        JsonObject o = out.add<JsonObject>();
        o["ssid"] = ssid;
        o["bssid"] = WiFi.BSSIDstr(i);
        o["rssi"] = WiFi.RSSI(i);
        o["channel"] = WiFi.channel(i);
        o["secure"] = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
    }

    // Simple selection sort over the JsonArray by rssi.
    const size_t count = out.size();
    for (size_t i = 0; i + 1 < count; i++) {
        size_t best = i;
        for (size_t j = i + 1; j < count; j++) {
            if ((int)out[j]["rssi"] > (int)out[best]["rssi"]) best = j;
        }
        if (best != i) {
            JsonDocument tmp;
            tmp.set(out[i]);
            out[i].set(out[best]);
            out[best].set(tmp);
        }
    }
    return count;
}

}  // namespace blsf
