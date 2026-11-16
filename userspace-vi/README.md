# userspace-vi — Tầng 5: Trải Nghiệm Người Dùng Việt

| Tệp | Việc |
|---|---|
| `text.cpp` | Chuẩn hoá tiếng Việt: bỏ dấu, nối chữ, so khớp không phân biệt hoa/thường **(dùng chung cho tìm kiếm, shell, hộp cát)** |
| `ime.cpp` + `vi_vowels.inc` | Bộ gõ Telex **và** VNI; preedit hỗ trợ backspace, auto-restore thuật ngữ phổ biến và khôi phục literal |
| `ibus/` | Adapter IBus C++17 đăng ký `yui-telex`/`yui-vni`; cần `libibus-1.0-dev` để build |
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
Backspace sửa lại chuỗi gõ, Escape commit nguyên văn chuỗi phím (Esc-restore). Một
allowlist nhỏ tự khôi phục các thuật ngữ như `word`, `window`, `software`, `user`,
`linux`; từ khác vẫn có thể khôi phục bằng Escape. Kiểu trường mật khẩu/PIN/số/URL/email
được chuyển qua nguyên trạng. Môi trường tối thiểu không cần IBus: `make`, `make test`
và `make check` tiếp tục chỉ dùng toolchain hiện có.

## Nguyên tắc

1. Tiếng Việt không phải bản dịch: tìm `he dieu hanh` phải khớp "hệ điều hành".
2. Bộ chuyển Telex/VNI là **hàm thuần**, test được không cần GUI; adapter IBus
   quản lý preedit/commit/backspace, Escape-restore, auto-restore allowlist và chuyển phím.
3. Giao diện chọn đường vẽ theo *phần cứng thật*, không theo cấu hình người dùng
   phải tự đoán.

## Trạng thái

- [x] Telex/VNI + đặt dấu + shell song ngữ + kế hoạch UI
- [x] P1.4 IBus Telex/VNI + preedit/backspace/English auto-restore/Escape-restore.
  Người dùng xác minh trên Ubuntu 24.04 (commit `a786a44`): `make ibus` build sạch,
  `make check` 2831 checks, D-Bus avg 7.16 ms/phím (min 6.18; max ~12 do nhịp nghỉ
  `pump` trong script Python), đạt tiêu chí latency trung bình < 10 ms/phím.
  Firefox/LibreOffice GUI được chuyển sang Desktop QA follow-up, không chặn P1.4.
- [ ] Bộ vẽ framebuffer + Vulkan/DRM (P2.6)
