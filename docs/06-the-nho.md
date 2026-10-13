# 6. Thẻ nhớ (TF)

## 6.1 Phần cứng & mount

- **SPI:** CS 42, MISO 41, SCK 48, MOSI 47 (theo bảng phân chân của nhà sản xuất).
- **Mount sau `display.begin()`,** vì SCK/MOSI dùng chung với SPI khởi tạo ST7701.
- **Tốc độ:** thử lần lượt 20 MHz → 4 MHz → 1 MHz. Thẻ 1,9 GB đang dùng chạy ở 20 MHz.
- **Khi mount,** tạo sẵn thư mục `/logs` và `/logs/sensors`.
- **Cắm/rút khi đang chạy:** cứ 5 giây task `storage` thử mở `/`. Mở hỏng thì
  coi như thẻ đã rút (`SD: card removed`); khi thẻ được cắm lại thì mount và
  ghi log `SD card inserted`.
- **Không có thẻ** thì mọi chức năng khác vẫn chạy bình thường; Cài đặt hiện
  "Không có thẻ".

## 6.2 Cấu trúc thẻ

```
/config.json                    cấu hình (đọc khi khởi động)
/firmware.bin                   firmware mới (Cài đặt → Cập nhật firmware)
/firmware.installed.bin         firmware.bin đổi tên sau khi cài thành công
/logs/events.log                nhật ký sự kiện
/logs/events.1.log              bản cũ (xoay vòng khi > 256 KB)
/logs/sensors/YYYY-MM-DD.csv    số liệu cảm biến theo ngày
/logs/sensors/undated.csv       khi chưa đồng bộ giờ NTP
/screenshots/shot_001.bmp ...   ảnh chụp màn hình (lệnh "shot sd")
```

## 6.3 `config.json`

Mọi trường đều **không bắt buộc**. Trường nào có sẽ ghi đè giá trị trong `config.h`.

```json
{
  "room": "Phòng khách",
  "wifi": [
    {"ssid": "RD-SEAI_2.4G", "password": ""},
    {"ssid": "ACLAB_Access", "password": "matkhau123"}
  ],
  "mqtt": {"host": "172.28.182.10", "port": 1883, "user": "", "password": ""},
  "tasmota": [
    {"name": "Sonoff M5-3C", "host": "172.28.182.3", "channels": 3,
     "labels": ["Đèn trần", "Quạt", "Ổ cắm"]},
    {"name": "TX Ultimate 4C", "host": "172.28.182.4", "channels": 4}
  ]
}
```

| Trường | Kiểu | Giới hạn | Tác dụng |
|---|---|---|---|
| `room` | chuỗi | 31 byte | tên phòng ở header |
| `wifi[]` | `{ssid, password}` | 4 mạng | thêm vào danh sách mạng đã biết (sau mạng lưu từ panel) |
| `mqtt.host/port/user/password` | | | broker MQTT |
| `tasmota[]` | `{name, host, channels, labels[]}` | 4 thiết bị × 8 kênh | **thay thế** danh sách trong `config.h` |

- **Cài đặt → config.json** hiện "Đã nạp" khi đọc thành công.
- **JSON sai cú pháp:** bỏ qua toàn bộ file, ghi log `config.json invalid: <lỗi>`.
- **Bảo mật:** mật khẩu trong `config.json` là văn bản thường, ai có thẻ đều
  đọc được. Mạng chọn trên panel được lưu trong **NVS**, không ghi ra thẻ.

## 6.4 Nhật ký sự kiện (`/logs/events.log`)

Mỗi dòng gồm thời điểm và nội dung. Trước khi đồng bộ NTP, thời điểm là
`+<giây kể từ khi bật>s`:

```
+0s Boot fw=1.1.0-ui reset=1
+0s config.json loaded: 1 Wi-Fi, mqtt=172.28.182.10
2026-10-13 09:09:12 Wi-Fi connected: RD-SEAI_2.4G 172.28.182.195 RSSI -77
2026-10-13 09:09:13 MQTT connected to 172.28.182.10:1883
2026-10-13 09:10:02 Relay 1 ON
2026-10-13 09:10:30 Tasmota TX Ultimate 4C ch2 ON
2026-10-13 09:15:41 Tasmota Sonoff M5-3C (172.28.182.3) offline
2026-10-13 09:20:00 Wi-Fi disconnected
```

| Sự kiện được ghi | Nguồn |
|---|---|
| Khởi động + mã reset | `setup()` |
| `config.json` nạp / lỗi | `storage` |
| Relay bật/tắt | `app` |
| Wi-Fi kết nối/mất, kết nối từ panel, kết nối thất bại + lý do | `net` |
| MQTT kết nối | `net` |
| Tasmota online/offline, bật/tắt kênh | `devices` |
| Dữ liệu cảm biến cũ (stale) | `app` |
| Cập nhật firmware bắt đầu/thành công/lỗi, OTA | `storage`, `net` |
| Cắm thẻ | `storage` |

- **Xoay vòng:** file vượt 256 KB thì đổi tên thành `events.1.log` (bản cũ hơn
  bị xoá).
- **Mọi dòng cũng in ra serial** dưới dạng `[event] ...`.
- **Ghi không chặn:** task nào cũng gọi `storage::logEvent()` được; dòng log đi
  vào queue (24 phần tử), queue đầy thì dòng bị bỏ.

## 6.5 Log cảm biến (`/logs/sensors/YYYY-MM-DD.csv`)

```csv
time,primary,metric1,metric2
2026-10-13 09:10:00,26.40,62.0,412.0
2026-10-13 09:11:00,26.55,61.0,438.0
```

- **Mỗi phút một dòng,** chỉ ghi khi trạng thái là `Ok` (không ghi khi Error/Stale).
- **Hiện đang ghi số liệu giả lập.** Khi có cảm biến thật qua MQTT, file sẽ
  chứa số liệu thật.

## 6.6 Cập nhật firmware từ thẻ

1. Build: `pio run`. File nằm ở `.pio/build/esp32-4848s040/firmware.bin`.
2. Chép vào **gốc thẻ** với tên `firmware.bin`.
3. Cắm thẻ, đợi tối đa 10 phút hoặc khởi động lại. Cài đặt → Cập nhật firmware
   hiện "firmware.bin · x MB".
4. Chạm 1 lần: hiện "Chạm lần nữa để cài đặt" (hết hiệu lực sau 4 giây). Chạm
   lần 2 để bắt đầu.
5. Màn tiến trình hiện lên. Task `storage` ghi vào phân vùng OTA còn lại theo
   từng khối 4 KB.
6. **Thành công:** đổi tên file thành `firmware.installed.bin`, ghi log, khởi
   động lại vào firmware mới.
7. **Lỗi:** ẩn màn tiến trình, ghi log `SD update failed at x/y bytes`; firmware
   đang chạy giữ nguyên.

*Trạng thái: đã cài đặt, chưa thử với file thật.*

## 6.7 Thống kê dung lượng

- `SD.usedBytes()` phải **quét toàn bộ bảng FAT**: mất 230–270 ms trên thẻ 1,9
  GB, thẻ lớn sẽ lâu hơn.
- Vì vậy task `storage` chỉ tính khi mount, khi cắm/rút thẻ, sau khi chụp màn
  hình, và **10 phút một lần**. Lần quét nào quá 200 ms thì in ra log.
- **Bài học:** bản trước gọi hàm này mỗi 5 giây ngay trong vòng lặp vẽ UI, nên
  UI bị khựng định kỳ và trông như treo khi cuộn (xem 08-nhat-ky.md).

## 6.8 Chụp màn hình vào thẻ

Lệnh serial `shot sd`:
- lưu `/screenshots/shot_NNN.bmp` (480×480, 24-bit, khoảng 690 KB);
- task `ui` chỉ lấy snapshot; task `storage` mã hoá BMP và ghi file;
- log in `Saved /screenshots/shot_003.bmp`.
