# 2. Build & nạp firmware

## 2.1 Công cụ

| Công cụ | Phiên bản | Ghi chú |
|---|---|---|
| PlatformIO Core | 6.2.0 | |
| Platform | `espressif32@6.13.0` | Arduino-ESP32 2.0.17 (ESP-IDF 4.4.7) |
| esptool | 4.5.1 | gói `tool-esptoolpy@1.40501.0` |
| LVGL | 9.2.2 | |
| LovyanGFX | 1.2.30 | |
| ArduinoJson | 7.x | |
| PubSubClient | 2.8 | |

**Lỗi toolchain đã gặp:** `pio run` báo `ModuleNotFoundError: No module named 'intelhex'`
ngay ở bước tạo `bootloader.bin`. Sửa bằng cách cài vào penv của PlatformIO:

```bash
~/.platformio/penv/Scripts/python -m pip install intelhex
```

**`pio.exe: No such file or directory`** (21/10/2026): extension PlatformIO của
VS Code đang cài lại `~/.platformio/penv`. Dùng PlatformIO của Python hệ thống,
cùng bộ package trong `~/.platformio`:

```bash
python -m platformio run -t upload --upload-port COM4
```

## 2.2 `platformio.ini`: các dòng bắt buộc và lý do

```ini
[env:esp32-4848s040]
platform = espressif32@6.13.0
board = esp32-s3-devkitc-1-n16r8v      ; board JSON custom: reference/LovyanGFX-GUITION-ESP32-4848S040/board
framework = arduino

board_build.flash_mode = qio
board_build.flash_size = 16MB
board_upload.flash_size = 16MB          ; (!) xem 2.3
board_build.partitions = partitions_16MB.csv
board_build.arduino.memory_type = qio_opi   ; flash QIO + PSRAM Octal

build_flags =
    -DARDUINO_USB_CDC_ON_BOOT=0         ; Serial = UART0 (qua CH340)
    -DARDUINO_USB_MODE=0
    -DCORE_DEBUG_LEVEL=3
    -DBOARD_HAS_PSRAM
    -DLV_CONF_INCLUDE_SIMPLE            ; LVGL dùng include/lv_conf.h
    -DLV_LVGL_H_INCLUDE_SIMPLE          ; font C sinh ra include "lvgl.h"
    -Iinclude
    -std=gnu++17                        ; code dùng namespace lồng (hmi::port), constexpr...
build_unflags = -std=gnu++11

lib_deps =
    lovyan03/LovyanGFX@^1.1.9
    knolleary/PubSubClient@^2.8
    bblanchon/ArduinoJson@^7.2.1
    mathertel/OneButton@^2.0.2          ; hiện không dùng
    lvgl/lvgl@9.2.2

[platformio]
default_envs = esp32-4848s040
boards_dir = reference/LovyanGFX-GUITION-ESP32-4848S040/board
```

## 2.3 Lỗi quan trọng nhất: `board_upload.flash_size = 16MB`

**Triệu chứng:** nạp thành công nhưng **không có byte serial nào** và màn hình đen.

**Nguyên nhân:**

1. Board JSON custom khai báo `"upload": {"flash_size": "8MB"}`. PlatformIO
   **ưu tiên giá trị này** hơn `board_build.flash_size`, nên header của
   bootloader/firmware ghi 8 MB (byte thứ 4 của `bootloader.bin` = `0x3f`).
2. Bảng phân vùng `partitions_16MB.csv` dùng tới 0xFFFFFF, nên bootloader coi bảng
   phân vùng sai và **không chạy app**.
3. Lỗi này hoàn toàn im lặng:
   - eFuse `UART_PRINT_CONTROL` đã tắt log ROM;
   - bootloader Arduino được build với `CONFIG_BOOTLOADER_LOG_LEVEL_NONE`.

**Cách tìm ra:** nạp một sketch tối giản (chỉ in chữ), rồi thêm lần lượt từng
dòng của `platformio.ini` cho đến khi mất log. Dòng gây lỗi là
`board_build.partitions = partitions_16MB.csv` (vì header vẫn là 8 MB).

**Kiểm tra nhanh** (phải ra `0x4f` = 16 MB):

```bash
python -c "print(hex(open('.pio/build/esp32-4848s040/bootloader.bin','rb').read(4)[3]))"
```

## 2.4 Serial

- USB-C → CH340 → **UART0** (TX 43 / RX 44), 115200 baud.
- Windows hiện tên `USB-SERIAL CH340 (COMx)`. **Số COM đổi theo cổng USB:**
  board từng là COM4, có lúc thành **COM18**; hiện là **COM4**. COM5 / COM6 trên
  máy này là cổng Bluetooth, không phải board.
- Cổng biến mất giữa lúc nạp (`Could not open COM4 … doesn't exist`): cáp hoặc
  cổng USB lỏng. Cắm lại, dùng cáp có dữ liệu, cắm thẳng vào máy.
- Xác định đúng board bằng MAC:

```bash
python ~/.platformio/packages/tool-esptoolpy@1.40501.0/esptool.py --port COM4 --chip esp32s3 read_mac
# MAC: 98:3d:ae:e1:8b:bc
```

- Lỗi `Could not open COMx, the port is busy`: đang có chương trình khác mở
  cổng (Serial Monitor, Arduino IDE…), hoặc board đã sang COM khác.
- Mở/đóng cổng **không** reset board, vì DTR/RTS được giữ ở mức nhả. Có thể
  mở monitor mà không mất trạng thái.

## 2.5 Lệnh

```bash
pio run                                  # build (~1-2 phút; build lại toàn bộ ~2-4 phút)
pio run -t upload --upload-port COM4     # build + nạp
pio device monitor -p COM4 -b 115200     # xem log; gõ lệnh console (07-go-loi.md)
pio run -t clean
```

- Build nhanh nhờ `lib_ldf_mode = chain` (trước là `chain+`, mỗi lần build mất
  khoảng 10 phút; 08-nhat-ky.md, mục 24).
- Không cần `pio run -t uploadfs`: font và icon đã biên dịch vào firmware.
  Phân vùng `spiffs` được firmware tự format thành LittleFS lần đầu chạy.

## 2.6 Cập nhật không cần dây

| Cách | Trạng thái |
|---|---|
| **Thẻ nhớ**: chép `firmware.bin` vào thẻ, rồi vào Cài đặt → Thẻ nhớ → Cập nhật firmware → chạm 2 lần. Firmware đang chạy được sao lưu vào `/backup` trước (`sd restore` để quay lại) | Đã cài đặt, chưa thử (xem 06-the-nho.md) |
| **ArduinoOTA** qua Wi-Fi: `pio run -t upload -e esp32-4848s040-ota` (sửa `upload_port` = IP panel, mật khẩu `OTA_PASSWORD`) | Đã cài đặt, chạy khi có Wi-Fi, chưa thử |
| MQTT OTA / web `/update` | **Chưa cài đặt** (README cũ có nhắc nhưng code không có) |

`firmware.bin` cho thẻ nhớ nằm ở `.pio/build/esp32-4848s040/firmware.bin` sau khi build.

## 2.7 Dung lượng

| | Dùng |
|---|---|
| Flash (app) | 1,77 MB / 6,4 MB (khoảng 27%) |
| RAM tĩnh | 61 KB / 320 KB (18,6%) |
| RAM nội trống khi chạy | khoảng 133 KB; khoảng 80–90 KB khi kết nối HTTPS tới web Tasmota đang mở (bộ đệm TLS) |
| PSRAM trống | ~7,3 MB / 8 MB (framebuffer 450 KB, widget LVGL, lịch sử, nhật ký, trình xem tệp) |
