# 4. Giao diện HMI (LVGL 9)

## 4.1 Tổng quan

- **Thư viện:** LVGL 9.2.2, chạy trong task `ui`. LovyanGFX chỉ còn làm driver
  LCD và cảm ứng bên dưới (`src/hmi/hmi_port.cpp`).
- **Render từng phần:** 1 buffer 480×64 RGB565 (60 KB) trong SRAM nội. Flush
  là phép chép đồng bộ nên buffer thứ hai không giúp gì; một buffer cao hơn
  thì số lượt vẽ mỗi khung giảm một nửa. Flush đảo byte cả khối
  (`lv_draw_sw_rgb565_swap`) rồi `pushImage` dạng `swap565_t`, để LovyanGFX
  `memcpy` từng hàng vào framebuffer trong PSRAM. Nếu không cấp phát được SRAM
  nội thì lùi về PSRAM (chậm hơn).
- **Vẽ trên 2 nhân:** `LV_USE_OS = LV_OS_FREERTOS`, `LV_DRAW_SW_DRAW_UNIT_CNT = 2`
  (mỗi luồng vẽ 8 KB stack). Chỉ task `ui` gọi LVGL; hai luồng vẽ là nội bộ của LVGL.
- **Không dùng fade cho vùng lớn** (chuyển trang, bảng nhập mật khẩu Wi-Fi,
  màn hình khởi động). LVGL 9.2 coi đối tượng có nền đặc là che kín vùng đó
  ngay cả khi `opa` đang chạy hiệu ứng, nên không vẽ lại phần bên dưới mà trộn
  lên nội dung cũ trong bộ đệm vẽ (dải vừa vẽ trước). Trên màn hình thật, kết
  quả là bàn phím lặp lại mỗi 64 px.
- **Biên dịch `-O2`** thay cho `-Os` (`platformio.ini`).
- **Bộ nhớ LVGL ở PSRAM:** `LV_USE_STDLIB_MALLOC = LV_STDLIB_CUSTOM`, cấp phát
  trong `src/hmi/lv_mem_psram.c` (PSRAM trước, hết thì lùi về SRAM nội). Model
  và bản chụp `s_snap` (khoảng 5,5 KB mỗi cái) cũng nằm ở PSRAM (`newModel()`).
- **Chỉ vẽ lại phần thay đổi:** model có cờ dirty theo nhóm (`D_PRIMARY`,
  `D_RELAYS`, `D_DEVICES`…). Mỗi vòng, task `ui` chỉ cập nhật widget của nhóm
  có cờ bật, và LVGL chỉ invalidate vùng của các widget đó.
- **Chu kỳ:** task `ui` chạy mỗi 5 ms; `LV_DEF_REFR_PERIOD` = 20 ms.

## 4.2 Nhận diện Trường Đại học Bách Khoa

Theo bộ nhận diện trong `docs/brand/source/brand-guidelines.pdf`:
- **Màu thương hiệu:** xanh `#1488DB` và xanh đậm `#030391` (trang 04).
- **Trên nền tối phải dùng logo có viền trắng** (trang 05).

| Vị trí | Nội dung |
|---|---|
| **Màn khởi động** (2 giây, tắt ngay, không mờ dần) | logo 132 px; "ĐẠI HỌC QUỐC GIA TP.HCM"; "TRƯỜNG ĐẠI HỌC BÁCH KHOA"; vạch xanh `#1488DB`; "Smart Home Panel". Chỉ hiện lúc bật nguồn, không hiện lại khi đổi bộ màu; chạm vào lúc này không có tác dụng |
| **Header** | logo 32 px trước tên phòng |
| **Cài đặt → GIỚI THIỆU** | logo 48 px, tên ĐHQG-HCM, tên trường, phiên bản firmware (thay cho hàng "Firmware" cũ) |
| **Bộ màu "Bách Khoa"** | màu chữ ký `#1488DB` (Cài đặt → Màu chủ đạo, hoặc `theme 3`) |
| **Màn hình chờ** | cùng cụm logo với màn khởi động (mục 4.10) |

**File logo:**
- Ảnh gốc độ phân giải cao: `assets/brand/bk-logo.png` (1657×1674, lấy từ
  bộ nhận diện).
- Ảnh LVGL `src/hmi/images/bk_logo_{32,48,132}.c` (RGB565 + alpha) được sinh bằng:

```bash
python tools/png_to_lvgl.py assets/brand/bk-logo.png 48 bk_logo_48 src/hmi/images/bk_logo_48.c --outline
```

`--outline` thêm viền trắng theo cách sau: nới rộng hình dạng logo ở độ phân
giải gấp 4, tô trắng, đặt logo lên trên, rồi mới thu nhỏ để mép viền mịn.
Thêm logo/ảnh mới: sinh file `.c`, khai báo `LV_IMAGE_DECLARE(ten);` (**có dấu
`;`**, macro của LVGL 9 không tự thêm) trong `hmi_theme.h`, rồi dùng
`lv_image_set_src(obj, &ten)`.

## 4.3 Khung màn hình

Trang Tổng quan:

```
y=0    ┌────────────────────────────────────────────┐
y=16   │ [BK] Phòng khách            09:32  ᯤ  ⏚ AC │  header (chỉ ở Tổng quan)
       │      Wi-Fi OK · MQTT mất kết nối            │  dòng trạng thái kết nối
y=64   ├────────────────────────────────────────────┤
       │                                            │
       │      gauge + 2 thẻ chỉ số                  │  y 64..392 (328 px)
       │                                            │
y=408  ├────────────────────────────────────────────┤
       │ [⌂ Tổng quan] [⚙ Điều khiển] [⚙ Cài đặt]  │  thanh điều hướng 448×56
y=464  └────────────────────────────────────────────┘
```

Lề 16 px, lưới 8 px, vùng chạm từ 44 px trở lên. Các trang đổi **tức thì**,
không có hiệu ứng mờ (xem 4.1). Header và thanh điều hướng chỉ hiện ở nơi cần
(`selectPage()`):

| Trang | Header | Thanh điều hướng | Vùng nội dung |
|---|---|---|---|
| Tổng quan | có | có | y 64..392 (`kContentY`, `kContentH`) |
| Điều khiển, Cài đặt | ẩn | có | y 4..392 (`kTabY`, `kTabH`) |
| Trang con của Cài đặt: Wi-Fi, chi tiết, tệp, xem tệp | ẩn | ẩn, thoát bằng nút ← | toàn màn hình: thanh tiêu đề 64 px (`kBarH`), nội dung x 24..456, y 64..456 |

- **Trang con dùng một lề duy nhất 24 px** (`kSubPad`): mũi tên ←, nút
  bên phải, nội dung hai bên và mép dưới. Thẻ và ghi chú lấy độ rộng 100% của
  cột chứa (448 px ở tab, 432 px ở trang con).
- **Tab chính: lề nhìn thấy đều 16 px** ở cả bốn phía, đo trên ảnh chụp: tính tới pixel
  đầu tiên được vẽ (logo, chữ hoa của tiêu đề mục, mép thẻ), không phải tới
  mép hộp LVGL.
- **Thanh tiêu đề trang con** (`barTitle()`, hằng `kBar*` trong `hmi_ui.cpp`):
  - mũi tên ← và nút hành động bên phải (làm mới) cách mép 24 px, nằm trên
    đường giữa y = 32;
  - tiêu đề căn giữa màn hình, chữ dài bị cắt thành "…";
  - trang Tệp ghi đường dẫn dạng "logs / 2026", ở thư mục gốc ghi "Tệp trên thẻ".
- Đồng hồ trong header bị ẩn khi chưa đồng bộ giờ (thay vì hiện "--:--").

**Dòng trạng thái kết nối** (màu vàng cam khi có lỗi):

| Trạng thái | Chữ | Icon Wi-Fi |
|---|---|---|
| Đang kết nối (20 giây đầu) | "Đang kết nối…" | xám |
| Wi-Fi + MQTT OK | "Đã kết nối · MQTT" | trắng |
| Wi-Fi OK, MQTT lỗi | "Wi-Fi OK · MQTT mất kết nối" | trắng |
| Mất Wi-Fi | "Mất kết nối · đang thử lại" | wifi-off màu cảnh báo |

## 4.4 Các trang

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
  phải ("Đang kết nối…", "Trực tuyến", hoặc **"Ngoại tuyến"** màu cảnh báo).
  Mỗi kênh một hàng. Khi thiết bị ngoại tuyến, các hàng mờ 50% và **không bấm
  được**. Dữ liệu đi qua web (05-ket-noi.md, mục 5.3), hỏi lại mỗi 1 giây.

### Cài đặt (cuộn dọc)

Ngoài trang Cài đặt chỉ giữ 8 hàng hay dùng; phần chi tiết nằm trong trang con.

| Nhóm | Hàng | Hành vi |
|---|---|---|
| (đầu trang) | thẻ logo BK › | trang Giới thiệu |
| KẾT NỐI | Wi-Fi: "SSID · RSSI dBm" / "Mất kết nối" › | mở trang Wi-Fi và quét |
| | Mạng & MQTT: IP / "MQTT lỗi" / "Chờ Wi-Fi" › | trang Mạng & MQTT |
| MÀN HÌNH | Độ sáng màn hình (slider 0–100%, đèn thật 50–100%) | xem trước khi kéo, lưu NVS khi thả |
| | Màn hình chờ sau 2 phút (công tắc) | lưu NVS; xem "Màn hình chờ" bên dưới |
| | Màu chủ đạo: Lime / Ember / Cobalt / Bách Khoa ● | chạm để đổi vòng, lưu NVS |
| DỮ LIỆU & HỆ THỐNG | Thẻ nhớ: "Trống x / y GB" / "Không có thẻ" › | trang Thẻ nhớ |
| | Lịch sử cảm biến: "x giờ" › | biểu đồ 1 giờ / 24 giờ / 7 ngày |
| | Hệ thống: "RAM x% · y giờ" › | trang Hệ thống |

### Trang chi tiết (trang con của Cài đặt)

Các hàng có dấu `›` mở một trang chi tiết dùng chung, gồm nút quay lại, tiêu
đề và các thẻ chứa cặp tên–giá trị. Nội dung **tự cập nhật khi đang mở** (khi
thông tin hệ thống, thẻ nhớ, kết nối hoặc giờ thay đổi).

Trang chi tiết có thể mở trang chi tiết khác (`openSubDetail()`); nút ←
khi đó quay về trang cha, ví dụ Thẻ nhớ → config.json → ← về Thẻ nhớ.

| Hàng trong Cài đặt | Trang chi tiết |
|---|---|
| Thẻ nhớ | trạng thái, loại thẻ (SD/SDHC), tốc độ SPI, dung lượng đã dùng (thanh %); các hàng **Tệp trên thẻ ›**, **config.json ›** (nạp từ thẻ / flash), **Cập nhật firmware** (chạm 2 lần trong 4 giây); đường dẫn log, sao lưu firmware |
| config.json (trong Thẻ nhớ) | nạp từ đâu, tên phòng, số mạng Wi-Fi, broker MQTT, số thiết bị Tasmota |
| Mạng & MQTT | Wi-Fi: trạng thái, SSID, cường độ sóng (Tốt/Trung bình/Yếu), BSSID, kênh; địa chỉ: IP, gateway, subnet, DNS, MAC, hostname; rồi MQTT: trạng thái kèm mã `PubSubClient::state()` diễn giải tiếng Việt, broker, đăng nhập, client ID, keep-alive, chính sách thử lại, các topic |
| Hệ thống | bộ nhớ kiểu "instrument", **stream 4 lần/giây**: RAM nội (màu chủ đạo) và PSRAM (tím) đặt cạnh nhau, mỗi loại có tiêu đề màu (kiêm chú thích), đã dùng/tổng và **% cỡ 64 px** (vàng cam khi > 90%); bên dưới là **một biểu đồ 15 giây chung cho cả hai đường**, có trục giá trị 100/75/50/25/0 khớp lưới, mốc "15 GIÂY TRƯỚC → BÂY GIỜ" và chấm điểm mới nhất cho từng đường; Flash: % firmware so với khe OTA trên thanh 24 đoạn |
| Hệ thống (tiếp) | phân bổ PSRAM, LittleFS; thời gian: đã chạy, giờ hệ thống (sau NTP), múi giờ, lý do khởi động lần này; hàng **Nhật ký sự kiện ›** |
| Lịch sử cảm biến | chọn 1 giờ / 24 giờ / 7 ngày; ba biểu đồ nhiệt độ, độ ẩm, công suất với giá trị mới nhất và min/max trên trục; vẽ lại mỗi 10 giây (06-the-nho.md, mục 6.11) |
| Nhật ký sự kiện (trong Hệ thống) | 100 sự kiện mới nhất, mới ở trên, giờ và nội dung; cập nhật khi có sự kiện mới |
| thẻ Giới thiệu | logo BK, sản phẩm, firmware, ngày build, phần cứng, màn hình, chip, CPU, MAC, ESP-IDF, LVGL |

- **Màu giá trị:** xanh lá = tốt; vàng cam = cần chú ý (mất kết nối, sóng
  yếu, heap thấp, chưa có NTP, reset do watchdog/brownout); thanh % chuyển
  vàng cam khi dùng quá 85%.
- **Trang Hệ thống chạy trực tiếp:** `liveMemory()` trong task `ui` đọc
  `heap_caps_*` **mỗi 250 ms**, sửa chữ/thanh **tại chỗ** (thanh trượt 300 ms)
  và đẩy một điểm vào `lv_chart` (60 điểm = 15 giây, chế độ SHIFT); không dựng
  lại trang. Lúc chụp màn hình, đường PSRAM nhô lên khoảng 5%: đó là bộ đệm
  snapshot ~450 KB. Lần cập nhật 5 giây của `SystemInfo` cũng không dựng
  lại trang này.
- **Nguồn dữ liệu:**
  - `SystemInfo` do task `net` gửi mỗi 5 giây;
  - `StorageInfo` do task `storage` gửi khi mount, khi cắm/rút thẻ và mỗi 10 phút.

### Tệp trên thẻ (trang con của Cài đặt)

Mở từ Cài đặt → **Thẻ nhớ** → **Tệp trên thẻ ›**.

- **Danh sách:** thư mục đứng trước, sắp theo tên; icon theo loại tệp (thư mục,
  văn bản/log, csv, json/bin, ảnh); kích thước tệp; tối đa 32 mục mỗi thư mục.
- **Thanh trên:** nút ← lên thư mục cha (ở thư mục gốc thì về trang Thẻ nhớ),
  tiêu đề là đường dẫn dạng "logs / sensors" ("Tệp trên thẻ" ở thư mục gốc),
  nút tải lại.
- **Chạm một tệp** mở trang xem tệp (06-the-nho.md, mục 6.10):
  - đường dẫn và kích thước;
  - `.csv`: **bảng** (`lv_table`), 200 dòng mới nhất, dòng tiêu đề tô nền;
  - `.jpg .jpeg .png .bmp`: **ảnh** thu nhỏ vừa 432 × 432;
  - tệp văn bản (`.txt .log .json .cfg .ini .md .yaml`): cả tệp nếu ≤ 64 KB,
    còn tệp lớn thì **64 KB cuối** (chỗ có log mới nhất), bắt đầu từ đầu một
    dòng, chia nhãn khoảng 1 KB;
  - nút **Xoá tệp**: chạm lần đầu hiện "Chạm lần nữa để xoá" (4 giây), chạm lần
    hai mới xoá. Thư mục chỉ xoá được khi trống. Sau khi xoá, quay về danh sách
    đã làm mới.
- **Mọi thao tác với thẻ chạy trong task `storage`**, UI chỉ gửi yêu cầu:
  - callback `fileList` / `fileRead` / `fileDelete` → `storage::requestList` /
    `requestRead` / `requestDelete`;
  - kết quả gửi về bằng `hmi::setFileList` / `setFileView`;
  - không có thẻ thì trả lời ngay là "Không có thẻ nhớ", không treo ở trạng thái
    đang tải.

**Giữ vị trí cuộn khi làm mới:** trang chi tiết, danh sách Wi-Fi và danh sách
tệp được vẽ lại mỗi khi dữ liệu đổi. Vị trí cuộn được lưu trước khi xoá và khôi
phục sau khi dựng lại (`lv_obj_update_layout` rồi `lv_obj_scroll_to_y`), nên
không còn nhảy lên đầu. Chỉ khi **mở** một trang hoặc thư mục khác thì mới về đầu.

### Wi-Fi (trang con của Cài đặt)
- Nút quay lại, tiêu đề, nút quét lại (thành spinner khi đang quét).
- **Danh sách mạng:**
  - Mỗi SSID một dòng, trùng tên thì giữ mạng sóng mạnh nhất; tối đa 20 mạng.
  - Icon mức sóng: ≥ -60 dBm đầy, ≥ -72 dBm trung bình, còn lại yếu.
  - Icon khoá nếu mạng có mật khẩu; nhãn "Đã kết nối" cho mạng hiện tại,
    "Đã lưu" cho mạng đã có mật khẩu trên panel.
- **Chạm một mạng sẽ mở màn nhập mật khẩu** (toàn màn hình):
  - ô mật khẩu ẩn ký tự bằng `•`, nút mắt để hiện/ẩn;
  - **bàn phím** (`buildKeyboard()`) kín phần dưới màn hình (y 188–480), gồm
    5 hàng phím cao khoảng 48 px:
    - **chữ thường / chữ hoa:** hàng số `1–0`, ba hàng chữ kiểu điện thoại, rồi
      hàng `1#` `-` `cách` `.` `✓`;
    - **ký hiệu (`1#`):** `! @ # $ % ^ & * ( ) - _ = + [ ] { } ; : ' " / \ ? < > ,`;
    - phím chữ màu `#262626`, phím chức năng `#161616`, phím đang nhấn `#4A4A4A`;
      **phím ✓ dùng màu chủ đạo**;
    - nhấn phím chữ thì hiện bong bóng phóng to (popover); giữ phím ⌫ để xoá liên tục;
    - font Be Vietnam Pro, tự lùi về Montserrat 16 cho ký hiệu ⌫ ✓ (`lv_font_t::fallback`);
    - mỗi lần mở lại màn hình thì về chế độ chữ thường;
  - nút **Kết nối** hoặc phím OK để gửi, phím ẩn bàn phím để đóng;
  - **mạng mở:** không có bàn phím; thẻ "Mạng mở · không cần mật khẩu · dữ liệu
    không được mã hoá" thế chỗ ô mật khẩu, nút Kết nối rộng hết cột;
  - **mạng đã lưu mật khẩu** (nhãn "Đã lưu" trong danh sách): thẻ "Đã lưu mật
    khẩu", nút Kết nối dùng mật khẩu đã lưu, kèm nút "Nhập mật khẩu khác";
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

**Căn giữa chữ theo chiều dọc với Be Vietnam Pro.** Font chừa khoảng trống
phía trên chữ hoa cho dấu tiếng Việt chồng tầng, nên chữ không dấu (`q`, `1`,
`ABC`) canh giữa theo hình học sẽ trông **thấp khoảng 2 px**. Cách bù:
- **phím bàn phím:** `kbDrawTask()` đặt `ofs_y = -2` cho nhãn, trừ ký hiệu
  Montserrat;
- **ô mật khẩu:** lề 11/15 thay vì 13/13. Vùng nội dung phải đúng bằng một dòng
  (52 − 2 viền − 26 lề = 24 px); nếu nhỏ hơn, ô một dòng sẽ tự cuộn và **đẩy
  chữ ra khỏi tầm nhìn**.

### Màn cập nhật firmware
Phủ toàn màn hình, gồm icon tải xuống, thanh tiến trình và dòng "x% · không
tắt nguồn". Board tự khởi động lại khi xong.

## 4.5 Hành vi chung

- **Chạm → nhả mới kích hoạt:** nền đổi sang màu nhấn trong 150 ms. Trượt tay
  ra khỏi đối tượng thì huỷ.
- **Nghiệp vụ là nguồn sự thật duy nhất:** chạm relay trên panel chỉ gửi yêu
  cầu, công tắc chỉ đổi khi `app` xác nhận. Vì vậy màn hình, MQTT và lệnh
  serial luôn khớp nhau. Riêng Tasmota (đi qua web, khoảng 0,4 s) thì công tắc
  đổi ngay, rồi chỉnh theo trạng thái web trả về; lỗi thì trả lại như cũ.
- **Màn hình chờ:** sau 2 phút không chạm (mục 4.10). Lần chạm đầu tiên chỉ
  đánh thức màn hình (bị "nuốt"), không bấm nhầm nút.
- **Không có cảm ứng:** mọi phần tử bấm được nằm trong `lv_group` mặc định, có
  viền focus 2 px. Định nghĩa `HMI_BTN_PREV_PIN`, `HMI_BTN_NEXT_PIN`,
  `HMI_BTN_OK_PIN` trong `config.h` để dùng nút vật lý (board này không còn
  chân trống).

## 4.6 Bộ màu

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
| **Bách Khoa** | `#1488DB` | `#A78BFA` | `#5AC8FA` | `#FFC94D` | `#3DDC97` | `#FF9F1C` | `#FF4D5E` |

- **Cảnh báo luôn khác sắc với màu chữ ký,** vì vậy Ember dùng vàng cho cảnh báo.
- **Đổi bộ màu:** Cài đặt → Màu chủ đạo, hoặc lệnh `theme 0|1|2|3` (lệnh serial
  không lưu NVS). Task `ui` dựng lại toàn bộ widget (`lv_obj_clean` rồi build
  lại). Dữ liệu nằm trong model nên không mất; vị trí cuộn và trang chi tiết
  đang mở được giữ nguyên.
- **Thêm bộ màu:** thêm một dòng vào `kPalettes` (`src/hmi/hmi_theme.cpp`) và
  tăng `kPaletteCount` (`hmi_theme.h`).

## 4.7 Font & icon

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
  `° · – • … × ± ≤ ≥`; 4 bpp, không nén.
- **Be Vietnam Pro không có mũi tên** (`← → ›`…): đừng dùng trong chuỗi giao
  diện (viết "5–60 giây"). Mũi tên dạng icon thì lấy từ Lucide (`ICON_CHEVRON`,
  `ICON_BACK`).
- **Kiểm tra sau khi thêm chữ:** `python tools/check_font_coverage.py`.
  - Tool quét mọi chuỗi trong `src/` và `config.h`, đối chiếu với glyph **thật
    sự có** trong các font đã biên dịch.
  - Báo ký tự nào sẽ hiện thành **ô vuông** trên màn hình.
  - Cũng báo các ký tự nằm trong dải `TEXT` mà file TTF không có.
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

Icon đang có (tên macro trong `src/hmi/hmi_theme.h`): `ICON_WIFI`,
`ICON_WIFI_OFF`, `ICON_WIFI_LOW`, `ICON_WIFI_HIGH`, `ICON_PLUG`, `ICON_PLUG_ZAP`,
`ICON_DROPLET`, `ICON_ZAP`, `ICON_HOUSE`, `ICON_SLIDERS`, `ICON_SETTINGS`,
`ICON_BULB`, `ICON_SNOWFLAKE`, `ICON_SUN`, `ICON_CLOCK`, `ICON_MEMORY`,
`ICON_SERVER`, `ICON_GLOBE`, `ICON_INFO`, `ICON_ALERT`, `ICON_ERROR`,
`ICON_THERMO`, `ICON_PALETTE`, `ICON_CHEVRON`, `ICON_LOCK`, `ICON_REFRESH`,
`ICON_BACK`, `ICON_EYE`, `ICON_EYE_OFF`, `ICON_DOWNLOAD`, `ICON_DRIVE`,
`ICON_FILE`, `ICON_FILE_PLAIN`, `ICON_FILE_IMAGE`, `ICON_FILE_CODE`,
`ICON_FILE_SHEET`, `ICON_FOLDER`, `ICON_FOLDER_TREE`, `ICON_TRASH`, `ICON_CHECK`.

## 4.8 API của HMI (`include/hmi.h`)

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
| `setFileList(list)` / `setFileView(view, text, pixels)` | trình duyệt và trình xem tệp (văn bản, ảnh chép vào PSRAM) |
| `showPage(n)` / `showDetail(topic)` / `scrollPage(dy)` | chuyển trang (0..6), mở trang chi tiết (0..8), cuộn (gửi request vào queue) |
| `previewFile(path)` / `previewKeyboard(open)` | mở trình xem tệp / màn nhập mật khẩu (dùng cho lệnh serial) |
| `benchmark(policy)` / `injectDrag(x, y0, y1, ms)` | đo tốc độ vẽ / chạm giả lập |
| `screenshot(Print&)` / `screenshotFramebuffer(Print&, ms)` / `screenshotBmp(Print&)` | chụp màn hình bằng LVGL / từ framebuffer thật / BMP (task gọi tự gửi dữ liệu đi) |

Callback (chạy trong task `ui`, **chỉ được post**): `relayToggle`,
`brightnessChanged`, `autoDimChanged`, `themeChanged`, `backlight`, `wifiScan`,
`wifiJoin`, `firmwareUpdate`, `deviceToggle`, `fileList`, `fileRead`, `fileDelete`.

## 4.9 Thêm một màn hình mới

1. Thêm giá trị vào `enum Page` trong `src/hmi/hmi_ui.cpp`, trước `P_COUNT`. Nếu
   là trang con thì không thêm vào thanh điều hướng (`kNavCount`).
2. Viết `buildXxx(lv_obj_t* page)`. Trang cuộn được thì gọi `makeScrollPage(page)`
   rồi dùng `sectionTitle`, `settingsCard`, `settingsRow`, `switchRow`.
3. Gọi hàm đó trong `buildAll()`.
4. Dữ liệu cần hiển thị: thêm trường vào `Model` (`hmi_model.h`), một cờ `D_*`,
   một setter có `ModelLock` (`hmi_model.cpp`), và `applyXxx()` gọi trong `tick()`.
5. Hành động của người dùng: thêm callback vào `Callbacks` (`hmi.h`). Trong
   `main.cpp`, callback chỉ post vào queue của task phụ trách.

## 4.10 Màn hình chờ

Khi bật "Màn hình chờ sau 2 phút" và không có thao tác nào trong 2 phút
(`HMI_DIM_AFTER_MS` = 120000):

- Đèn nền giảm về mức sàn (`HMI_DIM_BRIGHTNESS_PCT`).
- Hiện cụm nhận diện Bách Khoa trên nền đen: logo, "ĐẠI HỌC QUỐC GIA TP.HCM",
  "TRƯỜNG ĐẠI HỌC BÁCH KHOA", vạch xanh BK, "Smart Home Panel". Đây là cùng
  cụm `brandLockup()` của màn hình khởi động.
- Cụm logo nằm chính giữa; mỗi phút chỉ dịch tối đa 4 px (mắt không thấy)
  để không điểm ảnh nào hiển thị một hình hàng giờ. Dịch 36 px như bản đầu
  trông như lỗi bố cục.
- Chạm lần đầu chỉ đánh thức (bị nuốt, không bấm nhầm nút bên dưới), rồi
  trở lại trang đang mở.
- Không hiện khi đang cập nhật firmware (`updatePct >= 0`).

