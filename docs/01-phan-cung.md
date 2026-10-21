# 1. Phần cứng & chân pin

## 1.1 Board

| Mục | Giá trị |
|---|---|
| Board | GUITION ESP32-4848S040 (bản có 3 relay) |
| Module | ESP32-S3-WROOM-1 **N16R8**: 16 MB flash QIO, 8 MB PSRAM **Octal** |
| Chip | ESP32-S3 revision v0.2, MAC `98:3d:ae:e1:8b:bc` |
| Màn hình | 4" IPS 480×480, controller **ST7701**, giao tiếp RGB 16-bit (RGB565) |
| Cảm ứng | **GT911**, I2C, điện dung |
| USB-C | qua chip **CH340** vào UART0 (không phải USB native) |
| Thẻ nhớ | khe TF, giao tiếp SPI |
| Nguồn | cắm điện (không có pin), header hiển thị "AC" |

## 1.2 Bảng GPIO (đã kiểm chứng)

Nguồn: driver chạy được của project reference, cùng bảng phân chân của nhà sản xuất
`reference/LovyanGFX-GUITION-ESP32-4848S040/Docs/GUITION-4-0-inch-ESP32-4848S040/5-IO pin distribution/4.0 inches IO pin distribution.xlsx`.

| Khối | Tín hiệu | GPIO | Ghi chú |
|---|---|---|---|
| LCD data | B0..B4 | 4, 5, 6, 7, 15 | |
| | G0..G5 | 8, 20, 3, 46, 9, 10 | |
| | R0..R4 | 11, 12, 13, 14, 0 | GPIO0 là chân strapping BOOT |
| LCD sync | DE / VSYNC / HSYNC / PCLK | 18 / 17 / 16 / 21 | |
| ST7701 init (SPI 3 dây) | CS / SCK / SDA | 39 / 48 / 47 | chỉ dùng trong `display.begin()` |
| Đèn nền | BL (PWM) | 38 | `Light_PWM` của LovyanGFX |
| Cảm ứng GT911 | SDA / SCL | 19 / 45 | `I2C_NUM_1`, 400 kHz; INT, RST không nối |
| Thẻ TF | CS (D3) / MISO (D1) / SCK / MOSI | **42 / 41 / 48 / 47** | SCK, MOSI dùng chung với SPI init ST7701 |
| Relay (active HIGH) | K1 / K2 / K3 | 40 / 2 / 1 | |
| Serial | TX / RX | 43 / 44 | UART0 → CH340 |

### Các ràng buộc quan trọng

- **GPIO 9–12 là chân data của LCD.** Cấu hình cũ có `BUTTON_PIN_1..4 = 9..12`.
  Bật nút bấm trên các chân này sẽ làm hỏng hình. Board không còn GPIO trống
  cho nút vật lý; xem `HMI_BTN_*_PIN` trong `config.h` nếu dùng phiên bản phần
  cứng khác.
- **Thẻ nhớ dùng chung SCK (48) và MOSI (47) với SPI khởi tạo ST7701.** Vì
  vậy phải mount thẻ **sau** `display.begin()`. LovyanGFX chỉ dùng hai chân này
  lúc khởi tạo panel, nên dùng chung là an toàn.
- **Không khởi tạo `Wire` trên GPIO 19/45.** Cảm ứng đã do `Touch_GT911` của
  LovyanGFX quản lý. Hai driver I2C trên cùng chân sẽ làm hỏng dữ liệu cảm ứng.
  File `include/touch_gt911.h` cũ không còn được dùng.
- **GPIO 19/20 không dùng được cho USB native** vì đã dành cho cảm ứng và LCD.
  Do đó serial phải đi qua CH340 và UART0.

## 1.3 Thẻ nhớ

- Đã thử với thẻ **1,9 GB FAT**: mount ở **20 MHz**. Code thử lần lượt 20 MHz,
  4 MHz, 1 MHz (demo của nhà sản xuất dùng 1 MHz).
- Hỗ trợ cắm/rút khi đang chạy: task `storage` kiểm tra thẻ mỗi 5 giây.
- Chi tiết: [06-the-nho.md](06-the-nho.md).

## 1.4 Wi-Fi / anten (vấn đề phần cứng còn tồn tại)

| Phép đo | Kết quả |
|---|---|
| Laptop cạnh board, mạng `RD-SEAI_2.4G` | 95%, kênh 1, 2.4 GHz, 802.11n, **mạng mở** |
| Board, cùng mạng (vị trí 1) | **-77 dBm**. Tất cả mạng quét được đều ở mức icon yếu nhất (< -72 dBm) |
| Board, vị trí 2 (sau khi cắm sang cổng USB khác) | `NO_AP_FOUND`: không thấy mạng |
| Board, 15/10/2026 (danh sách quét lúc khởi động) | RFThings VietNam **-57 dBm**, RD-SEAI_2.4G **-77 dBm** (laptop 91%), cùng kênh 1; vào RD-SEAI luôn `AUTH_EXPIRE` |
| Board, 21/10/2026 (vị trí khác) | chỉ thấy UIT / Khoa ở -92 đến -96 dBm |

Board thu và phát sóng kém hơn nhiều so với laptop. Nguyên nhân có thể là:

- vị trí đặt board;
- vật che anten PCB (anten nằm ở cạnh module ESP32-S3);
- nhiễu từ bus RGB tốc độ cao của màn hình.

Firmware đã tối ưu phía phần mềm (xem [05-ket-noi.md](05-ket-noi.md)), nhưng
**cần đặt board gần router hơn và không che anten**. Mỗi lần khởi động, log
Serial in RSSI của mọi mạng tại chỗ đặt panel để kiểm tra vị trí. Tasmota đi
qua web nên chỉ cần một mạng có Internet mà panel bắt tốt.

## 1.5 eFuse (đã đọc bằng espefuse)

| eFuse | Giá trị | Ảnh hưởng |
|---|---|---|
| `UART_PRINT_CONTROL` | Disabled (0b11) | Không có log ROM (`ESP-ROM:esp32s3...`) khi boot. Đây là **bình thường** |
| `DIS_USB_SERIAL_JTAG` | False | |
| Flash type | quad | |

## 1.6 Bảng phân vùng (`partitions_16MB.csv`)

| Tên | Loại | Offset | Kích thước |
|---|---|---|---|
| nvs | data/nvs | 0x9000 | 20 KB: Wi-Fi đã lưu, độ sáng, bộ màu… |
| otadata | data/ota | 0xE000 | 8 KB |
| app0 | app/ota_0 | 0x10000 | 6400 KB |
| app1 | app/ota_1 | 0x650000 | 6400 KB |
| spiffs | data/spiffs | 0xC90000 | 3456 KB: **LittleFS** (bản sao `config.json`, lịch sử cảm biến mỗi giờ; 06-the-nho.md, mục 6.11) |
| coredump | data/coredump | 0xFF0000 | 64 KB |

Hai khe OTA dùng chung vùng app 12.800 KB; mỗi khe chứa tối đa 6400 KB. Firmware
hiện chiếm khoảng **1,77 MB / 6,4 MB** (khoảng 27%).
