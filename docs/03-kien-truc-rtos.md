# 3. Kiến trúc FreeRTOS

## 3.1 Vì sao FreeRTOS, không phải Zephyr

- ESP32 (ESP-IDF / Arduino) **vốn chạy trên FreeRTOS**. Dùng FreeRTOS "triệt
  để" nghĩa là tổ chức firmware thành các task có queue, mutex và watchdog,
  đồng thời giữ nguyên các driver đã chạy tốt: panel RGB ST7701, GT911, LVGL,
  Wi-Fi.
- Zephyr chưa hỗ trợ đầy đủ panel RGB của ESP32-S3 trên board này. Chuyển sang
  Zephyr gần như phải viết lại toàn bộ mà không có lợi ích tương xứng.

## 3.2 Các task

| Task | Core | Ưu tiên | Stack | Queue vào | Việc | File |
|---|---|---|---|---|---|---|
| `ui` | 1 | 3 | 10 KB | `Request` (8) | **Task duy nhất gọi LVGL**: vẽ, cảm ứng, đèn nền, màn hình chờ | `src/hmi/hmi_ui.cpp` |
| `app` | 1 | 4 | 4 KB | `AppMsg` (16) | relay trên panel, cảm biến (giả lập/thật), lịch sử cảm biến (`datalog`), lưu cài đặt NVS | `src/main.cpp` |
| `net` | 0 | 2 | 8 KB | `NetMsg` (4) | Wi-Fi manager, MQTT, ArduinoOTA, NTP, cấp trạng thái cho header | `src/main.cpp`, `src/wifi_manager.cpp` |
| `devices` | 0 | 2 | 10 KB | `Toggle` (8) | Tasmota qua web (HTTPS, cần thêm stack cho bắt tay TLS) | `src/tasmota.cpp` |
| `storage` | 0 | 1 | 12 KB | `Item` (24) | thẻ nhớ + LittleFS: log, lịch sử, thống kê, cập nhật/sao lưu firmware, xem tệp, giải mã ảnh | `src/storage.cpp` |
| `console` | 0 | 1 | 4 KB | (Serial) | lệnh serial | `src/main.cpp` |
| 2 luồng vẽ LVGL | bất kỳ | — | 8 KB mỗi luồng | (nội bộ LVGL) | vẽ song song trên 2 nhân (`LV_DRAW_SW_DRAW_UNIT_CNT = 2`); không gọi API LVGL | thư viện LVGL |

- **Chia core:** stack Wi-Fi của ESP-IDF chạy trên core 0, nên mọi thứ liên
  quan mạng và thẻ nhớ đặt ở core 0. Giao diện và logic cần phản hồi nhanh đặt
  ở core 1.
- **Ưu tiên:** `app` (4) cao hơn `ui` (3) để bật/tắt relay không phải chờ
  khung hình. Việc của `app` rất ngắn nên không làm chậm UI.
- **Vòng `loop()` của Arduino tự xoá** (`vTaskDelete`) sau `setup()`.

Stack còn trống đo bằng lệnh `tasks` (21/10/2026; `devices` còn khoảng 5,3 KB
khi kết nối TLS đang mở):

```
heap 133 KB (min 129 KB), psram free 7265 KB
  ui       stack free  4360 B  prio 3
  app      stack free  3128 B  prio 4
  net      stack free  4924 B  prio 2
  devices  stack free  9256 B  prio 2
  storage  stack free  8728 B  prio 1
  console  stack free  1564 B  prio 1
```

## 3.3 Luồng dữ liệu

```
                 ┌──────────── hmi::set*() (khoá ModelLock) ───────────────┐
                 │                                                          ▼
 console ─AppMsg─▶ app ─────────────── setRelay/setPrimary ──────▶  HMI model (mutex)
    │             ▲  ▲                                                      │ snapshot
    │             │  └─AppMsg── net ◀──── MQTT callback                     ▼
    │             │             ▲  └──── setLink/setClock/setSystemInfo ─▶ ui task ──▶ LVGL ──▶ LCD
    ├─NetMsg──────┼─────────────┘                                           │
    │             │       callbacks (chạy trong ui, chỉ được post) ─────────┘
    │             └─ relayToggle / save* ── AppMsg
    │                 wifiScan / wifiJoin ─ NetMsg ─▶ net
    │                 deviceToggle ──────── Toggle ─▶ devices ─HTTPS─▶ web ─▶ Tasmota
    │                 firmwareUpdate ────── Item ───▶ storage ─SPI──▶ thẻ TF
    └─ shot sd ─────────────────────────── Item ───▶ storage
       mọi task ── storage::logEvent() ─── Item ───▶ storage (không chặn)
                   └── datalog::addEvent() ─▶ vòng đệm PSRAM (mutex)
       app ─────── datalog::addSample() ────▶ lịch sử PSRAM ◀─ save/load ─ storage
```

## 3.4 Quy tắc an toàn đa task

1. **Chỉ task `ui` gọi LVGL.** Task khác cần chuyển trang, cuộn hay chụp màn
   hình thì gửi `Request` qua queue (`hmi::showPage`, `hmi::scrollPage`,
   `hmi::screenshot*`).
2. **Model HMI có mutex đệ quy** (`ModelLock`, `src/hmi/hmi_model.cpp`). Mọi
   `hmi::set*()` gọi được từ bất kỳ task nào. Mỗi vòng (5 ms), task `ui` khoá,
   chép model sang `s_snap`, xoá cờ dirty rồi mở khoá. Phần vẽ chỉ đọc `s_snap`.
3. **Callback của HMI chạy trong task `ui`** nên chỉ được `xQueueSend(..., 0)`:
   không ghi NVS, không gọi mạng, không chạm thẻ nhớ. Vi phạm là UI đơ và
   watchdog sẽ reset board.
4. **Trạng thái dùng chung ngoài model** là `std::atomic`: `g_relay[]`,
   `g_relayChange[]`, `g_publishPending`, `g_minFreeHeap`. Lịch sử cảm biến và
   nhật ký sự kiện trong PSRAM (`src/datalog.cpp`) có mutex riêng.
5. **Chỉ task `storage` chạm thẻ nhớ** sau `storage::start()`. Trước đó (trong
   `setup()`) có đọc đồng bộ `config.json` và ghi dòng log "Boot".
6. **Chụp màn hình:** task `ui` chỉ lấy snapshot (`lv_snapshot_take`, ~vài chục
   ms). Phần mã hoá và gửi ra serial/thẻ nhớ (chậm) chạy ở task gọi.
7. **Gửi vào queue không bao giờ chờ.** Queue đầy thì bỏ qua message (log, lệnh
   lặp lại). Không task nào bị kẹt vì task khác.

## 3.5 Watchdog

- `setup()` gọi `esp_task_wdt_init(10, true)`: timeout 10 giây, **panic khi quá hạn**.
- `ui` và `app` đăng ký (`esp_task_wdt_add`) và reset mỗi vòng.
- Treo quá 10 giây thì board tự khởi động lại. Lần boot sau, `events.log` ghi
  `Boot fw=... reset=<mã>` (mã 6/7 = watchdog, xem 07-go-loi.md).
- `net`, `devices`, `storage` **không** đăng ký, vì chúng được phép chặn lâu
  (TCP timeout, flash firmware từ thẻ).

## 3.6 Thứ tự khởi động (`setup()`)

1. Serial 115200, in firmware + lý do reset.
2. Đặt relay về LOW.
3. `display.begin()`, `display.setBrightness(255)`. Không `pinMode` GPIO38 sau
   đó: làm vậy tách chân khỏi PWM và đèn nền kẹt ở 100% (08-nhat-ky.md, mục 24).
4. `datalog::begin()`: cấp phát vòng đệm lịch sử / nhật ký trong PSRAM.
5. `storage::begin()` mount thẻ (sau display vì dùng chung SCK/MOSI) và LittleFS,
   nạp lịch sử (bản mới hơn giữa thẻ và flash), nạp sẵn đuôi `events.log` → log
   "Boot" → đọc `config.json` (từ thẻ, không có thì bản sao trong flash).
6. NVS `hmi`: độ sáng, màn hình chờ, bộ màu → đưa vào model HMI.
7. Watchdog 10 giây (panic).
8. Tạo queue `app`, `net`.
9. `hmi::configureDevices()` (trang Điều khiển dựng theo danh sách này) →
   `hmi::begin()` tạo task `ui`.
10. `tasmota::start()` (với URL web), `storage::start()`, tạo task `app`, `net`,
    `console`. Task `net` quét Wi-Fi một lần (đồng bộ) trước khi thử kết nối.

## 3.7 Bản đồ mã nguồn

```
include/
  config.h              cấu hình: Wi-Fi, MQTT, chỉ số, web Tasmota, tên relay, đèn nền
  datalog.h             lịch sử cảm biến + nhật ký sự kiện trong PSRAM
  hmi.h                 API công khai của HMI (setters, callbacks, screenshot)
  storage.h             thẻ nhớ + LittleFS (task storage)
  wifi_manager.h        Wi-Fi (chạy trong task net)
  tasmota.h             thiết bị Tasmota qua web (task devices)
  lgfx_4848s040_reference.h   driver LCD/cảm ứng LovyanGFX (đã kiểm chứng)
  lv_conf.h             cấu hình LVGL 9 (OS FreeRTOS, 2 luồng vẽ, cấp phát PSRAM)
src/
  main.cpp              setup, task app/net/console, MQTT, callbacks, lệnh serial
  datalog.cpp           vòng đệm 10 s/24 h + 1 phút/7 ngày, 400 sự kiện, lưu/nạp
  wifi_manager.cpp      mạng đã biết, quét, mạng mở, kết nối, giãn cách thử lại
  tasmota.cpp           client HTTPS keep-alive tới web, poll 1 s, toggle
  storage.cpp           mount, log, config.json, LittleFS, lịch sử, sao lưu, xem tệp, ảnh
  hmi/hmi_model.*       trạng thái + dirty bit + mutex, bộ đệm văn bản/ảnh của trình xem
  hmi/hmi_ui.cpp        widget LVGL, task ui, request queue, screenshot
  hmi/hmi_theme.*       token màu, 4 bộ màu, style, icon
  hmi/hmi_port.*        LVGL ↔ LovyanGFX: flush, cảm ứng, tick, nút vật lý tuỳ chọn
  hmi/lv_mem_psram.*    bộ cấp phát LVGL vào PSRAM, đếm bộ nhớ, lệnh bench
  hmi/fonts/*.c         font/icon sinh bởi tools/gen_lvgl_fonts.sh
  hmi/images/*.c        logo BK sinh bởi tools/png_to_lvgl.py
tools/
  gen_lvgl_fonts.sh     sinh font LVGL (Be Vietnam Pro + Lucide)
  check_font_coverage.py   kiểm tra ký tự trong chuỗi UI có trong font
  png_to_lvgl.py        PNG → ảnh LVGL (có viền trắng cho logo trên nền tối)
  tabular_digits.py     làm chữ số đều rộng trong TTF
  screenshot.py         chụp màn hình qua serial (`shot`, `shot fb`, chuỗi lệnh)
assets/fonts/           TTF nguồn + giấy phép Lucide
config/layout.yaml      mô tả bố cục (tham khảo, firmware không đọc)
```
