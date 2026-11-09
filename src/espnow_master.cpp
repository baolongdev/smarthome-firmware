#include "espnow_master.h"
#include "espnow_proto.h"

#include <ArduinoJson.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>
#include <freertos/task.h>
#include <freertos/queue.h>

#include <string.h>
#include <atomic>

#include "config.h"
#include "storage.h"
#include "hmi.h"
#include "hmi/hmi_model.h"

namespace espnow {
namespace {

constexpr uint8_t kMaxNodes = 8;
constexpr uint8_t kQueueSize = 32;
constexpr uint32_t kTelemetryMs = 5000;  // system status every 5 s
constexpr uint32_t kSensorMs = 10000;    // sensor data every 10 s

storage::EspNowConfig s_cfg;
bool s_cfgValid = false;
bool s_ready = false;
uint32_t s_lastInitTry = 0;
uint8_t s_channel = ESPNOW_CHANNEL;

portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
portMUX_TYPE s_qMux = portMUX_INITIALIZER_UNLOCKED;

struct RxPkt {
    uint8_t mac[kMacLen];
    uint8_t data[kMaxPayload];
    uint16_t len = 0;
};
RxPkt s_q[kQueueSize];
volatile uint8_t s_qHead = 0, s_qTail = 0;
std::atomic<uint32_t> s_dropped{0};
std::atomic<uint32_t> s_rxTotal{0};
std::atomic<uint32_t> s_txTotal{0};
std::atomic<uint32_t> s_txFailed{0};

NodeState s_nodes[kMaxNodes];
uint8_t s_nodeCount = 0;
char s_status[64] = "init";
QueueHandle_t s_cmdQ = nullptr;

uint16_t s_seq = 1;
uint32_t s_lastTelemetry = 0;
uint32_t s_lastSensor = 0;

struct Cmd { uint8_t type; uint8_t index; bool on; };

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
    portENTER_CRITICAL_ISR(&s_qMux);
    uint8_t next = (uint8_t)((s_qHead + 1) % kQueueSize);
    if (next == s_qTail) { s_dropped.fetch_add(1, std::memory_order_relaxed); portEXIT_CRITICAL_ISR(&s_qMux); return; }
    RxPkt& p = s_q[s_qHead];
    memcpy(p.mac, mac, kMacLen);
    memcpy(p.data, data, n);
    p.len = (uint16_t)n;
    s_qHead = next;
    portEXIT_CRITICAL_ISR(&s_qMux);
    s_rxTotal.fetch_add(1, std::memory_order_relaxed);
}
void IRAM_ATTR onSent(const uint8_t* mac, esp_now_send_status_t status) {
    if (status == ESP_NOW_SEND_SUCCESS) s_txTotal.fetch_add(1, std::memory_order_relaxed);
    else s_txFailed.fetch_add(1, std::memory_order_relaxed);
    if (!mac) return;
    portENTER_CRITICAL_ISR(&s_mux);
    for (uint8_t i = 0; i < s_nodeCount; i++) {
        uint8_t m[kMacLen];
        if (parseMac(s_nodes[i].mac, m) && memcmp(m, mac, kMacLen) == 0) {
            if (status == ESP_NOW_SEND_SUCCESS) s_nodes[i].txOk++;
            else s_nodes[i].txFail++;
            break;
        }
    }
    portEXIT_CRITICAL_ISR(&s_mux);
}

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

uint16_t nextSeq() { return s_seq++; }

void sendAck(const uint8_t mac[kMacLen], uint8_t type, uint16_t seq, uint8_t result) {
    if (!s_ready) return;
    uint8_t payload[sizeof(espnow_proto::PayloadAck)];
    espnow_proto::PayloadAck ack{};
    ack.type = type;
    ack.seq_hi = static_cast<uint8_t>((seq >> 8) & 0xFF);
    ack.seq_lo = static_cast<uint8_t>(seq & 0xFF);
    ack.result = result;
    memcpy(payload, &ack, sizeof(ack));
    uint8_t frame[espnow_proto::kMaxFrameLen];
    size_t flen = espnow_proto::encodeFrame(
        static_cast<uint8_t>(espnow_proto::MsgType::TEL_ACK),
        espnow_proto::FLAG_IS_RESPONSE, seq, payload, sizeof(ack), frame, sizeof(frame));
    if (flen) esp_now_send(mac, frame, flen);
}

void sendTelemetry(const uint8_t mac[kMacLen]) {
    if (!s_ready) return;
    hmi::ModelLock lock;
    hmi::Model& m = hmi::model();
    uint8_t payload[sizeof(espnow_proto::PayloadSystemStatus)];
    espnow_proto::PayloadSystemStatus st{};
    st.relay_mask = 0;
    for (uint8_t i = 0; i < RELAY_COUNT; i++) if (m.relay[i]) st.relay_mask |= (1u << i);
    st.rssi = WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0;
    st.uptime_s = m.sys.uptimeS;
    st.free_heap = m.sys.heapKB * 1024;
    st.wifi_connected = WiFi.status() == WL_CONNECTED ? 1 : 0;
    st.espnow_ready = s_ready ? 1 : 0;
    st.temp = m.primary.value;
    st.humidity = m.metric[0].value;
    st.power_w = m.metric[1].value;
    memcpy(payload, &st, sizeof(st));

    uint8_t frame[espnow_proto::kMaxFrameLen];
    size_t flen = espnow_proto::encodeFrame(
        static_cast<uint8_t>(espnow_proto::MsgType::TEL_SYSTEM_STATUS),
        espnow_proto::FLAG_IS_RESPONSE, nextSeq(), payload, sizeof(st), frame, sizeof(frame));
    if (!flen) return;
    esp_now_send(mac, frame, flen);
}

void sendSensorData(const uint8_t mac[kMacLen]) {
    if (!s_ready) return;
    hmi::ModelLock lock;
    hmi::Model& m = hmi::model();
    uint8_t payload[sizeof(espnow_proto::PayloadSensorData)];
    espnow_proto::PayloadSensorData sd{};
    sd.temp = m.primary.value;
    sd.humidity = m.metric[0].value;
    sd.power_w = m.metric[1].value;
    memcpy(payload, &sd, sizeof(sd));

    uint8_t frame[espnow_proto::kMaxFrameLen];
    size_t flen = espnow_proto::encodeFrame(
        static_cast<uint8_t>(espnow_proto::MsgType::TEL_SENSOR_DATA),
        espnow_proto::FLAG_IS_RESPONSE, nextSeq(), payload, sizeof(sd), frame, sizeof(frame));
    if (!flen) return;
    esp_now_send(mac, frame, flen);
}

void sendConfigResp(const uint8_t mac[kMacLen]) {
    if (!s_ready) return;
    uint8_t payload[sizeof(espnow_proto::PayloadConfigResp)];
    espnow_proto::PayloadConfigResp cf{};
    strlcpy(cf.masterMac, s_cfg.peerCount ? "" : ESPNOW_MASTER_MAC, sizeof(cf.masterMac));
    if (s_cfg.peerCount > 0) formatMac(s_cfg.peers[0], cf.masterMac);
    cf.channel = s_channel;
    cf.enabled = s_cfg.enabled ? 1 : 0;
    memcpy(payload, &cf, sizeof(cf));

    uint8_t frame[espnow_proto::kMaxFrameLen];
    size_t flen = espnow_proto::encodeFrame(
        static_cast<uint8_t>(espnow_proto::MsgType::TEL_CONFIG),
        espnow_proto::FLAG_IS_RESPONSE, nextSeq(), payload, sizeof(cf), frame, sizeof(frame));
    if (!flen) return;
    esp_now_send(mac, frame, flen);
}

void sendPong(const uint8_t mac[kMacLen], uint32_t echoTs) {
    if (!s_ready) return;
    uint8_t payload[sizeof(espnow_proto::PayloadPong)];
    espnow_proto::PayloadPong pg{};
    pg.timestamp = millis();
    pg.echo_timestamp = echoTs;
    memcpy(payload, &pg, sizeof(pg));

    uint8_t frame[espnow_proto::kMaxFrameLen];
    size_t flen = espnow_proto::encodeFrame(
        static_cast<uint8_t>(espnow_proto::MsgType::TEL_PONG),
        espnow_proto::FLAG_IS_RESPONSE, nextSeq(), payload, sizeof(pg), frame, sizeof(frame));
    if (!flen) return;
    esp_now_send(mac, frame, flen);
}

bool doInit() {
    if (s_ready) return true;
    if (!s_cfgValid || !s_cfg.enabled) { strlcpy(s_status, "disabled", sizeof(s_status)); return false; }
    uint8_t pmk[ESP_NOW_KEY_LEN], lmk[ESP_NOW_KEY_LEN];
    if (!parseHexKey(s_cfg.pmk, pmk) || !parseHexKey(s_cfg.lmk, lmk)) {
        strlcpy(s_status, "invalid pmk/lmk", sizeof(s_status));
        return false;
    }
    wifi_mode_t mode = WIFI_MODE_NULL;
    esp_wifi_get_mode(&mode);
    if (mode == WIFI_MODE_NULL) WiFi.mode(WIFI_STA);
    const bool wifiConnected = WiFi.status() == WL_CONNECTED;
    if (wifiConnected) s_channel = WiFi.channel();
    else esp_wifi_set_channel(s_channel, WIFI_SECOND_CHAN_NONE);

    esp_err_t err = esp_now_deinit();
    if (err != ESP_OK && err != ESP_ERR_ESPNOW_NOT_INIT) {
        snprintf(s_status, sizeof(s_status), "deinit err %d", (int)err);
        return false;
    }
    err = esp_now_init();
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

    uint8_t type, flags; uint16_t seq; const uint8_t* payload; size_t payloadLen;
    if (!espnow_proto::decodeFrame(p.data, p.len, type, flags, seq, payload, payloadLen)) {
        storage::logEvent("ESP-NOW bad frame from %s", mac);
        return;
    }

    bool needAck = (flags & espnow_proto::FLAG_ACK_REQUIRED) != 0;
    auto result = espnow_proto::ACK_OK;

    switch (static_cast<espnow_proto::MsgType>(type)) {
        case espnow_proto::MsgType::CMD_RELAY_SET: {
            if (payloadLen < sizeof(espnow_proto::PayloadRelaySet)) { result = espnow_proto::ACK_INVALID_PAYLOAD; break; }
            const auto* pl = reinterpret_cast<const espnow_proto::PayloadRelaySet*>(payload);
            if (pl->id < RELAY_COUNT) postRelay(1, pl->id, pl->on != 0);
            else result = espnow_proto::ACK_INVALID_PAYLOAD;
            break;
        }
        case espnow_proto::MsgType::CMD_RELAY_TOGGLE: {
            if (payloadLen < sizeof(espnow_proto::PayloadRelayToggle)) { result = espnow_proto::ACK_INVALID_PAYLOAD; break; }
            const auto* pl = reinterpret_cast<const espnow_proto::PayloadRelayToggle*>(payload);
            if (pl->id < RELAY_COUNT) postRelay(0, pl->id, false);
            else result = espnow_proto::ACK_INVALID_PAYLOAD;
            break;
        }
        case espnow_proto::MsgType::CMD_RELAYS_MASK: {
            if (payloadLen < sizeof(espnow_proto::PayloadRelaysMask)) { result = espnow_proto::ACK_INVALID_PAYLOAD; break; }
            const auto* pl = reinterpret_cast<const espnow_proto::PayloadRelaysMask*>(payload);
            for (uint8_t i = 0; i < RELAY_COUNT; i++) postRelay(1, i, (pl->mask >> i) & 1);
            break;
        }
        case espnow_proto::MsgType::CMD_STATUS_REQUEST: {
            sendTelemetry(p.mac);
            break;
        }
        case espnow_proto::MsgType::CMD_CONFIG_GET: {
            sendConfigResp(p.mac);
            break;
        }
        case espnow_proto::MsgType::CMD_CONFIG_SET: {
            if (payloadLen < sizeof(espnow_proto::PayloadConfigSet)) { result = espnow_proto::ACK_INVALID_PAYLOAD; break; }
            const auto* pl = reinterpret_cast<const espnow_proto::PayloadConfigSet*>(payload);
            storage::EspNowConfig ec = s_cfg;
            ec.enabled = pl->enabled != 0;
            ec.channel = pl->channel ? pl->channel : ESPNOW_CHANNEL;
            strlcpy(ec.pmk, pl->pmk, sizeof(ec.pmk));
            strlcpy(ec.lmk, pl->lmk, sizeof(ec.lmk));
            uint8_t mac[6];
            if (parseMac(pl->masterMac, mac)) {
                memcpy(ec.peers[0], mac, 6);
                ec.peerCount = 1;
            } else ec.peerCount = 0;
            s_cfg = ec;
            const bool ok = storage::saveEspNowConfig(ec);
            esp_now_deinit();
            s_ready = false;
            s_lastInitTry = 0;
            storage::logEvent("ESP-NOW config set from %s ok=%d ch=%u", mac, (int)ok, ec.channel);
            result = ok ? espnow_proto::ACK_OK : espnow_proto::ACK_ERR;
            break;
        }
        case espnow_proto::MsgType::CMD_BRIGHTNESS_SET: {
            if (payloadLen < sizeof(espnow_proto::PayloadBrightnessSet)) { result = espnow_proto::ACK_INVALID_PAYLOAD; break; }
            const auto* pl = reinterpret_cast<const espnow_proto::PayloadBrightnessSet*>(payload);
            hmi::setBrightness(pl->pct);
            storage::logEvent("ESP-NOW brightness %u from %s", pl->pct, mac);
            break;
        }
        case espnow_proto::MsgType::CMD_REBOOT: {
            storage::logEvent("ESP-NOW reboot from %s", mac);
            if (needAck) sendAck(p.mac, type, seq, espnow_proto::ACK_OK);
            esp_restart();
            break;
        }
        case espnow_proto::MsgType::CMD_PING: {
            uint32_t echoTs = 0;
            if (payloadLen >= sizeof(espnow_proto::PayloadPing)) {
                const auto* pl = reinterpret_cast<const espnow_proto::PayloadPing*>(payload);
                echoTs = pl->timestamp;
            }
            sendPong(p.mac, echoTs);
            break;
        }
        default: {
            result = espnow_proto::ACK_INVALID_TYPE;
            break;
        }
    }

    if (needAck && type != static_cast<uint8_t>(espnow_proto::MsgType::CMD_REBOOT)) {
        sendAck(p.mac, type, seq, result);
    }
}

void task(void*) {
    esp_task_wdt_add(nullptr);
    for (;;) {
        while (s_qTail != s_qHead) {
            portENTER_CRITICAL(&s_qMux);
            RxPkt p = s_q[s_qTail];
            s_qTail = (uint8_t)((s_qTail + 1) % kQueueSize);
            portEXIT_CRITICAL(&s_qMux);
            handlePacket(p);
        }
        esp_task_wdt_reset();
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
    if (s_cfg.peerCount == 0 ||
        (s_cfg.peerCount &&
         s_cfg.peers[0][0] == 0 && s_cfg.peers[0][1] == 0 && s_cfg.peers[0][2] == 0 &&
         s_cfg.peers[0][3] == 0 && s_cfg.peers[0][4] == 0 && s_cfg.peers[0][5] == 0)) {
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
    // Periodic telemetry to allowed peers (broadcast to first peer or allowlist)
    if (s_ready) {
        uint32_t now = millis();
        if (now - s_lastTelemetry >= kTelemetryMs) {
            s_lastTelemetry = now;
            // Send system status to first allowed peer (or broadcast if allowlist empty)
            if (s_cfg.peerCount > 0) {
                sendTelemetry(s_cfg.peers[0]);
            }
        }
        if (now - s_lastSensor >= kSensorMs) {
            s_lastSensor = now;
            if (s_cfg.peerCount > 0) {
                sendSensorData(s_cfg.peers[0]);
            }
        }
    }

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
    out.printf("espnow slave: %s rx=%lu tx=%lu fail=%lu drop=%lu peers=%u\n", s_status, (unsigned long)s_rxTotal.load(), (unsigned long)s_txTotal.load(), (unsigned long)s_txFailed.load(), (unsigned long)s_dropped.load(), peerCount());
    for (uint8_t i = 0; i < s_nodeCount; i++) out.printf("  %s rx=%lu txOk=%lu txFail=%lu age=%lus\n", s_nodes[i].mac, (unsigned long)s_nodes[i].rx, (unsigned long)s_nodes[i].txOk, (unsigned long)s_nodes[i].txFail, s_nodes[i].lastRxMs ? (unsigned long)((millis() - s_nodes[i].lastRxMs) / 1000) : 0UL);
}

}  // namespace espnow
