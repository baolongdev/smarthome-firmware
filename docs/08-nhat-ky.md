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

---

## Việc còn lại

| Việc | Cần gì |
|---|---|
| Ổn định Wi-Fi | đặt board gần router / không che anten; thử lại |
| Thử điều khiển TX Ultimate từ panel | Wi-Fi ổn định |
| Sonoff M5-3C | xác nhận thiết bị đang bật và IP hiện tại |
| MQTT | địa chỉ broker thật trong mạng `172.28.182.x` |
| Xác nhận cuộn bằng tay trên máy thật | đã kiểm chứng bằng `drag` giả lập, cần người dùng vuốt thử |
| Thử cập nhật firmware từ thẻ, nhập mật khẩu Wi-Fi | thao tác chạm thật |
| Cảm biến thật | nối cảm biến hoặc gửi số liệu qua MQTT `.../sensor` |
| Xử lý `MQTT_TOPIC_CONFIG`, MQTT OTA | chưa cài đặt |
| Cập nhật canvas thiết kế theo firmware hiện tại | tuỳ chọn |
| Commit | chờ người dùng đồng ý |
