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
/logs/sensors/YYYY-MM-DD.csv    số liệu cảm biến theo ngày (giữ "logDays" ngày, mặc định 90)
/logs/sensors/undated.csv       khi chưa đồng bộ giờ NTP
/data/history.bin               lịch sử cảm biến (vòng đệm PSRAM), lưu 10 phút/lần
/backup/firmware-prev.bin       firmware đang chạy, chép ra trước mỗi lần cập nhật từ thẻ
/backup/firmware-prev.txt       phiên bản của bản sao đó
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
  "logDays": 90,
  "tasmotaApi": "https://smarthome.blong.id.vn/api/tasmota",
  "tasmota": [
    {"name": "Sonoff M5-3C", "key": "primary", "channels": 3,
     "labels": ["Đèn trần", "Quạt", "Ổ cắm"]},
    {"name": "TX Ultimate 4C", "key": "secondary", "channels": 4}
  ]
}
```

| Trường | Kiểu | Giới hạn | Tác dụng |
|---|---|---|---|
| `room` | chuỗi | 31 byte | tên phòng ở header |
| `wifi[]` | `{ssid, password}` | 4 mạng | thêm vào danh sách mạng đã biết (sau mạng lưu từ panel) |
| `mqtt.host/port/user/password` | | | broker MQTT |
| `logDays` | số | 1..3650 | số ngày giữ tệp CSV cảm biến; task `storage` xoá tệp cũ hơn mỗi 6 giờ, khi đã có giờ NTP |
| `tasmotaApi` | chuỗi | 127 byte | URL web điều khiển Tasmota (05-ket-noi.md, mục 5.3); `""` thì không điều khiển Tasmota |
| `tasmota[]` | `{name, key, channels, labels[]}` | 4 thiết bị × 8 kênh | **thay thế** danh sách trong `config.h`; `key` là tên thiết bị trên web (mặc định `primary`, `secondary` theo thứ tự) |

- **Cài đặt → Thẻ nhớ → config.json** hiện "Từ thẻ" hoặc "Từ flash".
- **Bản sao trong flash:** `config.json` hợp lệ trên thẻ được chép vào LittleFS
  (chỉ khi nội dung đổi). Khởi động không có thẻ thì dùng bản sao này (mục 6.11).
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
2026-10-13 09:15:41 Tasmota Sonoff M5-3C offline
2026-10-13 09:20:00 Wi-Fi disconnected
```

| Sự kiện được ghi | Nguồn |
|---|---|
| Khởi động + mã reset | `setup()` |
| `config.json` nạp / lỗi | `storage` |
| Relay bật/tắt | `app` |
| Wi-Fi kết nối/mất, kết nối từ panel, kết nối thất bại + lý do | `net` |
| MQTT kết nối | `net` |
| Tasmota online/offline, bật/tắt kênh, lỗi web (`gateway error (HTTP n)`) | `devices` |
| Lịch sử lưu, xoá CSV cũ, sao lưu / khôi phục firmware, xoá tệp | `storage` |
| Dữ liệu cảm biến cũ (stale) | `app` |
| Cập nhật firmware bắt đầu/thành công/lỗi, OTA | `storage`, `net` |
| Cắm thẻ | `storage` |

- **Xoay vòng:** file vượt 256 KB thì đổi tên thành `events.1.log` (bản cũ hơn
  bị xoá).
- **Mọi dòng cũng in ra serial** dưới dạng `[event] ...`.
- **Ghi không chặn:** task nào cũng gọi `storage::logEvent()` được; dòng log đi
  vào queue (24 phần tử), queue đầy thì dòng bị bỏ.
- **Xem trên panel:** 400 dòng gần nhất cũng nằm trong PSRAM (Cài đặt → Hệ
  thống → Nhật ký sự kiện), kể cả khi không có thẻ.

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
3. Cắm thẻ, đợi tối đa 10 phút hoặc khởi động lại. Cài đặt → Thẻ nhớ → Cập
   nhật firmware hiện "firmware.bin · x MB".
4. Chạm 1 lần: hiện "Chạm lần nữa để cài đặt" (hết hiệu lực sau 4 giây). Chạm
   lần 2 để bắt đầu.
5. Task `storage` chép firmware đang chạy ra `/backup/firmware-prev.bin`
   (mục 6.11), rồi màn tiến trình hiện lên và firmware mới được ghi vào phân
   vùng OTA còn lại theo từng khối 4 KB.
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

## 6.8 Duyệt và xoá tệp trên panel

Cài đặt → Thẻ nhớ → **Tệp trên thẻ**: duyệt thư mục, xem kích thước, xem tệp
(CSV dạng bảng, ảnh, văn bản tới 64 KB; mục 6.10) và xoá tệp (chạm 2 lần). Mỗi lần xoá đều ghi vào `events.log`
(`File deleted from panel: <đường dẫn>`). Chi tiết giao diện: 04-giao-dien-hmi.md.

## 6.9 Chụp màn hình vào thẻ

Lệnh serial `shot sd`:
- lưu `/screenshots/shot_NNN.bmp` (480×480, 24-bit, khoảng 690 KB);
- task `ui` chỉ lấy snapshot; task `storage` mã hoá BMP và ghi file;
- log in `Saved /screenshots/shot_003.bmp`.

## 6.10 Xem trước tệp

Chạm một tệp trong trình duyệt tệp (Cài đặt → Thẻ nhớ → Tệp trên thẻ):

| Loại | Hiển thị |
|---|---|
| `.csv` | **bảng** (`lv_table`): dòng tiêu đề, tối đa 200 dòng mới nhất (dòng mới ở trên), cột số căn phải, bảng rộng thì cuộn ngang |
| `.jpg` `.jpeg` `.png` `.bmp` | **ảnh**: task `storage` giải mã bằng LovyanGFX vào sprite trong PSRAM, thu nhỏ vừa 432 × 432 (giữ tỉ lệ, không phóng to), rồi đưa sang LVGL dạng RGB565 |
| `.txt` `.log` `.json` `.md` … | văn bản, tối đa 64 KB (tệp dài: 64 KB cuối) |

- Ảnh chụp màn hình BMP 480×480 giải mã trong khoảng 670 ms.
- JPEG lớn (ảnh điện thoại vài MB) có thể mất vài giây.
- CSV dài: storage đọc 64 KB cuối rồi chép dòng tiêu đề lên đầu, để bảng
  vẫn có tên cột.
- Lệnh serial `view <đường dẫn>` mở tệp trực tiếp, dùng khi kiểm tra.

## 6.11 Tận dụng PSRAM và flash

| Nơi | Dùng cho | Dung lượng |
|---|---|---|
| PSRAM | framebuffer màn hình | 450 KB |
| PSRAM | lịch sử cảm biến: 8640 ô × 10 s (24 giờ) + 10080 ô × 1 phút (7 ngày), mỗi ô 3 số `float` (`src/datalog.cpp`) | 219 KB |
| PSRAM | nhật ký sự kiện: 400 dòng gần nhất (nạp sẵn 24 KB cuối của `events.log` khi khởi động) | 44 KB |
| PSRAM | trình xem tệp: văn bản tới 64 KB (chia nhãn ~1 KB để LVGL không phải dàn trang cả khối), ảnh tới 432 × 432 | 2 × 64 KB + 2 × 365 KB |
| PSRAM | widget LVGL, model UI | khoảng 65 KB |
| Flash | LittleFS trên phân vùng `spiffs` 3,4 MB (mount ở `/lfs`, tự format lần đầu) | |
| Flash | `/config.json`: bản sao của `config.json` hợp lệ gần nhất trên thẻ (chỉ ghi khi nội dung đổi); dùng khi không có thẻ | vài KB |
| Flash | `/history.bin`: lịch sử cảm biến, mỗi giờ | 219 KB |

- **Lịch sử cảm biến** (Cài đặt → Lịch sử cảm biến, hoặc chạm vào
  đồng hồ / thẻ chỉ số ở Tổng quan): ba biểu đồ nhiệt độ, độ ẩm, công suất cho
  khung 1 giờ / 24 giờ / 7 ngày.
  - Mỗi ô là trung bình các mẫu trong 10 giây (hoặc 1 phút); ô không có mẫu là
    khoảng trống.
  - Khi khởi động, nạp bản mới hơn giữa thẻ và flash (so số thứ tự `seq`).
    Nếu cả lúc lưu lẫn lúc khởi động đều đã có giờ NTP thì thời gian tắt máy
    được điền thành khoảng trống.
- **Ghi flash ngắn và thưa:** mỗi lần ghi flash, cache bị tắt, có thể làm màn
  hình RGB chớp. Vì vậy lịch sử chỉ ghi ra flash mỗi giờ, theo từng khối 4 KB
  cách nhau 20 ms (đo: 4,5 giây cho 225 KB); ghi ra thẻ thì liền một lần
  (đo: 209 ms).
- **Sao lưu firmware:** trước khi cài `/firmware.bin`, firmware đang chạy được
  chép ra `/backup/firmware-prev.bin`. Lệnh serial `sd restore` cài lại bản đó
  (bản đang chạy lại trở thành bản sao lưu).
- **Lệnh serial:** `history save` (lưu lịch sử ra thẻ và flash ngay),
  `sd restore`, `sd tree`, `sd wipe`.
