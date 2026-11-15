#pragma once
// ESP-NOW slave (4848S040 panel). Fixed channel, encrypted peers (PMK+LMK).
// Receives legacy binary commands and MCP JSON requests from the Master.
// MCP relay.set calls are queued to the app task and answered after GPIO/UI state changes.

#include <Arduino.h>
#include <stdint.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include "storage.h"

namespace espnow {

static constexpr size_t kMacLen = 6;
// ESP-NOW v1 application payload ceiling; keep in sync with the Nova master.
static constexpr size_t kMaxPayload = 250;

struct NodeState {
    char mac[18];
    int8_t rssi = 0;
    uint8_t channel = 0;
    uint32_t rx = 0;
    uint32_t txOk = 0;
    uint32_t txFail = 0;
    uint32_t lastRxMs = 0;
};

struct DeviceConfig {
    char room[32] = {};
    char relayNames[3][32] = {};
    bool customRoom = false;
};

// Queue item shared with the application task. MCP relay requests carry their
// peer and request id so the application can respond only after GPIO/UI state
// has been updated.
struct RelayCommand {
    uint8_t type = 1;  // 0=toggle, 1=set, 2=set all selected relays
    uint8_t index = 0; // zero-based relay index
    uint8_t mask = 0;
    bool on = false;
    bool mcpResponse = false;
    uint32_t requestId = 0;
    uint8_t peerMac[kMacLen] = {};
};

void setCommandQueue(QueueHandle_t q);  // receives espnow relay commands
void sendRelaySetResult(const uint8_t mac[kMacLen], uint32_t requestId,
                        uint8_t channel, bool state);
void sendRelayMaskSetResult(const uint8_t mac[kMacLen], uint32_t requestId,
                            uint8_t mask, bool state);
void begin(const storage::EspNowConfig& cfg);
void loop();
void loadDeviceConfig();
DeviceConfig getDeviceConfig();
bool setDeviceConfig(const char* room, const char* relay1,
                     const char* relay2, const char* relay3);
bool hasCustomRoomName();

bool send(const uint8_t mac[kMacLen], const uint8_t* data, size_t len);
bool sendHex(const char* macText, const char* hex);
bool addPeer(const uint8_t mac[kMacLen], uint8_t channel = 0);

bool ready();
const char* statusText();
uint8_t peerCount();
uint8_t nodeCount();
bool node(uint8_t i, NodeState& out);
void print(Print& out);

}  // namespace espnow
