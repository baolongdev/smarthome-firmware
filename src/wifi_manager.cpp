#include "wifi_manager.h"

#include <Preferences.h>
#include <WiFi.h>

#include "config.h"
#include "hmi.h"

namespace wifimgr {
namespace {

constexpr uint8_t kMaxKnown = storage::kMaxWifi + 2;
constexpr uint32_t kAttemptMs = 12000;  // one attempt per known network
constexpr uint32_t kRetryMinMs = 5000;  // pause between failed attempts, doubling to...
constexpr uint32_t kRetryMaxMs = 60000;
constexpr uint32_t kJoinMs = 15000;     // user join timeout
constexpr uint8_t kMaxScan = 20;

struct Cred {
    char ssid[33];
    char pass[65];
};

Cred s_known[kMaxKnown];
uint8_t s_knownCount = 0;
uint8_t s_next = 0;           // next known network to try
uint32_t s_attemptAt = 0;     // when the current automatic attempt started
bool s_attempting = false;
uint32_t s_nextAttempt = 0;
uint32_t s_retryDelay = kRetryMinMs;
bool s_scanning = false;
bool s_joining = false;
uint32_t s_joinAt = 0;
Cred s_join;
volatile uint8_t s_lastReason = 0;  // WIFI_REASON_* of the last disconnect

void addKnown(const char* ssid, const char* pass) {
    if (!ssid || !ssid[0] || s_knownCount >= kMaxKnown) return;
    for (uint8_t i = 0; i < s_knownCount; i++) {
        if (strcmp(s_known[i].ssid, ssid) == 0) return;
    }
    strlcpy(s_known[s_knownCount].ssid, ssid, sizeof(Cred::ssid));
    strlcpy(s_known[s_knownCount].pass, pass ? pass : "", sizeof(Cred::pass));
    s_knownCount++;
}

void connectTo(const char* ssid, const char* pass) {
    WiFi.disconnect(false, false);
    s_lastReason = 0;
    if (pass && pass[0]) WiFi.begin(ssid, pass);
    else WiFi.begin(ssid);
}

void tryNextKnown() {
    if (!s_knownCount) return;
    const Cred& c = s_known[s_next];
    s_next = (s_next + 1) % s_knownCount;
    Serial.printf("WiFi: trying %s\n", c.ssid);
    connectTo(c.ssid, c.pass);
    s_attemptAt = millis();
    s_attempting = true;
}

const char* failMessage(uint8_t reason) {
    switch (reason) {
        case WIFI_REASON_AUTH_FAIL:
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_HANDSHAKE_TIMEOUT:
            return "Sai mật khẩu";
        case WIFI_REASON_AUTH_EXPIRE:
        case WIFI_REASON_ASSOC_EXPIRE:
        case WIFI_REASON_BEACON_TIMEOUT:
            return "Sóng yếu, router không phản hồi";
        case WIFI_REASON_NO_AP_FOUND:
            return "Không tìm thấy mạng";
        default:
            return "Không kết nối được";
    }
}

void finishScan(int16_t n) {
    hmi::WifiNetwork list[kMaxScan];
    uint8_t count = 0;
    for (int16_t i = 0; i < n && count < kMaxScan; i++) {
        const String ssid = WiFi.SSID(i);
        if (ssid.isEmpty()) continue;  // hidden networks
        bool dup = false;              // results are sorted by RSSI: keep the first
        for (uint8_t j = 0; j < count && !dup; j++) dup = strcmp(list[j].ssid, ssid.c_str()) == 0;
        if (dup) continue;
        strlcpy(list[count].ssid, ssid.c_str(), sizeof(list[count].ssid));
        list[count].rssi = WiFi.RSSI(i);
        list[count].secure = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
        count++;
    }
    WiFi.scanDelete();
    s_scanning = false;
    hmi::setWifiNetworks(list, count);
    hmi::setWifiScanning(false);
    Serial.printf("WiFi: scan found %u networks\n", count);
}

}  // namespace

void begin(const storage::Config& sd) {
    Preferences p;
    p.begin("wifi", false);  // read-write: a read-only open of a new namespace logs an error
    const String ssid = p.getString("ssid", "");
    const String pass = p.getString("pass", "");
    p.end();
    addKnown(ssid.c_str(), pass.c_str());
    for (uint8_t i = 0; i < sd.wifiCount; i++) addKnown(sd.wifiSsid[i], sd.wifiPass[i]);
    addKnown(WIFI_SSID, WIFI_PASSWORD);

    WiFi.onEvent([](WiFiEvent_t, WiFiEventInfo_t info) {
        s_lastReason = info.wifi_sta_disconnected.reason;
    }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);

    WiFi.setHostname(WIFI_HOSTNAME);
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);  // modem sleep drops auth frames at weak RSSI (-75..-80 dBm here)
    WiFi.setTxPower(WIFI_POWER_19_5dBm);
    // Arduino's auto-reconnect retries every ~1 s; routers may then ignore the
    // MAC for a while (endless AUTH_EXPIRE).  Retries are paced in loop() instead.
    WiFi.setAutoReconnect(false);
    Serial.printf("WiFi: %u known network(s)\n", s_knownCount);
    tryNextKnown();
}

void loop() {
    const uint32_t now = millis();

    if (s_scanning) {
        const int16_t n = WiFi.scanComplete();
        if (n >= 0) finishScan(n);
        else if (n == WIFI_SCAN_FAILED) {
            s_scanning = false;
            hmi::setWifiScanning(false);
        }
        return;  // no reconnect attempts while the radio is scanning
    }

    if (s_joining) {
        const bool fatal = s_lastReason == WIFI_REASON_AUTH_FAIL || s_lastReason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
                           s_lastReason == WIFI_REASON_HANDSHAKE_TIMEOUT || s_lastReason == WIFI_REASON_NO_AP_FOUND;
        if (WiFi.status() == WL_CONNECTED) {
            s_joining = false;
            Preferences p;
            p.begin("wifi", false);
            p.putString("ssid", s_join.ssid);
            p.putString("pass", s_join.pass);
            p.end();
            // The joined network becomes the first known one.
            memmove(&s_known[1], &s_known[0], sizeof(Cred) * (kMaxKnown - 1));
            s_known[0] = s_join;
            if (s_knownCount < kMaxKnown) s_knownCount++;
            for (uint8_t i = 1; i < s_knownCount; i++) {  // drop the older duplicate
                if (strcmp(s_known[i].ssid, s_join.ssid) == 0) {
                    memmove(&s_known[i], &s_known[i + 1], sizeof(Cred) * (s_knownCount - i - 1));
                    s_knownCount--;
                    break;
                }
            }
            s_next = 0;
            hmi::setWifiJoin(hmi::WifiJoin::Connected, WiFi.localIP().toString().c_str());
            storage::logEvent("Wi-Fi joined from panel: %s", s_join.ssid);
        } else if ((fatal && now - s_joinAt > 3000) || now - s_joinAt > kJoinMs) {
            s_joining = false;
            const char* msg = failMessage(s_lastReason);
            hmi::setWifiJoin(hmi::WifiJoin::Failed, msg);
            storage::logEvent("Wi-Fi join %s failed: %s (reason %u)", s_join.ssid, msg, s_lastReason);
            s_attemptAt = now;  // resume known networks
            tryNextKnown();
        }
        return;
    }

    // Automatic connection: one attempt per known network, then back off.
    if (WiFi.status() == WL_CONNECTED) {
        s_attempting = false;
        s_retryDelay = kRetryMinMs;
        return;
    }
    if (s_attempting) {
        if (now - s_attemptAt < kAttemptMs) return;
        WiFi.disconnect(false, false);
        s_attempting = false;
        s_nextAttempt = now + s_retryDelay;
        Serial.printf("WiFi: not connected, next try in %lus\n", (unsigned long)(s_retryDelay / 1000));
        s_retryDelay = s_retryDelay * 2 > kRetryMaxMs ? kRetryMaxMs : s_retryDelay * 2;
        return;
    }
    if (s_knownCount && (int32_t)(now - s_nextAttempt) >= 0) tryNextKnown();
}

void startScan() {
    if (s_scanning) return;
    if (WiFi.status() != WL_CONNECTED) WiFi.disconnect(false, false);  // a pending attempt blocks scanning
    s_scanning = WiFi.scanNetworks(true, false) == WIFI_SCAN_RUNNING;
    hmi::setWifiScanning(s_scanning);
    if (!s_scanning) Serial.println("WiFi: scan could not start");
}

void join(const char* ssid, const char* password) {
    strlcpy(s_join.ssid, ssid, sizeof(Cred::ssid));
    strlcpy(s_join.pass, password ? password : "", sizeof(Cred::pass));
    s_joining = true;
    s_joinAt = millis();
    hmi::setWifiJoin(hmi::WifiJoin::Connecting, "");
    Serial.printf("WiFi: joining %s\n", ssid);
    connectTo(s_join.ssid, s_join.pass);
}

bool connected() { return WiFi.status() == WL_CONNECTED; }

}  // namespace wifimgr
