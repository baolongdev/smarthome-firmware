# 5. Kết nối: Wi-Fi, MQTT, Tasmota, NTP, OTA

## 5.1 Wi-Fi (`src/wifi_manager.cpp`, chạy trong task `net`)

### Danh sách mạng đã biết (thử theo thứ tự)
1. Mạng đã kết nối thành công từ panel, lưu trong NVS (namespace `wifi`, khoá `ssid`, `pass`).
2. Danh sách `wifi` trong `/config.json` trên thẻ nhớ (tối đa 4 mạng).
3. `WIFI_SSID` / `WIFI_PASSWORD` trong `config.h` (hiện là `RD-SEAI_2.4G`, mạng mở).

Trùng SSID thì chỉ giữ lần xuất hiện đầu. Tối đa 6 mạng.

**Mạng mở:** `connectTo()` không gửi mật khẩu cho SSID mà lần quét gần nhất
thấy là mạng mở (`isOpen()`). Gửi mật khẩu đã lưu cho một router mở thì ESP32
chỉ chấp nhận AP có mã hoá và router không bao giờ trả lời (`AUTH_EXPIRE`).
Đây là lỗi đã gặp khi RD-SEAI_2.4G bỏ mật khẩu mà `config.h` vẫn giữ mật khẩu cũ.

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

### Quét (lúc khởi động, từ trang Wi-Fi hoặc lệnh `wifi`)
- **Lúc khởi động** quét đồng bộ một lần (khoảng 2 s) trước lần kết nối đầu,
  để biết mạng nào đang mở. Log in mọi mạng panel nghe được, kèm RSSI tại chỗ
  đặt panel:
  ```
  WiFi:    -57 dBm ch1  WPA  RFThings VietNam
  WiFi:    -77 dBm ch1  open RD-SEAI_2.4G
  ```
- Các lần sau quét không chặn (`WiFi.scanNetworks(true)`), kết quả lấy trong `loop()`.
- Không dùng `WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN)` / `setSortMethod()`:
  trên Arduino-ESP32 2.0.17 chúng làm lần quét bất đồng bộ không bao giờ xong.
- Đang chưa kết nối thì ngắt lần thử dở dang trước khi quét, vì lần thử đó
  làm quét thất bại. Trong lúc quét không thử kết nối.
- Bỏ SSID ẩn, gộp SSID trùng (giữ mạng mạnh nhất), tối đa 20 mạng.

### Kết nối do người dùng chọn
- Mạng đã lưu mật khẩu hiện nhãn "Đã lưu"; nút Kết nối gửi `wifimgr::kUseSaved`
  thay cho mật khẩu, Wi-Fi manager tự lấy mật khẩu đã lưu (không áp dụng cho
  mạng mở). Lệnh serial tương đương: `wifi join <ssid>`.
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
Board thu sóng yếu hơn laptop nhiều (xem [01-phan-cung.md](01-phan-cung.md) mục 1.4):
- RD-SEAI_2.4G: laptop 91% (khoảng -45 dBm), panel **-77 dBm**, lại cùng kênh 1
  với RFThings (-57 dBm). Panel luôn nhận `AUTH_EXPIRE`, kể cả khi bỏ mật khẩu,
  chỉ định đúng BSSID, đổi MAC hoặc chỉ dùng 802.11b (08-nhat-ky.md, mục 33).
  Cách xử lý: đặt panel gần router hơn, hoặc dùng mạng panel bắt tốt.
- Điều khiển Tasmota không còn phụ thuộc vào mạng này, vì đi qua web (mục 5.3).
- Lệnh chẩn đoán: `devices` (Wi-Fi, IP, RSSI), `wifi join <ssid>`,
  `wifi bssid <aa:bb:cc:dd:ee:ff> <kênh> <ssid>`, `wifi mac <ssid>`, `wifi 11b <ssid>`.

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

## 5.3 Tasmota qua web (`src/tasmota.cpp`, task `devices`)

Panel **không gọi thẳng IP thiết bị** nữa. Mọi lệnh đi qua web
`https://smarthome.blong.id.vn/api/tasmota` (`web/src/app/api/tasmota/route.ts`).
Máy chủ web nằm cùng mạng với thiết bị, nên panel chỉ cần có Internet: vào
Wi-Fi nào cũng được (ví dụ RFThings), không cần cùng mạng RD-SEAI với thiết bị.

| Thao tác | Request | Trả về |
|---|---|---|
| Đọc trạng thái (cả 2 thiết bị) | `GET <api>` | `{"states":{"primary":{"power1":true,…},"secondary":{…}}}` (`null` nếu web không tới được thiết bị) |
| Bật/tắt kênh n | `POST <api>` `{"device":"primary","channel":n,"action":"toggle"}` | `{"device":"primary","states":{…}}` |

| Thông số | Giá trị |
|---|---|
| Hỏi trạng thái | mỗi 1 s (`TASMOTA_POLL_MS`), một request lấy cả 2 thiết bị; thay đổi hiện lên panel trong khoảng 1–1,5 s |
| Kết nối | HTTPS qua Cloudflare, giữ một kết nối TLS để dùng lại (khoảng 0,4 s/request; mở kết nối mới mất khoảng 3 s) |
| Client HTTP | tự viết trên `WiFiClientSecure`, đọc trọn body chunked/`Content-Length`. `HTTPClient` của Arduino bỏ sót phần kết thúc của body chunked, nên request kế tiếp trên kết nối dùng lại bị treo tới hết timeout (đo được 6–9 s) |
| Kết nối chết | chờ dòng trạng thái tối đa 2 s, rồi gửi lại trên kết nối mới |
| Ngoại tuyến | sau 3 lần hỏi lỗi liên tiếp (giữ trạng thái cũ khi mạng chập chờn) |
| Bật/tắt | công tắc đổi ngay (lạc quan), rồi chỉnh theo trạng thái web trả về; lỗi thì trả lại như cũ |
| Bảo mật | `setInsecure()`: không kiểm tra chứng chỉ (API chỉ bật/tắt relay) |
| RAM | bộ đệm TLS khoảng 60 KB RAM nội (RAM nội trống còn khoảng 80–90 KB) |

### Thiết bị
| Thiết bị | Tên trên web (`key`) | Kênh |
|---|---|---|
| Sonoff M5-3C | `primary` | 3 |
| TX Ultimate 4C-120 | `secondary` | 4 |

### Cấu hình
- **Mặc định** trong `config.h`: `TASMOTA_API_URL`, `TASMOTA_1_*`, `TASMOTA_2_*`.
- **Ghi đè** bằng `/config.json`:

```json
"tasmotaApi": "https://smarthome.blong.id.vn/api/tasmota",
"tasmota": [
  {"name": "Sonoff M5-3C", "key": "primary", "channels": 3,
   "labels": ["Đèn trần", "Quạt", "Ổ cắm"]},
  {"name": "TX Ultimate 4C", "key": "secondary", "channels": 4}
]
```

Không ghi `key` thì thiết bị thứ nhất là `primary`, thứ hai là `secondary`.
`labels` để trống thì hiện "Kênh 1", "Kênh 2"…

Lệnh serial `devices`:
```
  gateway: https://smarthome.blong.id.vn/api/tasmota
  Wi-Fi RFThings VietNam  IP 192.168.130.171  RSSI -73 dBm
  Sonoff M5-3C     primary    online  mask=0x07  ok 5 fail 0  last err 0  last 391 ms
  TX Ultimate 4C   secondary  online  mask=0x00  ok 5 fail 0  last err 0  last 391 ms
```

## 5.4 NTP

Khi có Wi-Fi: `configTime(GMT+7, "pool.ntp.org", "time.google.com")`. Trước khi
đồng bộ, header hiện `--:--`, còn log thẻ nhớ dùng mốc `+<giây>s` thay cho
ngày giờ.

## 5.5 ArduinoOTA

Bật khi có Wi-Fi lần đầu. Hostname `esp32-4848s040`, cổng 3232, mật khẩu
`OTA_PASSWORD` (`admin`; nên đổi). Log in `OTA server at: esp32-4848s040.local:3232`.
