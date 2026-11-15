# Handoff cho AI: ESP-NOW Waveshare master và CH340 slave

## Vai trò thiết bị

- **Master:** Waveshare ESP32-S3-Touch-LCD-1.85C, firmware ESP-IDF trong `/data/apps/aclab/nova-firmware`.
- **Slave:** ESP32-4848S040 dùng USB-UART CH340, firmware PlatformIO trong repo hiện tại (`/data/apps/aclab/nova/firmware`).
- MAC master đã xác nhận: `84:C7:BB:60:24:60`.
- MAC slave đã xác nhận trước đó: `CC:68:C7:97:3C:D4`.
- Cả hai đang dùng Wi-Fi `RD-SEAI_2.4G`, kênh 1. ESP-NOW dùng chung radio/kênh với Wi-Fi khi đã kết nối.

Không đổi vai trò hai bo mạch: **Waveshare là master, CH340 là slave**. Tên file `src/espnow_master.cpp` là tên module lịch sử; trong firmware CH340, log thực tế xác định nó chạy chế độ slave (`ESP-NOW slave ready`).

## Cách discovery hoạt động

1. Waveshare phát beacon broadcast định kỳ có nhãn `NOVA_DISCOVERY_V1` và 8 byte đầu của HMAC-SHA256 tính từ PMK.
2. CH340 xác thực beacon bằng PMK. Khi hợp lệ, CH340 tự học MAC master, thêm peer, rồi phát phản hồi `GUITION_SLAVE_V1`.
3. Waveshare xác thực phản hồi, thêm slave vào danh sách thiết bị. Gói điều khiển/MCP tiếp theo dùng ESP-NOW unicast mã hóa bằng LMK.
4. Không cần master quét Wi-Fi AP của slave. Discovery chạy trực tiếp qua broadcast ESP-NOW; không cần MCP server hay Internet.

PMK phải giống nhau để xác thực beacon; LMK phải giống nhau để mã hóa unicast. Firmware slave mặc định lấy khóa từ `include/config.h` (`ESPNOW_PMK`, `ESPNOW_LMK`), trừ khi phần cấu hình lưu trữ (`/config.json`/UI) ghi đè. Firmware master đọc khóa từ `CONFIG_ESP_NOW_PMK_HEX` và `CONFIG_ESP_NOW_LMK_HEX` trong `sdkconfig` của repo ESP-IDF. Không ghi giá trị khóa vào tài liệu hoặc log.

## Sự cố đã gặp và trạng thái hiện tại

Trước khi đồng bộ khóa, CH340 nhận gói từ MAC master nhưng log `ESP-NOW ignored 84:C7:BB:60:24:60`. Nguyên nhân là beacon không qua xác thực PMK, nên master không được thêm vào danh sách discovered.

Đã đồng bộ PMK/LMK cấu hình master theo khóa mặc định của firmware CH340, build và nạp lại Waveshare. Sau đó log CH340 xác nhận:

```text
[event] ESP-NOW slave ready ch1 peers=0 wifi=0
[event] ESP-NOW master auto-discovered 84:C7:BB:60:24:60
```

Như vậy scan/discovery master → slave đã hoạt động. Khi kiểm tra lại, ưu tiên xác nhận các log này và trạng thái `espnow.devices` trên Waveshare. Nếu xuất hiện lại `ESP-NOW ignored <MAC>`, kiểm tra PMK của cấu hình lưu trên CH340 có ghi đè giá trị mặc định không; nếu master không thấy slave, kiểm tra log `GUITION_SLAVE_V1`/nhận slave beacon và LMK hai phía.

## Lưu ý khi phát triển

- Giữ discovery broadcast không mã hóa nhưng phải có HMAC; chỉ chấp nhận đúng nhãn và chữ ký.
- Không cho phép broadcast điều khiển relay. Lệnh điều khiển phải là unicast mã hóa.
- Peer đang được pin trong cấu hình có thể ảnh hưởng auto-discovery; kiểm tra `peerCount`/allowlist nếu log báo bỏ qua thiết bị sau discovery.
- Không bật relay GPIO theo phỏng đoán. Các chân relay chưa được xác nhận phần cứng; `relay.set` có thể trả lỗi chưa cấu hình GPIO.
- Log CH340 qua `/dev/serial/by-id/usb-1a86_USB_Serial-if00-port0`, 115200 baud. Log Waveshare qua `/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_84:C7:BB:60:24:60-if00`, 115200 baud.

## MCP để AI đọc trạng thái và điều khiển

Master đã có `espnow.devices`, `espnow.list_tools`, và `espnow.call`. Luồng sử dụng: lấy MAC bằng `espnow.devices`, đọc manifest bằng `espnow.list_tools`, rồi gọi tool slave bằng `espnow.call`.

Slave công bố tool `device` với `op`:

- `info`: tên vị trí, loại bo, vai trò, kênh, Wi-Fi/RSSI, uptime và `sensor_data` cho biết nguồn cảm biến là mô phỏng hay phần cứng.
- `relays`: tên và trạng thái thực tế của relay 1–3.
- `set`: điều khiển relay bằng `channel` (1–3) và `state` (`true` bật, `false` tắt).
- Với `op="set"`, bỏ trường `channel` để đặt cả ba relay trong một lệnh batch.

Manifest đầy đủ: [`ESP_NOW_MCP_MANIFEST.json`](ESP_NOW_MCP_MANIFEST.json). Bản inline do `sendMcpToolsList()` gửi được rút gọn để vừa payload ESP-NOW 250 byte; giữ hai manifest đồng bộ khi đổi tên tool hoặc schema. Lệnh cũ `relay.set` vẫn được nhận để tương thích.

Slave Guition công bố một quạt ở relay 1 và hai đèn ở relay 2–3. Master có `espnow.inspect` để tự lấy inventory (device name, actuator names/states, MCP functions) và `espnow.control` để nhận target tự nhiên, tra tên từ slave rồi điều khiển relay nếu có đúng một mục khớp. Tên không được slave công bố hoặc tên trùng nhau sẽ trả về danh sách/ambiguous error; model không nên đoán channel.
