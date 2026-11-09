#pragma once
// ESP-NOW slave (4848S040 panel). Fixed channel, encrypted peers (PMK+LMK).
// Receives relay commands from master and posts to app queue.
// JSON: {"cmd":"relay","id":0,"on":true} {"cmd":"toggle","id":1} {"cmd":"relays","mask":5}
// Binary: [id,u8][on,u8]

#include <Arduino.h>
#include <stdint.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include "storage.h"

namespace espnow {

static constexpr size_t kMacLen = 6;
static constexpr size_t kMaxPayload = 200;

struct NodeState {
    char mac[18];
    int8_t rssi = 0;
    uint8_t channel = 0;
    uint32_t rx = 0;
    uint32_t txOk = 0;
    uint32_t txFail = 0;
    uint32_t lastRxMs = 0;
};

void setCommandQueue(QueueHandle_t q);  // receives espnow relay commands
void begin(const storage::EspNowConfig& cfg);
void loop();

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
