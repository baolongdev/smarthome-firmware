#include "espnow_proto.h"

#include <string.h>

namespace espnow_proto {

uint16_t crc16(const uint8_t* data, size_t len) {
    uint16_t crc = 0xFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= static_cast<uint16_t>(data[i]) << 8;
        for (int b = 0; b < 8; b++) {
            if (crc & 0x8000u) crc = (crc << 1) ^ 0x1021u;
            else crc = crc << 1;
        }
    }
    return crc;
}

uint16_t readSeq(const uint8_t* buf) {
    return static_cast<uint16_t>(buf[4]) | (static_cast<uint16_t>(buf[5]) << 8);
}

uint16_t readLen(const uint8_t* buf) {
    return static_cast<uint16_t>(buf[6]) | (static_cast<uint16_t>(buf[7]) << 8);
}

size_t validateFrame(const uint8_t* buf, size_t bufLen) {
    if (bufLen < kMinFrameLen) return 0;
    if (buf[0] != kMagic0 || buf[1] != kMagic1) return 0;
    uint16_t len = readLen(buf);
    if (len > kMaxPayload) return 0;
    size_t total = kHeaderLen + len + kCrcLen;
    if (bufLen < total) return 0;
    uint16_t crcCalc = crc16(buf, kHeaderLen + len);
    uint16_t crcRecv = static_cast<uint16_t>(buf[kHeaderLen + len]) |
                       (static_cast<uint16_t>(buf[kHeaderLen + len + 1]) << 8);
    if (crcCalc != crcRecv) return 0;
    return len;
}

size_t encodeFrame(uint8_t type, uint8_t flags, uint16_t seq, const uint8_t* payload, size_t payloadLen, uint8_t* out, size_t outMax) {
    if (payloadLen > kMaxPayload) return 0;
    size_t total = kHeaderLen + payloadLen + kCrcLen;
    if (outMax < total) return 0;

    Frame* f = reinterpret_cast<Frame*>(out);
    f->magic[0] = kMagic0;
    f->magic[1] = kMagic1;
    f->type = type;
    f->flags = flags;
    f->seq[0] = static_cast<uint8_t>(seq & 0xFF);
    f->seq[1] = static_cast<uint8_t>((seq >> 8) & 0xFF);
    f->len[0] = static_cast<uint8_t>(payloadLen & 0xFF);
    f->len[1] = static_cast<uint8_t>((payloadLen >> 8) & 0xFF);

    if (payload && payloadLen) memcpy(out + kHeaderLen, payload, payloadLen);

    uint16_t crc = crc16(out, kHeaderLen + payloadLen);
    out[kHeaderLen + payloadLen] = static_cast<uint8_t>(crc & 0xFF);
    out[kHeaderLen + payloadLen + 1] = static_cast<uint8_t>((crc >> 8) & 0xFF);
    return total;
}

bool decodeFrame(const uint8_t* buf, size_t bufLen,
                 uint8_t& outType, uint8_t& outFlags, uint16_t& outSeq,
                 const uint8_t*& outPayload, size_t& outPayloadLen) {
    size_t payloadLen = validateFrame(buf, bufLen);
    if (payloadLen == 0) return false;
    outType = buf[2];
    outFlags = buf[3];
    outSeq = readSeq(buf);
    outPayload = buf + kHeaderLen;
    outPayloadLen = payloadLen;
    return true;
}

}  // namespace espnow_proto
