#pragma once
// ESP-NOW protocol layer for Nova Smart Home Panel (slave).
// Frame format (little-endian):
//   [0..1]   magic 0xAA 0x55
//   [2]      type
//   [3]      flags  (bit0 ack_required, bit1 is_response)
//   [4..5]   seq
//   [6..7]   len
//   [8..]    payload (len bytes)
//   [8+len..] crc16 (CCITT 0x1021, init 0xFFFF, little-endian)

#include <Arduino.h>
#include <stdint.h>
#include <stddef.h>

namespace espnow_proto {

static constexpr uint8_t kMagic0 = 0xAA;
static constexpr uint8_t kMagic1 = 0x55;
static constexpr size_t kHeaderLen = 8;   // magic+type+flags+seq+len
static constexpr size_t kCrcLen = 2;      // crc16
static constexpr size_t kMinFrameLen = kHeaderLen + kCrcLen;  // 10
static constexpr size_t kMaxPayload = 200;
static constexpr size_t kMaxFrameLen = kHeaderLen + kMaxPayload + kCrcLen;

// Flags
static constexpr uint8_t FLAG_ACK_REQUIRED = 0x01;
static constexpr uint8_t FLAG_IS_RESPONSE = 0x02;

// Message types: master -> slave (0x01..0x0F), slave -> master (0x81..0x8F)
enum class MsgType : uint8_t {
    // Master -> Slave commands
    CMD_RELAY_SET = 0x01,        // {id, on}
    CMD_RELAY_TOGGLE = 0x02,     // {id}
    CMD_RELAYS_MASK = 0x03,      // {mask}
    CMD_STATUS_REQUEST = 0x04,   // {}
    CMD_CONFIG_GET = 0x05,       // {}
    CMD_CONFIG_SET = 0x06,       // {masterMac[18], channel, pmk[33], lmk[33], enabled}
    CMD_BRIGHTNESS_SET = 0x07,   // {pct}
    CMD_REBOOT = 0x08,           // {}
    CMD_PING = 0x09,             // {timestamp}

    // Slave -> Master telemetry / responses
    TEL_SYSTEM_STATUS = 0x81,    // {relay_mask, rssi, uptime_s, free_heap, wifi_connected, espnow_ready, temp, humidity, power_w}
    TEL_SENSOR_DATA = 0x82,      // {temp, humidity, power_w}
    TEL_CONFIG = 0x83,           // {masterMac[18], channel, enabled}
    TEL_PONG = 0x84,             // {timestamp, echo_timestamp}
    TEL_ACK = 0x85,              // {type, seq_hi, seq_lo, result}
};

// ACK result codes
static constexpr uint8_t ACK_OK = 0;
static constexpr uint8_t ACK_ERR = 1;
static constexpr uint8_t ACK_INVALID_TYPE = 2;
static constexpr uint8_t ACK_INVALID_PAYLOAD = 3;
static constexpr uint8_t ACK_CRC_ERROR = 4;

// Packed frame (little-endian, no padding)
struct alignas(1) Frame {
    uint8_t magic[2];      // 0xAA 0x55
    uint8_t type;
    uint8_t flags;
    uint8_t seq[2];        // little-endian
    uint8_t len[2];        // little-endian
    // payload follows (variable)
    // crc16 follows (little-endian)
} __attribute__((packed));

// Fixed payload structures (packed, little-endian)
struct alignas(1) PayloadRelaySet { uint8_t id; uint8_t on; } __attribute__((packed));
struct alignas(1) PayloadRelayToggle { uint8_t id; } __attribute__((packed));
struct alignas(1) PayloadRelaysMask { uint8_t mask; } __attribute__((packed));
struct alignas(1) PayloadConfigSet {
    char masterMac[18];
    uint8_t channel;
    char pmk[33];
    char lmk[33];
    uint8_t enabled;
} __attribute__((packed));
struct alignas(1) PayloadBrightnessSet { uint8_t pct; } __attribute__((packed));
struct alignas(1) PayloadPing { uint32_t timestamp; } __attribute__((packed));
struct alignas(1) PayloadSystemStatus {
    uint8_t relay_mask;
    int8_t rssi;
    uint32_t uptime_s;
    uint32_t free_heap;
    uint8_t wifi_connected;
    uint8_t espnow_ready;
    float temp;
    float humidity;
    float power_w;
} __attribute__((packed));
struct alignas(1) PayloadSensorData { float temp; float humidity; float power_w; } __attribute__((packed));
struct alignas(1) PayloadConfigResp {
    char masterMac[18];
    uint8_t channel;
    uint8_t enabled;
} __attribute__((packed));
struct alignas(1) PayloadPong { uint32_t timestamp; uint32_t echo_timestamp; } __attribute__((packed));
struct alignas(1) PayloadAck { uint8_t type; uint8_t seq_hi; uint8_t seq_lo; uint8_t result; } __attribute__((packed));

// CRC16-CCITT (init 0xFFFF, poly 0x1021)
uint16_t crc16(const uint8_t* data, size_t len);

// Validate magic + crc of a raw buffer. Returns payload length if valid, 0 otherwise.
size_t validateFrame(const uint8_t* buf, size_t bufLen);

// Encode a frame into out buffer. Returns total frame bytes written, or 0 if out too small.
size_t encodeFrame(uint8_t type, uint8_t flags, uint16_t seq, const uint8_t* payload, size_t payloadLen, uint8_t* out, size_t outMax);

// Decode a raw buffer into fields. Returns true if valid.
bool decodeFrame(const uint8_t* buf, size_t bufLen,
                 uint8_t& outType, uint8_t& outFlags, uint16_t& outSeq,
                 const uint8_t*& outPayload, size_t& outPayloadLen);

uint16_t readSeq(const uint8_t* buf);
uint16_t readLen(const uint8_t* buf);

}  // namespace espnow_proto
