# 5. Kết nối: Wi-Fi, MQTT, Tasmota, NTP, OTA

## 5.1 Wi-Fi (`src/wifi_manager.cpp`, chạy trong task `net`)

### Danh sách mạng đã biết (thử theo thứ tự)
1. Mạng đã kết nối thành công từ panel, lưu trong NVS (namespace `wifi`, khoá `ssid`, `pass`).
2. Danh sách `wifi` trong `/config.json` trên thẻ nhớ (tối đa 4 mạng).
3. `WIFI_SSID` / `WIFI_PASSWORD` trong `config.h` (hiện là `RD-SEAI_2.4G`, mạng mở).

Trùng SSID thì chỉ giữ lần xuất hiện đầu. Tối đa 6 mạng.

### Cài đặt radio
| Cài đặt | Lý do |
|---|---|
| `WiFi.setSleep(false)` | modem sleep làm rơi gói xác thực khi sóng yếu |
| `WiFi.setTxPower(WIFI_POWER_19_5dBm)` | công suất phát tối đa |
| `WiFi.setAutoReconnect(false)` | auto-reconnect của Arduino thử lại mỗi ~1 giây; router có thể bỏ qua MAC đó (lỗi `AUTH_EXPIRE` liên tục) |

### Kết nối tự động, có giãn cách
```
thử mạng N (12 s) ── OK ──▶ đã kết nối (khoảng chờ về lại 5 s)
      │ hỏng
      ▼
ngắt kết nối, chờ 5 s → 10 s → 20 s → 40 s → 60 s (tối đa) → thử mạng N+1 ...
```
Khi mất kết nối, board thử lại ngay rồi giãn dần như trên. Log:
`WiFi: trying <ssid>`, `WiFi: not connected, next try in 10s`.

### Quét (từ trang Wi-Fi hoặc lệnh `wifi`)
- Quét không chặn (`WiFi.scanNetworks(true)`), kết quả lấy trong `loop()`.
- Đang chưa kết nối thì ngắt lần thử dở dang trước khi quét, vì lần thử đó
  làm quét thất bại. Trong lúc quét không thử kết nối.
- Bỏ SSID ẩn, gộp SSID trùng (giữ mạng mạnh nhất), tối đa 20 mạng.

### Kết nối do người dùng chọn
- Timeout 15 giây. Gặp lỗi chắc chắn (`AUTH_FAIL`, handshake timeout,
  `NO_AP_FOUND`) thì báo ngay sau 3 giây.
- **Thành công:**
  - lưu SSID và mật khẩu vào NVS;
  - đưa mạng lên đầu danh sách đã biết;
  - ghi log `Wi-Fi joined from panel`.
- **Thất bại:** báo lý do lên màn hình (bảng ở 04-giao-dien-hmi.md), rồi quay
  lại thử các mạng đã biết.

### Mã lý do hay gặp
| Mã | Tên | Ý nghĩa thực tế trên board này |
|---|---|---|
| 2 | AUTH_EXPIRE | router không trả lời bước xác thực: sóng yếu hoặc router đang chặn MAC |
| 8 | ASSOC_LEAVE | chính board ngắt (khi đổi mạng / trước khi quét), bình thường |
| 39 | TIMEOUT | hết thời gian kết nối |
| 201 | NO_AP_FOUND | không thấy mạng: sai SSID hoặc quá xa router |
| 202 | AUTH_FAIL | sai mật khẩu, hoặc xác thực thất bại |

### Vấn đề còn tồn tại
Board thu sóng rất yếu (xem [01-phan-cung.md](01-phan-cung.md) mục 1.4). Lần
lượt đã quan sát:
- kết nối ngay lúc boot, RSSI -77;
- `AUTH_EXPIRE` liên tục trong 3 phút (khi còn auto-reconnect);
- `NO_AP_FOUND` sau khi đổi vị trí.

## 5.2 MQTT (task `net`)

| Cài đặt | Giá trị |
|---|---|
| Broker | `MQTT_BROKER:MQTT_PORT` trong `config.h` (`192.168.1.100:1883`, **chưa đúng mạng**), ghi đè được bằng `config.json` → `mqtt` |
| Client ID | `esp32-4848s040-<random hex>` |
| Keep-alive | 60 s |
| Timeout kết nối TCP | 1 s (`WiFiClient::setTimeout(1)`), để broker chết không làm đơ task |
| Thử lại | 5 s, nhân đôi, tối đa 60 s |
| LWT | `.../availability` = `offline` (retain) |

### Topic (`MQTT_TOPIC_BASE` = `smarthome/device/esp32-4848s040`)

| Topic | Hướng | Nội dung |
|---|---|---|
| `.../state` | panel → (retain) | trạng thái, gửi khi kết nối và mỗi lần relay đổi |
| `.../availability` | panel → (retain) | `online` / `offline` |
| `.../command` | → panel | lệnh relay / độ sáng |
| `.../sensor` | → panel | số liệu cảm biến thật (tắt giả lập) |
| `.../config` | → panel | có subscribe, **chưa xử lý** |
| `.../ota` | | **chưa cài đặt** |

**Lệnh (`.../command`):**
```json
{"relay": 0, "state": true}          // relay 0..2
{"relays": [true, false, true]}      // cả 3 relay
{"brightness": 200}                  // 0..255 (đổi sang %)
```

**Số liệu cảm biến (`.../sensor`):**
```json
{"primary": 26.4, "metric1": 62, "metric2": 412}
{"error": true}                      // chỉ số chính hiện ERROR
```
Payload đầu tiên tắt giả lập vĩnh viễn (đến lần khởi động sau). Quá 60 giây
(`HMI_SENSOR_STALE_MS`) không có dữ liệu thì chuyển sang `Stale`.

**Trạng thái (`.../state`):**
```json
{
  "device": "Smart Home Panel", "type": "panel",
  "fw_version": "1.1.0-ui", "hw_version": "1.0",
  "uptime": 3600, "free_heap": 86000, "min_free_heap": 60000,
  "wifi_rssi": -77, "ip": "172.28.182.195",
  "relays": [{"index": 0, "state": true, "last_change": 12}, ...]
}
```

## 5.3 Tasmota (`src/tasmota.cpp`, task `devices`)

Điều khiển qua **HTTP API của Tasmota**, không cần broker MQTT.

| Thao tác | Request | Trả về |
|---|---|---|
| Đọc trạng thái | `GET http://<host>/cm?cmnd=Power0` | `{"POWER1":"OFF","POWER2":"ON",...}` |
| Bật/tắt kênh n | `GET http://<host>/cm?cmnd=Power<n>%20TOGGLE` | `{"POWER<n>":"ON"}` |

Thiết bị chỉ có 1 relay trả về `"POWER"` thay vì `"POWER1"`; code xử lý cả hai.

| Thông số | Giá trị |
|---|---|
| Hỏi trạng thái | mỗi 3 s (`TASMOTA_POLL_MS`), mỗi lượt chỉ 1 request để lệnh bật/tắt được xử lý trước |
| Timeout HTTP | 1,5 s (kết nối + đọc) |
| Thiết bị không phản hồi | đánh dấu ngoại tuyến, thử lại mỗi 15 s |
| Log thẻ nhớ | `Tasmota <tên> (<ip>) online/offline`, `Tasmota <tên> ch<n> ON/OFF` |

### Thiết bị hiện có (người dùng cung cấp)
| Thiết bị | IP | Kênh | Ghi chú |
|---|---|---|---|
| Sonoff M5-3C | 172.28.182.3 | 3 (web UI có thêm T4) | **không phản hồi**, cả từ máy tính (13/10/2026) |
| TX Ultimate 4C-120 | 172.28.182.4 | 4 (+ `POWER5` = T5) | máy tính đọc được: Tasmota 15.5.0.2, `POWER1..4` OFF, `POWER5` ON |

`POWER5`/T5 (thường là đèn viền/LED của công tắc cảm ứng) cố ý không hiển
thị. Muốn hiện thì đặt `channels: 5`.

### Cấu hình
- **Mặc định** trong `config.h`: `TASMOTA_1_*`, `TASMOTA_2_*`.
- **Ghi đè** bằng `/config.json` (tối đa 4 thiết bị, mỗi thiết bị tối đa 8 kênh):

```json
"tasmota": [
  {"name": "Sonoff M5-3C", "host": "172.28.182.3", "channels": 3,
   "labels": ["Đèn trần", "Quạt", "Ổ cắm"]},
  {"name": "TX Ultimate 4C", "host": "172.28.182.4", "channels": 4}
]
```

`labels` để trống thì hiện "Kênh 1", "Kênh 2"… Tên thiết bị nên viết bằng chữ
ASCII, vì tiêu đề nhóm được in hoa bằng `toupper()`.

Lệnh serial `devices` in trạng thái:
```
  Sonoff M5-3C     172.28.182.3    OFFLINE mask=0x00
  TX Ultimate 4C   172.28.182.4    online  mask=0x00
```

## 5.4 NTP

Khi có Wi-Fi: `configTime(GMT+7, "pool.ntp.org", "time.google.com")`. Trước khi
đồng bộ, header hiện `--:--`, còn log thẻ nhớ dùng mốc `+<giây>s` thay cho
ngày giờ.

## 5.5 ArduinoOTA

Bật khi có Wi-Fi lần đầu. Hostname `esp32-4848s040`, cổng 3232, mật khẩu
`OTA_PASSWORD` (`admin`; nên đổi). Log in `OTA server at: esp32-4848s040.local:3232`.
