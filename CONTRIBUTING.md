# Hướng dẫn đóng góp code – Nova Smart Home Panel

Repo: `baolongdev/smarthome-firmware`  
Nhánh chính: `main` (đã được bảo vệ)

---

## 1. Quy tắc làm việc với nhánh `main`

| Quy tắc | Chi tiết |
|---------|----------|
| **Cấm push trực tiếp vào `main`** | Bất kỳ ai (bao gồm cả chủ sở hữu) đều không được push commit trực tiếp lên `main`. |
| **Phải qua Pull Request** | Tất cả thay đổi phải được push lên một nhánh riêng, rồi gửi PR để merge vào `main`. |
| **Cần ít nhất 1 review** | PR cần được duyệt bởi ít nhất 1 người trước khi merge. |
| **Enforce admins** | Quy tắc áp dụng cho tất cả, không có excepción cho admin. |

---

## 2. Luồng làm việc chuẩn (contributor workflow)

### Bước 1: Cập nhật nhánh local về mới nhất
```bash
git checkout main
git pull origin main
```

### Bước 2: Tạo nhánh tính năng mới
Đặt tên nhánh có ý nghĩa, ví dụ:
```bash
git checkout -b feature/ten-tinh-nang
# hoặc
git checkout -b fix/ten-loi
```

### Bước 3: Thay đổi code và commit
```bash
git add .
git commit -m "feat: mô tả ngắn gọn thay đổi"
```

### Bước 4: Push nhánh lên GitHub
```bash
git push origin feature/ten-tinh-nang
```

### Bước 5: Tạo Pull Request
Mở trình duyệt vào:  
`https://github.com/baolongdev/smarthome-firmware/compare/main...feature/ten-tinh-nang`  
hoặc vào trang repo và nhấn **Compare & pull request**.

- **Base:** `main`
- **Compare:** nhánh tính năng của bạn
- Tiêu đề PR: `feat: ...` hoặc `fix: ...`
- Mô tả: ghi rõ thay đổi, ảnh chụp màn hình (nếu có), cách kiểm thử.

### Bước 6: Review và merge
- Đợi ít nhất 1 người review và approve.
- Sau khi approve, nhấn **Merge pull request** → **Confirm merge**.
- Nhánh tính năng có thể xóa sau khi merge (tùy chọn).

---

## 3. Ví dụ thực tế

```bash
# Cập nhật main
git checkout main && git pull origin main

# Tạo nhánh mới
git checkout -b feature/ota-progress-bar

# Sửa code
# ... chỉnh sửa các file ...

# Commit
git add -A
git commit -m "feat: hiển thị tiến độ OTA trên overlay"

# Push
git push origin feature/ota-progress-bar

# Sau đó vào GitHub tạo PR từ nhánh feature/ota-progress-bar vào main
```

---

## 4. Quy ước commit message

Sử dụng conventional commits:

| Loại | Ý nghĩa | Ví dụ |
|------|---------|-------|
| `feat` | Tính năng mới | `feat: thêm giao thức ESP-NOW chuẩn` |
| `fix` | Sửa lỗi | `fix: sửa crash khi quét Wi-Fi` |
| `docs` | Cập nhật tài liệu | `docs: viết hướng dẫn đóng góp` |
| `style` | Format code, không thay đổi logic | `style: căn lại căn lề hmi_ui.cpp` |
| `refactor` | Refactor không thay đổi tính năng | `refactor: tách ra module espnow_proto` |
| `build` | Thay đổi build system, dependencies | `build: nâng cấp PlatformIO` |
| `ci` | Thay đổi CI/CD | `ci: thêm workflow build` |

---

## 5. Cấu trúc repo quan trọng

```
firmware/
├── include/
│   ├── config.h              # Cấu hình biên dịch (Wi-Fi, ESP-NOW, OTA URL...)
│   ├── espnow_proto.h        # Giao thức ESP-NOW (frame, CRC, message types)
│   ├── espnow_master.h       # Header ESP-NOW slave
│   ├── hmi.h                 # API HMI (callbacks, model structs)
│   ├── storage.h             # Lưu trữ SD/NVS
│   └── wifi_manager.h        # Quản lý Wi-Fi (scan/join)
├── src/
│   ├── main.cpp              # Entry point, các task (net, app, espnow, console)
│   ├── espnow_master.cpp     # Triển khai ESP-NOW slave
│   ├── espnow_proto.cpp      # Encode/decode khung ESP-NOW
│   ├── wifi_manager.cpp      # Quản lý kết nối Wi-Fi
│   ├── storage.cpp           # Lưu đọc config.json
│   └── hmi/
│       ├── hmi_ui.cpp        # Giao diện LVGL (splash, điều khiển, cài đặt)
│       ├── hmi_model.h/.cpp  # Trạng thái HMI, dirty bits
│       └── hmi_theme.cpp/.h  # Màu sắc, font, helper
├── docs/
│   └── ESP_NOW_PROTOCOL.md   # Tài liệu chi tiết giao thức ESP-NOW
└── platformio.ini            # Cấu hình PlatformIO
```

---

## 6. Cách kiểm thử trước khi gửi PR

1. **Biên dịch:** `pio run -e esp32-4848s040` phải thành công.
2. **Nạp thử:** `pio run -e esp32-4848s040 -t upload --upload-port /dev/ttyUSB0` (khi có panel cắm).
3. **Kiểm tra giao diện:**
   - Splash khởi động: logo không bị cắt, giữ 5s.
   - Trang chủ: đồng hồ, độ ẩm, công suất (W) hiển thị đúng.
   - Cài đặt → Kết nối → Wi-Fi: quét, nhập mật khẩu, kết nối thành công.
   - Cài đặt → ESP-NOW: nhập MAC/kênh/PMK/LMK, lưu và áp dụng ngay.
4. **Kiểm tra Serial:** mở `pio device monitor --port /dev/ttyUSB0 --baud 115200`, xem log boot và các sự kiện ESP-NOW/Wi-Fi.

---

## 7. Liên hệ

Nếu có thắc mắc về quy trình hoặc code, hãy mở **Issue** trên repo hoặc bình luận vào PR liên quan.
