# 8. Nhật ký thay đổi & vấn đề đã gặp

Ghi theo thứ tự thời gian, mỗi mục gồm: triệu chứng → cách tìm ra → nguyên nhân
→ cách sửa. Chưa commit vào git (trạng thái ngày 13/10/2026).

---

## 1. Nạp firmware xong không có serial log, màn hình đen

- **Triệu chứng:** nạp thành công, serial 0 byte (kể cả log ROM).
- **Cách tìm ra:**
  1. esptool vẫn nói chuyện được với chip, firmware trên chip khớp bản build,
     `otadata` trỏ vào app0.
  2. Đọc eFuse: thấy `UART_PRINT_CONTROL = Disabled`.
  3. Nạp sketch tối giản: board mặc định thì có log; board JSON custom cộng
     `partitions_16MB.csv` thì mất log.
- **Nguyên nhân:**
  - Board JSON khai báo `upload.flash_size = 8MB`, nên header ghi 8 MB, trong
    khi bảng phân vùng là 16 MB. Bootloader từ chối chạy app.
  - eFuse và bootloader đều tắt log, nên lỗi hoàn toàn im lặng.
- **Sửa:** thêm `board_upload.flash_size = 16MB`. Header thành `0x4f`.
- **Phụ:** cài `intelhex` vào penv của PlatformIO, vì thiếu module này `pio run`
  không tạo được `bootloader.bin`.

## 2. Giao diện đầu tiên (LovyanGFX, font VLW trên SPIFFS)

- Màu thương hiệu BK, logo, font Be Vietnam Pro dạng VLW tự sinh
  (`make_vlw.py`). Font VLW cũ chỉ có cỡ 69 px và thiếu dấu câu.
- Có một lần reset không rõ lý do, nên thêm log `Reset reason`.
- **Lỗi đi kèm:** khi chụp màn hình nhị phân qua serial, log `[W]` của Wi-Fi
  chen vào làm hỏng ảnh. Sửa bằng `Serial.setDebugOutput(false)` trong lúc gửi.

## 3. Thiết kế UI/UX (canvas "ESP32 480 HMI")

Thiết kế theo brief: Home Dashboard 480×480 có gauge 270°, 2 thẻ chỉ số, thanh
điều hướng; bảng các trạng thái; design system; pixel specs. 5 ảnh tham khảo
trong brief **không được gửi kèm**, nên phần phân tích ảnh chưa làm.

## 4. Chuyển sang LVGL 9.2

- LovyanGFX chỉ còn làm driver LCD và cảm ứng. Font Be Vietnam Pro và icon
  Lucide biên dịch thẳng vào firmware (`gen_lvgl_fonts.sh`), chữ số được làm
  đều rộng (`tabular_digits.py`).
- **Tách lớp:** `hmi_model` (dữ liệu), `hmi_ui` (vẽ), `hmi_theme` (màu, style),
  `hmi_port` (driver).
- **Lỗi hiển thị phát hiện qua ảnh chụp và đã sửa:**
  - nhãn 16/36 đè lên đầu vòng cung;
  - đơn vị lệch baseline so với số (tính từ `base_line` của font);
  - giá trị trong Cài đặt bị co thành "…";
  - pill "SENSOR ERROR" quá dài, đổi thành "ERROR".

## 5. Bộ màu

- **Lần 1:** teal trên nền xám xanh. Người dùng thấy chói.
- **Lần 2:** đơn sắc xám. Người dùng thấy nhạt.
- **Lần 3:** nền xám, có màu ở những chỗ nổi bật.
- **Lần 4 (hiện tại):** 3 bộ **Lime / Ember / Cobalt**, đổi được ngay trên máy và
  lưu vào NVS. Đổi bộ màu thì dựng lại toàn bộ widget.

## 6. Màn hình Wi-Fi + bàn phím

Quét, chọn mạng, nhập mật khẩu (bàn phím LVGL dùng font Montserrat cho ký hiệu),
kết nối, báo lỗi theo mã lý do, lưu vào NVS.

## 7. Thẻ nhớ

- **Chân:** lấy từ bảng phân chân của nhà sản xuất (CS 42, MISO 41, SCK 48,
  MOSI 47), không đoán.
- **Chức năng:** `config.json`, log sự kiện, log cảm biến theo ngày, cập nhật
  firmware từ thẻ, ảnh BMP.
- **Log thừa:** `vfs_api open(): does not exist` in ra mỗi 5 giây. Sửa bằng
  cách gọi `SD.exists()` trước khi mở.

## 8. Wi-Fi: `NO_AP_FOUND` dù laptop bắt được 95%

- **Phát hiện:** `config.h` đúng tên mạng (`RD-SEAI_2.4G`, mạng mở, kênh 1).
  Màn Wi-Fi mới cho thấy board thấy mọi mạng ở mức sóng yếu nhất.
- **Kết luận:** board thu/phát sóng kém (phần cứng/vị trí đặt). Đã có lúc kết
  nối được, RSSI -77.

## 9. Trang Cài đặt cuộn một chút là treo

- **Nguyên nhân:** cứ 5 giây, `updateStorageStatus()` gọi `SD.usedBytes()`.
  Hàm này quét cả bảng FAT (đo được 230–270 ms) và chạy chung vòng lặp với
  LVGL, nên UI khựng định kỳ.
- **Sửa:** chuyển kiến trúc sang FreeRTOS (mục 10). Thống kê thẻ chạy ở task
  `storage`, 10 phút một lần.
- **Trạng thái:** cần người dùng cuộn thử lại.

## 10. Kiến trúc FreeRTOS

- **6 task:** `ui`, `app`, `net`, `devices`, `storage`, `console`, giao tiếp
  qua queue.
- Model HMI có mutex đệ quy và snapshot; chỉ task `ui` gọi LVGL.
- Callback của HMI chỉ được post vào queue.
- **Watchdog** 10 giây, panic khi quá hạn, cho `ui` và `app`.
- **Chụp màn hình:** task `ui` chỉ lấy snapshot, task gọi tự gửi dữ liệu.
- **Lỗi đi kèm:** ảnh chụp qua serial lại hỏng vì log từ nhiều task chen vào.
  Sửa bằng định dạng dòng `#base64` (mỗi dòng ghi một lần), và tool bỏ qua các
  dòng khác.

## 11. Thiết bị Tasmota

- Người dùng cung cấp 2 thiết bị: Sonoff M5-3C `172.28.182.3`, TX Ultimate 4C
  `172.28.182.4`.
- **Kiểm tra từ máy tính (chỉ đọc):**
  - `.4` trả về `POWER1..4` OFF, `POWER5` ON, Tasmota 15.5.0.2;
  - `.3` không phản hồi.
- Thêm task `devices`, điều khiển qua HTTP API. Trang Điều khiển cuộn được,
  chia nhóm theo thiết bị, có trạng thái online/offline; slider độ sáng chuyển
  sang Cài đặt.
- **Đã kiểm chứng:** Sonoff hiện "Ngoại tuyến". Điều khiển TX Ultimate từ panel
  **chưa thử** được vì board không giữ được Wi-Fi.
- Không tự bật/tắt thiết bị thật nào trong quá trình phát triển.

## 12. Wi-Fi: `AUTH_EXPIRE` liên tục

- **Triệu chứng:** sau vài lần boot, `Reason: 2 - AUTH_EXPIRE` lặp mỗi giây
  trong hơn 3 phút.
- **Nguyên nhân:**
  - auto-reconnect của Arduino thử lại mỗi ~1 giây; với sóng yếu, router không
    nghe thấy hoặc chặn tạm MAC của board;
  - thông báo cũ còn hiểu nhầm `AUTH_EXPIRE` là "Sai mật khẩu".
- **Sửa:**
  - tắt auto-reconnect, tự thử lại với giãn cách 5 → 60 giây;
  - tắt modem sleep, đặt công suất phát 19,5 dBm;
  - `AUTH_EXPIRE`/`ASSOC_EXPIRE`/`BEACON_TIMEOUT` báo "Sóng yếu, router không phản hồi".
- **Kết quả:** hết `AUTH_EXPIRE` dồn dập. Ở vị trí mới, board vẫn `NO_AP_FOUND`
  (giới hạn phần cứng).

## 13. Cổng COM đổi

Board chuyển từ COM4 sang **COM18** sau khi cắm cổng USB khác. Đã xác nhận
đúng board bằng MAC `98:3d:ae:e1:8b:bc`.

## 14. Vuốt không cuộn được trang Cài đặt / Điều khiển

- **Triệu chứng:** người dùng báo "scroll bị crash". Hỏi lại thì là "**chưa bao
  giờ cuộn được**".
- **Kiểm tra:**
  - cuộn bằng API (`lv_obj_scroll_by`) 48 lần không crash, nên trang có cuộn
    được, chỉ là thao tác vuốt không tới được trang;
  - đọc mã LVGL 9.2: `lv_obj_hit_test()` trả về `false` cho mọi đối tượng
    **không có cờ `CLICKABLE`**.
- **Nguyên nhân:**
  - `theme::box()` và `label()` xoá cờ `CLICKABLE`, nên trang, thẻ, tiêu đề
    nhóm và nhãn đều "vô hình" với cảm ứng;
  - vuốt bắt đầu ở đó thì LVGL lùi lên tới màn hình gốc, mà màn hình gốc không
    cuộn được, nên không có gì xảy ra;
  - trang chỉ cuộn được nếu ngón tay bắt đầu đúng trên một hàng bấm được.
- **Sửa:** `makeScrollPage()` và khung danh sách Wi-Fi bật lại cờ `CLICKABLE`.
  Hàng bấm được vẫn nhận lượt chạm trước, vì LVGL tìm từ phần tử con sâu nhất.
- **Kiểm chứng:** thêm lệnh `drag` (cảm ứng giả lập trong `hmi_port.cpp`).
  Vuốt từ vùng không bấm được: trang Cài đặt cuộn tới cuối; trang Điều khiển
  cuộn tới nhóm TX Ultimate 4C (ảnh chụp trước/sau).
- **Thay đổi đi kèm (đã có sẵn trong working tree):**
  - label không còn tự cuộn được;
  - chỉ trang đang hiển thị mới được cập nhật;
  - thông tin hệ thống cập nhật mỗi 5 giây;
  - dùng file `partitions_16MB.csv` riêng (bố cục giống `default_16MB.csv`).

## 15. Nhận diện Trường Đại học Bách Khoa

- Logo BK có viền trắng (đúng quy định cho nền tối) xuất hiện ở màn khởi động,
  header và thẻ Giới thiệu; thêm bộ màu "Bách Khoa" (`#1488DB`).
- Thêm tool `tools/png_to_lvgl.py` (PNG → ảnh LVGL RGB565A8, có tuỳ chọn viền).
- **Lỗi biên dịch:** `LV_IMAGE_DECLARE` của LVGL 9 không tự thêm `;`, khác với
  `LV_FONT_DECLARE`.
- Board đổi lại từ COM18 về **COM4** khi cắm lại vào cổng USB cũ.

## 16. Trang chi tiết trong Cài đặt

- Các hàng Thẻ nhớ, config.json, Mạng, MQTT, Bộ nhớ trống, Thời gian chạy và
  thẻ Giới thiệu chạm vào được; mỗi hàng mở một trang chi tiết (`P_DETAIL`,
  `applyDetail()`).
- `SystemInfo`/`StorageInfo` mở rộng: BSSID, kênh, gateway, subnet, DNS, MAC,
  trạng thái MQTT, heap/PSRAM/flash, lý do reset, chip, ESP-IDF, loại thẻ, tốc
  độ SPI, tóm tắt `config.json`.
- **Kiểm chứng:** chạm thật (lệnh `drag` với y0 = y1) vào cả 7 hàng, chụp màn
  hình từng trang chi tiết.
- **Lưu ý khi test:** lệnh `scroll` (`lv_obj_scroll_by`) không chặn ở cuối
  trang, có thể đẩy trang quá xa. Muốn cuộn như người dùng thì dùng `drag`.

## 17. Chữ hiện thành ô vuông

- **Triệu chứng:** một số chữ trong các trang chi tiết hiện thành ô vuông.
- **Tìm ra:** tool mới `tools/check_font_coverage.py` quét mọi chuỗi trong code
  và đối chiếu với glyph trong font đã biên dịch. Hai ký tự thiếu:
  - `→` trong "5 → 60 giây" (trang Mạng, MQTT);
  - `×` trong "480×480" (trang Giới thiệu).
- **Nguyên nhân sâu hơn:** **Be Vietnam Pro không có mũi tên**. Khi thêm
  `←`/`→` vào dải font, `lv_font_conv` báo lỗi và dừng giữa chừng: font không
  được sinh lại, nhưng tool kiểm tra bản đầu vẫn báo OK vì chỉ so với danh
  sách dải ký tự.
- **Sửa:**
  - thêm `× ± ≤ ≥` (font có) vào font;
  - đổi `→` thành `–` trong chuỗi;
  - tool kiểm tra giờ đọc glyph thật từ các file `.c` đã sinh.
- **Phụ:** `tools/screenshot.py` tự thử lại khi khung ảnh thiếu điểm.
- **Sự cố khi test:** vài cú chạm giả lập bị lệch toạ độ (do quán tính khi
  cuộn), làm bật/tắt nhầm công tắc "Tự giảm sáng". Công tắc đã trở về trạng
  thái **bật** như ban đầu. Từ đây phải chụp màn hình trước khi chạm.

## 18. Bàn phím nhập mật khẩu

- **Lỗi vị trí:** `lv_keyboard_create()` tự canh *đáy giữa*, nên
  `set_pos(0, 188)` thành dịch thêm 188 px xuống dưới đáy, mất 3 hàng phím.
  Sửa bằng `lv_obj_align(TOP_LEFT, 0, 188)`.
- **Viết lại bố cục:** 5 hàng, có hàng số, font Be Vietnam Pro (dùng Montserrat
  làm font dự phòng cho ký hiệu), màu phím rõ hơn, phím ✓ theo màu chủ đạo, popover.
- **Lỗi biên dịch:** `lv_buttonmatrix_ctrl_t` là enum, C++ cần ép kiểu
  (`KB_CTRL()`).
- **Chữ lệch xuống khoảng 2 px** trong phím và trong ô nhập (do khoảng trống
  dành cho dấu của font). Đã bù và đo lại trên ảnh chụp: lệch 0–0,5 px.
- **Ô nhập trống sau khi gõ:** vùng nội dung (52 − 2 viền − 28 lề = 22 px)
  nhỏ hơn chiều cao dòng (24 px), nên ô một dòng tự cuộn và đẩy chữ ra ngoài.
  Sửa: lề 11/15 cho vùng nội dung đúng 24 px, chỉ cho cuộn ngang.
- **Khi test:** lệnh `kbd`; log tạm in chỉ số phím, ký tự và chế độ (đã gỡ). Có
  2 lần chữ gõ ra lạ, nhiều khả năng do người dùng chạm cùng lúc với cú chạm
  giả lập.

## 19. Trang Thẻ nhớ tự nhảy lên đầu; trình duyệt tệp

- **Nhảy lên đầu:** `applyDetail()` xoá rồi dựng lại danh sách mỗi lần dữ liệu
  đổi (5 giây một lần), nên vị trí cuộn về 0. Danh sách Wi-Fi bị y hệt.
  - **Sửa:** lưu `scroll_y`, dựng lại, `lv_obj_update_layout()`, rồi
    `lv_obj_scroll_to_y()`.
  - **Kiểm chứng:** ảnh chụp trước và sau một lần làm mới giống hệt.
- **Trình duyệt tệp:** trang "Tệp trên thẻ" và trang xem tệp (xem trước nội
  dung, xoá chạm 2 lần), xử lý trong task `storage`.
  - **Kiểm chứng:** `/` → `logs` → `events.log` → cuộn nội dung, trong một phiên
    ghi log liên tục, không có chạm thật nào xen vào.
- **Tối ưu:** task `ui` chỉ chép model sang snapshot khi có thay đổi (model giờ
  chứa danh sách tệp và nội dung xem trước, khoảng 3 KB).
- RAM tĩnh 76,7 KB (+11 KB).

## 20. Giới thiệu lên đầu Cài đặt; trang Bộ nhớ trực tiếp

- Thẻ Giới thiệu (logo BK) chuyển lên đầu trang Cài đặt.
- Trang Bộ nhớ stream 4 lần/giây, có biểu đồ 15 giây (bật lại `LV_USE_CHART`);
  sửa tại chỗ, không dựng lại trang (xem 04-giao-dien-hmi.md).
- Thêm lệnh serial `detail <n>` để test mà không lẫn với thao tác chạm của
  người dùng trên máy.
- **Phát hiện:** heap nội đang dùng **88%**; thấp nhất còn **33 KB**, khối liên
  tục lớn nhất 25 KB (trước khi thêm trình duyệt tệp là 52–62 KB). Các khoản
  chiếm nhiều:
  - 2 buffer vẽ LVGL 480×48 (2 × 45 KB);
  - RAM tĩnh 76,7 KB (model + snapshot chứa danh sách tệp, nội dung xem trước);
  - stack của 6 task (38 KB) và Wi-Fi.

  Có thể lấy lại bằng cách giảm buffer vẽ xuống 480×32, hoặc chuyển model,
  snapshot và bộ đệm tệp sang PSRAM. **Chưa làm.**

## 21. Trang Bộ nhớ kiểu "instrument"

- **Bỏ:** thấp nhất từ lúc bật, khối liên tục lớn nhất, "Dùng cho".
- **Dựng lại theo hướng data-first:**
  - số % lớn đọc trước;
  - biểu đồ có trục giá trị (nhãn đặt đúng 5 vạch lưới `div_line_count(5)`),
    mốc thời gian, chấm điểm mới nhất (`lv_chart_get_point_pos_by_id(…, cnt-1)`);
  - không dùng thẻ nền, chỉ đường kẻ mảnh giữa các khối;
  - Flash dùng thanh chia đoạn.
- **Cảm hứng:** prompt "Premium ESP32 Data Visualization UI" do người dùng gửi.
  Bộ 10 component và 5 màn hình trong prompt **chưa làm**.
- **RAM nội lên 91%** (296/324 KB) sau khi thêm biểu đồ, chỉ còn khoảng 28 KB
  trống. Nên giảm buffer vẽ hoặc chuyển dữ liệu UI sang PSRAM (mục 20).

---

## 22. Gộp RAM và PSRAM vào một biểu đồ

- Hai biểu đồ riêng được thay bằng `memPanel()`: hai chỉ số đặt cạnh nhau ở trên
  (`memMetric()`), bên dưới là **một `lv_chart` có hai series** (accent và accent2)
  dùng chung trục 0–100 % và mốc thời gian, mỗi series có chấm điểm mới nhất riêng.
- Tiêu đề màu của mỗi chỉ số đóng vai trò chú thích, không cần legend riêng.
- Giảm được một chart (tiết kiệm một ít RAM nội). Ảnh chụp lúc kiểm tra: RAM 95%,
  PSRAM 6%, nên vẫn cần làm việc giảm RAM ở mục 21.

---

## 23. Giải phóng RAM nội: 95% xuống 49%

- **Nguyên nhân:** LVGL dùng `malloc` của C. Với cấu hình Arduino
  (`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL = 4096`), mọi khối dưới 4 KB, tức gần
  như toàn bộ widget và style, đều nằm ở SRAM nội.
- **Cách làm:**
  - Thêm bộ cấp phát LVGL riêng `src/hmi/lv_mem_psram.c` (PSRAM trước, SRAM nội
    dự phòng) và đặt `LV_STDLIB_CUSTOM` trong `lv_conf.h`.
  - Giảm buffer vẽ từ 480×48 xuống 480×32 (tiết kiệm 2 × 15 KB).
  - Model và `s_snap` được cấp phát ở PSRAM qua `newModel()` (tiết kiệm 11 KB).
  - Buffer 4 KB dùng khi cập nhật firmware từ thẻ nhớ chuyển sang PSRAM.
- **Kết quả đo** (lệnh `tasks`, trang Bộ nhớ): RAM nội trống **171 KB**
  (thấp nhất 167 KB), so với khoảng 16 KB trước đó; PSRAM còn trống 7,5 MB.
  Giao diện và thao tác cuộn vẫn bình thường.

---

## 24. Header/footer theo trang, đo tốc độ vẽ, build nhanh hơn

- **Header chỉ còn ở Tổng quan.** Điều khiển và Cài đặt dùng thêm 52 px phía
  trên. Các trang con của Cài đặt ẩn luôn thanh điều hướng và dùng gần trọn
  màn hình (y 12..468). Đồng hồ ẩn khi chưa có giờ.
- **Độ sáng không đổi khi kéo thanh trượt:** `setup()` gọi `pinMode(38, OUTPUT)`
  và `digitalWrite(HIGH)` sau `display.begin()`. Hai lệnh này tách GPIO38 khỏi
  kênh LEDC mà `Light_PWM` đã gắn, nên đèn nền luôn ở 100%. Đã bỏ, thay bằng
  `display.setBrightness(255)`. Mức 10% trước đây gần như tắt màn hình, nên giờ
  thanh trượt 10..100% được ánh xạ sang duty `HMI_BACKLIGHT_MIN_DUTY` (40)..255.
- **Lề và thanh tiêu đề:** mọi lề nhìn thấy đều 16 px (đo trên ảnh chụp).
  Thanh tiêu đề trang con cao 64 px, mũi tên và nút làm mới cách mép 24 px,
  tiêu đề căn giữa. Trang Tệp không còn dòng "/" hay "/logs" lặp lại tên thư
  mục.
- **Trang con dùng một lề 24 px cho mọi thứ** (mũi tên, tiêu đề, nội dung,
  mép dưới), đo trên ảnh chụp: thanh tiêu đề x 24..456, nội dung x 24..455.
- **Căn giữa thanh tiêu đề trang con:** tiêu đề nằm thấp hơn mũi tên quay lại
  khoảng 2,5 px (đo trên ảnh chụp: tâm chữ hoa ở y 38,5, mũi tên ở y 36) vì font
  chừa chỗ cho dấu phía trên. Đã dời lên bằng `kBarTitleY = 11` (khối 2 dòng của
  trang Tệp dời theo); sau khi sửa, tâm chữ ở y 36,5.
- **Lệnh console `bench [psram|hybrid]`:** dựng lại toàn bộ UI rồi đo 20 lần
  vẽ lại toàn màn hình (tính cả flush). Kết quả:

  | Bộ cấp phát LVGL | Vẽ lại (Cài đặt / Tổng quan) | RAM nội trống |
  |---|---|---|
  | psram (mặc định) | khoảng 100–105 ms / 62 ms | 167 KB |
  | hybrid (khối ≤ 1 KB ở RAM nội, giữ 96 KB dự trữ) | như trên | 96 KB |

  Để widget ở PSRAM **không làm chậm** việc vẽ, nên mặc định là `LVMEM_PSRAM`.
  Toàn bộ widget LVGL chỉ chiếm khoảng 48 KB.
- **Build:** `lib_ldf_mode` đổi từ `chain+` sang `chain`. Một lần build sửa
  `hmi_ui.cpp` + `hmi_theme.h` giảm từ khoảng 10 phút xuống 3 phút 38 giây,
  sửa riêng `hmi_ui.cpp` còn khoảng 2 phút. Thủ phạm là scons tiền xử lý
  mọi `#if` của LVGL ở mỗi lần build (tiến trình Python tốn khoảng 480 giây
  CPU). Windows Defender quét từng tiến trình gcc; thêm loại trừ (cần quyền
  admin) cho `%USERPROFILE%\.platformio` và thư mục dự án để nhanh hơn nữa.

---

## 25. Cuộn mượt hơn, chuyển trang không còn "vẽ dần"

Đo bằng `bench` (vẽ lại toàn màn hình, tính cả flush, đơn vị ms):

| Bước | Cài đặt | Tổng quan | Bộ nhớ |
|---|---|---|---|
| Ban đầu: vẽ / flush / tổng | 57 / 42 / 100 | 62 / 42 / 104 | 11 / 41 / 53 |
| Flush đảo byte rồi memcpy; 1 buffer 480×64 | 44 / 24 / 68 | 45 / 24 / 69 | 7 / 24 / 31 |
| `-O2` | 39 / 25 / 64 | 39 / 25 / 64 | 7 / 25 / 31 |
| 2 luồng vẽ (2 nhân) | **30 / 25 / 56** | **30 / 25 / 55** | **6 / 25 / 31** |

- **Flush chậm** là do framebuffer lưu RGB565 đảo byte: `pushImage` dạng
  `rgb565_t` khiến LovyanGFX đổi từng điểm ảnh. Sau khi sửa, flush ~25 ms là
  giới hạn băng thông PSRAM (DMA màn hình đọc liên tục ~28 MB/s). Đã thử ghi
  thẳng vào framebuffer kèm đảo byte theo từ 32 bit: kết quả y hệt, nên giữ
  cách dùng LovyanGFX.
- **Bỏ fade khi chuyển trang** (xem 4.1).
- **Stack luồng vẽ:** `StackType_t` của ESP-IDF dài 1 byte, nên
  `LV_DRAW_THREAD_STACK_SIZE` là số byte thật. Lần đầu đặt 32 KB làm RAM nội
  tụt từ 168 xuống 103 KB; đặt 8 KB thì còn 151 KB.
- **Tổng kết:** cuộn trang Cài đặt nhanh gần gấp đôi (khoảng 10 lên 18 khung/giây
  khi vẽ lại toàn màn hình). Màn hình vẫn có thể bị xé hình (tearing) vì chỉ có
  một framebuffer; IDF 4.4 không hỗ trợ hai framebuffer cho panel RGB.

---

## 26. Tận dụng PSRAM, flash và thẻ nhớ

Trước đó PSRAM còn trống 7,4 MB và phân vùng flash 3,4 MB bỏ không. Đã thêm:

- **`src/datalog.cpp` (PSRAM, ~263 KB):**
  - lịch sử cảm biến 24 giờ / 10 giây và 7 ngày / 1 phút;
  - 400 sự kiện gần nhất.
- **Trang mới:** "Lịch sử cảm biến" (biểu đồ 1 giờ / 24 giờ / 7 ngày) và
  "Nhật ký sự kiện", trong mục Cài đặt → DỮ LIỆU. Chạm vào đồng hồ hoặc thẻ
  chỉ số ở Tổng quan cũng mở trang lịch sử, và nút ← quay về Tổng quan.
- **LittleFS** trên phân vùng `spiffs`: bản sao `config.json` dùng khi không
  có thẻ, lịch sử mỗi giờ.
- **Thẻ nhớ:**
  - lịch sử lưu 10 phút/lần;
  - tự xoá CSV cảm biến cũ hơn `logDays`;
  - sao lưu firmware đang chạy trước khi cập nhật (`sd restore`).
- **Trình xem tệp đọc tới 64 KB** (trước 1,5 KB).
- **Sửa:** stack task `storage` từ 6 lên 8 KB. Lúc trước chỉ còn trống 792 B;
  giờ còn 4,5 KB.
- **Đo được:** ghi lịch sử ra thẻ 209 ms, ra flash 4,5 giây (khối 4 KB, nghỉ
  20 ms). RAM nội trống 148 KB, PSRAM trống 7,3 MB.
- **Còn lại:**
  - Khi mới có vài phút dữ liệu, khung 24 giờ / 7 ngày chỉ có một điểm ở mép
    phải và gần như không thấy.
  - Log báo lỗi "does not exist" khi tệp chưa có lần đầu (vô hại).

---

## 27. Rác hình khi mở bảng nhập mật khẩu Wi-Fi

- **Triệu chứng:** chạm một mạng trong danh sách Wi-Fi thì màn hình hiện mảng
  màu xanh/tím và bàn phím lặp lại, khoảng 200 ms rồi mới đúng.
  `lv_snapshot` (lệnh `shot`) không thấy lỗi vì LVGL vẽ lại riêng ra bộ nhớ.
- **Công cụ:** lệnh `shot fb [ms]` chép framebuffer thật sau `ms` mili giây.
  `tools/screenshot.py` nhận thêm chuỗi lệnh, ví dụ `"kbd;shot fb 100"`.
- **Loại trừ:**
  - Chỉ một luồng vẽ vẫn lỗi y hệt, nên không phải do vẽ hai luồng.
  - Đảo byte ngược lại sau flush thì hết sai màu nhưng vẫn còn bóng lặp mỗi
    64 px (bằng chiều cao bộ đệm vẽ).
- **Nguyên nhân:** `lv_obj_fade_in()` của bảng nền đặc toàn màn hình. LVGL coi
  bảng là che kín nên không vẽ phần bên dưới, mà trộn bảng bán trong suốt lên
  nội dung cũ trong bộ đệm. Flush đảo byte tại chỗ làm phần rác đó sai màu thêm.
- **Sửa:**
  - Bỏ fade ở bảng mật khẩu và ở màn hình khởi động (`lv_obj_delete_delayed`).
  - Kiểm tra trên framebuffer thật: sạch ngay từ 30 ms.
  - Thanh tiêu đề của bảng đổi sang kiểu chung: mũi tên cách mép 24 px, tiêu
    đề và tên mạng căn giữa.

---

## 28. Gom trang Cài đặt

- **Từ 15 hàng / 6 mục xuống 8 hàng / 3 mục.** Ngoài trang Cài đặt chỉ còn
  phần hay dùng: Wi-Fi, Mạng & MQTT, 3 hàng màn hình, Thẻ nhớ, Lịch sử cảm
  biến, Hệ thống.
- **Gộp vào trang con:**
  - Mạng + MQTT thành một trang.
  - Bộ nhớ + Thời gian chạy + lối vào Nhật ký sự kiện thành trang "Hệ thống".
  - Tệp trên thẻ, config.json và Cập nhật firmware vào trang Thẻ nhớ.
- **Điều hướng lồng nhau:** `ui.detailParent`, `openSubDetail()`. Nút ← quay về
  trang cha; trình duyệt tệp ở thư mục gốc thì quay về trang Thẻ nhớ.
- **Lỗi tránh được:** `ui.sdFw` được đặt về `nullptr` trước khi
  `applyDetail()` xoá danh sách, vì `applyStorage()` chạy cả khi trang khác
  đang mở.
- **Đổi màu chủ đạo không còn nhảy về đầu trang:** `buildAll()` dựng lại mọi
  widget theo bảng màu mới. Giờ nó lưu vị trí cuộn của trang đang mở, rồi sau
  khi dựng xong gọi `lv_obj_update_layout()` + `lv_obj_scroll_to_y()`. Đã thử
  trên board: chạm "Màu chủ đạo" khi đang cuộn giữa trang, hàng vẫn ở y 228.

---

## 29. Xem CSV dạng bảng, xem trước ảnh

- **CSV:**
  - `lv_table` (bật `LV_USE_TABLE`), dòng tiêu đề tô qua `LV_EVENT_DRAW_TASK_ADDED`.
  - 200 dòng mới nhất, độ rộng cột ước theo ký tự dài nhất (9 px/ký tự + 28 px).
- **Ảnh JPG/PNG/BMP:**
  - Đọc kích thước từ header tệp, giải mã bằng LovyanGFX vào `LGFX_Sprite` trong
    PSRAM, đảo byte về RGB565 thường, chép sang bộ đệm ảnh của model.
  - UI dùng `lv_image_dsc_t` trỏ vào bộ đệm đó; gọi `lv_image_cache_drop()` vì
    địa chỉ không đổi giữa các ảnh.
- **Lỗi phát hiện khi làm:** `kTextCap = 64 * 1024` khai báo kiểu `uint16_t`
  nên thành 0. Trình xem văn bản 64 KB ở mục 26 thực ra chưa từng chạy (đọc
  0 byte, mọi tệp bị coi là "phần cuối"). Đã sửa sang `uint32_t`; mở thử
  `events.log` và CSV đều đúng.
- **Khác:**
  - Mở tệp mới thì cuộn về đầu trang.
  - Stack task `storage` tăng lên 12 KB cho bộ giải mã ảnh.
- **Chưa thử:** JPG và PNG thật, vì thẻ hiện chỉ có ảnh BMP do panel tự chụp.

---

## 30. Độ sáng tối thiểu 50%

- Thanh trượt hiện 0–100%, nhưng ánh xạ sang duty LEDC
  `HMI_BACKLIGHT_MIN_DUTY` (128, tức 50%) .. 255. Kéo hết xuống vẫn nhìn rõ
  màn hình (trước đó 10% gần như tắt hẳn).
- Tự giảm sáng (`HMI_DIM_BRIGHTNESS_PCT` = 0) hạ về đúng mức sàn đó.
- Model nhận 0..100 (trước kẹp 10..100). Mức đã lưu trong NVS vẫn dùng được.

---

## 31. Màn hình chờ logo Bách Khoa

- Khi tự giảm sáng thì hiện `ui.saver`: nền đen kèm `brandLockup()`, cụm dùng
  chung với màn hình khởi động. Logo dời vị trí mỗi phút để chống lưu ảnh.
- Thoát: wake guard của port nuốt lần chạm đầu rồi gọi `onWake()`, hàm này ẩn
  màn hình chờ.
- Công tắc trong Cài đặt đổi tên thành "Màn hình chờ sau 30 giây".
- Không che tiến trình cập nhật firmware.

---

## 32. Bảng kết nối cho mạng mở (không mật khẩu)

- **Lỗi cũ:** ô mật khẩu và bàn phím bị ẩn nhưng nút và dòng trạng thái vẫn ở
  chỗ cũ, nên phía trên trống một khoảng, nút "Kết nối" lơ lửng.
- **Sửa:** `sheetLayout(secure)`:
  - mạng mở: thẻ "Mạng mở · không cần mật khẩu · dữ liệu không được mã hoá"
    thế chỗ ô mật khẩu, nút "Kết nối" rộng hết cột, dòng trạng thái ngay dưới;
  - mạng có mật khẩu: giữ bố cục cũ.
- Lệnh serial `kbd open` mở bảng cho một mạng mở giả, để kiểm tra.

---

## 33. Điều khiển Tasmota: panel vào nhầm mạng, không vào được RD-SEAI

- **Triệu chứng:** Sonoff M5-3C (`172.28.182.3`) và TX Ultimate (`.4`) luôn
  "Ngoại tuyến" trên panel.
- **Nguyên nhân 1, mạng:** panel ở **RFThings VietNam** (`192.168.130.171`),
  còn thiết bị ở **RD-SEAI_2.4G** (`172.28.182.x`). RFThings là mạng được chọn
  trên panel (lưu NVS) nên luôn được thử trước.
- **Cập nhật (đo lại):** RD-SEAI_2.4G hiện là **mạng mở**, nhưng `config.h`
  vẫn lưu mật khẩu cũ, và mọi lần thử trước đều gửi kèm mật khẩu. Đã sửa:
  quét đồng bộ lúc khởi động, nhớ SSID nào đang mở (`s_open`) và kết nối mạng
  mở thì không gửi mật khẩu; nút "Kết nối" của mạng đã lưu gửi `kUseSaved`
  thay vì chuỗi rỗng. Thử lại đúng kiểu mạng mở, rồi thêm đổi MAC và chỉ dùng
  802.11b: vẫn `AUTH_EXPIRE`. Danh sách quét lúc khởi động cho thấy nguyên nhân:
  RD-SEAI ở **-77 dBm** tại panel (laptop 91%, khoảng -45 dBm), cùng kênh 1
  với RFThings -57 dBm. Router không nghe được panel; cần đặt panel gần router
  hơn (hoặc kiểm tra lọc MAC).
- **Nguyên nhân 2 (giả thuyết ban đầu), router:** vào RD-SEAI luôn nhận `AUTH_EXPIRE` (reason 2)
  sau khoảng 1 giây, kể cả khi chỉ định đúng AP (`wifi bssid
  14:1f:ba:74:d6:e8 1 RD-SEAI_2.4G`). Máy tính cùng chỗ vào AP đó với sóng
  89%, panel vào RFThings ở -62 dBm. Kết luận: router không trả lời xác thực
  của panel (MAC panel `98:3D:AE:E1:8B:BC`); cần kiểm tra lọc MAC, danh sách
  chặn, giới hạn số máy, PMF/WPA3 trên router.
- **Đã cải thiện phía panel:**
  - Tasmota: timeout 3 giây, thử lại 1 lần, chỉ báo OFFLINE sau 3 lần hỏi
    lỗi liên tiếp, đang OFFLINE thì 5 giây hỏi lại. Lệnh `devices` in Wi-Fi,
    IP, RSSI và số lần thành công / lỗi của từng thiết bị.
  - Mạng đã có mật khẩu (NVS / `config.json` / `config.h`) ghi "Đã lưu" trong
    danh sách; chạm vào thì kết nối luôn, có nút "Nhập mật khẩu khác". Lệnh
    serial `wifi join <ssid>`.
  - `WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN)` + `setSortMethod` làm việc
    quét bất đồng bộ không bao giờ xong trên Arduino-ESP32 2.0.17, nên đã bỏ.

---

## 34. Điều khiển Tasmota chỉ qua web, màn hình chờ sau 2 phút

- Panel không vào được RD-SEAI (mục 33), nên chuyển sang API của web
  `https://smarthome.blong.id.vn/api/tasmota`. Máy chủ web ở cùng mạng với
  thiết bị, panel chỉ cần Internet.
- **Bỏ hẳn cách gọi thẳng IP** (`/cm?cmnd=…`): `DeviceInfo.host` đổi thành
  `key` (`primary` / `secondary`); `config.json` dùng `"key"` và `"tasmotaApi"`.
- **Kết nối:** HTTPS giữ kết nối để dùng lại, khoảng 0,4 s/request (bắt tay
  lại khoảng 3 s). Máy chủ Node đóng kết nối nhàn rỗi sau khoảng 5 s, nên
  request trên kết nối cũ chỉ chờ 2 s rồi gửi lại trên kết nối mới.
- **Bật/tắt lạc quan:** công tắc đổi ngay, đúng theo trạng thái web trả về.
- **Stack và RAM:** task `devices` 10 KB stack (bắt tay TLS); RAM nội trống
  khoảng 80–90 KB vì bộ đệm TLS.
- **Đo:** cả hai thiết bị online qua mạng RFThings, `ok 5 fail 0`, 391 ms.
- **Màn hình chờ:** sau 2 phút thay vì 30 giây (`HMI_DIM_AFTER_MS`); nhãn
  Cài đặt tự hiện "sau 2 phút".

---

## 35. Đồng bộ Tasmota nhanh: client HTTP riêng

- **Triệu chứng:** đồng bộ chậm. Panel thường mất 0,4 s nhưng có lúc 6,5–9,5 s
  và "read Timeout"; hỏi 1 s/lần cũng không đỡ.
- **Đo:** từ máy tính web trả lời 0,4–0,7 s; curl dùng 1 kết nối cho 5 request.
  Web chạy sau Cloudflare, body trả về dạng `Transfer-Encoding: chunked`.
- **Nguyên nhân:** `HTTPClient` (Arduino-ESP32 2.0.17) dùng lại kết nối nhưng
  không đọc phần kết thúc của body chunked. Request sau đọc phải phần thừa đó
  rồi chờ tới timeout, sau đó phải bắt tay TLS lại (khoảng 3 s).
- **Sửa:** client HTTP/1.1 nhỏ tự viết trên `WiFiClientSecure` (đọc header,
  chunked, `Content-Length`, `Connection: close`), hỏi 1 s/lần.
- **Đo lại 40 s:** 22 lần hỏi, 0 lỗi, 0,41–0,51 s mỗi lần.

---

## 36. Ảnh chụp toàn bộ màn hình, sửa dựng lại trang chi tiết

- Chụp 24 màn hình qua COM4 vào `docs/images/hmi-*.png` (bảng trong
  `docs/README.md`), thay các ảnh cũ chụp từ giao diện trước.
- **Sửa:** đổi bảng màu khi đang ở trang chi tiết thì `buildAll()` dựng lại
  khung nhưng tiêu đề và nội dung trống (chúng do `showTopic()` tạo khi mở
  trang). Giờ `buildAll()` giữ chủ đề, trang cha, khung lịch sử và mở lại trang.
- **Màn hình chờ** đã kiểm chứng trên board: bật đúng ở giây thứ 120 không chạm
  (log `inactive … dimmed 1`).
- **PlatformIO trong `~/.platformio/penv` bị gỡ** (VS Code đang cài lại):
  build bằng `python -m platformio` của Python 3.12 hệ thống (Core 6.2.0).

---

## Việc còn lại

| Việc | Cần gì |
|---|---|
| Wi-Fi tại chỗ đặt panel | đặt panel nơi có mạng mạnh (RSSI ≥ -70 dBm theo danh sách quét lúc khởi động) |
| MQTT | địa chỉ broker thật trong mạng đang dùng |
| Thử cập nhật firmware từ thẻ, `sd restore` | một `firmware.bin` thật trên thẻ |
| Xem ảnh JPG/PNG | ảnh thật trên thẻ |
| Cảm biến thật | nối cảm biến hoặc gửi số liệu qua MQTT `.../sensor` |
| Xử lý `MQTT_TOPIC_CONFIG`, MQTT OTA | chưa cài đặt |
| Chụp lại ảnh Điều khiển / Wi-Fi khi panel có mạng | ảnh hiện tại chụp lúc không có Wi-Fi |
| Cập nhật canvas thiết kế theo firmware hiện tại | tuỳ chọn |
