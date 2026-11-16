#include "wifi_manager.h"

#include <Preferences.h>
#include <WiFi.h>
#include <esp_wifi.h>

#include <cstring>

#include "config.h"
#include "hmi.h"

namespace wifimgr {
namespace {

constexpr uint8_t kMaxKnown = hmi::kMaxSavedWifi + 1;
constexpr uint32_t kAttemptMs = 12000;  // one attempt per known network
constexpr uint32_t kRetryMinMs = 5000;  // pause between failed attempts, doubling to...
constexpr uint32_t kRetryMaxMs = 60000;
constexpr uint32_t kJoinMs = 15000;     // user join timeout
constexpr uint8_t kMaxScan = 20;

struct Cred {
    char ssid[33];
    char pass[65];
    bool autoJoin = true;
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
// SSIDs the last scan reported as open.  A stored password must not be sent
// to them: the ESP32 then only accepts a secured AP and an open router never
// answers (AUTH_EXPIRE) - how RD-SEAI_2.4G failed after it dropped its password.
char s_open[kMaxScan][33];
bool s_bootScan = false;
uint8_t s_openCount = 0;
bool s_notify = false;  // push the saved list to the HMI when it changes

bool isOpen(const char* ssid) {
    for (uint8_t i = 0; i < s_openCount; i++) {
        if (strcmp(s_open[i], ssid) == 0) return true;
    }
    return false;
}

int findKnownIndex(const char* ssid) {
    if (!ssid) return -1;
    for (uint8_t i = 0; i < s_knownCount; i++) {
        if (strcmp(s_known[i].ssid, ssid) == 0) return i;
    }
    return -1;
}

void pushSavedList() {
#if ESPNOW_SLAVE_ONLY
    hmi::setSavedWifi(nullptr, 0);
    return;
#endif
    hmi::SavedNetwork list[kMaxKnown];
    uint8_t n = 0;
    for (uint8_t i = 0; i < s_knownCount; i++) {
        if (!s_known[i].ssid[0]) continue;
        strlcpy(list[n].ssid, s_known[i].ssid, sizeof(list[0].ssid));
        list[n].autoJoin = s_known[i].autoJoin;
        n++;
    }
    hmi::setSavedWifi(list, n);
}

void addKnown(const char* ssid, const char* pass, bool autoJoin = true) {
    if (!ssid || !ssid[0]) return;
    const int idx = findKnownIndex(ssid);
    if (idx >= 0) {  // already known: keep the password, take the new autoJoin
        if (pass && pass[0]) strlcpy(s_known[idx].pass, pass, sizeof(Cred::pass));
        s_known[idx].autoJoin = autoJoin;
        return;
    }
    if (s_knownCount >= kMaxKnown) {
        // Drop the least recently added (last) to make room for the new one.
        memmove(&s_known[kMaxKnown - 2], &s_known[kMaxKnown - 1], sizeof(Cred));
        s_knownCount = kMaxKnown - 1;
    }
    strlcpy(s_known[s_knownCount].ssid, ssid, sizeof(Cred::ssid));
    strlcpy(s_known[s_knownCount].pass, pass ? pass : "", sizeof(Cred::pass));
    s_known[s_knownCount].autoJoin = autoJoin;
    s_knownCount++;
    s_notify = true;
}

void connectTo(const char* ssid, const char* pass) {
    WiFi.disconnect(false, false);
    s_lastReason = 0;
    const bool usePass = pass && pass[0] && !isOpen(ssid);
    Serial.printf("WiFi: begin %s (%s)\n", ssid, usePass ? "with password" : "open, no password");
    if (usePass) WiFi.begin(ssid, pass);
    else WiFi.begin(ssid);
}

void tryNextKnown() {
    if (!s_knownCount) return;
    uint8_t tries = s_knownCount;
    while (tries--) {  // skip networks the user turned auto-join off for
        const Cred& c = s_known[s_next];
        s_next = (s_next + 1) % s_knownCount;
        if (!c.autoJoin) continue;
        Serial.printf("WiFi: trying %s\n", c.ssid);
        connectTo(c.ssid, c.pass);
        s_attemptAt = millis();
        s_attempting = true;
        return;
    }
    s_attempting = false;
    s_nextAttempt = millis() + kRetryMaxMs;  // every network is disabled: idle
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
    s_openCount = 0;
    for (int16_t i = 0; i < n && count < kMaxScan; i++) {
        const String ssid = WiFi.SSID(i);
        if (ssid.isEmpty()) continue;  // hidden networks
        bool dup = false;              // results are sorted by RSSI: keep the first
        for (uint8_t j = 0; j < count && !dup; j++) dup = strcmp(list[j].ssid, ssid.c_str()) == 0;
        if (dup) continue;
        strlcpy(list[count].ssid, ssid.c_str(), sizeof(list[count].ssid));
        list[count].rssi = WiFi.RSSI(i);
        list[count].secure = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
        if (!list[count].secure) strlcpy(s_open[s_openCount++], list[count].ssid, sizeof(s_open[0]));
        if (s_bootScan)  // the boot scan lists what the panel hears (RSSI here, not at the laptop)
            Serial.printf("WiFi:   %4d dBm ch%-2d %s %s\n", list[count].rssi, WiFi.channel(i),
                          list[count].secure ? "WPA " : "open", list[count].ssid);
        const int knownIdx = findKnownIndex(list[count].ssid);
        const Cred* k = knownIdx >= 0 ? &s_known[knownIdx] : nullptr;
        list[count].saved = k && (k->pass[0] || !list[count].secure);
        count++;
    }
    WiFi.scanDelete();
    s_scanning = false;
    hmi::setWifiNetworks(list, count);
    hmi::setWifiScanning(false);
    Serial.printf("WiFi: scan found %u networks\n", count);
}

// Called when the panel is connected (or after a manual join): make this the
// preferred network, saved in NVS and pushed first to the auto-join list.
void promoteNetwork(const char* ssid, const char* pass) {
    Preferences p;
    p.begin("wifi", false);
    p.putString("ssid", ssid);
    p.putString("pass", pass ? pass : "");
    p.end();

    int idx = findKnownIndex(ssid);
    if (idx < 0) {
        addKnown(ssid, pass, true);
        idx = findKnownIndex(ssid);
    } else if (pass && pass[0]) {
        strlcpy(s_known[idx].pass, pass, sizeof(Cred::pass));
    }
    if (idx > 0) {  // move to the front so it is tried first at boot
        const Cred c = s_known[idx];
        memmove(&s_known[1], &s_known[0], sizeof(Cred) * (idx - 1));
        s_known[0] = c;
    }
    s_known[0].autoJoin = true;
    s_next = 0;
    s_notify = true;
}

}  // namespace

void begin(const storage::Config& sd) {
    Preferences p;
    p.begin("wifi", false);  // read-write: a read-only open of a new namespace logs an error
    const String ssid = p.getString("ssid", "");
    const String pass = p.getString("pass", "");
    p.end();
    if (ssid.length()) addKnown(ssid.c_str(), pass.c_str(), true);
    for (uint8_t i = 0; i < sd.wifiCount; i++) {
        Preferences ap;
        ap.begin("wifi", true);
        const bool autoJoin = ap.getBool(("auto:" + String(sd.wifiSsid[i])).c_str(), true);
        ap.end();
        addKnown(sd.wifiSsid[i], sd.wifiPass[i], autoJoin);
    }
    addKnown(WIFI_SSID, WIFI_PASSWORD, true);

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
#if ESPNOW_SLAVE_ONLY
    s_bootScan = false;
    s_scanning = false;
    s_joining = false;
    s_attempting = false;
    pushSavedList();
    hmi::setWifiNetworks(nullptr, 0);
    hmi::setWifiScanning(false);
    hmi::setWifiJoin(hmi::WifiJoin::Failed, "Slave chỉ dùng ESP-NOW");
    Serial.println("WiFi: disabled on ESP-NOW-only slave");
    return;
#endif
    Serial.printf("WiFi: %u known network(s)\n", s_knownCount);
    s_bootScan = true;
    finishScan(WiFi.scanNetworks(false, false));  // ~2 s, also fills the Wi-Fi page
    s_bootScan = false;
    pushSavedList();
    tryNextKnown();
}

void loop() {
#if ESPNOW_SLAVE_ONLY
    return;
#endif
    const uint32_t now = millis();
    if (s_notify) { s_notify = false; pushSavedList(); }

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
            promoteNetwork(s_join.ssid, s_join.pass);
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
#if ESPNOW_SLAVE_ONLY
    hmi::setWifiScanning(false);
    hmi::setWifiJoin(hmi::WifiJoin::Failed, "Slave chỉ dùng ESP-NOW");
    return;
#endif
    if (s_scanning) return;
    if (WiFi.status() != WL_CONNECTED) WiFi.disconnect(false, false);  // a pending attempt blocks scanning
    s_scanning = WiFi.scanNetworks(true, false) == WIFI_SCAN_RUNNING;
    hmi::setWifiScanning(s_scanning);
    if (!s_scanning) Serial.println("WiFi: scan could not start");
}

void join(const char* ssid, const char* password) {
#if ESPNOW_SLAVE_ONLY
    (void)ssid;
    (void)password;
    hmi::setWifiJoin(hmi::WifiJoin::Failed, "Slave chỉ dùng ESP-NOW");
    return;
#endif
    strlcpy(s_join.ssid, ssid, sizeof(Cred::ssid));
    strlcpy(s_join.pass, password ? password : "", sizeof(Cred::pass));
    if (strcmp(s_join.pass, kUseSaved) == 0) {  // "Kết nối" on a saved, secured network
        const int idx = findKnownIndex(ssid);
        const Cred* k = idx >= 0 ? &s_known[idx] : nullptr;
        strlcpy(s_join.pass, k ? k->pass : "", sizeof(Cred::pass));
    }
    s_joining = true;
    s_joinAt = millis();
    hmi::setWifiJoin(hmi::WifiJoin::Connecting, "");
    Serial.printf("WiFi: joining %s\n", ssid);
    connectTo(s_join.ssid, s_join.pass);
}

void forget(const char* ssid) {
    const int idx = findKnownIndex(ssid);
    if (idx < 0) return;
    const bool active = strcmp(ssid, WiFi.SSID().c_str()) == 0 && WiFi.status() == WL_CONNECTED;
    if (active) {
        WiFi.disconnect(false, false);
        s_attempting = false;
        s_nextAttempt = millis() + 1000;
    }
    // Remove from the runtime list, keeping priority order.
    memmove(&s_known[idx], &s_known[idx + 1], sizeof(Cred) * (s_knownCount - idx - 1));
    s_knownCount--;
    s_notify = true;

    // NVS: remove the password (if it was the last joined one) and the auto key.
    Preferences p;
    p.begin("wifi", false);
    if (p.getString("ssid", "") == ssid) {
        p.remove("ssid");
        p.remove("pass");
    }
    p.remove(("auto:" + String(ssid)).c_str());
    p.end();

    storage::logEvent("Wi-Fi forgotten from panel: %s", ssid);
    pushSavedList();
}

void setAutoJoin(const char* ssid, bool on) {
    const int idx = findKnownIndex(ssid);
    if (idx < 0) return;
    s_known[idx].autoJoin = on;
    Preferences p;
    p.begin("wifi", false);
    p.putBool(("auto:" + String(ssid)).c_str(), on);
    p.end();
    s_notify = true;
    storage::logEvent("Wi-Fi auto-join %s: %s", ssid, on ? "on" : "off");
    pushSavedList();
}

uint8_t savedList(hmi::SavedNetwork* out, uint8_t maxCount) {
#if ESPNOW_SLAVE_ONLY
    (void)out;
    (void)maxCount;
    return 0;
#endif
    uint8_t n = 0;
    for (uint8_t i = 0; i < s_knownCount && n < maxCount; i++) {
        if (!s_known[i].ssid[0]) continue;
        strlcpy(out[n].ssid, s_known[i].ssid, sizeof(out[0].ssid));
        out[n].autoJoin = s_known[i].autoJoin;
        n++;
    }
    return n;
}

// Diagnostics (console "wifi bssid <aa:bb:..> <channel> <ssid>"): join one
// specific access point with the stored password.
void joinBssid(const char* ssid, const uint8_t* bssid, int32_t channel) {
#if ESPNOW_SLAVE_ONLY
    (void)ssid;
    (void)bssid;
    (void)channel;
    hmi::setWifiJoin(hmi::WifiJoin::Failed, "Slave chỉ dùng ESP-NOW");
    return;
#endif
    join(ssid, kUseSaved);
    WiFi.disconnect(false, false);
    WiFi.begin(s_join.ssid, s_join.pass[0] ? s_join.pass : nullptr, channel, bssid);
    Serial.printf("WiFi: joining %s via %02x:%02x:%02x:%02x:%02x:%02x ch%ld, panel MAC %s\n", ssid, bssid[0], bssid[1],
                  bssid[2], bssid[3], bssid[4], bssid[5], (long)channel, WiFi.macAddress().c_str());
}

// Diagnostics (console "wifi mac"): switch the station to a locally
// administered MAC (until reboot) and retry, to tell a router-side MAC block
// apart from a radio problem.
void retryWithNewMac(const char* ssid) {
#if ESPNOW_SLAVE_ONLY
    (void)ssid;
    return;
#endif
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    mac[0] = (mac[0] | 0x02) & 0xFE;  // locally administered, unicast
    mac[5] ^= 0x5A;
    WiFi.disconnect(false, false);
    esp_wifi_stop();
    const esp_err_t err = esp_wifi_set_mac(WIFI_IF_STA, mac);
    esp_wifi_start();
    Serial.printf("WiFi: station MAC now %s (%s)\n", WiFi.macAddress().c_str(), esp_err_to_name(err));
    join(ssid, kUseSaved);
}

// Diagnostics (console "wifi 11b <ssid>"): 802.11b only - lowest rates, best
// sensitivity - for an AP that hears the panel badly.  Until reboot.
void retry11b(const char* ssid) {
#if ESPNOW_SLAVE_ONLY
    (void)ssid;
    return;
#endif
    WiFi.disconnect(false, false);
    const esp_err_t err = esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B);
    Serial.printf("WiFi: protocol 802.11b only (%s)\n", esp_err_to_name(err));
    join(ssid, kUseSaved);
}

bool connected() {
#if ESPNOW_SLAVE_ONLY
    return false;
#else
    return WiFi.status() == WL_CONNECTED;
#endif
}
bool busy() {
#if ESPNOW_SLAVE_ONLY
    return false;
#else
    return s_bootScan || s_scanning || s_joining || s_attempting;
#endif
}

}  // namespace wifimgr
