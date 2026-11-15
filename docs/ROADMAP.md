# Lộ trình — Roadmap

Nguyên tắc: **viết ra cái chạy được trước, ghi rõ cái chưa có, không hứa bằng
tài liệu.** Mỗi mục dưới đây nói rõ cần gì để làm, nên làm được ở đâu, và làm
sao biết là xong.

## Đã xong (và có test chứng minh)

- [x] Toàn bộ Tầng 4: `yuid`, JSON-RPC qua Unix socket, MCP 8 công cụ, C-ABI
  `libyui` (kiểm từ C11), đồ thị tri thức, kho mã hoá scrypt+SHAKE+HMAC,
  engine q4/t2/BITCOS với SIMD, tự chỉnh k/W, web nhiều bậc (có offline),
  công cụ OS.
- [x] Tầng 5 (phần logic): shell song ngữ 15 lệnh, bộ gõ Telex+VNI (84 test),
  gọt tỉa & nén app `.yuip` (strip ELF, khử trùng lặp, lọc locale, chống zip-slip),
  kế hoạch giao diện thích ứng.
- [x] Tầng 3 (phần chính sách): bảng năng lực, thẻ HMAC, hộp cát 3 bậc, audit,
  4 chính sách mẫu có test, schema manifest micro-VM.
- [x] Tầng 2 (phần chính sách): lập lịch LAVD trong Rust an toàn, ZRAM (codec +
  kho trang + thống kê), codec đối xứng Rust ↔ C++.
- [x] Tầng 1 (phần mô hình): `CapTable` + token ký số trong Rust `no_std`,
  SHA-256/HMAC tự viết có vector RFC, profiler phần cứng + phân loại máy.
- [x] Tầng 0 (phần mô tả): linker script x86_64, luồng khởi động 3 kiến trúc,
  phát hiện SIMD (AVX2/AVX-512/NEON/SVE/RVV).
- [x] Công cụ: `yui-profiler`, `yui-trim`, `yui-shell`, Makefile, 705 test, CI.

---

## Giai đoạn A — Đóng vòng lặp trên Linux host (2–4 tuần)

Mục tiêu: YuiVieOS chạy như **một tầng dịch vụ trên Linux**, dùng được hằng ngày
trước khi có nhân riêng. Đây là bước quan trọng nhất vì nó tạo người dùng thật.

- [ ] **A1. `yuid` chạy như systemd service** — unit + socket activation
  (`yuid.socket`), `ProtectSystem=strict`, `NoNewPrivileges=yes`, `MemoryMax=`.
  *Xong khi:* `systemctl status yuid` xanh, socket `0600`, test `--serve-once` vẫn qua.
- [ ] **A2. Bộ gõ thành IBus engine thật** — một tiến trình nhỏ (`ibus-yui`) gọi
  `ime.convert` qua socket, cắm vào IBus/Fcitx5. *Xong khi:* gõ Telex trong
  Firefox và LibreOffice ra chữ Việt đúng dấu.
- [ ] **A3. `libyui` publish** — `pkg-config` file, `soname` ổn định, ví dụ C và
  Python (ctypes). *Xong khi:* `pkg-config --cflags --libs yui` chạy từ một thư
  mục khác, ví dụ Python gọi được `ime.convert`.
- [ ] **A4. MCP dùng thật** — cấu hình mẫu cho một MCP client, tài liệu 8 công cụ
  kèm ví dụ. *Xong khi:* một agent bên ngoài gọi `graph_upsert` rồi `graph_query`
  thấy dữ liệu.
- [ ] **A5. Đóng gói** — `.deb` + AppImage/Flatpak cho `yuid`+`libyui`+CLI.

## Giai đoạn B — Rust thay dần tầng thấp (1–2 tháng)

- [ ] **B1. `yui-zram` dùng thật** — module ZRAM qua `/dev/zram0` hoặc FUSE,
  đo tỉ lệ nén trên dữ liệu thật (không chỉ test). *Xong khi:* 200 MB dữ liệu
  văn phòng nén còn < 80 MB, đọc lại đúng từng byte.
- [ ] **B2. Scheduler thật** — `sched_ext` BPF *hoặc* một userspace daemon đọc
  `sched_ext`-style; chính sách vẫn là crate `yui-sched` (Rust an toàn) để hot-swap.
  *Xong khi:* game giữ 60 fps khi có `make -j8` chạy nền; số liệu trước/sau.
- [ ] **B3. Ed25519 clean-room** — cài từ RFC 8032, vector test đầy đủ, ký/kiểm
  `.bundle`. *Xong khi:* bundle bị sửa 1 byte bị từ chối, bundle hợp lệ cài được.
- [ ] **B4. `graph.json` chịu tải** — ghi nguyên tử, WAL nhỏ, 100k nút vẫn mở < 1 s.

## Giai đoạn C — Tầng 3 thật (2–3 tháng)

- [ ] **C1. `yui-vmmd`** — supervisor micro-VM trên KVM (rust-vmm hoặc crosvm làm
  backend), đọc `vm-manifest.schema.json`, không mạng mặc định.
- [ ] **C2. Lớp Windows** — NTSYNC + VKD3D-Proton/DXVK trong micro-VM, đo hiệu
  năng so với Wine trần. *Xong khi:* một game DirectX 11 chạy, có số fps.
- [ ] **C3. Waydroid-lite** — container Android với chính sách `microvm`
  (không fs host, mạng qua `net.connect` của `yuid`).
- [ ] **C4. Kiểm chứng cách ly** — bộ test "cố thoát hộp cát": app thử đọc
  `/etc/shadow`, mở socket, `ptrace`; ghi lại cái gì chặn được.

## Giai đoạn D — Nhân riêng (6–12 tháng, cần máy thật)

- [ ] **D1. `yui-boot`** — stub 32→64 bit (x86_64 BIOS), UEFI, ARM64 EL2→EL1,
  RISC-V S-mode; nạp `YuiBootInfo`. Thử trên QEMU trước.
- [ ] **D2. Tầng 1 trong nhân** — `yui-cap` port sang nhân thật: cấp phát đối
  tượng, IPC đồng bộ, thu hồi bắc cầu ở mức MMU.
- [ ] **D3. Tầng 2 cơ chế** — vòng đời tác vụ, tickless timer, `io_uring`-style
  zero-copy, coalescing SSD, DVFS qua ACPI/OPP.
- [ ] **D4. Hiển thị** — framebuffer trực tiếp (máy yếu), Vulkan/DRM zero-copy
  (máy mạnh), font tiếng Việt + dựng hình.
- [ ] **D5. Toolchain tự chủ** — build được `yuid` trên chính YuiVieOS.

## Giai đoạn E — Hệ sinh thái

- [~] **E1. App Yui (Flutter)** — khung sườn đã có (`ui/app`: 3 màn hình, nói
  chuyện với `yuid` qua Unix socket). **Chưa build** — cần Flutter SDK; xem
  `ui/README.md`. Còn thiếu: build thật, ảnh chụp, FFI `libyui` cho đường độ trễ thấp.
- [ ] **E2. Chợ app có chữ ký** — `.yuip` + Ed25519, xác minh trước khi chạy.
- [ ] **E3. Đa ngôn ngữ dân tộc** — Tày, Nùng, Khmer, Chăm: bộ gõ và font.
- [ ] **E4. Tài liệu cho người dùng cuối** — tiếng Việt, có ảnh, không thuật ngữ.

---

## Việc *không* làm (và vì sao)

| Ý tưởng | Vì sao không |
|---|---|
| Fork một nhân có sẵn | Yêu cầu clean-room; và fork thường mang theo 30 năm nợ kỹ thuật |
| Nhúng libzstd/OpenSSL | Máy yếu không nên phụ thuộc phiên bản thư viện để khởi động |
| Viết AES-GCM tay | Dễ sai; chọn keystream SHAKE + HMAC-SHA-256 là lựa chọn *có thể kiểm chứng* |
| Tuyên bố "an toàn tuyệt đối" | `SECURITY.md` liệt kê 7 điều chưa bảo vệ được — giữ nguyên sự thật đó |
| Chạy mô hình lớn trong repo | Trọng số là dữ liệu người dùng, không phải mã nguồn |

## Cách đóng góp

1. Chọn một mục, mở issue nói rõ bạn định làm và "xong khi" là gì.
2. Mọi thay đổi phải giữ `make test` xanh và không thêm `unsafe` vào `crates/`.
3. Tài liệu song ngữ Việt–Anh cho mọi tính năng người dùng thấy.
4. Nếu bạn phát hiện tài liệu sai so với mã: **sửa tài liệu**, đừng sửa test cho khớp tài liệu.
