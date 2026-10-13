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
| `ui` | 1 | 3 | 10 KB | `Request` (8) | **Task duy nhất gọi LVGL**: vẽ, cảm ứng, đèn nền, tự giảm sáng | `src/hmi/hmi_ui.cpp` |
| `app` | 1 | 4 | 4 KB | `AppMsg` (16) | relay trên panel, cảm biến (giả lập/thật), lưu cài đặt NVS | `src/main.cpp` |
| `net` | 0 | 2 | 8 KB | `NetMsg` (4) | Wi-Fi manager, MQTT, ArduinoOTA, NTP, cấp trạng thái cho header | `src/main.cpp`, `src/wifi_manager.cpp` |
| `devices` | 0 | 2 | 6 KB | `Toggle` (8) | Tasmota qua HTTP | `src/tasmota.cpp` |
| `storage` | 0 | 1 | 6 KB | `Item` (24) | thẻ nhớ: log, thống kê dung lượng, cập nhật firmware, ảnh BMP | `src/storage.cpp` |
| `console` | 0 | 1 | 4 KB | (Serial) | lệnh serial | `src/main.cpp` |

- **Chia core:** stack Wi-Fi của ESP-IDF chạy trên core 0, nên mọi thứ liên
  quan mạng và thẻ nhớ đặt ở core 0. Giao diện và logic cần phản hồi nhanh đặt
  ở core 1.
- **Ưu tiên:** `app` (4) cao hơn `ui` (3) để bật/tắt relay không phải chờ
  khung hình. Việc của `app` rất ngắn nên không làm chậm UI.
- **Vòng `loop()` của Arduino tự xoá** (`vTaskDelete`) sau `setup()`.

Stack còn trống đo bằng lệnh `tasks` (sau khi chạy):

```
ui       stack free  4424 B  prio 3
app      stack free  2128 B  prio 4
net      stack free  5608 B  prio 2
devices  stack free  5292 B  prio 2
storage  stack free  3204 B  prio 1
console  stack free  2368 B  prio 1
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
    │                 deviceToggle ──────── Toggle ─▶ devices ─HTTP─▶ Tasmota
    │                 firmwareUpdate ────── Item ───▶ storage ─SPI──▶ thẻ TF
    └─ shot sd ─────────────────────────── Item ───▶ storage
       mọi task ── storage::logEvent() ─── Item ───▶ storage (không chặn)
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
   `g_relayChange[]`, `g_publishPending`, `g_minFreeHeap`.
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
3. `display.begin()`, GPIO38 HIGH (đèn nền).
4. `storage::begin()` mount thẻ (sau display vì dùng chung SCK/MOSI) → log
   "Boot" → đọc `config.json` (MQTT, tên phòng, danh sách Wi-Fi, Tasmota).
5. NVS `hmi`: độ sáng, tự giảm sáng, bộ màu → đưa vào model HMI.
6. Watchdog 10 giây (panic).
7. Tạo queue `app`, `net`.
8. `hmi::configureDevices()` (trang Điều khiển dựng theo danh sách này) →
   `hmi::begin()` tạo task `ui`.
9. `tasmota::start()`, `storage::start()`, tạo task `app`, `net`, `console`.

## 3.7 Bản đồ mã nguồn

```
include/
  config.h              cấu hình: Wi-Fi, MQTT, chỉ số, Tasmota, tên relay
  hmi.h                 API công khai của HMI (setters, callbacks, screenshot)
  storage.h             thẻ nhớ (task storage)
  wifi_manager.h        Wi-Fi (chạy trong task net)
  tasmota.h             thiết bị Tasmota (task devices)
  lgfx_4848s040_reference.h   driver LCD/cảm ứng LovyanGFX (đã kiểm chứng)
  lv_conf.h             cấu hình LVGL 9
src/
  main.cpp              setup, task app/net/console, MQTT, callbacks
  wifi_manager.cpp      mạng đã biết, quét, kết nối, giãn cách thử lại
  tasmota.cpp           poll/toggle qua HTTP
  storage.cpp           mount, log, config.json, thống kê, cập nhật, BMP
  hmi/hmi_model.*       trạng thái + dirty bit + mutex
  hmi/hmi_ui.cpp        widget LVGL, task ui, request queue, screenshot
  hmi/hmi_theme.*       token màu, 3 bộ màu, style, icon
  hmi/hmi_port.*        LVGL ↔ LovyanGFX: flush, cảm ứng, tick, nút vật lý tuỳ chọn
  hmi/fonts/*.c         font/icon sinh bởi tools/gen_lvgl_fonts.sh
tools/
  gen_lvgl_fonts.sh     sinh font LVGL (Be Vietnam Pro + Lucide)
  tabular_digits.py     làm chữ số đều rộng trong TTF
  screenshot.py         chụp màn hình qua serial
assets/fonts/           TTF nguồn + giấy phép Lucide
config/layout.yaml      mô tả bố cục (tham khảo, firmware không đọc)
```
