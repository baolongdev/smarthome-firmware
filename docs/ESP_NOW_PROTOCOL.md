# Giao thức ESP-NOW – Nova Smart Home Panel (Slave)

Tài liệu mô tả giao thức liên lạc giữa **Nova Smart Home Panel** (ESP32-S3, slave) và **thiết bị điều khiển khác** (master) qua ESP-NOW. Wi-Fi chỉ dùng để đồng bộ giờ (NTP). ESP-NOW là kênh điều khiển chính, chiếm ưu tiên.

---

## 1. Tổng quan kiến trúc

```
┌─────────────────┐         ESP-NOW (2.4 GHz, mã hóa PMK/LMK)         ┌─────────────────┐
│   MASTER        │  <================================================>  │   SLAVE (Panel)   │
│ (điều khiển)    │  CMD: relay, config, brightness, reboot, ping      │ (nova 4848s040)   │
│                 │  TEL: status, sensor, config, pong, ack            │                   │
└─────────────────┘                                                    └─────────────────┘
       ▲                                                                        ▲
       │                                                                        │
       │ NTP (Wi-Fi STA, chỉ khi có router)                                     │ OLED/LVGL HMI
       │                                                                        │ (hiển thị trạng thái)
       ▼                                                                        ▼
  pool.ntp.org /                                                         Cài đặt ESP-NOW trên thiết bị
  time.google.com                                                        (MAC, kênh, PMK/LMK)
```

### Vai trò
| Vai trò | Mô tả |
|---------|-------|
| **Master** | Thiết bị khác điều khiển (nút bấm, gateway, altra panel). Gửi lệnh, nhận telemetry. |
| **Slave** | Nova Panel. Nhận lệnh điều khiển relay, phản hồi trạng thái, gửi telemetry định kỳ. |

### Khung giao tiếp
- **Kênh**: ESP-NOW trên kênh 1–13 (mặc định 1). Khi Wi-Fi bật (NTP), ESP-NOW tự follows kênh của router.
- **Bảo mật**: PMK (Pairwise Master Key) + LMK (Link Master Key), khóa 16 byte hex. Mọi peer đều được thêm với mã hóa `encrypt = true`.
- **Peer allowlist**: Danh sách MAC master được phép. Nếu allowlist trống → chấp nhận mọi peer đã mã hóa.
- **Kích thước payload tối đa**: 200 bytes/frame (ESP-NOW limit ~250 byte, trừ phần header).

---

## 2. Định dạng khung dữ liệu (Frame)

Mọi khung ESP-NOW tuân thủ cấu trúc nhị phân sau (little-endian):

```
Offset  Kích thước  Trường         Mô tả
0       2           magic          0xAA 0x55 (dấu hiệu khung)
2       1           type           Loại tin nhắn (xem mục 3)
3       1           flags          Bit0: ack_required, Bit1: is_response
4       2           seq            Số thứ tự khung (tăng dần)
6       2           len            Độ dài payload (bytes)
8       len         payload        Dữ liệu chỉnh sửa theo loại
8+len   2           crc16          CRC16-CCITT (0xFFFF) tính từ byte 0 đến byte 8+len-1
```

### Ví dụ khung `CMD_RELAY_SET` (relay 0 = ON, seq=5, cần ack):
```
AA 55 01 01 05 00 02 00 00 01 XX XX
 ^ ^  ^  ^  ^  ^  ^  ^  ^  ^  ^
 | |  |  |  |  |  |  |  |  |  +-- CRC16 (2 byte)
 | |  |  |  |  |  |  |  |  +----- payload: id=0, on=1
 | |  |  |  |  |  |  |  +-------- len=2
 | |  |  |  |  |  |  +----------- seq=5
 | |  |  |  |  |  +-------------- flags=01 (ack_required)
 | |  |  |  |  +----------------- type=0x01 (CMD_RELAY_SET)
 | |  |  |  +-------------------- magic=0x55
 | |  |  +----------------------- magic=0xAA
 | |  +-------------------------- (padding/vị trí)
 +------------------------------- magic
```

### Kích thước khung tối thiểu (không payload): 10 byte (magic+type+flags+seq+len+crc).

---

## 3. Loại tin nhắn (Message Types)

### 3.1 Lệnh từ Master → Slave (type 0x01–0x0F)

| Giá trị | Tên | Mô tả | Payload |
|---------|-----|-------|---------|
| 0x01 | `CMD_RELAY_SET` | Bật/tắt một relay | `{uint8_t id; uint8_t on;}` |
| 0x02 | `CMD_RELAY_TOGGLE` | Đảo trạng thái relay | `{uint8_t id;}` |
| 0x03 | `CMD_RELAYS_MASK` | Bật/tắt nhiều relay cùng lúc | `{uint8_t mask;}` (bit i = relay i) |
| 0x04 | `CMD_STATUS_REQUEST` | Yêu cầu trạng thái hệ thống | rỗng |
| 0x05 | `CMD_CONFIG_GET` | Yêu cầu cấu hình ESP-NOW | rỗng |
| 0x06 | `CMD_CONFIG_SET` | Cập nhật cấu hình ESP-NOW | `{char masterMac[18]; uint8_t channel; char pmk[33]; char lmk[33]; uint8_t enabled;}` |
| 0x07 | `CMD_BRIGHTNESS_SET` | Đặt độ sáng màn hình | `{uint8_t pct;}` (0–100) |
| 0x08 | `CMD_REBOOT` | Khởi động lại thiết bị | rỗng |
| 0x09 | `CMD_PING` | Kiểm tra kết nối | `{uint32_t timestamp;}` |

### 3.2 Telemetry / Phản hồi từ Slave → Master (type 0x81–0x8F)

| Giá trị | Tên | Mô tả | Payload |
|---------|-----|-------|---------|
| 0x81 | `TEL_SYSTEM_STATUS` | Trạng thái hệ thống (phản hồi status request hoặc định kỳ) | `{uint8_t relay_mask; int8_t rssi; uint32_t uptime_s; uint32_t free_heap; uint8_t wifi_connected; uint8_t espnow_ready; float temp; float humidity; float power_w;}` |
| 0x82 | `TEL_SENSOR_DATA` | Dữ liệu cảm biến định kỳ | `{float temp; float humidity; float power_w;}` |
| 0x83 | `TEL_CONFIG` | Phản hồi config get | `{char masterMac[18]; uint8_t channel; uint8_t enabled;}` |
| 0x84 | `TEL_PONG` | Phản hồi ping | `{uint32_t timestamp; uint32_t echo_timestamp;}` |
| 0x85 | `TEL_ACK` | Xác nhận lệnh (nếu `ack_required`) | `{uint8_t type; uint8_t seq_hi; uint8_t seq_lo; uint8_t result;}` (result: 0=ok, 1=err) |

### 3.3 Flags

| Bit | Tên | Ý nghĩa |
|-----|-----|---------|
| 0 | `ACK_REQUIRED` | Slave phải phản hồi `TEL_ACK` sau khi xử lý |
| 1 | `IS_RESPONSE` | Đánh dấu khung là phản hồi (không yêu cầu ack tiếp) |

---

## 4. CRC16-CCITT

- Đa thức: `0x1021`
- Khởi tạo: `0xFFFF`
- Tính trên toàn bộ khung từ byte 0 đến cuối payload (trước trường crc16)
- Lưu dưới dạng little-endian (byte thấp trước)

Implement tham khảo (C):
```c
uint16_t crc16_ccitt(const uint8_t *data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int j = 0; j < 8; j++) {
            if (crc & 0x8000) crc = (crc << 1) ^ 0x1021;
            else crc = crc << 1;
        }
    }
    return crc;
}
```

---

## 5. Luồng xử lý (Sequence)

### 5.1 Điều khiển relay (Master → Slave)
```
Master: CMD_RELAY_SET {id=0, on=1, seq=10, ack_required=1}
        ──ESP-NOW──▶
Slave:  relay 0 bật
        ──ESP-NOW──▶ TEL_ACK {type=0x01, seq=10, result=0}
```

### 5.2 Yêu cầu trạng thái (Master → Slave)
```
Master: CMD_STATUS_REQUEST {seq=11}
        ──ESP-NOW──▶
Slave:  TEL_SYSTEM_STATUS {relay_mask=0b001, temp=25.5, humidity=58.0, power_w=180.0, ...}
```

### 5.3 Ping/Pong (kiểm tra đường nối)
```
Master: CMD_PING {timestamp=T1, seq=12}
        ──ESP-NOW──▶
Slave:  TEL_PONG {timestamp=T2 (slave nhận), echo_timestamp=T1}
```

### 5.4 Cấu hình ESP-NOW từ Master
```
Master: CMD_CONFIG_SET {masterMac="AA:BB:CC:DD:EE:FF", channel=1, pmk=..., lmk=..., enabled=1}
        ──ESP-NOW──▶
Slave:  lưu vào NVS /config.json, gọi esp_now_deinit() + esp_now_init() lại
        ──ESP-NOW──▶ TEL_ACK {type=0x06, result=0}
```

---

## 6. Truyền thông song song với Wi-Fi

| Tính năng | Kênh | Lưu ý |
|-----------|------|-------|
| Điều khiển relay | ESP-NOW | Chính, ưu tiên cao |
| Telemetry định kỳ | ESP-NOW | Gửi mỗi 5–10 giây |
| Đồng bộ giờ | Wi-Fi STA + NTP | Chỉ khi có router, `configTime(GMT+7)` |
| OTA firmware | HTTP qua Wi-Fi | Tùy chọn, cần `OTA_FIRMWARE_URL` |
| Cấu hình ban đầu | Wi-Fi scan/join | Chỉ lần đầu hoặc khi cần đổi mạng |

**Quy tắc đồngвис coexist**:
- Khi Wi-Fi **chưa kết nối**: ESP-NOW dùng kênh cố định (`ESPNOW_CHANNEL`, mặc định 1).
- Khi Wi-Fi **đã kết nối**: ESP-NOW tự chuyển sang kênh của router (`WiFi.channel()`) để tránh xung độtRadio.
- Không gọi `WiFi.disconnect()` chỉ vì ESP-NOW.

---

## 7. Lưu trữ cấu hình ESP-NOW

Cấu hình được lưu trong `/config.json` (SD card) và NVS (`espnow` namespace):

```json
{
  "espnow": {
    "enabled": true,
    "channel": 1,
    "pmk": "00112233445566778899AABBCCDDEEFF",
    "lmk": "00112233445566778899AABBCCDDEEFF",
    "peerCount": 1,
    "peers": [["AA","BB","CC","DD","EE","FF"]]
  }
}
```

Trường `peers` là danh sách MAC master được phép điều khiển. Nếu `peerCount = 0` → chấp nhận mọi peer đã mã hóa.

---

## 8. Mã lỗi (result trong TEL_ACK)

| Giá trị | Ý nghĩa |
|---------|---------|
| 0 | OK – lệnh đã xử lý thành công |
| 1 | ERR – lỗi không xác định |
| 2 | INVALID_TYPE – loại tin nhắn không hỗ trợ |
| 3 | INVALID_PAYLOAD – payload sai định dạng hoặc quá dài |
| 4 | CRC_ERROR – khung bị hỏng CRC |

---

## 9. Ví dụ mã C (Master gửi CMD_RELAY_SET)

```c
#include "espnow_proto.h"

uint8_t build_relay_set(uint8_t id, uint8_t on, uint16_t seq, uint8_t *out, size_t max) {
    EspNowFrame f = {};
    f.magic[0] = 0xAA; f.magic[1] = 0x55;
    f.type = CMD_RELAY_SET;
    f.flags = FLAG_ACK_REQUIRED;
    f.seq = seq;
    f.len = 2;
    f.payload[0] = id;
    f.payload[1] = on;
    f.crc = crc16_ccitt((uint8_t*)&f, 8 + f.len);
    size_t total = 8 + f.len + 2;
    if (total > max) return 0;
    memcpy(out, &f, total);
    return total;
}
```

---

## 10. Mở rộng trong tương lai

- Thêm loại tin nhắn mới chỉ cần gán giá trị `type` mới trong khoảng trống (0x10–0x7F cho lệnh, 0x90–0xEF cho telemetry).
- Thêm trường vào payload: tăng `len`, struct mới, Slave kiểm tra `len` trước khi đọc.
- Hỗ trợ fragmentation: nếu payload > 200 byte, chia thành nhiều khung có cùng `seq` và chỉ số đoạn (thêm trong `flags` hoặc reserved byte).
- Mã hóa end-to-end bổ sung ngoài ESP-NOW LMK nếu cần bảo mật cao hơn.

---

## 11. Tóm tắt cổng giao tiếp

| Kênh | Mục đích | Tần suất | Bảo mật |
|------|----------|----------|---------|
| ESP-NOW | Điều khiển + telemetry chính | Sự kiện + định kỳ 5s | PMK/LMK, peer allowlist |
| Wi-Fi STA | NTP giờ (GMT+7) | Khi kết nối, configTime | WPA2 router |
| HTTP (tùy chọn) | OTA firmware | Yêu cầu người dùng | URL server tuỳ chọn |
| Serial UART | Debug, config thủ công | Yêu cầu | Objekt cục bộ |
