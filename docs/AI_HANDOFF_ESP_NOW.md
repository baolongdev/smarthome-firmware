# Handoff cho AI: kiểm tra ESP-NOW Nova Master ↔ Guition Slave

## Mục tiêu

Cho Master Nova tự phát beacon khám phá có xác thực, nhận phản hồi từ panel Guition, thêm panel vào `espnow.devices`, rồi dùng ESP-NOW unicast mã hóa cho MCP. Không yêu cầu người dùng nhập MAC thủ công.

## Hai project và thiết bị

- **Slave Guition:** `/data/apps/aclab/smarthome/firmware`, PlatformIO env `esp32-4848s040`, ESP32-S3 480×480, cổng USB-UART CH340 `/dev/ttyUSB0`, MAC đã đọc được `cc:68:c7:97:3c:d4`.
- **Master Nova:** `/data/apps/aclab/nova/firmware`, profile `waveshare/esp32-s3-touch-lcd-1.85c`, cổng USB JTAG `/dev/ttyACM0`, MAC `84:c7:bb:60:24:60`.

## Cấu hình đã đối chiếu (2026-11-14)

- Guition có khóa mặc định thử nghiệm trong `include/config.h` (`ESPNOW_PMK`, `ESPNOW_LMK`). Nova `sdkconfig` và `build/config/sdkconfig.h` hiện đã được đặt cùng giá trị với mặc định đó. Image `build/xiaozhi.bin` build lúc 10:41 UTC và nạp vào `/dev/ttyACM0`; esptool báo hash verified.
- **Chưa xác nhận khóa runtime của Guition.** `src/main.cpp` nạp cấu hình lưu trữ trước, rồi chỉ chép khóa mặc định nếu trường PMK/LMK đang trống. `/config.json` trên LittleFS/SD có thể còn khóa cũ và ghi đè mặc định compile-time. Cần đọc/đặt lại cấu hình lưu trữ trên panel trước khi kết luận hai khóa runtime khớp.
- Cả hai đang cấu hình channel 1. Master đã từng log kết nối Wi-Fi `RD-SEAI_2.4G` ở channel 1. Nếu router đổi kênh, phải cho Slave theo cùng kênh hoặc kiểm tra lại cấu hình radio.
- Log Guition sau lần nạp gần nhất: `ESP-NOW slave ready ch1 peers=0 wifi=0`, `Master MAC auto-discovery`. Dòng này chỉ xác nhận stack khởi tạo, **không xác nhận đã nhận beacon**.
- Sau khi nạp lại Master, log boot xác nhận `Authenticated discovery beacon is being broadcast`; Wi-Fi Master nối `RD-SEAI_2.4G` trên channel 1.
- Mở log Guition trong 9 giây sau đó không thấy `ESP-NOW master auto-discovered ...`. Do đó phát hiện thiết bị **vẫn chưa được xác nhận**; kiểm tra khóa runtime đã lưu trên Guition và nhận callback/channel trước khi báo hoàn tất.

## Luồng mong đợi

1. Nova `EspNowManager::DiscoveryTask` gửi broadcast `NOVA_DISCOVERY_V1` kèm 8 byte HMAC-SHA256, ký bằng PMK, mỗi 3 giây.
2. Guition xác minh beacon trong `src/espnow_master.cpp::handlePacket`, thêm MAC Master làm peer encrypted unicast và broadcast phản hồi `GUITION_SLAVE_V1` có HMAC tương ứng.
3. Nova xác minh phản hồi, log `Discovered authenticated ESP-NOW slave ...`, đưa MAC vào danh sách node khám phá. MCP `espnow.devices` đọc danh sách này.
4. Beacon khám phá là broadcast không mã hóa nhưng có xác thực HMAC; dữ liệu MCP/điều khiển phải đi unicast mã hóa bằng LMK.

## Việc cần kiểm tra tiếp

- Lấy log khởi động sớm của **Master** sau reset, lọc tag `EspNow`: phải có `Authenticated discovery beacon is being broadcast`. Nếu thay vào đó có `Could not create authenticated discovery beacon` hoặc `Could not send authenticated discovery beacon`, xử lý lỗi đó trước.
- Giữ Slave mở serial sau khi Master chạy ít nhất 5 giây; kiểm tra event `ESP-NOW master auto-discovered ...`. Nếu không có event, bổ sung log tạm ở nhánh nhận beacon: channel nhận, độ dài payload, kết quả HMAC (không in khóa).
- Kiểm tra `storage::loadConfig()` và `/config.json`: nó có thể giữ PMK/LMK cũ dù `include/config.h` đã có khóa mặc định mới. Dùng trang cấu hình ESP-NOW trên Guition để lưu cùng PMK/LMK, hoặc xóa riêng các trường khóa đã lưu rồi khởi động lại; không format toàn bộ LittleFS nếu chưa sao lưu dữ liệu.
- Kiểm tra `espnow.devices` trên MCP Master sau khi thấy beacon phản hồi. Giao diện Slave hiển thị `slave ready` không đồng nghĩa Master đã phát hiện.
- Xác nhận radio channel thực tế trên cả hai bo tại cùng thời điểm. Master dùng channel Wi-Fi khi đã kết nối; Slave đang không kết nối Wi-Fi và dùng `ESPNOW_CHANNEL`/channel được lưu.
- Nếu HMAC phía Master không tạo được, kiểm tra `psa_crypto_init`, `psa_import_key` và `psa_mac_compute`; log lỗi cụ thể, tuyệt đối không log giá trị PMK/LMK.
- Nếu cần nạp lại Master, đóng mọi monitor đang giữ `/dev/ttyACM0` trước khi flash. Có thể kiểm tra chủ sở hữu bằng `fuser -v /dev/ttyACM0`. `idf.py monitor`/esptool reset có thể đưa board vào ROM download mode; sau flash xác nhận nó boot app bình thường.

## Giới hạn bảo mật

Khóa trong `include/config.h` chỉ là khóa mặc định để thử nghiệm. Không đưa khóa này hoặc khóa từ `sdkconfig` vào commit/public log. Trước khi triển khai thực tế, thay khóa ở cả hai thiết bị bằng giá trị riêng và giữ kín.
