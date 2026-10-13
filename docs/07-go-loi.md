# 7. Gỡ lỗi & xử lý sự cố

## 7.1 Lệnh serial (115200 baud, mỗi lệnh kết thúc bằng Enter)

| Lệnh | Tác dụng |
|---|---|
| `tasks` / `info` | heap, PSRAM trống, stack còn trống và ưu tiên của từng task |
| `toggle 1` .. `toggle 3` | bật/tắt relay trên panel |
| `page 0` .. `page 3` | chuyển trang: Tổng quan / Điều khiển / Cài đặt / Wi-Fi |
| `scroll <px>` | cuộn trang hiện tại (dương = xuống) |
| `wifi` | mở trang Wi-Fi và quét |
| `devices` | trạng thái các thiết bị Tasmota |
| `theme 0` .. `theme 2` | đổi bộ màu Lime / Ember / Cobalt (không lưu) |
| `sensor ok\|warn\|error\|loading\|stale` | xem trước các trạng thái của gauge (tắt giả lập) |
| `sensor sim` | bật lại giả lập cảm biến |
| `shot` | gửi ảnh màn hình qua serial (dùng với `tools/screenshot.py`) |
| `shot sd` | lưu ảnh BMP vào thẻ nhớ |

Lệnh không hợp lệ in ra `? <lệnh>`.

## 7.2 Chụp màn hình về máy tính

```bash
pip install pyserial pillow
python tools/screenshot.py COM18 shot.png      # đóng serial monitor trước
```

- **Giao thức:** firmware trả `SHOT 480 480`, rồi các dòng `#<base64>` chứa
  các bản ghi RLE (u16 số điểm ảnh liên tiếp, u16 màu RGB565 đã đảo byte), rồi
  `END`.
- **Vì sao dùng base64 theo dòng:** với 6 task cùng in log, dữ liệu nhị phân
  liền mạch sẽ bị log chen vào giữa và hỏng ảnh. Mỗi dòng base64 được ghi bằng
  một lệnh `write`, nên log chỉ chen được vào giữa các dòng, và tool bỏ qua mọi
  dòng không bắt đầu bằng `#`.
- **Thời gian:** khoảng 5–20 giây tuỳ nội dung màn hình. UI vẫn chạy bình
  thường trong lúc gửi.

## 7.3 Đọc log

| Log | Ý nghĩa |
|---|---|
| `Reset reason: N` | xem bảng 7.4 |
| `SD: mounted at 20000000 Hz, 1892 MB` / `SD: no card` | trạng thái thẻ nhớ |
| `SD: free-space scan took 267 ms` | lần quét FAT chậm (> 200 ms), bình thường với thẻ lớn |
| `[event] ...` | sự kiện, cũng được ghi vào `events.log` |
| `WiFi: trying X` / `not connected, next try in Ns` | Wi-Fi manager đang giãn cách thử lại |
| `_eventCallback(): Reason: N - ...` | lý do Wi-Fi ngắt (bảng ở 05-ket-noi.md) |
| `connect(): select returned due to timeout 1000 ms` + `MQTT connect failed rc=-2` | không tới được broker MQTT |
| `TOUCH DOWN X=.. Y=..` / `TOUCH UP` | mỗi lần chạm, dùng để kiểm tra cảm ứng |
| `[E][vfs_api.cpp] open(): /sd/... does not exist` | đã sửa (dùng `SD.exists()` trước). Nếu còn thấy là code mới gọi `SD.open` thiếu kiểm tra |
| `nvs_open failed: NOT_FOUND` | namespace NVS chưa tồn tại ở lần chạy đầu. Đã chuyển sang mở ở chế độ đọc-ghi để tránh lỗi này |

## 7.4 Mã reset (`esp_reset_reason()`)

| Mã | Ý nghĩa | Gợi ý |
|---|---|---|
| 1 | Bật nguồn / nút EN / nạp firmware | bình thường |
| 3 | `ESP.restart()` (sau khi cập nhật firmware) | bình thường |
| 4 | Panic (crash) | xem backtrace trên serial |
| 5 | Interrupt watchdog | ngắt bị khoá quá lâu |
| 6 | Task watchdog | task `ui` hoặc `app` treo quá 10 giây |
| 7 | Watchdog khác | |
| 9 | Brownout | nguồn yếu: đổi cáp hoặc cục sạc USB |

Mã reset được in ra serial và ghi vào dòng `Boot` của `events.log`, nên vẫn xem
lại được sau khi board đã tự khởi động lại.

## 7.5 Bảng triệu chứng → nguyên nhân

| Triệu chứng | Nguyên nhân / cách sửa |
|---|---|
| Nạp xong không có byte serial nào, màn hình đen | header flash 8 MB đi với bảng phân vùng 16 MB. Thêm `board_upload.flash_size = 16MB` (02-build-nap.md, mục 2.3) |
| Không thấy dòng `ESP-ROM:` khi boot | bình thường: eFuse đã tắt log ROM |
| `pio run` lỗi `No module named 'intelhex'` | `~/.platformio/penv/Scripts/python -m pip install intelhex` |
| `Could not open COMx, the port is busy` | đóng Serial Monitor/Arduino IDE, hoặc board đã sang COM khác (kiểm tra bằng `read_mac`) |
| Đèn nền sáng, màn hình đen | sai driver/timing. Dùng `lgfx_4848s040_reference.h`, kiểm tra `display.begin()` |
| UI khựng/treo định kỳ, nhất là khi cuộn | có thao tác chậm chạy trong task `ui` hoặc trong callback. Chuyển sang task khác (03-kien-truc-rtos.md, mục 3.4) |
| Board tự khởi động lại, mã reset 6 | task `ui`/`app` bị chặn quá 10 giây (watchdog) |
| Ảnh chụp từ `screenshot.py` bị sọc/hỏng | dùng `screenshot.py` bản mới (định dạng base64 theo dòng) |
| Cảm ứng chập chờn | có code gọi `Wire.begin(19, 45)` song song với LovyanGFX |
| Chữ tiếng Việt thành ô trống | ký tự nằm ngoài dải font. Sinh lại font bằng `tools/gen_lvgl_fonts.sh` |
| Phím bàn phím hiện ô vuông | phím phải dùng `lv_font_montserrat_16` (có ký hiệu LV_SYMBOL) |
| Lỗi biên dịch `namespace hmi::port`, khởi tạo aggregate | thiếu `-std=gnu++17` / `build_unflags = -std=gnu++11` |
| Lỗi biên dịch `abstract type DataWrapperT<SPIFFSFS>` | (UI LovyanGFX cũ) `#include <SPIFFS.h>` phải đứng trước LovyanGFX |
| `NO_AP_FOUND` dù laptop thấy mạng | board thu sóng kém. Đặt gần router, không che anten |
| `AUTH_EXPIRE` lặp mỗi giây | auto-reconnect quá dồn dập. Đã tắt, giờ thử lại có giãn cách |
| `MQTT connect failed rc=-2` | broker sai địa chỉ hoặc không tới được. Sửa `MQTT_BROKER` hoặc `config.json` |
| Tasmota "Ngoại tuyến" | thiết bị tắt, sai IP, hoặc khác mạng. Thử `curl http://<ip>/cm?cmnd=Power0` từ máy tính |
| Cài đặt → Thẻ nhớ "Không có thẻ" dù đã cắm | thẻ không phải FAT/FAT32, tiếp xúc kém. Xem log `SD: ...` |

## 7.6 Công cụ phần cứng hữu ích

```bash
# đọc MAC / xác định board trên cổng COM
python ~/.platformio/packages/tool-esptoolpy@1.40501.0/esptool.py --port COM18 --chip esp32s3 read_mac
# so sánh firmware trên chip với bản build
python .../esptool.py --port COM18 --chip esp32s3 verify_flash 0x0 bootloader.bin 0x8000 partitions.bin 0x10000 firmware.bin
# đọc eFuse (cần thêm: pip install cryptography bitstring==3.1.9 reedsolo ecdsa)
python .../espefuse.py --port COM18 --chip esp32s3 summary
```
