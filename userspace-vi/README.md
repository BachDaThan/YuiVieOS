# userspace-vi — Tầng 5: Trải Nghiệm Người Dùng Việt

| Tệp | Việc |
|---|---|
| `text.cpp` | Chuẩn hoá tiếng Việt: bỏ dấu, nối chữ, so khớp không phân biệt hoa/thường **(dùng chung cho tìm kiếm, shell, hộp cát)** |
| `ime.cpp` + `vi_vowels.inc` | Bộ gõ Telex **và** VNI, đặt dấu đúng chính tả; bộ đệm preedit có backspace/commit |
| `ibus/` | Adapter IBus C++17 đăng ký hai engine `yui-telex`/`yui-vni`; cần `libibus-1.0-dev` để build |
| `ui.cpp` | Kế hoạch giao diện thích ứng: `Framebuffer` (máy yếu) / `SoftwareComposite` / `GpuZeroCopy` (Vulkan/DRM) |
| `shell.cpp` | Shell song ngữ Việt–Anh, 15 lệnh, mỗi lệnh hai tên |
| `userspace-vi/ui/app/` | App Yui (Flutter) — khung sườn, chưa build (xem `ui/README.md`) |

## Build và đăng ký engine IBus

```bash
# Debian/Ubuntu
sudo apt install pkg-config libibus-1.0-dev
make ibus
sudo make install          # cài component XML và binary vào PREFIX
ibus restart
ibus engine yui-telex     # hoặc yui-vni
```

Có thể đặt `PREFIX="$HOME/.local"` để cài không cần quyền root. Hai engine xử lý
preedit theo từng phím; Space/dấu câu được commit rồi chuyển tiếp cho ứng dụng,
Backspace sửa lại chuỗi gõ, Escape huỷ preedit. Kiểu trường mật khẩu/PIN/số/URL/email
được chuyển qua nguyên trạng. Môi trường tối thiểu không cần IBus: `make`, `make test`
và `make check` tiếp tục chỉ dùng toolchain hiện có.

## Nguyên tắc

1. Tiếng Việt không phải bản dịch: tìm `he dieu hanh` phải khớp "hệ điều hành".
2. Bộ chuyển Telex/VNI là **hàm thuần**, test được không cần GUI; adapter IBus
   chỉ quản lý preedit/commit/backspace và chuyển phím qua IBus.
3. Giao diện chọn đường vẽ theo *phần cứng thật*, không theo cấu hình người dùng
   phải tự đoán.

## Trạng thái

- [x] Telex/VNI + đặt dấu + shell song ngữ + kế hoạch UI
- [~] IBus engine Telex/VNI + preedit/backspace/commit; cần `make ibus` để build
  (yêu cầu `pkg-config` + `libibus-1.0-dev`). Chưa xác minh trên Firefox/LibreOffice
  và chưa đo độ trễ đầu-cuối < 10 ms/phím, nên P1.4 chưa hoàn tất.
- [ ] Bộ vẽ framebuffer + Vulkan/DRM (P2.6)
