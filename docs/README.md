# Tài liệu firmware: Smart Home Panel (GUITION ESP32-4848S040)

Panel điều khiển nhà thông minh trên màn hình cảm ứng 4" 480×480, gồm:

- Giao diện LVGL 9 với 4 màn hình: Tổng quan, Điều khiển, Cài đặt, Wi-Fi.
- Firmware chạy trên FreeRTOS, chia thành 6 task.
- Thẻ nhớ TF dùng cho log, cấu hình và cập nhật firmware.
- Điều khiển relay trên panel và các thiết bị Tasmota trong mạng LAN.

| Khởi động | Tổng quan (bộ màu Bách Khoa) | Điều khiển | Cài đặt → Giới thiệu |
|---|---|---|---|
| ![splash](images/hmi-splash.png) | ![bk](images/hmi-bk.png) | ![control](images/hmi-control.png) | ![about](images/hmi-about.png) |

*(Ảnh chụp từ framebuffer thật bằng `tools/screenshot.py`.)*

---

## Mục lục

| # | Tài liệu | Nội dung |
|---|---|---|
| 1 | [Phần cứng & chân pin](01-phan-cung.md) | Board, module, bảng GPIO đã kiểm chứng, thẻ nhớ, relay, anten Wi-Fi |
| 2 | [Build & nạp firmware](02-build-nap.md) | `platformio.ini` bắt buộc, lỗi flash 8/16 MB, serial UART0, cổng COM, OTA |
| 3 | [Kiến trúc FreeRTOS](03-kien-truc-rtos.md) | 6 task, queue, mutex, watchdog, luồng dữ liệu, bản đồ mã nguồn |
| 4 | [Giao diện HMI (LVGL)](04-giao-dien-hmi.md) | Màn hình, bố cục, trạng thái, bộ màu, font, icon, cách mở rộng |
| 5 | [Kết nối: Wi-Fi, MQTT, Tasmota](05-ket-noi.md) | Wi-Fi manager, topic/payload MQTT, HTTP API Tasmota, NTP, OTA |
| 6 | [Thẻ nhớ](06-the-nho.md) | Cấu trúc thẻ, `config.json`, log, cập nhật firmware, ảnh chụp |
| 7 | [Gỡ lỗi & xử lý sự cố](07-go-loi.md) | Lệnh serial, chụp màn hình, đọc log, mã reset, bảng triệu chứng |
| 8 | [Nhật ký thay đổi & vấn đề đã gặp](08-nhat-ky.md) | Các lỗi đã tìm ra, nguyên nhân, cách sửa, theo thứ tự thời gian |

---

## Làm nhanh

```bash
cd firmware
pio run -t upload --upload-port COM18   # đổi COMx theo máy (xem 02-build-nap.md)
pio device monitor -p COM18 -b 115200
```

Log khởi động bình thường:

```
=== ESP32-4848S040 Smart Home Panel ===
Firmware: 1.1.0-ui  Hardware: 1.0
Reset reason: 1
SD: mounted at 20000000 Hz, 1892 MB
[event] Boot fw=1.1.0-ui reset=1
Tasks started: ui app net devices storage console
WiFi: 1 known network(s)
WiFi: trying RD-SEAI_2.4G
[event] Wi-Fi connected: RD-SEAI_2.4G 172.28.182.195 RSSI -77
```

---

## Trạng thái hiện tại (13/10/2026)

### Đã kiểm chứng trên board thật

| Hạng mục | Cách kiểm chứng |
|---|---|
| Boot, serial log qua CH340 | log khởi động đầy đủ |
| LCD + LVGL 9, 4 màn hình, 3 bộ màu | ảnh chụp framebuffer qua serial |
| Cảm ứng GT911 | log `TOUCH DOWN/UP` khi người dùng chạm; chạm hàng relay bật được relay |
| Chữ tiếng Việt, icon Lucide | ảnh chụp |
| Thẻ nhớ (thẻ 1,9 GB) | mount 20 MHz, ghi `events.log`, quét dung lượng 230–270 ms |
| FreeRTOS: 6 task | lệnh `tasks`: stack còn trống của mọi task > 2 KB |
| Wi-Fi: quét mạng, kết nối `RD-SEAI_2.4G`, NTP | log + ảnh màn Wi-Fi (có nhãn "Đã kết nối") |
| Tasmota: hiển thị thiết bị ngoại tuyến | Sonoff M5-3C hiện "Ngoại tuyến" (máy tính cũng không truy cập được) |

### Chưa kiểm chứng / còn tồn tại

| Hạng mục | Lý do / việc cần làm |
|---|---|
| **Wi-Fi không ổn định** | Board thu sóng rất yếu (~-77 dBm, laptop cạnh đó 95%). Ở vị trí mới thì `NO_AP_FOUND`. Cần đặt gần router, không che anten |
| Điều khiển TX Ultimate 4C (`172.28.182.4`) từ panel | Board chưa giữ được kết nối Wi-Fi để thử. Máy tính đọc được: 4 kênh + `POWER5` |
| Sonoff M5-3C (`172.28.182.3`) | Không phản hồi cả từ máy tính: thiết bị tắt hoặc đã đổi IP |
| MQTT | Broker `192.168.1.100` trong `config.h` không thuộc mạng `172.28.182.x`. Cần địa chỉ broker thật |
| Nhập mật khẩu Wi-Fi bằng bàn phím | Cần thao tác chạm thật |
| Lỗi treo khi cuộn trang Cài đặt | Đã sửa nguyên nhân (xem 08-nhat-ky.md, mục 9). Cần người dùng cuộn thử lại |
| Cập nhật firmware từ thẻ nhớ | Chưa có `firmware.bin` để thử |
| Cảm biến thật | Chưa nối. Số liệu đang **giả lập** (`HMI_SIMULATE_SENSORS 1`) |

---

## Bản thiết kế

Bản thiết kế UI/UX (Home Dashboard, các trạng thái, design system, pixel specs)
nằm trên canvas **"ESP32 480 HMI"**. Firmware hiện tại khác canvas ở các điểm:

- Tab giữa là **Điều khiển** (relay + Tasmota) thay cho "Chỉ số".
- Có 4 bộ màu Lime / Ember / Cobalt / Bách Khoa trên nền đen-xám, thay cho bảng màu teal ban đầu.
- Có logo Bách Khoa ở màn khởi động, header và mục Giới thiệu.
- Có thêm màn hình Wi-Fi và mục Thẻ nhớ trong Cài đặt.
