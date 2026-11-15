# userspace-vi — Tầng 5: Trải Nghiệm Người Dùng Việt

| Tệp | Việc |
|---|---|
| `text.cpp` | Chuẩn hoá tiếng Việt: bỏ dấu, nối chữ, so khớp không phân biệt hoa/thường **(dùng chung cho tìm kiếm, shell, hộp cát)** |
| `ime.cpp` + `vi_vowels.inc` | Bộ gõ Telex **và** VNI, đặt dấu đúng chính tả (84 test) |
| `ui.cpp` | Kế hoạch giao diện thích ứng: `Framebuffer` (máy yếu) / `SoftwareComposite` / `GpuZeroCopy` (Vulkan/DRM) |
| `shell.cpp` | Shell song ngữ Việt–Anh, 15 lệnh, mỗi lệnh hai tên |
| `userspace-vi/ui/app/` | App Yui (Flutter) — khung sườn, chưa build (xem `ui/README.md`) |

## Nguyên tắc

1. Tiếng Việt không phải bản dịch: tìm `he dieu hanh` phải khớp "hệ điều hành".
2. Bộ gõ là **hàm thuần**, test được không cần GUI, chạy được ở tầng hiển thị
   (không độ trễ, không qua IPC).
3. Giao diện chọn đường vẽ theo *phần cứng thật*, không theo cấu hình người dùng
   phải tự đoán.

## Trạng thái

- [x] Telex/VNI + đặt dấu + shell song ngữ + kế hoạch UI (test đầy đủ)
- [ ] IBus/Fcitx5 engine thật (P1.4)
- [ ] Bộ vẽ framebuffer + Vulkan/DRM (P2.6)
