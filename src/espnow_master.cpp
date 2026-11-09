#include "espnow_master.h"

#include <ArduinoJson.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>
#include <freertos/task.h>
#include <freertos/queue.h>

#include <string.h>
#include <atomic>

#include "config.h"
#include "storage.h"

namespace espnow {
namespace {

constexpr uint8_t kMaxNodes = 8;
constexpr uint8_t kQueueSize = 24;

storage::EspNowConfig s_cfg;
bool s_cfgValid = false;
bool s_ready = false;
uint32_t s_lastInitTry = 0;
uint8_t s_channel = ESPNOW_CHANNEL;

portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

struct RxPkt {
    uint8_t mac[kMacLen];
    uint8_t data[kMaxPayload];
    uint16_t len = 0;
};
RxPkt s_q[kQueueSize];
volatile uint8_t s_qHead = 0, s_qTail = 0;
std::atomic<uint32_t> s_dropped{0};
std::atomic<uint32_t> s_rxTotal{0};

NodeState s_nodes[kMaxNodes];
uint8_t s_nodeCount = 0;
char s_status[64] = "init";
QueueHandle_t s_cmdQ = nullptr;

struct Cmd { uint8_t type; uint8_t index; bool on; };  // type 0=toggle,1=set

int hexNibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
bool parseHexKey(const char* text, uint8_t out[ESP_NOW_KEY_LEN]) {
    if (!text || strlen(text) != ESP_NOW_KEY_LEN * 2) return false;
    for (size_t i = 0; i < ESP_NOW_KEY_LEN; i++) {
        int h = hexNibble(text[i * 2]), l = hexNibble(text[i * 2 + 1]);
        if (h < 0 || l < 0) return false;
        out[i] = (uint8_t)((h << 4) | l);
    }
    return true;
}
bool parseMac(const char* text, uint8_t mac[kMacLen]) {
    if (!text) return false;
    unsigned v[kMacLen] = {};
    if (sscanf(text, "%02x:%02x:%02x:%02x:%02x:%02x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) return false;
    for (size_t i = 0; i < kMacLen; i++) mac[i] = (uint8_t)v[i];
    return true;
}
void formatMac(const uint8_t mac[kMacLen], char out[18]) {
    snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}
bool decodeHex(const char* text, uint8_t* out, size_t cap, size_t& n) {
    n = 0;
    if (!text) return false;
    size_t len = strlen(text);
    if (len == 0 || (len & 1) || (len / 2) > cap) return false;
    for (size_t i = 0; i < len / 2; i++) {
        int h = hexNibble(text[i * 2]), l = hexNibble(text[i * 2 + 1]);
        if (h < 0 || l < 0) { n = 0; return false; }
        out[n++] = (uint8_t)((h << 4) | l);
    }
    return true;
}

NodeState* findNode(const uint8_t mac[kMacLen], bool create) {
    for (uint8_t i = 0; i < s_nodeCount; i++) {
        uint8_t m[kMacLen];
        if (parseMac(s_nodes[i].mac, m) && memcmp(m, mac, kMacLen) == 0) return &s_nodes[i];
    }
    if (!create || s_nodeCount >= kMaxNodes) return nullptr;
    NodeState& n = s_nodes[s_nodeCount++];
    formatMac(mac, n.mac);
    return &n;
}

bool peerAllowed(const uint8_t mac[kMacLen]) {
    for (uint8_t i = 0; i < s_cfg.peerCount; i++) if (memcmp(s_cfg.peers[i], mac, kMacLen) == 0) return true;
    return s_cfg.peerCount == 0;  // no allowlist => accept encrypted peers
}

void postRelay(uint8_t type, int id, bool on) {
    if (id < 0 || id >= RELAY_COUNT || !s_cmdQ) return;
    Cmd c{type, (uint8_t)id, on};
    xQueueSend(s_cmdQ, &c, 0);
}

void IRAM_ATTR onRecv(const uint8_t* mac, const uint8_t* data, int len) {
    if (!mac || !data || len <= 0) return;
    size_t n = (size_t)len > kMaxPayload ? kMaxPayload : (size_t)len;
    uint8_t next = (uint8_t)((s_qHead + 1) % kQueueSize);
    if (next == s_qTail) { s_dropped.fetch_add(1, std::memory_order_relaxed); return; }
    RxPkt& p = s_q[s_qHead];
    memcpy(p.mac, mac, kMacLen);
    memcpy(p.data, data, n);
    p.len = (uint16_t)n;
    s_qHead = next;
    s_rxTotal.fetch_add(1, std::memory_order_relaxed);
}
void IRAM_ATTR onSent(const uint8_t*, esp_now_send_status_t) {}

bool addPeerInternal(const uint8_t mac[kMacLen], uint8_t channel) {
    static const uint8_t bcast[kMacLen] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    if (memcmp(mac, bcast, kMacLen) == 0) return false;
    uint8_t lmk[ESP_NOW_KEY_LEN];
    if (!parseHexKey(s_cfg.lmk, lmk)) return false;
    esp_now_peer_info_t peer{};
    memcpy(peer.peer_addr, mac, kMacLen);
    memcpy(peer.lmk, lmk, ESP_NOW_KEY_LEN);
    peer.channel = channel ? channel : s_channel;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = true;
    esp_err_t e = esp_now_add_peer(&peer);
    if (e == ESP_OK || e == ESP_ERR_ESPNOW_EXIST) { findNode(mac, true); return true; }
    char m[18]; formatMac(mac, m); Serial.printf("[espnow] add peer %s err %d\n", m, (int)e);
    return false;
}

bool doInit() {
    if (s_ready) return true;
    if (!s_cfgValid || !s_cfg.enabled) { strlcpy(s_status, "disabled", sizeof(s_status)); return false; }
    uint8_t pmk[ESP_NOW_KEY_LEN], lmk[ESP_NOW_KEY_LEN];
    if (!parseHexKey(s_cfg.pmk, pmk) || !parseHexKey(s_cfg.lmk, lmk)) {
        strlcpy(s_status, "invalid pmk/lmk", sizeof(s_status));
        return false;
    }
    // Coexist with wifi_manager: keep STA; only force fixed channel when not connected.
    wifi_mode_t mode = WIFI_MODE_NULL;
    esp_wifi_get_mode(&mode);
    if (mode == WIFI_MODE_NULL) WiFi.mode(WIFI_STA);
    const bool wifiConnected = WiFi.status() == WL_CONNECTED;
    if (wifiConnected) s_channel = WiFi.channel();
    else esp_wifi_set_channel(s_channel, WIFI_SECOND_CHAN_NONE);
    esp_err_t err = esp_now_init();
    if (err != ESP_OK) { snprintf(s_status, sizeof(s_status), "init err %d", (int)err); return false; }
    esp_now_register_recv_cb(onRecv);
    esp_now_register_send_cb(onSent);
    esp_now_set_pmk(pmk);
    uint8_t added = 0;
    for (uint8_t i = 0; i < s_cfg.peerCount; i++) if (addPeerInternal(s_cfg.peers[i], s_channel)) added++;
    s_ready = true;
    snprintf(s_status, sizeof(s_status), "slave ready ch%u peers%u%s", s_channel, added, wifiConnected ? " (wifi)" : "");
    storage::logEvent("ESP-NOW slave ready ch%u peers=%u wifi=%d", s_channel, added, wifiConnected);
    Serial.printf("[espnow] %s\n", s_status);
    return true;
}

void handlePacket(const RxPkt& p) {
    char mac[18]; formatMac(p.mac, mac);
    portENTER_CRITICAL(&s_mux);
    NodeState* n = findNode(p.mac, true);
    if (n) { n->channel = s_channel; n->rx++; n->lastRxMs = millis(); }
    portEXIT_CRITICAL(&s_mux);
    if (!peerAllowed(p.mac)) { storage::logEvent("ESP-NOW ignored %s", mac); return; }

    // JSON: {"cmd":"relay","id":0,"on":true} / {"cmd":"toggle","id":1} / {"cmd":"relays","mask":5}
    if (p.len > 1 && p.data[0] == '{') {
        JsonDocument doc;
        if (!deserializeJson(doc, p.data, p.len)) {
            const char* cmd = doc["cmd"] | "";
            int id = doc["id"] | doc["relay"] | doc["channel"] | -1;
            bool on = doc["on"] | doc["state"] | false;
            if (!strcmp(cmd, "toggle")) postRelay(0, id, false);
            else if (!strcmp(cmd, "relay") || !strcmp(cmd, "set")) postRelay(1, id, on);
            else if (!strcmp(cmd, "relays") && doc["mask"].is<int>()) {
                int mask = doc["mask"]; for (int i = 0; i < RELAY_COUNT; i++) postRelay(1, i, (mask >> i) & 1);
            }
            storage::logEvent("ESP-NOW cmd %s id=%d on=%d from %s", cmd, id, (int)on, mac);
            return;
        }
    }
    if (p.len >= 2) { postRelay(1, p.data[0], p.data[1] != 0); storage::logEvent("ESP-NOW bin %s id=%u on=%u", mac, p.data[0], p.data[1]); }
}

void task(void*) {
    for (;;) {
        while (s_qTail != s_qHead) { RxPkt p = s_q[s_qTail]; s_qTail = (uint8_t)((s_qTail + 1) % kQueueSize); handlePacket(p); }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

}  // namespace

void setCommandQueue(QueueHandle_t q) { s_cmdQ = q; }

void begin(const storage::EspNowConfig& cfg) {
    s_cfg = cfg;
    s_cfgValid = true;
    s_channel = s_cfg.channel ? s_cfg.channel : ESPNOW_CHANNEL;
    // ensure master from config.h is allowed if config has no peers or placeholder MAC
    if (s_cfg.peerCount == 0 || (s_cfg.peerCount && s_cfg.peers[0][0]==0 && s_cfg.peers[0][1]==0 && s_cfg.peers[0][2]==0 && s_cfg.peers[0][3]==0 && s_cfg.peers[0][4]==0 && s_cfg.peers[0][5]==0)) {
        uint8_t m[kMacLen];
        if (parseMac(ESPNOW_MASTER_MAC, m)) { memcpy(s_cfg.peers[0], m, kMacLen); s_cfg.peerCount = 1; }
    }
    if (!s_cfg.enabled) s_cfg.enabled = ESPNOW_ENABLED;
    if (!s_cfg.pmk[0]) strlcpy(s_cfg.pmk, ESPNOW_PMK, sizeof(s_cfg.pmk));
    if (!s_cfg.lmk[0]) strlcpy(s_cfg.lmk, ESPNOW_LMK, sizeof(s_cfg.lmk));
    if (s_ready) { esp_now_deinit(); s_ready = false; }
    static bool taskStarted = false;
    if (!taskStarted) { taskStarted = true; xTaskCreatePinnedToCore(task, "espnow", 4096, nullptr, 2, nullptr, 0); }
    s_lastInitTry = 0;
}

void loop() {
    if (s_ready || !s_cfgValid || !s_cfg.enabled) return;
    uint32_t now = millis();
    if (now - s_lastInitTry < 1000) return;
    s_lastInitTry = now;
    doInit();
}

bool addPeer(const uint8_t mac[kMacLen], uint8_t channel) { return addPeerInternal(mac, channel); }
bool send(const uint8_t mac[kMacLen], const uint8_t* data, size_t len) { return s_ready && data && len && len <= kMaxPayload && esp_now_send(mac, data, len) == ESP_OK; }
bool sendHex(const char* macText, const char* hex) {
    uint8_t mac[kMacLen], buf[kMaxPayload]; size_t n = 0;
    if (!parseMac(macText, mac) || !decodeHex(hex, buf, sizeof(buf), n)) return false;
    return send(mac, buf, n);
}
bool ready() { return s_ready; }
const char* statusText() { return s_status; }
uint8_t peerCount() { esp_now_peer_num_t n{}; if (s_ready) esp_now_get_peer_num(&n); return (uint8_t)n.total_num; }
uint8_t nodeCount() { return s_nodeCount; }
bool node(uint8_t i, NodeState& out) { if (i >= s_nodeCount) return false; portENTER_CRITICAL(&s_mux); out = s_nodes[i]; portEXIT_CRITICAL(&s_mux); return true; }
void print(Print& out) {
    out.printf("espnow slave: %s rx=%lu drop=%lu peers=%u\n", s_status, (unsigned long)s_rxTotal.load(), (unsigned long)s_dropped.load(), peerCount());
    for (uint8_t i = 0; i < s_nodeCount; i++) out.printf("  %s rx=%lu age=%lus\n", s_nodes[i].mac, (unsigned long)s_nodes[i].rx, s_nodes[i].lastRxMs ? (unsigned long)((millis() - s_nodes[i].lastRxMs) / 1000) : 0UL);
}

}  // namespace espnow
