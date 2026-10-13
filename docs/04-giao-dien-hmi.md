# 4. Giao diện HMI (LVGL 9)

## 4.1 Tổng quan

- **Thư viện:** LVGL 9.2.2, chạy trong task `ui`. LovyanGFX chỉ còn làm driver
  LCD và cảm ứng bên dưới (`src/hmi/hmi_port.cpp`).
- **Render từng phần:** 2 buffer 480×48 RGB565 (45 KB mỗi buffer) trong SRAM
  nội, DMA-capable. Flush bằng `pushImage` vào framebuffer RGB trong PSRAM.
  Nếu không cấp phát được SRAM nội thì lùi về PSRAM (chậm hơn).
- **Chỉ vẽ lại phần thay đổi:** model có cờ dirty theo nhóm (`D_PRIMARY`,
  `D_RELAYS`, `D_DEVICES`…). Mỗi vòng, task `ui` chỉ cập nhật widget của nhóm
  có cờ bật, và LVGL chỉ invalidate vùng của các widget đó.
- **Chu kỳ:** task `ui` chạy mỗi 5 ms; `LV_DEF_REFR_PERIOD` = 20 ms.

## 4.2 Khung màn hình

```
y=0    ┌────────────────────────────────────────────┐
y=16   │ Phòng khách                 09:32  ᯤ  ⏚ AC │  header (cố định)
       │ Wi-Fi OK · MQTT mất kết nối                 │  dòng trạng thái kết nối
y=64   ├────────────────────────────────────────────┤
       │                                            │
       │         vùng nội dung: 4 trang             │  y 64..392 (328 px)
       │                                            │
y=408  ├────────────────────────────────────────────┤
       │ [⌂ Tổng quan] [⚙ Điều khiển] [⚙ Cài đặt]  │  thanh điều hướng 448×56
y=464  └────────────────────────────────────────────┘
```

Lề 16 px, lưới 8 px, vùng chạm từ 44 px trở lên. Header và thanh điều hướng
luôn hiển thị; các trang đổi bằng fade 200 ms.

**Dòng trạng thái kết nối** (màu vàng cam khi có lỗi):

| Trạng thái | Chữ | Icon Wi-Fi |
|---|---|---|
| Đang kết nối (20 giây đầu) | "Đang kết nối…" | xám |
| Wi-Fi + MQTT OK | "Đã kết nối · MQTT" | trắng |
| Wi-Fi OK, MQTT lỗi | "Wi-Fi OK · MQTT mất kết nối" | trắng |
| Mất Wi-Fi | "Mất kết nối · đang thử lại" | wifi-off màu cảnh báo |

## 4.3 Các trang

### Tổng quan
- **Gauge 270°** (232×232 tại x=124, y=64): vòng cung r=104, nét 12; số
  chính 64 px; đơn vị; nhãn; vạch cắt tại ngưỡng cảnh báo; nhãn min/max.
- **Pill trạng thái** dưới số, cho chỉ số chính (luôn có màu + icon + chữ):

| `SensorState` | Hiển thị |
|---|---|
| `Ok` | vòng cung màu chữ ký, pill ● ACTIVE (xanh lá) |
| `Ok` và giá trị ≥ ngưỡng | vòng cung vàng cam, pill ⚠ WARNING, "Cao hơn ngưỡng 30 °C" |
| `Loading` | spinner chạy trên vòng cung, `--.-`, pill LOADING |
| `Error` | `--.-`, "Lỗi đọc cảm biến", pill ⊗ ERROR (đỏ) |
| `Stale` | giữ số cuối nhưng làm mờ, "Dữ liệu lúc HH:MM", pill OFFLINE |

- **2 thẻ chỉ số** 220×88: Độ ẩm (icon giọt nước) và Công suất (icon tia sét).
- **Animation:** số và vòng cung chuyển trong 250 ms (ease-out).

### Điều khiển (cuộn dọc)
- **RELAY TRÊN PANEL:** 3 hàng (60 px) gồm icon, tên, "Đang bật · Kênh n",
  công tắc. Chạm cả hàng để bật/tắt. Icon có màu riêng khi bật: đèn vàng,
  điều hoà xanh dương, ổ cắm xanh lá.
- **Mỗi thiết bị Tasmota một nhóm:** tiêu đề tên thiết bị và trạng thái bên
  phải ("Đang kết nối…", IP, hoặc **"Ngoại tuyến"** màu cảnh báo). Mỗi kênh
  một hàng. Khi thiết bị ngoại tuyến, các hàng mờ 50% và **không bấm được**.

### Cài đặt (cuộn dọc)

| Nhóm | Hàng | Hành vi |
|---|---|---|
| KẾT NỐI | Wi-Fi: "SSID · RSSI dBm" / "Mất kết nối" › | mở trang Wi-Fi và quét |
| MÀN HÌNH | Độ sáng màn hình (slider 10–100%) | xem trước khi kéo, lưu NVS khi thả |
| | Tự giảm sáng sau 30 giây (công tắc) | lưu NVS |
| | Màu chủ đạo: Lime / Ember / Cobalt ● | chạm để đổi vòng, lưu NVS |
| THẺ NHỚ | Thẻ nhớ: "Trống x / y GB" / "Không có thẻ" | |
| | config.json: "Đã nạp" / "Không có tệp" | |
| | Cập nhật firmware: "firmware.bin · x MB" | **chạm 2 lần** trong 4 giây để cài |
| TRẠNG THÁI HỆ THỐNG | MQTT, Địa chỉ IP, Bộ nhớ trống, Thời gian chạy, Firmware | chỉ hiển thị |

### Wi-Fi (trang con của Cài đặt)
- Nút quay lại, tiêu đề, nút quét lại (thành spinner khi đang quét).
- **Danh sách mạng:**
  - Mỗi SSID một dòng, trùng tên thì giữ mạng sóng mạnh nhất; tối đa 20 mạng.
  - Icon mức sóng: ≥ -60 dBm đầy, ≥ -72 dBm trung bình, còn lại yếu.
  - Icon khoá nếu mạng có mật khẩu; nhãn "Đã kết nối" cho mạng hiện tại.
- **Chạm một mạng sẽ mở màn nhập mật khẩu** (toàn màn hình):
  - ô mật khẩu ẩn ký tự bằng `•`, nút mắt để hiện/ẩn;
  - bàn phím LVGL dùng font Montserrat 16, vì font này có ký hiệu ⌫ ↵;
  - nút **Kết nối** hoặc phím OK để gửi, phím ẩn bàn phím để đóng;
  - mạng mở thì không hiện bàn phím, ghi "Mạng mở, không cần mật khẩu";
  - mật khẩu dưới 8 ký tự bị chặn ngay trên màn hình.
- **Kết quả kết nối:**

| Kết quả | Hiển thị |
|---|---|
| Đang kết nối | spinner + "Đang kết nối…", nút bị khoá |
| Thành công | "Đã kết nối · <IP>" màu xanh lá, tự đóng sau 1,5 giây |
| Sai mật khẩu (AUTH_FAIL, handshake timeout) | "Sai mật khẩu" |
| Không thấy mạng (NO_AP_FOUND) | "Không tìm thấy mạng" |
| Router không phản hồi (AUTH_EXPIRE, ASSOC_EXPIRE, BEACON_TIMEOUT) | "Sóng yếu, router không phản hồi" |
| Khác / quá 15 giây | "Không kết nối được" |

### Màn cập nhật firmware
Phủ toàn màn hình, gồm icon tải xuống, thanh tiến trình và dòng "x% · không
tắt nguồn". Board tự khởi động lại khi xong.

## 4.4 Hành vi chung

- **Chạm → nhả mới kích hoạt:** nền đổi sang màu nhấn trong 150 ms. Trượt tay
  ra khỏi đối tượng thì huỷ.
- **Nghiệp vụ là nguồn sự thật duy nhất:** chạm relay chỉ gửi yêu cầu. Công tắc
  chỉ đổi khi `app` (hoặc `devices` với Tasmota) xác nhận trạng thái mới. Vì vậy
  màn hình, MQTT và lệnh serial luôn khớp nhau.
- **Tự giảm sáng:** sau 30 giây không chạm, đèn nền giảm còn 15%. Lần chạm đầu
  tiên chỉ đánh thức màn hình (bị "nuốt"), không bấm nhầm nút.
- **Không có cảm ứng:** mọi phần tử bấm được nằm trong `lv_group` mặc định, có
  viền focus 2 px. Định nghĩa `HMI_BTN_PREV_PIN`, `HMI_BTN_NEXT_PIN`,
  `HMI_BTN_OK_PIN` trong `config.h` để dùng nút vật lý (board này không còn
  chân trống).

## 4.5 Bộ màu

Nền đen và các lớp xám làm khung, giống nhau ở mọi bộ màu:

| Token | Màu | Dùng |
|---|---|---|
| bg | `#000000` | nền |
| surface | `#121212` | thẻ, thanh điều hướng, phím |
| elevated | `#1F1F1F` | chip icon, mục đang chọn, đường kẻ |
| pressed | `#292929` | nền khi nhấn |
| track | `#2E2E2E` | rãnh công tắc tắt |
| text / muted | `#F5F5F5` / `#8A8A8A` | chữ / nhãn, icon khi tắt |

Mỗi bộ có màu chữ ký riêng, dùng cho vòng cung, công tắc bật, slider, tab
đang chọn và focus:

| Bộ | Chữ ký | Icon công suất | Độ ẩm, điều hoà | Đèn | Thành công | Cảnh báo | Lỗi |
|---|---|---|---|---|---|---|---|
| **Lime** (mặc định) | `#C5F82A` | `#9B87FF` | `#4DA3FF` | `#FFC94D` | `#3DDC97` | `#FF9F1C` | `#FF4D5E` |
| **Ember** | `#FF6B2C` | `#B794FF` | `#4DA3FF` | `#FFD166` | `#3DDC97` | `#FFD23F` | `#FF3B4E` |
| **Cobalt** | `#4C7DFF` | `#C084FC` | `#38D9F5` | `#FFC94D` | `#3DDC97` | `#FFB020` | `#FF4D5E` |

- **Cảnh báo luôn khác sắc với màu chữ ký,** vì vậy Ember dùng vàng cho cảnh báo.
- **Đổi bộ màu:** Cài đặt → Màu chủ đạo, hoặc lệnh `theme 0|1|2`. Task `ui` dựng
  lại toàn bộ widget (`lv_obj_clean` rồi build lại). Dữ liệu nằm trong model
  nên không mất.
- **Thêm bộ màu:** thêm một dòng vào `kPalettes` (`src/hmi/hmi_theme.cpp`) và
  tăng `kPaletteCount` (`hmi_theme.h`).

## 4.6 Font & icon

| Font | Nguồn | Dùng |
|---|---|---|
| `hmi_font_64_sb` | Be Vietnam Pro SemiBold 64, chỉ `0-9 . - % –` | số chính |
| `hmi_font_28_sb` | SemiBold 28, chỉ số | số trên thẻ |
| `hmi_font_20_m` | Medium 20, ASCII + `°` | đơn vị |
| `hmi_font_16_sb` | SemiBold 16 | tên phòng, giờ, tiêu đề, ô mật khẩu |
| `hmi_font_14_m` | Medium 14 (font mặc định) | điều hướng, hàng cài đặt |
| `hmi_font_13_r` | Regular 13 | chú thích |
| `hmi_font_12_sb` | SemiBold 12 | pill, tiêu đề nhóm viết hoa |
| `hmi_icons_20`, `hmi_icons_14` | Lucide (ISC), nét mảnh | icon |
| `lv_font_montserrat_16` | có sẵn trong LVGL | **chỉ cho phím bàn phím** (ký hiệu ⌫ ↵ ←) |

- **Ký tự trong font chữ:** ASCII, toàn bộ chữ cái tiếng Việt có dấu,
  `° · – • …`; 4 bpp, không nén.
- **Chữ số đều rộng:** Be Vietnam Pro không có tính năng `tnum`, nên
  `tools/tabular_digits.py` nới mọi chữ số bằng chữ số rộng nhất và căn giữa,
  để số thay đổi không làm bố cục xê dịch.
- **Sinh lại font:** cần Node và `pip install fonttools`:

```bash
bash tools/gen_lvgl_fonts.sh
```

**Thêm icon:**
1. Tìm codepoint trong `node_modules/lucide-static/font/info.json` (gói `lucide-static`).
2. Thêm codepoint vào `ICONS20` (hoặc `ICONS14`) trong `tools/gen_lvgl_fonts.sh`, rồi chạy script.
3. Thêm `#define ICON_TEN "\uEXXX"` vào `src/hmi/hmi_theme.h`.

Icon đang có: wifi, wifi-off, wifi-high, wifi-low, plug, droplet, zap, house,
sliders, settings, lightbulb, snowflake, plug-zap, sun, clock, memory-stick,
server, globe, info, triangle-alert, circle-x, thermometer, palette,
chevron-right, lock, refresh-cw, arrow-left, eye, eye-off, download,
hard-drive, file-text, circle-check.

## 4.7 API của HMI (`include/hmi.h`)

Gọi được từ **bất kỳ task nào** (có khoá, chi phí thấp):

| Hàm | Ý nghĩa |
|---|---|
| `setPrimary(value, state)` / `setMetric(i, value, state)` | chỉ số chính / 2 thẻ chỉ số |
| `setLink(Link)` / `setClock("HH:MM")` / `setSystemInfo(info)` | header, trạng thái hệ thống |
| `setRelay(i, on)` | relay trên panel |
| `configureDevices(list, n)` (trước `begin`) / `setDeviceState(dev, mask, online)` | Tasmota |
| `setBrightness(pct)` / `setAutoDim(on)` / `setTheme(i)` / `setRoomName(s)` | cài đặt |
| `setWifiNetworks(list, n)` / `setWifiScanning(b)` / `setWifiJoin(state, msg)` | trang Wi-Fi |
| `setStorage(info)` / `setUpdateProgress(pct)` (-1 = ẩn) | thẻ nhớ, cập nhật |
| `showPage(n)` / `scrollPage(dy)` | chuyển trang (0..3), cuộn (gửi request vào queue) |
| `screenshot(Print&)` / `screenshotBmp(Print&)` | chụp màn hình (task gọi tự gửi dữ liệu đi) |

Callback (chạy trong task `ui`, **chỉ được post**): `relayToggle`,
`brightnessChanged`, `autoDimChanged`, `themeChanged`, `backlight`, `wifiScan`,
`wifiJoin`, `firmwareUpdate`, `deviceToggle`.

## 4.8 Thêm một màn hình mới

1. Thêm giá trị vào `enum Page` trong `src/hmi/hmi_ui.cpp`, trước `P_COUNT`. Nếu
   là trang con thì không thêm vào thanh điều hướng (`kNavCount`).
2. Viết `buildXxx(lv_obj_t* page)`. Trang cuộn được thì gọi `makeScrollPage(page)`
   rồi dùng `sectionTitle`, `settingsCard`, `settingsRow`, `switchRow`.
3. Gọi hàm đó trong `buildAll()`.
4. Dữ liệu cần hiển thị: thêm trường vào `Model` (`hmi_model.h`), một cờ `D_*`,
   một setter có `ModelLock` (`hmi_model.cpp`), và `applyXxx()` gọi trong `tick()`.
5. Hành động của người dùng: thêm callback vào `Callbacks` (`hmi.h`). Trong
   `main.cpp`, callback chỉ post vào queue của task phụ trách.
