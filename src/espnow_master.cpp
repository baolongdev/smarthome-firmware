#include "espnow_master.h"
#include "espnow_proto.h"

#include <ArduinoJson.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_task_wdt.h>
#include <mbedtls/md.h>
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
#include "wifi_manager.h"

namespace espnow {
namespace {

constexpr uint8_t kMaxNodes = 8;
constexpr uint8_t kQueueSize = 32;
constexpr uint32_t kTelemetryMs = 5000;  // system status every 5 s
constexpr uint32_t kSensorMs = 10000;    // sensor data every 10 s
constexpr char kMasterDiscovery[] = "NOVA_DISCOVERY_V1";
constexpr char kSlaveDiscovery[] = "GUITION_SLAVE_V1";

storage::EspNowConfig s_cfg;
bool s_cfgValid = false;
bool s_ready = false;
uint32_t s_lastInitTry = 0;
uint8_t s_channel = ESPNOW_CHANNEL;
uint8_t s_discoveredMaster[kMacLen] = {};
bool s_hasDiscoveredMaster = false;
DeviceConfig s_deviceConfig{};
bool s_deviceConfigLoaded = false;
uint32_t s_lastDiscoveryReply = 0;
uint32_t s_lastChannelScan = 0;
uint32_t s_lastMasterRx = 0;
uint8_t s_scanChannel = 1;

portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
portMUX_TYPE s_qMux = portMUX_INITIALIZER_UNLOCKED;

struct RxPkt {
    uint8_t mac[kMacLen];
    uint8_t data[kMaxPayload];
    uint16_t len = 0;
    uint8_t channel = 1;
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

bool validDeviceLabel(const char* text) {
    if (!text || !text[0]) return false;
    const size_t length = strnlen(text, 32);
    if (length == 0 || length >= 32) return false;
    for (size_t i = 0; i < length; ++i) {
        const uint8_t c = static_cast<uint8_t>(text[i]);
        if (c < 0x20 || c == 0x7f) return false;
    }
    return true;
}

void copyDeviceConfigToModel() {
    hmi::setRoomName(s_deviceConfig.room);
}

bool saveDeviceConfig() {
    Preferences prefs;
    if (!prefs.begin("devicecfg", false)) return false;
    bool ok = prefs.putString("room", s_deviceConfig.room) > 0;
    for (uint8_t i = 0; i < 3; ++i) {
        char key[8];
        snprintf(key, sizeof(key), "relay%u", static_cast<unsigned>(i + 1));
        ok = prefs.putString(key, s_deviceConfig.relayNames[i]) > 0 && ok;
    }
    prefs.end();
    return ok;
}
bool makeDiscoveryBeacon(const char* label, const char* keyText, uint8_t* out, size_t cap,
                         size_t& length) {
    const size_t labelLen = strlen(label);
    if (labelLen + 8 > cap) return false;
    uint8_t key[ESP_NOW_KEY_LEN], digest[32];
    const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!parseHexKey(keyText, key) || !md) return false;
    mbedtls_md_context_t context;
    mbedtls_md_init(&context);
    const int result = mbedtls_md_setup(&context, md, 1) == 0 &&
                       mbedtls_md_hmac_starts(&context, key, sizeof(key)) == 0 &&
                       mbedtls_md_hmac_update(&context,
                           reinterpret_cast<const uint8_t*>(label), labelLen) == 0 &&
                       mbedtls_md_hmac_finish(&context, digest) == 0;
    mbedtls_md_free(&context);
    if (!result) return false;
    memcpy(out, label, labelLen);
    memcpy(out + labelLen, digest, 8);
    length = labelLen + 8;
    return true;
}
bool verifyDiscoveryBeacon(const uint8_t* data, size_t len, const char* label,
                           const char* keyText) {
    uint8_t expected[64]; size_t expectedLen = 0;
    return makeDiscoveryBeacon(label, keyText, expected, sizeof(expected), expectedLen) &&
           len == expectedLen && memcmp(data, expected, expectedLen) == 0;
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
    return s_cfg.peerCount == 0 && s_hasDiscoveredMaster &&
           memcmp(s_discoveredMaster, mac, kMacLen) == 0;
}

bool postRelay(uint8_t type, int id, bool on) {
    if (id < 0 || id >= RELAY_COUNT || !s_cmdQ) return false;
    RelayCommand c{};
    c.type = type;
    c.index = static_cast<uint8_t>(id);
    c.on = on;
    return xQueueSend(s_cmdQ, &c, 0) == pdTRUE;
}

void IRAM_ATTR onRecv(const uint8_t* mac, const uint8_t* data, int len) {
    if (!mac || !data || len <= 0) return;
    if (static_cast<size_t>(len) > kMaxPayload) {
        s_dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    size_t n = static_cast<size_t>(len);
    portENTER_CRITICAL_ISR(&s_qMux);
    uint8_t next = (uint8_t)((s_qHead + 1) % kQueueSize);
    if (next == s_qTail) { s_dropped.fetch_add(1, std::memory_order_relaxed); portEXIT_CRITICAL_ISR(&s_qMux); return; }
    RxPkt& p = s_q[s_qHead];
    memcpy(p.mac, mac, kMacLen);
    memcpy(p.data, data, n);
    p.len = (uint16_t)n;
    // The slave scans one channel at a time while offline; preserve the tuned
    // channel so the worker can pin itself to the channel that heard a beacon.
    p.channel = s_channel;
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
    (void)channel;
    static const uint8_t bcast[kMacLen] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    if (memcmp(mac, bcast, kMacLen) == 0) return false;
    uint8_t lmk[ESP_NOW_KEY_LEN];
    if (!parseHexKey(s_cfg.lmk, lmk)) return false;
    esp_now_peer_info_t peer{};
    memcpy(peer.peer_addr, mac, kMacLen);
    memcpy(peer.lmk, lmk, ESP_NOW_KEY_LEN);
    // Bind to the radio's current home channel. The slave scans while offline,
    // so peers must not retain the startup channel after a scan hop.
    peer.channel = 0;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = true;
    esp_err_t e = esp_now_add_peer(&peer);
    if (e == ESP_OK || e == ESP_ERR_ESPNOW_EXIST) { findNode(mac, true); return true; }
    char m[18]; formatMac(mac, m); Serial.printf("[espnow] add peer %s err %d\n", m, (int)e);
    return false;
}

bool addBroadcastPeer() {
    static const uint8_t broadcast[kMacLen] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    esp_now_peer_info_t peer{};
    memcpy(peer.peer_addr, broadcast, kMacLen);
    peer.channel = 0;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;
    const esp_err_t err = esp_now_add_peer(&peer);
    return err == ESP_OK || err == ESP_ERR_ESPNOW_EXIST;
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

bool sendMcpPayload(const uint8_t mac[kMacLen], const char* data, size_t len) {
    return s_ready && mac && data && len > 0 && len <= kMaxPayload &&
           esp_now_send(mac, reinterpret_cast<const uint8_t*>(data), len) == ESP_OK;
}

void sendMcpError(const uint8_t mac[kMacLen], uint32_t requestId,
                  const char* method, int code, const char* message) {
    char response[kMaxPayload + 1];
    const int len = snprintf(response, sizeof(response),
        "{\"v\":1,\"type\":\"mcp_response\",\"method\":\"%s\",\"id\":%lu,\"error\":{\"code\":%d,\"message\":\"%s\"}}",
        method, static_cast<unsigned long>(requestId), code, message);
    if (len > 0 && static_cast<size_t>(len) <= kMaxPayload) {
        sendMcpPayload(mac, response, static_cast<size_t>(len));
    }
}

bool sendMcpToolsList(const uint8_t mac[kMacLen], uint32_t requestId) {
    // Compact manifest fits ESP-NOW v1's 250-byte payload. Device labels are
    // writable through `configure`; board/radio identity stays read-only.
    char response[kMaxPayload + 1];
    const int len = snprintf(response, sizeof(response),
        "{\"v\":1,\"type\":\"mcp_response\",\"method\":\"tools/list\",\"id\":%lu,\"result\":{\"tools\":[{\"name\":\"device\",\"inputSchema\":{\"properties\":{\"op\":{\"enum\":[\"info\",\"relays\",\"set\",\"config\",\"configure\"]}}}}]}}",
        static_cast<unsigned long>(requestId));
    return len > 0 && static_cast<size_t>(len) <= kMaxPayload &&
           sendMcpPayload(mac, response, static_cast<size_t>(len));
}

void sendMcpDeviceQuery(const uint8_t mac[kMacLen], uint32_t requestId,
                       const char* operation) {
    JsonDocument response;
    response["v"] = 1;
    response["type"] = "mcp_response";
    response["method"] = "tools/call";
    response["id"] = requestId;
    JsonObject result = response["result"].to<JsonObject>();
    hmi::ModelLock lock;
    hmi::Model& model = hmi::model();
    if (strcmp(operation, "info") == 0) {
        JsonObject device = result["device"].to<JsonObject>();
        device["board"] = "ESP32-4848S040";
        device["name"] = s_deviceConfig.room;
        device["role"] = "slave";
        device["channel"] = s_channel;
        const bool wifiConnected = WiFi.status() == WL_CONNECTED;
        device["wifi_connected"] = wifiConnected;
        device["rssi_dbm"] = wifiConnected ? WiFi.RSSI() : 0;
        device["uptime_s"] = millis() / 1000;
#if HMI_SIMULATE_SENSORS
        device["sensor_data"] = "simulated";
#else
        device["sensor_data"] = "hardware";
#endif
    } else if (strcmp(operation, "relays") == 0) {
        JsonArray relays = result["relays"].to<JsonArray>();
        for (uint8_t i = 0; i < RELAY_COUNT; ++i) {
            JsonObject relay = relays.add<JsonObject>();
            relay["channel"] = i + 1;
            relay["name"] = s_deviceConfig.relayNames[i];
            relay["on"] = model.relay[i];
        }
    } else {
        JsonObject config = result["config"].to<JsonObject>();
        config["room"] = s_deviceConfig.room;
        JsonArray names = config["relay_names"].to<JsonArray>();
        for (uint8_t i = 0; i < RELAY_COUNT; ++i) names.add(s_deviceConfig.relayNames[i]);
    }

    char body[kMaxPayload + 1];
    const size_t bodyLength = measureJson(response);
    if (bodyLength > kMaxPayload) {
        sendMcpError(mac, requestId, "tools/call", -32001,
                     "device response exceeds ESP-NOW payload limit");
        return;
    }
    const size_t written = serializeJson(response, body, sizeof(body));
    if (written == bodyLength) sendMcpPayload(mac, body, written);
}

bool queueMcpRelaySet(const uint8_t mac[kMacLen], uint32_t requestId,
                      uint8_t channel, bool state) {
    if (!s_cmdQ || channel == 0 || channel > RELAY_COUNT) return false;
    RelayCommand command{};
    command.type = 1;
    command.index = static_cast<uint8_t>(channel - 1);
    command.on = state;
    command.mcpResponse = true;
    command.requestId = requestId;
    memcpy(command.peerMac, mac, kMacLen);
    return xQueueSend(s_cmdQ, &command, 0) == pdTRUE;
}

bool queueMcpRelayMaskSet(const uint8_t mac[kMacLen], uint32_t requestId,
                          uint8_t mask, bool state) {
    if (!s_cmdQ || mask == 0 || (mask & ~((1u << RELAY_COUNT) - 1u)) != 0) return false;
    RelayCommand command{};
    command.type = 2;
    command.mask = mask;
    command.on = state;
    command.mcpResponse = true;
    command.requestId = requestId;
    memcpy(command.peerMac, mac, kMacLen);
    return xQueueSend(s_cmdQ, &command, 0) == pdTRUE;
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
    cf.masterMac[0] = '\0';
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

void handleMcpPacket(const RxPkt& packet) {
    JsonDocument request;
    const DeserializationError parseError =
        deserializeJson(request, packet.data, packet.len);
    if (parseError) {
        storage::logEvent("ESP-NOW invalid MCP JSON");
        return;
    }

    if (!request["id"].is<uint32_t>()) return;
    const uint32_t requestId = request["id"].as<uint32_t>();
    const char* type = request["type"] | "";
    const char* method = request["method"] | "";
    const char* responseMethod = strcmp(method, "tools/list") == 0 ? "tools/list" :
                                 strcmp(method, "tools/call") == 0 ? "tools/call" : "unknown";
    if (!request["v"].is<int>() || request["v"].as<int>() != 1 ||
        strcmp(type, "mcp_request") != 0) {
        sendMcpError(packet.mac, requestId, responseMethod, -32600, "invalid request");
        return;
    }

    if (strcmp(method, "tools/list") == 0) {
        char peer[18];
        formatMac(packet.mac, peer);
        const bool sent = sendMcpToolsList(packet.mac, requestId);
        Serial.printf("[espnow] tools/list id=%lu from %s ch%u reply=%s\n",
                      static_cast<unsigned long>(requestId), peer, s_channel,
                      sent ? "sent" : "send-failed");
        return;
    }
    if (strcmp(method, "tools/call") != 0) {
        sendMcpError(packet.mac, requestId, responseMethod, -32601, "method not found");
        return;
    }

    const JsonObjectConst params = request["params"].as<JsonObjectConst>();
    if (params.isNull() || !params["name"].is<const char*>()) {
        sendMcpError(packet.mac, requestId, responseMethod, -32602, "invalid params");
        return;
    }
    const char* name = params["name"].as<const char*>();
    const JsonObjectConst arguments = params["arguments"].as<JsonObjectConst>();
    const char* operation = arguments["op"] | "";
    if (strcmp(name, "device") == 0 &&
        (strcmp(operation, "info") == 0 || strcmp(operation, "relays") == 0 ||
         strcmp(operation, "config") == 0)) {
        sendMcpDeviceQuery(packet.mac, requestId, operation);
        return;
    }
    if (strcmp(name, "device") == 0 && strcmp(operation, "configure") == 0) {
        const char* room = arguments["room"].is<const char*>() ? arguments["room"].as<const char*>() : nullptr;
        const char* relay1 = arguments["relay1"].is<const char*>() ? arguments["relay1"].as<const char*>() : nullptr;
        const char* relay2 = arguments["relay2"].is<const char*>() ? arguments["relay2"].as<const char*>() : nullptr;
        const char* relay3 = arguments["relay3"].is<const char*>() ? arguments["relay3"].as<const char*>() : nullptr;
        if (!setDeviceConfig(room, relay1, relay2, relay3)) {
            sendMcpError(packet.mac, requestId, responseMethod, -32602,
                         "Cấu hình rỗng, quá dài hoặc không lưu được");
            return;
        }
        sendMcpDeviceQuery(packet.mac, requestId, "config");
        return;
    }
    if (strcmp(name, "device") != 0 && strcmp(name, "relay.set") != 0) {
        sendMcpError(packet.mac, requestId, responseMethod, -32601, "tool not found");
        return;
    }

    if (strcmp(name, "device") == 0 && strcmp(operation, "set") != 0) {
        sendMcpError(packet.mac, requestId, responseMethod, -32602,
                     "op phải là info, relays, set, config hoặc configure");
        return;
    }
    if (arguments.isNull() || !arguments["state"].is<bool>()) {
        sendMcpError(packet.mac, requestId, responseMethod, -32602, "state boolean required");
        return;
    }
    if (strcmp(name, "device") == 0 && arguments["channel"].isUnbound()) {
        const uint8_t allRelays = static_cast<uint8_t>((1u << RELAY_COUNT) - 1u);
        if (!queueMcpRelayMaskSet(packet.mac, requestId, allRelays,
                                  arguments["state"].as<bool>())) {
            sendMcpError(packet.mac, requestId, responseMethod, -32000, "relay queue full");
        }
        return;
    }
    if (!arguments["channel"].is<int>()) {
        sendMcpError(packet.mac, requestId, responseMethod, -32602,
                     "channel must be an integer from 1 to 3");
        return;
    }
    const int channel = arguments["channel"].as<int>();
    if (channel < 1 || channel > RELAY_COUNT) {
        sendMcpError(packet.mac, requestId, responseMethod, -32602,
                     "channel out of range");
        return;
    }

    if (!queueMcpRelaySet(packet.mac, requestId, static_cast<uint8_t>(channel),
                          arguments["state"].as<bool>())) {
        sendMcpError(packet.mac, requestId, responseMethod, -32000, "relay queue full");
    }
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
    addBroadcastPeer();
    s_ready = true;
    snprintf(s_status, sizeof(s_status), "slave ready ch%u peers%u%s", s_channel, added, wifiConnected ? " (wifi)" : "");
    storage::logEvent("ESP-NOW slave ready ch%u peers=%u wifi=%d", s_channel, added, wifiConnected);
    Serial.printf("[espnow] %s\n", s_status);
    return true;
}

void handlePacket(const RxPkt& p) {
    char mac[18]; formatMac(p.mac, mac);
    // Discovery carries no controls or credentials. Learn the Master address;
    // all application traffic remains on encrypted unicast.
    if (verifyDiscoveryBeacon(p.data, p.len, kMasterDiscovery, s_cfg.pmk)) {
        const bool wifiConnected = WiFi.status() == WL_CONNECTED;
        if (!wifiConnected && p.channel >= 1 && p.channel <= 13 && p.channel != s_channel) {
            s_channel = p.channel;
            esp_wifi_set_channel(s_channel, WIFI_SECOND_CHAN_NONE);
        }
        if (addPeerInternal(p.mac, s_channel)) {
            const bool firstDiscovery = !s_hasDiscoveredMaster ||
                                        memcmp(s_discoveredMaster, p.mac, kMacLen) != 0;
            memcpy(s_discoveredMaster, p.mac, kMacLen);
            s_hasDiscoveredMaster = true;
            s_lastMasterRx = millis();
            // Auto-discovery originally learned the master's MAC only in RAM.
            // Pin the first discovered master in the persisted ESP-NOW config
            // so the encrypted peer is restored on the next boot as well.
            if (s_cfg.peerCount == 0) {
                memcpy(s_cfg.peers[0], p.mac, kMacLen);
                s_cfg.peerCount = 1;
                s_cfg.channel = s_channel;
                const bool saved = storage::saveEspNowConfig(s_cfg);
                storage::logEvent("ESP-NOW master MAC %s saved=%d", mac, saved);
                Serial.printf("[espnow] saved master MAC %s (persisted=%d)\n", mac, saved);
            }
            Serial.printf("[espnow] master %s discovered on channel %u\n", mac, s_channel);
            // Reply at least once per master beacon window. A 15-second
            // throttle made a rebooted master wait almost that long to
            // rediscover an already-paired slave.
            if (firstDiscovery || millis() - s_lastDiscoveryReply >= 3000) {
                static const uint8_t broadcast[kMacLen] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
                uint8_t reply[64]; size_t replyLen = 0;
                if (makeDiscoveryBeacon(kSlaveDiscovery, s_cfg.pmk, reply, sizeof(reply), replyLen))
                    esp_now_send(broadcast, reply, replyLen);
                s_lastDiscoveryReply = millis();
                if (firstDiscovery) storage::logEvent("ESP-NOW master auto-discovered %s", mac);
            }
        }
        return;
    }
    if (!peerAllowed(p.mac)) { storage::logEvent("ESP-NOW ignored %s", mac); return; }
    s_lastMasterRx = millis();
    portENTER_CRITICAL(&s_mux);
    NodeState* n = findNode(p.mac, true);
    if (n) { n->channel = s_channel; n->rx++; n->lastRxMs = millis(); }
    portEXIT_CRITICAL(&s_mux);

    // Preserve the existing binary command protocol while accepting the
    // compact JSON MCP envelope used by Nova's espnow.call tool.
    if (p.len > 0 && p.data[0] == '{') {
        handleMcpPacket(p);
        return;
    }

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
            if (pl->id < RELAY_COUNT) {
                if (!postRelay(1, pl->id, pl->on != 0)) result = espnow_proto::ACK_ERR;
            }
            else result = espnow_proto::ACK_INVALID_PAYLOAD;
            break;
        }
        case espnow_proto::MsgType::CMD_RELAY_TOGGLE: {
            if (payloadLen < sizeof(espnow_proto::PayloadRelayToggle)) { result = espnow_proto::ACK_INVALID_PAYLOAD; break; }
            const auto* pl = reinterpret_cast<const espnow_proto::PayloadRelayToggle*>(payload);
            if (pl->id < RELAY_COUNT) {
                if (!postRelay(0, pl->id, false)) result = espnow_proto::ACK_ERR;
            }
            else result = espnow_proto::ACK_INVALID_PAYLOAD;
            break;
        }
        case espnow_proto::MsgType::CMD_RELAYS_MASK: {
            if (payloadLen < sizeof(espnow_proto::PayloadRelaysMask)) { result = espnow_proto::ACK_INVALID_PAYLOAD; break; }
            const auto* pl = reinterpret_cast<const espnow_proto::PayloadRelaysMask*>(payload);
            for (uint8_t i = 0; i < RELAY_COUNT; i++) {
                if (!postRelay(1, i, (pl->mask >> i) & 1)) result = espnow_proto::ACK_ERR;
            }
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
        // A disconnected slave has no Wi-Fi channel to follow. Sweep channels
        // until the master's authenticated beacon is heard, then stay pinned
        // there for encrypted MCP requests and responses.
        const wl_status_t wifiStatus = WiFi.status();
        const bool wifiDisconnected = wifiStatus == WL_DISCONNECTED ||
                                      wifiStatus == WL_NO_SSID_AVAIL ||
                                      wifiStatus == WL_CONNECT_FAILED;
        if (s_ready && wifiDisconnected && !wifimgr::busy() &&
            WiFi.scanComplete() != WIFI_SCAN_RUNNING) {
            const uint32_t now = millis();
            // The master broadcasts its discovery beacon every 3 seconds. A
            // short timeout lets a slave follow the master's Wi-Fi channel
            // change quickly, without waiting for the old channel to recover.
            if (s_hasDiscoveredMaster && now - s_lastMasterRx > 6000) {
                s_hasDiscoveredMaster = false;
                s_lastDiscoveryReply = 0;
                s_scanChannel = s_channel;
                storage::logEvent("ESP-NOW master idle; resuming channel scan");
            }
            if (!s_hasDiscoveredMaster && now - s_lastChannelScan >= 250) {
                s_scanChannel = (s_scanChannel % 13) + 1;
                const esp_err_t channelError = esp_wifi_set_channel(
                    s_scanChannel, WIFI_SECOND_CHAN_NONE);
                if (channelError == ESP_OK) {
                    s_channel = s_scanChannel;
                } else {
                    Serial.printf("[espnow] channel scan set %u failed: %s\n",
                                  s_scanChannel, esp_err_to_name(channelError));
                }
                s_lastChannelScan = now;
            }
        }
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

}  // namespace

void loadDeviceConfig() {
    if (s_deviceConfigLoaded) return;
    strlcpy(s_deviceConfig.room, HMI_ROOM_NAME, sizeof(s_deviceConfig.room));
    const char* defaults[3] = {RELAY_NAME_1, RELAY_NAME_2, RELAY_NAME_3};
    for (uint8_t i = 0; i < 3; ++i)
        strlcpy(s_deviceConfig.relayNames[i], defaults[i], sizeof(s_deviceConfig.relayNames[i]));
    Preferences prefs;
    if (prefs.begin("devicecfg", true)) {
        prefs.getString("room", s_deviceConfig.room, sizeof(s_deviceConfig.room));
        for (uint8_t i = 0; i < 3; ++i) {
            char key[8];
            snprintf(key, sizeof(key), "relay%u", static_cast<unsigned>(i + 1));
            prefs.getString(key, s_deviceConfig.relayNames[i], sizeof(s_deviceConfig.relayNames[i]));
        }
        prefs.end();
    }
    s_deviceConfig.customRoom = strcmp(s_deviceConfig.room, HMI_ROOM_NAME) != 0;
    s_deviceConfigLoaded = true;
    copyDeviceConfigToModel();
}

DeviceConfig getDeviceConfig() {
    loadDeviceConfig();
    return s_deviceConfig;
}

bool setDeviceConfig(const char* room, const char* relay1,
                     const char* relay2, const char* relay3) {
    loadDeviceConfig();
    const char* values[4] = {room, relay1, relay2, relay3};
    for (const char* value : values) {
        if (value != nullptr && !validDeviceLabel(value)) return false;
    }
    const DeviceConfig previous = s_deviceConfig;
    if (room) strlcpy(s_deviceConfig.room, room, sizeof(s_deviceConfig.room));
    if (relay1) strlcpy(s_deviceConfig.relayNames[0], relay1, sizeof(s_deviceConfig.relayNames[0]));
    if (relay2) strlcpy(s_deviceConfig.relayNames[1], relay2, sizeof(s_deviceConfig.relayNames[1]));
    if (relay3) strlcpy(s_deviceConfig.relayNames[2], relay3, sizeof(s_deviceConfig.relayNames[2]));
    s_deviceConfig.customRoom = strcmp(s_deviceConfig.room, HMI_ROOM_NAME) != 0;
    if (!saveDeviceConfig()) {
        s_deviceConfig = previous;
        return false;
    }
    copyDeviceConfigToModel();
    storage::logEvent("ESP-NOW device labels updated: room=%s", s_deviceConfig.room);
    return true;
}

bool hasCustomRoomName() {
    loadDeviceConfig();
    return s_deviceConfig.customRoom;
}

void sendRelaySetResult(const uint8_t mac[kMacLen], uint32_t requestId,
                        uint8_t channel, bool state) {
    char response[kMaxPayload + 1];
    const int len = snprintf(response, sizeof(response),
        "{\"v\":1,\"type\":\"mcp_response\",\"method\":\"tools/call\",\"id\":%lu,\"result\":{\"ok\":true,\"channel\":%u,\"state\":%s}}",
        static_cast<unsigned long>(requestId), static_cast<unsigned>(channel),
        state ? "true" : "false");
    if (len > 0 && static_cast<size_t>(len) <= kMaxPayload &&
        !sendMcpPayload(mac, response, static_cast<size_t>(len))) {
        storage::logEvent("ESP-NOW MCP response send failed id=%lu",
                          static_cast<unsigned long>(requestId));
    }
}

void sendRelayMaskSetResult(const uint8_t mac[kMacLen], uint32_t requestId,
                            uint8_t mask, bool state) {
    char response[kMaxPayload + 1];
    const int len = snprintf(response, sizeof(response),
        "{\"v\":1,\"type\":\"mcp_response\",\"method\":\"tools/call\",\"id\":%lu,\"result\":{\"ok\":true,\"channels_mask\":%u,\"state\":%s}}",
        static_cast<unsigned long>(requestId), static_cast<unsigned>(mask),
        state ? "true" : "false");
    if (len > 0 && static_cast<size_t>(len) <= kMaxPayload &&
        !sendMcpPayload(mac, response, static_cast<size_t>(len))) {
        storage::logEvent("ESP-NOW MCP bulk relay response send failed id=%lu",
                          static_cast<unsigned long>(requestId));
    }
}

void setCommandQueue(QueueHandle_t q) { s_cmdQ = q; }

void begin(const storage::EspNowConfig& cfg) {
    s_cfg = cfg;
    s_cfgValid = true;
    s_channel = s_cfg.channel ? s_cfg.channel : ESPNOW_CHANNEL;
    s_hasDiscoveredMaster = false;
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
            if (s_cfg.peerCount > 0) {
                sendTelemetry(s_cfg.peers[0]);
            } else if (s_hasDiscoveredMaster) sendTelemetry(s_discoveredMaster);
        }
        if (now - s_lastSensor >= kSensorMs) {
            s_lastSensor = now;
            if (s_cfg.peerCount > 0) {
                sendSensorData(s_cfg.peers[0]);
            } else if (s_hasDiscoveredMaster) sendSensorData(s_discoveredMaster);
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
