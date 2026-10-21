# 7. Gỡ lỗi & xử lý sự cố

## 7.1 Lệnh serial (115200 baud, mỗi lệnh kết thúc bằng Enter)

**Hệ thống**

| Lệnh | Tác dụng |
|---|---|
| `tasks` / `info` | heap, PSRAM trống, stack còn trống và ưu tiên của từng task |
| `bench [psram\|hybrid]` | dựng lại UI và đo thời gian vẽ lại toàn màn hình (vẽ + flush) |

**Giao diện**

| Lệnh | Tác dụng |
|---|---|
| `page 0` .. `page 6` | chuyển trang: Tổng quan / Điều khiển / Cài đặt / Wi-Fi / trang chi tiết / Tệp trên thẻ / xem tệp |
| `detail 0` .. `detail 8` | mở trang chi tiết: Mạng & MQTT (0, 1), Hệ thống (2), Thời gian chạy (3), Thẻ nhớ (4), config.json (5), Giới thiệu (6), Lịch sử cảm biến (7), Nhật ký sự kiện (8) |
| `scroll <px>` | cuộn trang hiện tại bằng API (dương = xuống), **không** đi qua đường cảm ứng; không dùng cho trang chi tiết (cuộn cả khung) |
| `drag x y y 40` | chạm (tap) tại (x, y), đi qua đường cảm ứng thật |
| `drag x y0 y1 [ms]` | giả lập một lần vuốt dọc qua **đúng đường xử lý cảm ứng** của LVGL (hit-test, kéo, quán tính), ví dụ `drag 240 380 120 300` |
| `kbd` / `kbd open` | mở màn nhập mật khẩu cho một mạng giả có mật khẩu / mạng mở |
| `view <đường dẫn>` | mở tệp trên thẻ trong trình xem (bảng CSV, ảnh, văn bản) |
| `theme 0` .. `theme 3` | đổi bộ màu Lime / Ember / Cobalt / Bách Khoa (không lưu) |
| `sensor ok\|warn\|error\|loading\|stale` | xem trước các trạng thái của gauge (tắt giả lập) |
| `sensor sim` | bật lại giả lập cảm biến |
| `toggle 1` .. `toggle 3` | bật/tắt relay trên panel |

**Chụp màn hình**

| Lệnh | Tác dụng |
|---|---|
| `shot` | gửi ảnh màn hình (vẽ lại bằng LVGL) qua serial, dùng với `tools/screenshot.py` |
| `shot fb [ms]` | chụp **framebuffer thật** sau `ms` mili giây (thấy được xé hình, rác hình lúc chuyển cảnh) |
| `shot sd` | lưu ảnh BMP vào thẻ nhớ |

**Mạng và thiết bị**

| Lệnh | Tác dụng |
|---|---|
| `wifi` | mở trang Wi-Fi và quét |
| `wifi join <ssid>` | kết nối mạng đã lưu mật khẩu (mạng mở thì không gửi mật khẩu) |
| `wifi bssid <aa:bb:cc:dd:ee:ff> <kênh> <ssid>` | chẩn đoán: kết nối thẳng một AP, in MAC của panel |
| `wifi mac <ssid>` | chẩn đoán: đổi sang MAC khác (tới khi khởi động lại) rồi kết nối |
| `wifi 11b <ssid>` | chẩn đoán: chỉ dùng 802.11b (tới khi khởi động lại) rồi kết nối |
| `devices` | Wi-Fi/IP/RSSI của panel, URL web, trạng thái và thống kê từng thiết bị Tasmota |

**Thẻ nhớ và dữ liệu**

| Lệnh | Tác dụng |
|---|---|
| `sd tree` | in toàn bộ cây thư mục trên thẻ kèm dung lượng tệp |
| `sd wipe` | **xóa sạch thẻ** (mọi tệp và thư mục), rồi tạo lại `/logs/sensors`; thư viện SD không format được FAT nên đây là cách thay thế |
| `sd restore` | cài lại firmware trong `/backup/firmware-prev.bin` |
| `history save` | lưu lịch sử cảm biến ra thẻ và flash ngay |

Lệnh không hợp lệ in ra `? <lệnh>`.

## 7.2 Chụp màn hình về máy tính

```bash
pip install pyserial pillow
python tools/screenshot.py COM4 shot.png                     # đóng serial monitor trước
python tools/screenshot.py COM4 fb.png "kbd;shot fb 60"      # gửi kèm chuỗi lệnh (cách nhau bằng ;)
```

Không có Pillow thì tool lưu `.ppm`. Ảnh trong `docs/images/` được chụp theo cách này.

Ảnh bị thiếu điểm (log chen vào giữa một dòng dữ liệu) thì tool tự thử lại tối đa 3 lần.

- **Giao thức:** firmware trả `SHOT 480 480`, rồi các dòng `#<base64>` chứa
  các bản ghi RLE (u16 số điểm ảnh liên tiếp, u16 màu RGB565 đã đảo byte), rồi
  `END`.
- **Vì sao dùng base64 theo dòng:** với nhiều task cùng in log, dữ liệu nhị phân
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
| `WiFi:    -77 dBm ch1  open RD-SEAI_2.4G` | danh sách mạng quét lúc khởi động, RSSI tại chỗ đặt panel |
| `WiFi: trying X` / `WiFi: begin X (open, no password)` / `not connected, next try in Ns` | Wi-Fi manager đang kết nối / giãn cách thử lại |
| `Tasmota: 2 device(s) via https://…` | chế độ điều khiển Tasmota qua web |
| `history: saved to SD in 209 ms` / `history: restored (seq N)` | lưu / nạp lịch sử cảm biến |
| `image: <tệp> 480x480 -> 432x432 in 668 ms` | giải mã ảnh cho trình xem tệp |
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
| Vuốt không cuộn được trang | đối tượng không có cờ `CLICKABLE` bị LVGL bỏ qua khi hit-test. Trang/khung cuộn phải `CLICKABLE` (xem `makeScrollPage()`). Kiểm tra bằng lệnh `drag` |
| UI khựng/treo định kỳ, nhất là khi cuộn | có thao tác chậm chạy trong task `ui` hoặc trong callback. Chuyển sang task khác (03-kien-truc-rtos.md, mục 3.4) |
| Board tự khởi động lại, mã reset 6 | task `ui`/`app` bị chặn quá 10 giây (watchdog) |
| Ảnh chụp từ `screenshot.py` bị sọc/hỏng | dùng `screenshot.py` bản mới (định dạng base64 theo dòng) |
| Cảm ứng chập chờn | có code gọi `Wire.begin(19, 45)` song song với LovyanGFX |
| Chữ hiện thành ô vuông | ký tự không có trong font. Chạy `python tools/check_font_coverage.py` để biết ký tự nào, ở dòng nào. Thêm vào `TEXT` trong `tools/gen_lvgl_fonts.sh` rồi sinh lại font; nếu TTF không có ký tự đó (ví dụ mũi tên) thì đổi chuỗi |
| Phím bàn phím hiện ô vuông | phím phải dùng `lv_font_montserrat_16` (có ký hiệu LV_SYMBOL) |
| Lỗi biên dịch `namespace hmi::port`, khởi tạo aggregate | thiếu `-std=gnu++17` / `build_unflags = -std=gnu++11` |
| Lỗi biên dịch `abstract type DataWrapperT<SPIFFSFS>` | (UI LovyanGFX cũ) `#include <SPIFFS.h>` phải đứng trước LovyanGFX |
| `NO_AP_FOUND` dù laptop thấy mạng | board thu sóng kém. Đặt gần router, không che anten |
| `AUTH_EXPIRE` lặp mỗi giây | auto-reconnect quá dồn dập. Đã tắt, giờ thử lại có giãn cách |
| `AUTH_EXPIRE` với một mạng cụ thể | gửi mật khẩu cho mạng mở (đã sửa: quét lúc khởi động), hoặc router không nghe được panel (RSSI tại panel ≤ -75 dBm). Xem danh sách quét lúc khởi động |
| Rác hình / bàn phím lặp lại mỗi 64 px khi chuyển cảnh | hiệu ứng `opa`/fade trên đối tượng lớn nền đặc (04-giao-dien-hmi.md, mục 4.1). Kiểm tra bằng `shot fb <ms>` |
| Kéo thanh độ sáng không có tác dụng | GPIO38 bị `pinMode`/`digitalWrite` sau `display.begin()`, tách khỏi PWM (08-nhat-ky.md, mục 24) |
| Tasmota cập nhật chậm, log `read Timeout` | `HTTPClient` dùng lại kết nối với body chunked; đã thay bằng client riêng (05-ket-noi.md, mục 5.3) |
| `pio.exe: No such file or directory` | PlatformIO trong `~/.platformio/penv` đang được VS Code cài lại; dùng `python -m platformio run …` |
| COM4 biến mất giữa lúc nạp | cáp/cổng USB lỏng: cắm lại, đổi cáp có dữ liệu, cắm thẳng vào máy |
| `MQTT connect failed rc=-2` | broker sai địa chỉ hoặc không tới được. Sửa `MQTT_BROKER` hoặc `config.json` |
| Tasmota "Ngoại tuyến" | panel không có Internet, web lỗi, hoặc web không tới được thiết bị (`null` trong `states`). Thử `curl https://smarthome.blong.id.vn/api/tasmota`, xem `devices` |
| Cài đặt → Thẻ nhớ "Không có thẻ" dù đã cắm | thẻ không phải FAT/FAT32, tiếp xúc kém. Xem log `SD: ...` |

## 7.6 Công cụ phần cứng hữu ích

```bash
# đọc MAC / xác định board trên cổng COM
python ~/.platformio/packages/tool-esptoolpy@1.40501.0/esptool.py --port COM4 --chip esp32s3 read_mac
# so sánh firmware trên chip với bản build
python .../esptool.py --port COM4 --chip esp32s3 verify_flash 0x0 bootloader.bin 0x8000 partitions.bin 0x10000 firmware.bin
# đọc eFuse (cần thêm: pip install cryptography bitstring==3.1.9 reedsolo ecdsa)
python .../espefuse.py --port COM4 --chip esp32s3 summary
```
