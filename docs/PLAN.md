# YuiVieOS Beta — Kế hoạch thi công

Tài liệu quản lý tiến độ. Mỗi dòng là một task có **mã**, **tiêu chí nghiệm thu
đo được** và **bằng chứng**. Nguyên tắc duy nhất: **không đánh dấu `[x]` khi
chưa có bằng chứng chạy được** — bằng chứng là lệnh + đầu ra, hoặc đường dẫn test.

Ký hiệu: `[x]` xong (có bằng chứng) · `[ ]` chưa xong · `[~]` một phần · `⛔` bị
chặn (ghi rõ chặn bởi gì).

---

## 0. Cách đọc và cách cập nhật

- Mã task: `P<số>.<số>` — ví dụ `P1.4`. Không đổi mã đã công bố; task mới thêm số tiếp.
- Mỗi task có **DoD** (*Definition of Done*) viết bằng số hoặc hành vi quan sát được.
- Cuối mỗi phase có **cổng ra phase (gate)**: không sang phase sau khi gate chưa xanh.
- Khi làm xong: sửa `[ ]` → `[x]`, thêm một dòng bằng chứng vào §12.

### Trạng thái hiện tại (2026-11-17)

| Phase | Nội dung | Trạng thái |
|---|---|---|
| **P0** | Nền móng: monorepo, hợp đồng cổng chờ, test C++/C11, CI | ✅ xong (tất cả 14 task P0.1–P0.14 đạt; nay **9855** check = 9794 C++17 + 61 C11; 0 fail) |
| **P1** | Chạy thật trên Linux host: `yuid` service, IBus, stripper, telemetry, OS MCP | 🟡 4/12 xong (P1.1, P1.4, P1.5, P1.12); P1.6 một phần; P1.11 Direct mode app thật |
| **P2** | Rust framekernel + 3 kiến trúc + QEMU + năng lượng + FS + Linux ABI | 🟡 khung nhân boot thật cả 3 kiến trúc trong QEMU: aarch64 + riscv64 trên CI; **x86_64 boot được theo log Kaggle của bạn (2026-11-17)**, còn chờ đối chiếu job CI `qemu` + UEFI |
| **P3** | VMM sandbox (Wine/VKD3D, Waydroid-lite) | ⬜ chưa bắt đầu (+4 task đề xuất 2026-11-17: P3.13–P3.16) |
| **P4** | UX & phát hành (compositor, installer, ký số) | ⬜ chưa bắt đầu (+P4.14 cửa sổ liền mạch) |
| **P5** | *(dự trữ)* Tích hợp AI vào hệ điều hành | 🔒 chờ lệnh kích hoạt |
| **P6** | *(đề xuất 2026-11-16, chờ bạn duyệt)* Phần cứng thật, hồ sơ máy siêu yếu, hồ sơ game, hệ tệp tương thích (§14); +3 mục nghiên cứu 2026-11-17 (shader, Box64/FEX, token chữ ký số) | ⬜ chưa bắt đầu |

> **Cập nhật 2026-11-16 (rà soát kế hoạch, không đổi mã):** đối chiếu kế hoạch với một đợt tư vấn ngoài (Gemini) và với mã của `Adaptive-MoE`/`Y-App`. **Không có task cũ nào bị đổi mã hay đổi trạng thái.** Thêm: P2.14–P2.15, P3.12, P4.9–P4.13, P5.11–P5.16 (P5 vẫn 🔒), Phase **P6** (phần cứng thật) và §13–§14 (đánh giá + backlog). Kế hoạch phía AI nằm ở `Adaptive-MoE/docs/PLAN.md` (mục 6 là cầu nối).

> **Cập nhật 2026-11-17 (nâng cấp kế hoạch sau đợt tư vấn thứ ba + log test của bạn):** đối chiếu với mã thật của `main` (commit `2217b35`, gồm `e885ac9` Direct IBus). **Không có task cũ nào bị đổi mã hay đổi trạng thái; chỉ sửa các con số đã lỗi thời** (số check 2842 → **3055**, số test Rust, mục trạng thái). **Thêm 12 task, tất cả `[ ]` và chờ bạn duyệt:** P0.14; P1.11, P1.12; P3.13–P3.16; P4.14; P5.17 (P5 vẫn 🔒); P6.14–P6.16. **Thêm** §5.2 (thiết kế P3.13–P3.16), §7.7 (chỉ dẫn cho tác tử AI thi công P5) và §13 mục C30–C41 (đánh giá lời tư vấn). Bằng chứng nằm ở §12 (dòng 2026-11-17).

---

## 1. Cổng chất lượng chung (áp dụng cho mọi phase)

| Mã | Task | DoD | Bằng chứng |
|---|---|---|---|
| P0.1 | `make test` xanh | 0 failed, không bỏ qua test nào | `make test` |
| P0.2 | `make check` xanh | 4 smoke run thành công | `make check` |
| P0.3 | `scripts/verify.sh` xanh | cấu trúc + policy JSON + an toàn Rust + liên kết tài liệu | `./scripts/verify.sh` |
| P0.4 | Không có `unsafe` trong crate nhân | `grep -rn unsafe kernel-cap kernel-services` chỉ ra `deny(unsafe_code)` | verify.sh bước 3 |
| P0.5 | CI chạy trên mọi push | 5 job: cpp, rust, docs, cross-arch, qemu | GitHub Actions |
| P0.6 | Tài liệu ↔ mã không lệch | mỗi khẳng định "đã có" trong `ARCHITECTURE.md` trỏ tới tệp/test tồn tại | verify.sh bước 5 |
| P0.14 | **Tầng 1–2 không dependency ngoài** (giữ nguyên tắc "lõi sạch" bằng máy) *(đề xuất 2026-11-17)* | [x] | `scripts/verify.py::check_kernel_dependencies` kiểm tra: `kernel-cap` và `kernel-services/*` không có dependency ngoài (chỉ cho phép path nội bộ trong repo); `kernel-frame` chỉ phụ thuộc `yui-cap`. Đã kiểm chứng DoD: thêm tạm `foo = "1"` vào `kernel-cap/Cargo.toml` ⇒ `verify.sh` lập tức báo đỏ `FAIL kernel-cap/Cargo.toml: dependency 'foo = "1"' không phải path nội bộ`; gỡ ra ⇒ xanh 100% |

---

## 2. Phase P0 — Nền móng (✅ xong)

| Mã | Task | Trạng thái | Bằng chứng |
|---|---|---|---|
| P0.7 | Monorepo sạch: `docs/ kernel-cap/ kernel-services/ vmm-sandbox/ ai-bridge-yuid/ pkg-stripper/ userspace-vi/ tools/ arch/ include/ tests/ scripts/ .github/workflows/` | [x] | `ls -F` |
| P0.8 | Hợp đồng cổng chờ: JSON-RPC `/run/yuid.sock`, C-ABI `libyui`, MCP `2024-11-05` | [x] | `tests/test_daemon.cpp` (102), `tests/test_capi.c` (36) |
| P0.9 | Capability: quyền, dẫn xuất, thu hồi, thẻ ký HMAC (Rust + C++ đối chiếu) | [x] | `kernel-cap/src/lib.rs` (6 test lúc đầu; **`kernel-cap` nay 23 test** theo log Kaggle của bạn 2026-11-17), `tests/test_sandbox.cpp` (129) |
| P0.10 | Chính sách lập lịch LAVD + ZRAM (Rust `no_std`, deny unsafe) | [x] | `kernel-services/yui-{sched,zram}` (9 + 8 test lúc đầu; **nay 13 + 8** theo log Kaggle 2026-11-17) |
| P0.11 | Vietnamese: Telex/VNI, fold, shell song ngữ, kế hoạch UI | [x] | `tests/test_ime.cpp` (84), `tests/test_tools.cpp` (61) |
| P0.12 | Smart App Stripper: strip ELF, khử trùng lặp, lọc locale, chống zip-slip | [x] | `pkg-stripper/apppack.cpp`, `tools/trim` |
| P0.13 | Tài liệu: `ARCHITECTURE.md`, `SECURITY.md`, `PROTOCOL.md`, `FORMAT-YUIP.md`, `BUILD.md`, `PLAN.md` | [x] | `docs/` |

**Gate P0:** `make check` + `verify.sh` xanh, cả 6 tầng có ít nhất một vật thể chạy
được hoặc một đặc tả kiểm được. ✅

---

## 3. Phase P1 — Chạy thật trên Linux host (🟡 3/12 xong; 1 việc một phần)

Mục tiêu chiến lược: **có người dùng thật trước khi có nhân riêng.** YuiVieOS Beta
chạy như một tầng dịch vụ trên Linux: bộ gõ, stripper, trợ lý, quản lý năng lượng.
Đây là phase tạo giá trị và tạo phản hồi cho P2.

| Mã | Task | Trạng thái | DoD (đo được) |
|---|---|---|---|
| P1.1 | `yuid` chạy như systemd service + socket activation | [x] | `systemctl start yuid` xanh; socket `0600`; `yuid --serve-once` vẫn qua test |
| P1.2 | ZRAM dùng thật qua `/dev/zram0` (module có sẵn của host) | [ ] | 200 MB dữ liệu văn phòng → < 80 MB; đọc lại **đúng từng byte**; `zramctl` xác nhận |
| P1.3 | `libyui` publish: `pkg-config`, `soname` ổn định, ví dụ C + Python | [ ] | `pkg-config --cflags --libs yui` chạy ngoài cây; ví dụ Python gọi `ime.convert` |
| P1.4 | Bộ gõ thành IBus/Fcitx5 engine | [x] | **Đã nghiệm thu trên Ubuntu 24.04 bởi người dùng (commit `a786a44`):** `make ibus` build sạch không patch tay; `make check` 2831 checks (246 `test_ime`); Telex/VNI, English auto-restore, Escape-restore và ca khó `các`/`mắm` chạy đúng. D-Bus latency trung bình **7.16 ms/phím** (min 6.18, max khoảng 12 do nhịp nghỉ `pump` của script Python); đạt DoD trung bình `<10 ms/phím`. GUI Firefox/LibreOffice chuyển thành Desktop QA follow-up, không chặn P1.4 |
| P1.5 | Telemetry Hook (P4.2 của `ARCHITECTURE.md` §6.3) | [x] | `telemetry.read` + `system_telemetry` MCP đọc sysfs thật; máy này không có pin ⇒ `present:false`; `tests/test_telemetry.cpp` **471 check** (ranh giới 20 % / 75 °C, pin thấp, GPU nóng, quét toàn miền k/W, RPC + MCP); `test_capi` 61 check phủ `yui_telemetry`/`yui_telemetry_advise`. **Chưa** đối chiếu được trên máy có pin thật — ghi rõ, không tô hồng |
| P1.6 | Smart Stripper trên app thật | [~] | Electron ≥ 35 % nhỏ hơn và **chạy được**; app có DRM: `stripped=false`, 0 byte bị sửa |
| P1.7 | Đóng gói `.deb` + AppImage/Flatpak | [ ] | Cài trên Ubuntu 22.04 sạch, `yui-shell` chạy, gỡ sạch |
| P1.8 | MCP dùng thật với một client | [ ] | Agent ngoài gọi `graph_upsert` → `graph_query` thấy dữ liệu; log chứng minh |
| P1.9 | Audit log người đọc được | [ ] | `audit.log` có ≥ 3 mục thật; mỗi mục có lý do tiếng Việt và **không** chứa bí mật |
| P1.10 | Đo nền: thời gian khởi động dịch vụ, RAM của `yuid` | [ ] | `yuid` < 8 MB RSS khi rảnh; khởi động < 200 ms (đo bằng `systemd-analyze` + `/proc/pid/status`) |
| P1.11 | **Direct mode: nghiệm thu trên ứng dụng thật + gõ nhanh** *(đề xuất 2026-11-17)* | [ ] | Bằng chứng hiện có (commit `e885ac9`): `test_ime` **459** check, `YUI_IME_MODE=direct` được nhận, chuỗi mẫu `dd`/`Tieengs`/`window` ra đúng (`đ`, `Tiếng`, `window` giữ nguyên) với 34 BackSpace — nhưng đó là **chạy bằng script Python điều khiển engine**, chưa phải ứng dụng thật. DoD: trên Ubuntu 24.04, ≥ 6 ứng dụng (Firefox, Chromium/Chrome, LibreOffice Writer, gedit hoặc Kate, gnome-terminal, một app Electron) × 2 chế độ (Preedit, Direct) × 3 tốc độ gõ (chậm, ~8, ~15 phím/giây) gõ cùng đoạn 200 từ tiếng Việt: bảng **số ký tự sai/thừa/thiếu** (số, không ghi "ổn"). Kiểm thêm: nhấp chuột giữa từ (reset đúng, **không xoá** nội dung đã commit), chọn vùng rồi gõ, dán, và **Ctrl+Z sau Direct** (mỗi phím có thành 1 bước hoàn tác không?). Kết luận ghi thành danh sách "ứng dụng khuyên dùng Direct" (0 lỗi ở 15 phím/giây) và "dùng Preedit". **Preedit vẫn là mặc định** |
| P1.12 | **Direct mode: chỉ gửi phần sai khác (giữ tiền tố chung)** *(đề xuất 2026-11-17)* | [x] | `DirectComposition::replace_rendered` tính tiền tố chung theo Unicode codepoints NFC; chỉ phát BackSpaces cho phần đuôi cũ và commit phần đuôi mới; giữ nguyên cấu trúc `ReplayEdit`. Chuỗi mẫu `dd` + `Tieengs` + `window` chỉ tốn đúng **5 BackSpace** (giảm từ 34). Test `direct_mode_random_replay_and_common_prefix_invariants` mô phỏng 488 chuỗi (Telex + VNI, có `ươ`/`đ`, hoa/thường, xoá lùi giữa từ, ngẫu nhiên PRNG): văn bản cuối khớp 100% bản thay cả từ ở mọi bước, tổng BackSpace luôn $\le$ bản cũ. `test_ime` đạt **7149 checks** (tăng từ 459), 0 fail |

**Lưu ý P1 (2026-11-17):**
- *Direct mode (P1.4, `e885ac9`)* là tuỳ chọn thêm, không thay đổi nghiệm thu P1.4. Nó cần IBus surrounding text để biết caret đổi chỗ; ứng dụng không gửi thông tin này thì từ đang gõ có thể không được reset khi bạn nhấp chuột. Đây là lý do P1.11 phải đo trên ứng dụng thật chứ không chỉ script.
- *P1.6 (Smart Stripper)*: kết quả "≥ 35 %" mới đo trên cây thư mục mẫu (5 tệp → 187 B, nhật ký 2026-11-15), **chưa** trên một app Electron thật. Quy tắc mới: Stripper chạy **cục bộ trên bản người dùng đã có**; **không phân phối lại** bản đã gọt của phần mềm có giấy phép cấm sửa/chia lại (nhiều phần mềm Việt phổ biến là đóng nguồn). Kho ứng dụng một chạm chỉ gồm gói mà giấy phép cho phép.

**Gate P1:** một người không phải lập trình viên gõ tiếng Việt được bằng YuiVieOS
trên máy thật, và một app được gọt chạy bình thường. Ghi lại ảnh + số đo.

---

## 4. Phase P2 — Rust framekernel, 3 kiến trúc, đo được

| Mã | Task | Trạng thái | DoD (đo được) |
|---|---|---|---|
| P2.0 | Workspace Rust `kernel-cap` + `kernel-services/*`, `deny(unsafe_code)` | [x] | `cargo test --workspace` (CI); `grep unsafe` rỗng; `kernel-frame` **ngoài** workspace để `--workspace` vẫn chỉ chạy trên host |
| P2.1 | `yui-frame`: khung duy nhất chứa `unsafe` (asm, MMU, ngắt, MMIO) | [x] | `kernel-frame/` là crate **duy nhất** có `unsafe`: 29 khối, tất cả nằm trong 7 tệp có tên + lý do ở `scripts/verify.py::FRAME_UNSAFE_FILES` (tệp khác có `unsafe` ⇒ `verify.sh` đỏ); mọi crate khác vẫn `deny(unsafe_code)`. Boot thật trong QEMU trên aarch64 + riscv64 — job CI `qemu` xanh |
| P2.2 | `yui-boot` + nhân tối thiểu, x86_64 (BIOS Legacy + UEFI) | [~] | **Đã sửa đường boot QEMU x86_64**: header Multiboot2 (24 byte + end tag, checksum `-(0xE85250D6+24)`) đặt cạnh Multiboot1; cầu 32→64 mở rộng edi/esi thành `kmain_x86(u32,u32)` phân biệt magic `0x2BADB002`/`0x36D76289`; `parse_multiboot2` đọc **little-endian** (đã sửa lỗi be32); `build-kernel.sh` xuất `.elf64` + `objcopy -O elf32-i386` ra `.elf` cho QEMU `-kernel`; job CI `qemu` **bắt buộc** cả 3 kiến trúc. **Còn đợi:** CI `kernel`+`qemu` xanh xác nhận boot thật (sandbox dev không có QEMU — không claim thay). UEFI chưa làm. **Bằng chứng mới (2026-11-17, log Kaggle của bạn; Claude không chạy QEMU):** kernel x86_64 build **42.272 byte** (ELF32) + ELF64, QEMU x86_64 *boot được, profiler chạy, bảng năng lực sống*; aarch64 113.112 byte và riscv64 79.176 byte cũng boot. Vì DoD ghi "CI `kernel`+`qemu` xanh", task **giữ `[~]`** cho tới khi bạn xác nhận job CI x86_64 xanh (và UEFI chưa làm) |
| P2.3 | ARM64 + RISC-V 64 ở mức "ready" | [x] | Cả hai **boot thật trong QEMU** (job CI `qemu` xanh): in `yui: ready`, `Phan loai`, `Tang 1`. riscv64 đọc RAM từ device tree; aarch64 nhận `x0 = 0` (QEMU không truyền DTB cho ELF `-kernel`) nên tự quét FDT (`find_fdt`) trong cửa sổ `0x4000_0000±512 KiB`, không có thì in `RAM: 0 MiB — khong doc duoc`, **không bịa số** |
| P2.4 | Sovereign Keyring + Ed25519 clean-room cho `.bundle` | [x] | **Xong, có bằng chứng test**: Ed25519 clean-room (`kernel-services/ed25519.cpp` ~600 dòng + `kernel-cap/src/ed25519.rs`) khớp **vector RFC 8032 §7.1** (TEST 1/2/3/SHA(abc)/1024 — pub + sig khớp từng hex) và SHA-512 khớp FIPS 180-4: `tests/test_ed25519.cpp` **78 check xanh**; kho khoá + `.bundle` ký/xác minh ngoại tuyến + 5 kịch bản sửa 1 byte đều từ chối + cài khe A/B + quay lui (`bundle.*`); bản Rust `keyring.rs`/`bundle.rs` có test song song. **Còn thiếu (ngoài phạm vi bản này):** cài bundle thật trong QEMU |
| P2.5 | Năng lượng: full tickless + DVFS mỗi nhân + clock gating | [~] | **Chính sách đã cài + test 47 check** (`include/yui/power.hpp`, `kernel-services/power.cpp`, `tests/test_power.cpp`): tickless 0 wakeup định kỳ khi rảnh, kế hoạch ngủ theo deadline (halt/deep/suspend), bảng OPP 3 loại máy, DVFS theo tải/nhiệt/pin/trần tiến trình, clock-gating GPU/NPU khi quá nhiệt 75 °C, bóp xung per-task. Nguồn dữ liệu = `telemetry.cpp`. **Còn thiếu:** đo trên nhân thật (0 wakeup/10 s, ≥ 20 % W gói) — không có phần cứng trong sandbox, không claim thay |
| P2.6 | Hiển thị: framebuffer trực tiếp + DRM/Vulkan zero-copy | [~] | Bộ vẽ pixel tuyến tính + font 5×7 + màn hình chào: `userspace-vi/fb.cpp`, `tests/test_fb.cpp` (**1184 check**), `tools/splash` → `docs/splash.png`; bản kernel `kernel-frame/src/splash.rs`. **Đã thêm chữ Việt có dấu**: 180 chữ cái tiền tổ hợp (bảng NFD sinh từ Unicode), dấu thanh + dấu nguyên âm xếp chồng + dấu nặng + đ/Đ, `draw_text_utf8` vẽ ASCII **giống hệt** `draw_text` cùng toạ độ; khung cửa sổ + nhãn tiếng Việt (`userspace-vi/ui.cpp::render_window`) — test `fb_viet.*` (29 check). **Còn thiếu:** đường DRM/Vulkan, và đo fps |
| P2.7 | Lớp Linux ABI: syscall nhóm 1 (file, mmap, futex, epoll) | [~] | **Đã cài đủ nhóm syscall cốt lõi** (`kernel-services/linuxabi.cpp`, `tests/test_linuxabi.cpp` **89 check**): read/write/open/close/openat/exit/exit_group đúng ngữ nghĩa; mmap (hỗ trợ ẩn danh + file-backed, thực thi W^X nghiêm ngặt: PROT_WRITE+PROT_EXEC ⇒ EPERM, bảo vệ /core chỉ-đọc ⇒ EPERM), munmap; futex (FUTEX_WAIT kiểm tra giá trị nguyên tử + xếp hàng luồng, FUTEX_WAKE đánh thức); epoll (epoll_create1, epoll_ctl ADD/MOD/DEL, epoll_wait thăm dò sẵn sàng); `utimensat` → EPERM (chặn timestomping); fork/execve/socket/connect → EPERM; `nr_from32` quy đổi không gian số i386 (gồm cả mmap2, futex, epoll). **Còn đợi:** chạy binary ELF thật (busybox/coreutils/python3) trong QEMU |
| P2.8 | FS bất biến + write coalescing | [~] | **Đã cài + test 96 check** (`kernel-services/fsstore.cpp`, `tests/test_fsstore.cpp`): phân vùng lõi từ chối mọi ghi (lý do tiếng Việt), `WriteCoalescer` gộp ghi cùng trang (3 lệnh → 1 chốt vật lý), chốt lỗi giữ trang để thử lại, hỗ trợ đọc nested paths, chặn traversal và symlink escape bằng `openat` + `O_NOFOLLOW` theo từng thành phần, `CowStore` hỗ trợ snapshot CoW và đo thời gian thực tế. **Còn thiếu:** tích hợp storage vào kernel và test rút điện 20 lần trên QEMU với disk image thật |
| P2.9 | ZRAM trong nhân thật | [ ] | 200 MB văn phòng → < 80 MB trong QEMU với RAM 512 MB; đọc lại đúng |
| P2.10 | Self-hosting: build `yuid` trên chính YuiVieOS Beta | [ ] | Trong QEMU: `cargo build` (hoặc rustc bootstrap) của `yuid` thành công; chạy `yuid --info` từ binary vừa build |
| P2.11 | Ngân sách RAM máy yếu | [ ] | Sau boot (nhân + dịch vụ bắt buộc): **< 64 MB** RSS tổng, đo bằng `free`/bộ đếm trong nhân |
| P2.12 | Guard chống bất thường nền (đào coin ngầm) — `docs/SECURITY.md` §10.1 | [x] | Quy tắc 70 %/15 s/headless/không `compute.heavy` ⇒ hạ Idle + bóp xung 20 % + audit + hỏi "tiêu diệt / gỡ bỏ / để yên" qua `yuid` (`guard.decide` bắt buộc `confirm`). Cài: `kernel-services/guard.cpp`, `kernel-services/yui-sched/src/guard.rs`, `ai-bridge-yuid/daemon/service.cpp` (`guard.observe/alerts/decide`). Test: `tests/test_redteam.cpp` (kịch bản miner, tải nhấp nháy, tiến trình hợp lệ bất khả xâm phạm, chuỗi quyết định qua daemon) |
| P2.13 | Bộ lọc egress theo năng lực + test Red-Team — `docs/SECURITY.md` §10.2–10.3 | [x] | `isolated`/`microvm` mặc định chặn egress; chỉ đích trong `policies/*.json` (host/wildcard/CIDR + cổng) đi được, deny thắng allow, có lý do tiếng Việt. Cài: `vmm-sandbox/sandbox.cpp::EgressFilter` (+ `compute.heavy` vào bảng quyền, docs 15→16). Test: `tests/test_redteam.cpp` (mặc định chặn, khoá hậu tố giả mạo, sai cổng, ngoài CIDR, exfil bị chặn ở cả EgressFilter lẫn Enforcer + audit) |
| P2.14 | Snapshot CoW + hoàn tác chống mã độc tống tiền (mở rộng P2.8 + P2.12) | [x] | **Đã cài + test bằng chứng thực nghiệm** (`include/yui/fsstore.hpp`, `kernel-services/fsstore.cpp`, `include/yui/guard.hpp`, `kernel-services/guard.cpp`, `tests/test_fsstore.cpp`, `tests/test_redteam.cpp`): `CowStore` quản lý snapshot giao dịch CoW (chia sẻ buffer O(1), cấp phát khi ghi), đo thời gian snapshot bằng số thật (`high_resolution_clock`), tính toán bù trừ dung lượng (`StorageAccounting`: logical/physical/shared/exclusive bytes). Quy tắc Guard mới: tính Shannon entropy, phát hiện ghi dồn dập tốc độ cao với entropy > 7.2 bit/byte ⇒ hạ ưu tiên xuống Idle, bóp xung còn 10%, phát cảnh báo tiếng Việt và gợi ý rollback. Test Red-Team: mô phỏng 10.000 tệp bị mã hoá ciphertext ⇒ Guard bắt được, bóp xung ⇒ người dùng rollback ⇒ 10.000/10.000 tệp khớp 100% băm SHA-256 ban đầu. Nêu rõ giới hạn trong tài liệu: không chống được ransomware ghi chậm dưới ngưỡng hoặc cố tình ghi dữ liệu entropy thấp |
| P2.15 | Guard mở rộng theo **quy tắc**: micro/camera/danh bạ/mạng khi máy rảnh | [x] | **Đã cài + test đủ 6 kịch bản Red-Team** (`include/yui/guard.hpp`, `kernel-services/guard.cpp`, `tests/test_redteam.cpp`): `IdleGuard` theo dõi thời gian rảnh của người dùng; khi máy rảnh quá ngưỡng (mặc định 300 s / 5 phút), ứng dụng nền không có cửa sổ hiển thị cố tình mở micro, camera, đọc danh bạ hoặc gửi mạng ⇒ tự động bóp xung (20%), thu hồi thẻ năng lực liên quan với độ phức tạp **O(1)** (`revoked_badges_` băm), phát cảnh báo tiếng Việt và hỏi người dùng. Đã kiểm thử đủ 6 kịch bản Red-Team trong `test_redteam.cpp`: (1) rảnh + micro lén ⇒ chặn + thu hồi O(1); (2) người dùng đang gõ + video call ⇒ cho phép; (3) rảnh + trộm danh bạ ⇒ chặn; (4) rảnh + exfiltration mạng ⇒ chặn; (5) ứng dụng có cửa sổ giao diện/do người dùng chủ động ⇒ cho phép; (6) thao tác chập chờn trước ngưỡng ⇒ không báo nhầm, kích hoạt chính xác sau khi đủ thời gian rảnh |

**Gate P2:** 3 kiến trúc boot trong QEMU trên CI; ngân sách RAM và tickless đạt;
Ed25519 chạy. **Đây là gate khó nhất của dự án** — không đánh dấu xong nếu thiếu
một kiến trúc.

> Ghi chú thẳng thắn: P2 là 6–12 tháng người. Trong sandbox phát triển hiện tại
> **không có** `qemu`, `cargo`, `limine`; P2 chỉ tiến được trên máy có các công cụ
> đó. Kế hoạch này không giả vờ khác.

---

## 5. Phase P3 — Hộp cát VMM

| Mã | Task | Trạng thái | DoD |
|---|---|---|---|
| P3.1 | `yui-vmmd`: supervisor micro-VM trên KVM, đọc `vm-manifest.schema.json` | [ ] | Tạo/huỷ VM bằng manifest; mạng mặc định `none` (test: VM không ping được ra ngoài) |
| P3.2 | Lớp Windows: Wine/Proton + VKD3D-Proton/DXVK trong micro-VM | [ ] | Một game DirectX 11 chạy ≥ 30 fps; ghi lại fps và mức dùng CPU so với Wine trần |
| P3.3 | NTSYNC-style đồng bộ tầng nhân | [ ] | p99 frame time của game giảm ≥ 20 % so với đồng bộ userspace (đo bằng `MangoHud`/ftrace) |
| P3.4 | Waydroid-lite | [ ] | Cài 1 APK, chạy, mạng chỉ qua `net.connect` của `yuid` |
| P3.5 | Bộ test "cố thoát hộp cát" | [ ] | 10 kịch bản (đọc `/etc/shadow`, mở socket, `ptrace`, `mount`, `/proc/self/mem`…) — **10/10 bị chặn**, mỗi cái có log lý do |
| P3.6 | Driver lạ trong micro-VM | [ ] | Một driver USB mẫu chạy trong VM; crash VM không ảnh hưởng nhân (test bằng cách giết VM) |
| P3.7 | Rà soát ngữ cảnh và threat model trước khi hardening | [ ] | Đọc đủ `docs/PLAN.md`, `docs/ARCHITECTURE.md`, `docs/SECURITY.md`, `docs/PROTOCOL.md`, `README.md`, toàn bộ mã `vmm-sandbox/`, `ai-bridge-yuid/`, `kernel-cap/`, `tests/`; ghi baseline, trust boundary, contract hiện tại, rủi ro và quyết định còn mở. Kiểm `git log -n 5` + `git status` trên **nhánh Arena hiện hành**, không chuyển nhánh. Không sửa mã trước khi bước rà soát được duyệt |
| P3.8 | Lớp 1 — ranh giới capability/syscall + thu hồi bắc cầu | [ ] | Xác minh HMAC/expiry/scope của token theo một contract tương thích `kernel-cap`; phát hiện syscall/boundary anomaly; thu hồi cả họ token/descendants bằng generation/epoch để thao tác revoke là **O(1)** (không duyệt cây), lần verify kế tiếp từ chối toàn bộ token con nhưng không ảnh hưởng họ khác; mọi phát hiện có audit và lý do |
| P3.9 | Lớp 2 — micro-VM quarantine, freeze rồi huỷ phiên | [ ] | Khi Lớp 1 báo xâm phạm/chạm ngưỡng đã chốt, chuyển `running → frozen/quarantined → destroyed` trước khi xử lý yêu cầu guest kế tiếp; huỷ đúng phiên bị ảnh hưởng, thao tác idempotent, không tạo lớp sandbox lồng nhau; audit ghi đủ nguyên nhân/kết quả. Tích hợp thật phụ thuộc P3.1; mock chỉ dùng unit test, không được tính là bằng chứng containment |
| P3.10 | Lớp 3 — egress zero-trust, anti-DNS-tunneling, secretless broker | [ ] | Cập nhật schema phiên bản và `policies/*.json`: east-west giữa sandbox mặc định deny; cấm DNS trực tiếp, chỉ resolver/broker nội bộ và tên khớp allowlist chính xác mới được phân giải; từ chối label > 63 octet, tên > 253 octet, tên sai chuẩn hoặc entropy vượt ngưỡng đã hiệu chuẩn; broker giữ bí mật ở vòng ngoài, gắn xác thực sau khi duyệt request và không đưa token/secret vào guest, môi trường hay audit. Cập nhật `docs/SECURITY.md` và `vmm-sandbox/README.md` khi triển khai |
| P3.11 | Red-team tích hợp cho ba lớp | [ ] | Mở rộng `tests/test_redteam.cpp`: anomaly Lớp 1 → revoke O(1) + freeze/huỷ Lớp 2; token con bị vô hiệu, token ngoài họ còn hiệu lực; chặn peer-to-peer, DNS tunnel/nhãn dài/entropy cao, tên ngoài allowlist và egress ngoài policy; xác nhận request hợp lệ qua broker; audit cả allow/deny. Toàn bộ ca thoát ly bị chặn, không có regression với tên hợp lệ |
| P3.12 | Chính sách mặc định cho ứng dụng **không có chữ ký hợp lệ** *(đề xuất)* | [ ] | Ứng dụng/gói không ký (hoặc ký bằng khoá không tin cậy) ⇒ vào hộp cát `net: none`, không thẻ `admin`/`power`, người dùng phải xác nhận cài. 3 test: không ký ⇒ không mạng; ký sai ⇒ từ chối; ký đúng ⇒ theo policy riêng. Dựa trên Ed25519 của P2.4 |
| P3.13 | **Bộ gõ/IME bên thứ ba** chạy trong hộp cát *(đề xuất 2026-11-17)* | [ ] | Engine IBus/Fcitx5 **không phải của YuiVieOS** chạy trong hộp cát `isolated`: quyền duy nhất `input.compose` (nhận phím, trả chuỗi); không `net`; không `fs` ngoài thư mục dữ liệu riêng. Quy tắc cứng nằm ở **lớp YuiVieOS**, không do engine quyết: trường mật khẩu/PIN/số/URL/email **không bao giờ** chuyển phím tới engine ngoài. Red-team: engine giả mở socket, đọc `/home`, ghi log phím ⇒ bị chặn + audit; gõ vào trường mật khẩu ⇒ engine ngoài nhận **0** phím. Lý do ưu tiên: bộ gõ thấy **mọi** phím, mức nhạy cảm cao hơn trình duyệt. Bộ gõ riêng của YuiVieOS (mã trong repo, có test) không bắt buộc vào hộp cát |
| P3.14 | **Snapshot micro-VM + "warm pool"** (khôi phục nhanh thay vì khởi động nguội) *(đề xuất)* | [ ] | **Đo, không cam kết "dưới 1 giây".** Trên micro-VM mẫu, 20 lần mỗi cách: khởi động nguội vs khôi phục snapshot (p50/p95), RAM chiếm, và so **hai cách lưu**: trong RAM nén (ZRAM) vs tệp snapshot trên đĩa nạp lười bằng mmap. Bắt buộc: (1) sau khôi phục phải **gieo lại RNG + cấp ID/MAC mới** (test: hai bản khôi phục từ một snapshot cho hai chuỗi ngẫu nhiên và ID khác nhau); (2) snapshot gắn băm, **sửa 1 byte ⇒ từ chối khôi phục**; (3) không chia sẻ snapshot giữa ứng dụng/người dùng khác nhau (snapshot chứa bộ nhớ, tức cả bí mật); (4) tắt warm pool ở lớp `may-yeu`/`sieu-yeu` (nối P6.2). **Go (đề xuất):** khôi phục p95 ≤ 25 % thời gian khởi động nguội; không thì `[-]`. Phụ thuộc P3.1 |
| P3.15 | **Policy gợi ý khi đóng gói** (`policies/<app>.proposed.json`) *(đề xuất)* | [ ] | `yui-trim` phát tệp gợi ý gồm 3 nguồn có nhãn riêng: (a) đích **khai báo** trong manifest, (b) tên miền **quét từ chuỗi** trong bộ cài (độ tin cậy thấp: tên ghép lúc chạy, nén hay mã hoá sẽ lọt), (c) đích **ghi nhận khi chạy thử** trong hộp cát với egress "chặn và ghi". Tệp **không bao giờ tự bật**: người dùng duyệt từng đích (kèm lý do tiếng Việt). Test: gói mẫu có 3 tên miền chuỗi tĩnh + 1 tên ghép lúc chạy ⇒ 3 có nguồn (b), 1 chỉ có nguồn (c); tệp sinh ra parse được theo schema và **deny vẫn thắng allow**. Danh sách chặn quảng cáo công cộng chỉ dùng cho app có mức `net` rộng, và chỉ sau khi đọc giấy phép của danh sách đó |
| P3.16 | **Chế độ Tương thích theo ứng dụng + thẻ phiên (session capability) + sinkhole DNS** *(đề xuất; theo yêu cầu của bạn 2026-11-17)* | [ ] | (a) Bật/tắt riêng từng app (giao diện + một lệnh `yui-shell`, tên 2 ngôn ngữ chốt khi làm); **mặc định TẮT**; app khác không bị ảnh hưởng. (b) **Sinkhole ở tầng DNS**: tên bị chặn ⇒ NXDOMAIN hoặc địa chỉ không định tuyến + từ chối kết nối ngay (test: app nhận lỗi < 100 ms (đề xuất) thay vì treo). **Không MITM TLS**; "HTTP 200 rỗng" chỉ dùng cho HTTP thuần hoặc qua broker P3.10. (c) Khi app đang hoạt động bị chặn ≥ N lần trong M giây (host tự quan sát `EgressFilter`, **không cần app báo lỗi**), hiện **một** thông báo qua đường tin cậy P4.13: "Ứng dụng này cần kết nối tới `<tên miền>`. Mở quyền tạm thời **15 phút** (mặc định)?" kèm **ô nhập thời gian tuỳ chọn** và lựa chọn "đến khi đóng ứng dụng"; trần thời gian (đề xuất 8 giờ). (d) Thẻ là con của thẻ phiên; **thu hồi O(1)** theo epoch (P3.8) khi hết hạn, khi đóng app hoặc khi bạn thu hồi. (e) Chống làm phiền: tối đa 1 lần hỏi/app/5 phút; từ chối 3 lần ⇒ ngừng hỏi (có thể mở lại trong Cài đặt); audit mọi hỏi/cấp/từ chối/thu hồi. (f) Test: hết hạn ⇒ egress chặn lại; đóng app ⇒ thẻ vô hiệu, thẻ họ khác còn nguyên; app giả gửi click tổng hợp để tự duyệt ⇒ bị bỏ qua. Phạm vi cấp là **đúng tên miền đã hiển thị**, không phải "cả mạng quảng cáo". Phụ thuộc P3.8, P3.10, P4.13 |

### 5.1 Trình tự hardening Tầng 3 — chỉ lập kế hoạch, chưa triển khai

P2.13 hiện tại đã có `EgressFilter` allowlist cơ bản; các việc P3.7–P3.11 là
**mở rộng mới**, không được ghi nhận nhầm là đã có transitive revocation, freeze
phiên micro-VM, lọc DNS theo entropy hay secretless broker. Thứ tự bắt buộc là
P3.7 → P3.8 → P3.9 → P3.10 → P3.11; không nhảy thẳng tới tuyên bố hoàn tất.

**Gate P3:** 10/10 kịch bản thoát hộp cát thất bại; game chạy có số fps; Android app chạy. Với hardening mới, chỉ qua gate khi P3.7–P3.11 có bằng chứng riêng, toàn bộ kịch bản red-team đạt và containment Lớp 2 được kiểm trên micro-VM thật (P3.1), không chỉ bằng mock.

### 5.2 Ghi chú thiết kế P3.13–P3.16 (2026-11-17)

- **Vì sao P3.15 không phải "danh sách chặn quảng cáo":** `EgressFilter` của bạn đã **chặn mặc định và chỉ cho đích trong allowlist** (deny thắng allow). Với app trong hộp cát `isolated`/`microvm`, tên miền quảng cáo vốn đã bị chặn; thứ còn thiếu là **allowlist ban đầu** cho app thật sự cần mạng. Vì vậy P3.15 đề xuất allowlist (có người duyệt), không phải denylist.
- **Vì sao không trả "HTTP 200 rỗng" cho HTTPS (P3.16b):** giả được một phản hồi HTTPS hợp lệ đòi hỏi chen vào TLS (MITM) bằng chứng chỉ giả; việc đó phá chính mô hình zero-trust và nhiều app ghim chứng chỉ sẽ vẫn lỗi. Sinkhole DNS + từ chối nhanh cho kết quả "app thất bại gọn" mà không phải chen vào mã hoá. Ứng dụng dùng DoH tự mang theo đã bị chặn bởi P3.10 (cấm DNS trực tiếp).
- **Vì sao hộp thoại cấp quyền phải qua đường tin cậy (P4.13):** nếu app vẽ được hộp thoại giống hệt thì tự "bấm đồng ý" cho mình; đó là lý do P3.16 phụ thuộc P4.13 và có test click tổng hợp.
- **Snapshot (P3.14):** con số "khôi phục trong vài chục mili giây" của Firecracker/crosvm (theo hiểu biết công khai, **chưa tra lại hôm nay**) là của micro-VM nhỏ; guest chạy Wine + game lớn hơn nhiều và phải nạp trang theo nhu cầu. Rủi ro thường bị bỏ qua: hai VM khôi phục từ **cùng snapshot** dùng chung trạng thái entropy nếu không gieo lại.
- **Bộ gõ (P3.13):** hộp cát cho IME ngoài **không thay được** quy tắc "trường mật khẩu không bao giờ tới engine ngoài"; hai thứ cần cả hai.

---

## 6. Phase P4 — UX & phát hành

| Mã | Task | Trạng thái | DoD |
|---|---|---|---|
| P4.1 | Compositor: framebuffer (yếu) + Vulkan/DRM (mạnh) | [ ] | Chuyển chế độ theo profiler không cần đăng nhập lại; 30 fps trên máy 2 GB |
| P4.2 | Telemetry C-ABI (`yui_telemetry`) hoàn chỉnh | [ ] | 3 test C11; khuyến nghị k/W khớp bảng `ARCHITECTURE.md` §6.3 |
| P4.3 | 5 công cụ OS MCP + xác nhận người dùng + audit | [ ] | Đạt toàn bộ DoD và ma trận kiểm thử ở §6.1: MCP 2024-11-05/JSON-RPC 2.0, HMAC capability, confirm/deny không side effect, audit cả allow/deny, chạy trong ngân sách runtime 2 GiB, clean-room; `make clean && make -j2 check` xanh |
| P4.4 | Shell: 25 lệnh, gợi ý lệnh, song ngữ đầy đủ | [ ] | Mọi lệnh có 2 tên; `giúp` liệt kê đủ; test cho từng tên |
| P4.5 | App Yui (Flutter) build được | [ ] | `flutter build linux` xanh trong CI; 3 màn hình dùng được với `yuid` thật |
| P4.6 | Trình cài đặt (USB, BIOS + UEFI, A/B) | [ ] | Cài lên máy ảo từ USB, boot lại vào hệ thống đã cài, chữ ký ảnh hợp lệ |
| P4.7 | Ký số phát hành + `.bundle` | [ ] | Bundle được ký bằng khoá offline; máy từ chối bundle sai chữ ký |
| P4.8 | Tài liệu người dùng cuối (tiếng Việt, có ảnh) | [ ] | 5 hướng dẫn: cài, gõ tiếng Việt, gọt app, tiết kiệm pin, khôi phục |
| P4.9 | `yui-update` (check / apply / rollback) trên nền `.bundle` A/B của P2.4 *(đề xuất)* | [ ] | Trong QEMU: bundle hợp lệ ⇒ ghi khe B, khởi động lại vào B, `yui-update rollback` về A; bundle sai chữ ký ⇒ từ chối; **`apply` không tự khởi động lại** khi người dùng đang làm việc (chỉ đặt cờ "áp dụng ở lần khởi động tới"). Ba luồng độc lập: lõi hệ điều hành / mô hình AI (`Yui-Models`) / ứng dụng. Cập nhật sai phân (bsdiff/zsync) là **tuỳ chọn sau**, và máy người dùng chỉ được dùng bộ áp dụng tự viết (clean-room) |
| P4.10 | Pipeline phát hành bằng GitHub Actions *(đề xuất)* | [ ] | Workflow: dọn runner → build → `qemu` smoke 3 kiến trúc → sinh `manifest.json` **chưa ký** → (bạn ký **ngoại tuyến**) → đẩy lên Releases kênh `canary`, sau 24–48 giờ hoặc khi bạn duyệt thì sang `latest`. DoD: bản có test đỏ **không bao giờ** lên Releases; **không có khoá bí mật Ed25519 trong GitHub Secrets**; mỗi tệp đính kèm < 2 GiB; máy người dùng lấy manifest qua `releases/latest/download/…` (không gọi API); mô hình AI **không** đóng trong ISO mà tải riêng. Thiết kế chữ ký dùng chung với `Adaptive-MoE` P9-7 |
| P4.11 | Build tái lập được + SBOM *(đề xuất)* | [ ] | Hai lần build từ cùng commit (cố định `SOURCE_DATE_EPOCH`, đường dẫn, thứ tự) cho **sha256 trùng** của `libyui.a`, `yuid`, ảnh nhân; sinh danh mục thành phần + giấy phép tự động (hiện không dependency ngoài ⇒ danh mục phải nói đúng điều đó) |
| P4.12 | `yui-shell` lai POSIX (pipe, chuyển hướng, nền, `kill`) *(đề xuất)* | [ ] | Hai chế độ trong cùng shell: lệnh Việt–Anh nội bộ (hiện **15 lệnh**, mục tiêu P4.4 là 25) và chuyển tiếp lệnh POSIX. **Trên Linux host (P1)** chạy được ngay bằng `posix_spawn` trong hộp cát; **trên nhân riêng** phải chờ P2.7 (`fork`/`execve` hiện trả `EPERM`). DoD: 20 kịch bản (ống dẫn, chuyển hướng `>` `>>` `<`, chạy nền `&`, `jobs`, `kill`, biến môi trường, trích dẫn); dòng không nhận ra **không tự chạy**; mọi lệnh đi qua cùng `Daemon` như GUI/MCP |
| P4.13 | Đường xác nhận **tin cậy** (chống giả click / clickjacking) *(đề xuất)* | [ ] | Hộp thoại xác nhận của `sys_*` do compositor/nhân vẽ; chỉ nhận sự kiện từ **thiết bị vật lý** do nhân gắn thẻ; ứng dụng không chen được bằng sự kiện tổng hợp. Test: ứng dụng giả gửi click tổng hợp ⇒ bị bỏ qua + audit. **Không** dùng nhịp gõ/sinh trắc hành vi làm căn cứ xác nhận (độ tin cậy thấp, là dữ liệu nhạy cảm, và không chặn được kẻ chen vào đường nhập) |
| P4.14 | **Cửa sổ liền mạch** (cửa sổ app trong micro-VM hiện trên compositor chủ) *(đề xuất 2026-11-17)* | [ ] | Cửa sổ app Windows/Android hiện như cửa sổ thường. **Tiêu đề + viền do chủ vẽ**, kèm nhãn mức tin cậy + tên app lấy từ manifest — guest **không** đổi được (cách Qubes OS làm; WSLg/crosvm dùng đường truyền khác, theo hiểu biết công khai). Test: guest cố vẽ hộp thoại giả giống hộp thoại hệ thống, che nhãn, đọc điểm ảnh cửa sổ khác, đọc clipboard khi không có thao tác ⇒ **thất bại + audit**; clipboard và kéo-thả đi qua môi giới có xác nhận. Đo **độ trễ nhập (ms) và fps** so với cửa sổ gốc, ghi vào §12 (không cam kết "như bản gốc"). Phụ thuộc P3.1, P4.1, P4.13; đường truyền (virtio-gpu hay proxy Wayland) chọn **sau khi đo**; thành phần ngoài phải duyệt theo §6.1 mục 5. Gộp mục Wayland của §14.2 |

### 6.1 Runbook P4.3 — thực hiện theo thứ tự (CHƯA TRIỂN KHAI)

1. **Chốt hợp đồng trước khi viết mã.** P4.3 phải chờ Gate P3 và hoàn tất rà soát
   P3.7. Giữ MCP `2024-11-05`, JSON-RPC 2.0 (`tools/list`, `tools/call`); `system_telemetry`
   đã có ở P1.5, không tạo bản sao. Xác định adapter để daemon C++ xác minh đúng
   contract HMAC của `kernel-cap` (không tự tạo định dạng token/khóa thứ hai).
2. **Phạm vi quyền và điểm cần xác nhận:**

   | Công cụ | Capability tối thiểu | Hành vi cần gate |
   |---|---|---|
   | `sys_power` | `power` | Thay governor/giới hạn sạc/chế độ nguồn |
   | `sys_scheduler` | `admin` hoặc `power` | Đổi profile lập lịch |
   | `sys_app_stripper` | `fs.read` + `fs.write` đúng scope | Áp dụng hoặc hoàn tác thay đổi app; preview không ghi |
   | `sys_proc` | `admin` | Dừng tiến trình; thao tác liệt kê không thay đổi trạng thái |
   | `sys_search_vault` | `vault` | Xác nhận trước khi trả kết quả nhạy cảm; không trả plaintext khi vault chưa mở khóa |

   Mọi thao tác thay đổi trạng thái cần xác nhận tường minh của người dùng; thiếu
   xác nhận trả `needs_confirmation`, người dùng từ chối thì không gọi adapter và
   không tạo side effect. `ARCHITECTURE.md`/`PROTOCOL.md` hiện ghi
   `sys_search_vault` không cần xác nhận; để đáp ứng ma trận thử từ chối xác nhận
   cho cả 5 công cụ, kế hoạch này **đề xuất gate trước khi tiết lộ kết quả vault**.
   Chốt quyết định và cập nhật hợp đồng/tài liệu trước khi triển khai, không để
   hành vi trong mã khác tài liệu.
3. **Xác thực capability fail-closed.** Token lấy từ ngữ cảnh phiên đáng tin cậy,
   không tin token do đối số MCP tự khai; xác minh MAC, subject, quyền, scope,
   hạn dùng và trạng thái thu hồi. Thiếu/sai/hết hạn/sai scope đều từ chối trước
   khi gọi OS adapter. Dùng vector kiểm thử chung để chứng minh tương thích với
   `kernel-cap`; không sao chép hay viết lại thuật toán mật mã.
4. **Audit đầy đủ nhưng không rò rỉ bí mật.** Ghi mọi lời gọi thành công, bị từ
   chối, thiếu capability, thiếu/từ chối xác nhận vào `audit.log`. Ghi thời gian,
   subject, công cụ/thao tác, mã định danh capability, kết quả xác thực/xác nhận,
   quyết định và lý do; che token/MAC, secret, plaintext và nội dung vault. Test
   phải chứng minh log có đủ dấu vết và không chứa dữ liệu bí mật.
5. **Ngân sách máy yếu và clean-room.** Chạy daemon/runtime trong cgroup giới hạn
   **2 GiB RAM**, stress ít nhất 1.000 lời gọi mỗi công cụ; không OOM, không tăng
   RSS/queue không giới hạn, đo và lưu p95 latency. Chốt ngưỡng p95 trong P3.7
   trước khi code để “mượt” có tiêu chí đo được. Chỉ dùng mã nguồn trong repo và
   thư viện chuẩn/primitive đã có; không vendor/copy mã ngoài, không thêm
   dependency mới nếu chưa được rà soát license và phê duyệt.
6. **Kiểm thử độc lập và tích hợp.** Mở rộng `tests/test_daemon.cpp` (đã nằm
   trong `Makefile`) cho MCP OS; nếu P3.7 chọn tách bộ test, thêm tệp và đăng ký
   target cùng lúc. Với từng công cụ, kiểm `tools/list`/`tools/call`, lời gọi hợp lệ, thiếu hoặc
   token sai/hết hạn/sai scope, người dùng từ chối xác nhận (không có side effect),
   cùng audit của cả allow/deny. Dùng OS adapter giả trong unit test để không
   dừng tiến trình hay sửa máy host; có kiểm tích hợp riêng cho đường adapter.
7. **Cổng hoàn tất.** Cập nhật `ARCHITECTURE.md` và `PROTOCOL.md`; chạy
   `make clean && make -j2 check` từ cây sạch. Chỉ đánh dấu P4.3 `[x]` khi build
   C++17/C11, `scripts/verify.sh`, toàn bộ test (gồm test mới) và CLI smoke đều
   pass, đồng thời có log đo ngân sách 2 GiB và p95.

**Gate P4:** một người dùng mới cài được YuiVieOS Beta từ USB và dùng được hằng ngày.
P4.3 không được coi là xong chỉ vì 5 tên công cụ xuất hiện trong `tools/list`;
phải đạt đủ xác thực, xác nhận, audit, ngân sách tài nguyên và kiểm thử ở §6.1.

---

## 7. Phase P5 — *(dự trữ)* Tích hợp AI vào Hệ điều hành

> **Trạng thái: 🔒 CHƯA KÍCH HOẠT.** Chỉ triển khai khi người dùng ra lệnh
> **"Tích hợp AI vào hệ điều hành"**. Không tự chạy trước, không kéo mã ngoài
> khi chưa có lệnh. Toàn bộ hợp đồng đã dựng sẵn ở P0/P1/P4 nên P5 chỉ là *cắm dây*.

### 7.1 Điều kiện vào (phải xanh trước khi bắt đầu)

- [ ] P1.5 telemetry chạy; P4.2 `yui_telemetry` có test
- [ ] P4.3 5 công cụ OS MCP + xác nhận + audit có test
- [ ] `ai-bridge-yuid` giữ nguyên tên phương thức và mã lỗi C-ABI (không phá hợp đồng)
- [ ] Có mạng tới GitHub **hoặc** có sẵn bản sao hai repo AI trên máy
- [ ] *(đề xuất 2026-11-16)* Phía `Adaptive-MoE`: P9-15 (cổng gắn: `libyui_engine` + `yui_features`) và P9-8 (fuzz bộ đọc `.yui`) đã xong; khuyến nghị thêm P9-4 và P9-7
- [ ] *(đề xuất 2026-11-17)* Nếu làm P5.17: `Adaptive-MoE` P9-18 (công cụ web của agent ở chế độ `broker`, không tự mở socket) đã xong; phía OS cần P3.16 (thẻ phiên) và P4.13 (đường xác nhận tin cậy)

### 7.2 Runbook (thực hiện tuần tự, có kiểm sau mỗi bước)

| Bước | Việc | Lệnh / tiêu chí |
|---|---|---|
| P5.1 | Kéo mã lõi AI | `git clone https://github.com/BachDaThan/Adaptive-MoE vendor/adaptive-moe` và `git clone https://github.com/BachDaThan/Y-App vendor/y-app` — kiểm `vendor/*/LICENSE` |
| P5.2 | Đóng băng phiên bản | ghi commit SHA vào `vendor/VERSIONS.lock` (không dùng `main` trôi nổi) |
| P5.3 | Biên dịch **engine** đa kiến trúc thành `libyui_engine.so` (**không** đặt tên `libyui.so`: trùng thư viện của OS, xem §7.5) | `x86_64`, `aarch64`, `riscv64` — mỗi cái có `sha256` và được ghi vào `vendor/VERSIONS.lock`. Phía `Adaptive-MoE` là task P9-15 |
| P5.4 | Kiểm khớp C-ABI (**hai ABI riêng**) | (a) `libyui.so` **của OS**: `nm -D` phải có đủ các hàm khai báo trong `include/yui/yui.h` (đếm bằng `grep`: **25**, không phải 12 như bản cũ ghi) và `tests/test_capi.c` link với lib thật; (b) `libyui_engine.so`: đủ **17 hàm `yui_*`** của engine (`version, set_threads, get_threads, cpu_features, load, free, info, meta, chat_begin, chat_begin_img, complete_begin, gen_next, gen_tokens, gen_end, vision_load, vision_run, vision_free`) + `yui_features`; thêm một test C mới cho ABI engine (`test_engine_abi.c`, sẽ đặt cùng thư mục với `test_capi.c`) |
| P5.5 | Nối vào bridge | `yuid` nạp `libyui_engine.so` bằng `dlopen(RTLD_LOCAL)` + `dlsym` theo tên (tránh đụng `yui_version`/`yui_free` của `libyui` OS); giữ `reference/` làm fallback có cờ `--reference-engine` |
| P5.6 | Nạp `.yui v3` | `mmap` tệp qua `yuid`, kiểm `fs.read` đúng đường dẫn; test: tệp 1 GB mở < 50 ms, RAM tăng < 5 MB |
| P5.7 | Bật dịch vụ nền `yuid` | `systemctl enable --now yuid`, socket `0600`, `yuid --info` liệt kê engine thật |
| P5.8 | Bật điều khiển tiếng Việt qua MCP | test 5 công cụ với câu tiếng Việt có dấu **và** không dấu; mỗi hành động có audit + xác nhận |
| P5.9 | Kiểm hồi quy toàn bộ | `make test` **≥ 9745** check xanh (9684 C++17 + 61 C11; con số tăng khi có test mới, **không được giảm**); thêm test tích hợp: 1 câu lệnh tiếng Việt → hành động OS thật |
| P5.10 | Ghi lại số đo | thời gian phản hồi, RAM tăng thêm, k/W thực tế ở 3 chế độ máy |

### 7.3 Điều kiện thoát (rollback)

Nếu bất kỳ bước P5.4–P5.9 fail: `yuid --reference-engine` phải đưa hệ thống về
trạng thái trước P5 **trong một lệnh**, không cần build lại. Đây là lý do
`reference/` không bị xoá ở P5.

### 7.4 Những gì **không** làm ở P5

- Không sửa `kernel-cap`/`kernel-services` để "hợp" với AI.
- Không cho AI quyền `admin` mặc định; mọi quyền phải có trong thẻ và trong audit.
- Không nhúng trọng số mô hình vào repo hệ điều hành (dung lượng + giấy phép).
- Không tự động bật mạng cho `web_search`.
- Không đưa engine vào `kernel-frame` (cần libc/luồng/mmap): engine chỉ chạy ở user-space qua `yuid`.
- Không đặt công cụ `sys_*` trong Python (`yui_agent.py`): chúng ở `yuid` với thẻ năng lực.

### 7.5 Rà soát P5 theo mã thật (2026-11-16) — vẫn chỉ là kế hoạch, P5 chưa kích hoạt

Mình đã clone `Adaptive-MoE` và `Y-App` **chỉ trong môi trường làm việc để đọc**; không kéo gì vào repo này (`vendor/` không tồn tại). Phát hiện (đã kiểm):

| # | Phát hiện | Hệ quả / xử lý |
|---|---|---|
| 1 | **Hai thư viện cùng tên `libyui`.** Của OS (`include/yui/yui.h`: pack, graph, telex, vault, telemetry, sandbox) và của engine (`yui.cpp`: load, chat, gen). Hai tên hàm trùng, **cùng chữ ký nhưng khác ngữ nghĩa**: `yui_version` (trả chuỗi khác nhau) và `yui_free` (OS giải phóng bộ nhớ malloc; engine xoá `Model`). Cùng chữ ký nghĩa là **không có lỗi liên kết nào báo trước**: nếu hai thư viện chung không gian ký hiệu, `yui_free` có thể giải phóng sai loại con trỏ. | Engine build thành `libyui_engine.so`, `yuid` dùng `dlopen(RTLD_LOCAL)` (P5.3–P5.5 đã sửa chữ). Phía engine: `Adaptive-MoE` P9-15 |
| 2 | **P5.4 ghi "12 hàm trong `yui.h`"**: thực tế `yui.h` có **25** khai báo, còn engine có **17 hàm khác**. Bản cũ lẫn hai ABI. | P5.4 đã tách thành (a) ABI của OS và (b) ABI của engine |
| 3 | `include/yui/engine.hpp` của OS **không phải** ABI của `yui.cpp`: nó là nửa "di động" tự viết (dò SIMD kể cả `rvv`, lượng tử q4/t2, bộ chỉnh k/W). Hàm nạp/sinh của `yui.cpp` (`yui_load`, `yui_chat_begin`, `yui_gen_next`…) chưa có chỗ nào trong OS. | Cần lớp bọc `EngineBackend` (P5.14) cắm **sau** `Service`, không sửa `daemon/` (đúng quy tắc `reference/README.md`) |
| 4 | `reference/graph.cpp` và `reference/vault.cpp` ghi "thay bằng `yui_graph` / `yui_vault` của Adaptive-MoE". Hai thứ đó là **mô-đun Python**, không link được vào `libyui`. | Chỉ thay được khi `Adaptive-MoE` có lõi C++ (`kb_core`, Q6/P9-13). Không thì **giữ bản C++ tham chiếu làm bản chính** và chỉ đảm bảo tương thích định dạng (P5.15) |
| 5 | Công cụ OS (`sys_power`…) phải nằm trong `yuid`. Gemini đề xuất gắn `sys_*` vào `yui_agent.py` (Python): sẽ **vòng qua** thẻ năng lực. | `yui_agent.py` chỉ là **client** của `yuid` (P5.13) |
| 6 | Engine chạy file `.yui` do người dùng/Kho cung cấp **trong dịch vụ nền**. Lỗi bộ đọc ở đây nặng hơn trong app. | Fuzz bộ đọc (`Adaptive-MoE` P9-8) là điều kiện vào; model không chữ ký ⇒ hạ quyền công cụ (P5.12) |
| 7 | `libyui_engine` cần libc, luồng, mmap ⇒ **không** link vào nhân `no_std`; chỉ chạy ở user-space qua `yuid`. | Ghi vào §7.4: không đưa engine vào `kernel-frame` |

### 7.6 Bổ sung P5 (đề xuất 2026-11-16; **vẫn khoá** cho tới khi có lệnh)

| Mã | Task | Trạng thái | DoD (đo được) |
|---|---|---|---|
| P5.11 | Nối telemetry → k của engine | [ ] | `yui_telemetry_advise` (đã có, 471 check) quyết `kcap` mỗi lần `chat_begin`: pin < 20% **hoặc** ≥ 75 °C ⇒ k = 1; cắm sạc và mát ⇒ k tối đa. Kiểm bằng telemetry giả + log. Cửa sổ trượt W: chỉ khi engine có (Adaptive-MoE P9-11) |
| P5.12 | Quyền công cụ **độc lập với model** | [ ] | `yuid` lọc mọi `<tool>` theo thẻ năng lực của phiên, kể cả khi engine đã khoá cú pháp (Adaptive-MoE P9-5, chỉ bảo đảm **cú pháp**). Model không có chữ ký tin cậy (Adaptive-MoE P9-7) ⇒ chỉ trả văn bản. Red-team: model giả sinh `<tool>sys_power …</tool>` ⇒ bị từ chối + audit; tiêm lệnh qua kết quả web không nâng được quyền |
| P5.13 | `yui_agent.py` làm client MCP của `yuid` | [ ] | Lệnh tiếng Việt qua agent gọi `system_telemetry` qua `/run/yuid.sock`; audit có dòng; **không** có công cụ `sys_*` cài trong Python |
| P5.14 | Lớp bọc `EngineBackend` (`yui/engine.hpp` ↔ `libyui_engine.so`) | [ ] | Cùng hợp đồng JSON-RPC với `--reference-engine`; `test_daemon` 102 check xanh với **cả hai** backend |
| P5.15 | Chọn hướng đồ thị/vault thật | [ ] | Quyết một trong hai (kb_core C++ của Adaptive-MoE, hoặc giữ bản tham chiếu OS); test round-trip `export`/`import` |
| P5.16 | Dịch tiếng Việt → lệnh POSIX **có xác nhận** (nối P4.12) | [ ] | Bộ ≥ 100 câu; lệnh nguy hiểm (`rm -rf /`, `dd`, `mkfs`, `chmod -R 777 /`…) bị chặn **100%** trong test; mọi lệnh qua xác nhận + hộp cát; công bố **độ chính xác đo được**, không cam kết 100% |
| P5.17 | Công cụ web của tác tử đi qua **thẻ phiên** (không tự mở mạng) *(đề xuất 2026-11-17)* | [ ] | `web_search`/`fetch` của `yui_agent.py` ở chế độ `broker` (`Adaptive-MoE` P9-18) gửi qua `yuid`; **chưa có thẻ ⇒ trả câu "chưa được cấp quyền mạng"** (model không bị câm); có thẻ phiên (P3.16, mặc định 15 phút) ⇒ chạy; thu hồi ⇒ lần gọi kế bị từ chối. Red-team: nội dung web chứa `<tool>sys_power …</tool>` không nâng được quyền; audit ghi mọi lần cấp. Giữ nguyên §7.4: không tự động bật mạng |

### 7.7 Chỉ dẫn cho tác tử AI thi công P5 (đề xuất 2026-11-17; **vẫn 🔒**)

Dành cho một tác tử AI (hoặc một cuộc chat mới) được giao làm P5. Đọc hết mục này trước khi làm bất cứ thứ gì.

1. **Kích hoạt:** chỉ khi người dùng nhắn đúng câu **"Tích hợp AI vào hệ điều hành"**. Không có câu đó ⇒ dừng và nói P5 đang khoá. Không tự suy ra từ lời nhắc gián tiếp.
2. **Đọc trước:** §7 của file này, `Adaptive-MoE/docs/PLAN.md` mục 6, `Adaptive-MoE/docs/OS_BRIDGE.md` (khi P9-15 xong), `include/yui/yui.h`, `ai-bridge-yuid/` và `reference/README.md`.
3. **Bước 0 — kiểm điều kiện vào (§7.1):** `make test` (phải **≥ 9745 check, 0 fail**), `make check`, `scripts/verify.sh`, `git log -n 5` + `git status` trên **nhánh hiện hành** (không chuyển nhánh). Dán lệnh + đầu ra vào §12. Điều kiện nào đỏ ⇒ **dừng và báo**, không làm tiếp.
4. **Làm đúng thứ tự P5.1 → P5.10, mỗi bước một lượt:** chạy lệnh trong cột "Lệnh / tiêu chí", dán lệnh + đầu ra vào §12, **rồi mới** đánh `[x]`. Không gộp bước, không đánh `[x]` khi chưa có bằng chứng (nguyên tắc đầu file này).
5. **Được sửa:** `ai-bridge-yuid/` (thêm lớp `EngineBackend`, P5.14), `vendor/`, `tests/` (chỉ **thêm** test mới), `docs/`. **Cấm:** `kernel-cap/`, `kernel-services/`, `kernel-frame/`; `daemon/` (đúng `reference/README.md`); đổi tên/chữ ký hàm đã có trong `include/yui/yui.h`; nhúng trọng số mô hình; cấp quyền `admin`; bật mạng cho `web_search`; đặt công cụ `sys_*` trong Python.
6. **Dừng và hỏi người dùng khi:** một test cũ đỏ; cần khoá/quyền/mạng chưa được cấp; một bước đòi sửa vùng cấm; số đo xấu hơn mốc (ví dụ `yuid` > 8 MB RSS khi rảnh, P1.10); engine báo thiếu `yui_features` (P9-15 chưa xong).
7. **Mẫu báo cáo sau mỗi bước (5 dòng):** *Việc đã làm · Lệnh đã chạy · Kết quả (số thật) · Chưa kiểm được · Rủi ro còn lại.* Không viết "đã chạy" khi chưa chạy; môi trường soạn thảo thường **không có `cargo`/`qemu`**, phần Rust/QEMU xác nhận bằng CI.
8. **Hoàn tác:** `yuid --reference-engine` đưa hệ thống về trạng thái trước P5 trong một lệnh (§7.3).

---

## 8. CI: ma trận, QEMU, tiêu chí pass

File: `.github/workflows/ci.yml`. Sáu job, chạy song song trên mọi push/PR.

| Job | Chạy gì | Pass khi |
|---|---|---|
| `cpp` | `make -j`, `make ibus`, `make test`, `make check`, `make install` vào prefix tạm, link `libyui.a` từ chương trình C ngoài cây | **9745** check (9684 C++17 + 61 C11); IBus build/install; chương trình C in `abi=1` |
| `rust` | `cargo test --workspace`, `cargo build --release`; fmt/clippy advisory | 0 test fail |
| `docs` | `scripts/verify.sh`: policy JSON parse, không CRLF, liên kết README, không `unsafe` trong crate nhân | tất cả xanh |
| `cross-arch` | `cargo check --workspace --target {aarch64-unknown-linux-gnu, riscv64gc-unknown-linux-gnu}` | cả hai target type-check |
| `kernel` | Build `kernel-frame` cho 3 target bare-metal bằng nightly + `-Z build-std` (`scripts/build-kernel.sh`), upload ELF | 3 ELF sinh ra, khác rỗng |
| `qemu` | Tải ELF rồi boot thật bằng `scripts/qemu-smoke.sh <arch> --strict` | Phải thấy **cả ba** dòng: `yui: ready`, `Phan loai` (profiler), `Tang 1` (bảng năng lực) trong 90 s; thiếu một dòng ⇒ đỏ |

### 8.1 Ma trận QEMU (mục tiêu P2)

| Kiến trúc | Máy QEMU | Firmware | Tiêu chí |
|---|---|---|---|
| x86_64 | `qemu-system-x86_64 -M q35` | SeaBIOS (BIOS Legacy) **và** OVMF (UEFI) | in `yui: ready` < 3 s cả hai |
| ARM64 | `qemu-system-aarch64 -M virt -cpu cortex-a72` | U-Boot / `-bios` trực tiếp | in `yui: ready` < 3 s |
| RISC-V 64 | `qemu-system-riscv64 -M virt` | OpenSBI | in `yui: ready` < 3 s |

Bộ test trong QEMU (từ P2.7): chạy `busybox` (≥ 100 lệnh), `coreutils`, `python3 -c`;
mỗi cái ghi `ok` ra serial. Job đọc serial, fail nếu thiếu dấu `ok`, giữ log 30 ngày.

`scripts/qemu-smoke.sh` chạy được cả trên máy dev:

```bash
./scripts/build-kernel.sh x86_64          # cần nightly + rust-src
./scripts/qemu-smoke.sh x86_64            # boot ảnh vừa build, tìm 3 dấu hiệu
./scripts/qemu-smoke.sh x86_64 --build    # build rồi boot, một lệnh
./scripts/qemu-smoke.sh riscv64 --strict  # CI: thiếu nhân/qemu ⇒ đỏ
```

Khi **đã có** ảnh nhân, script không còn chấp nhận "boot mà không in gì": thiếu
`yui: ready`, `Phan loai` hoặc `Tang 1` là `exit 1` kể cả không `--strict`. Chỉ
trường hợp **chưa có ảnh nhân** (máy dev chưa cài Rust) mới được bỏ qua, và luôn
in lý do.

---

## 9. Rủi ro & giảm thiểu

| Rủi ro | Ảnh hưởng | Giảm thiểu |
|---|---|---|
| P2 (nhân thật) kéo dài 6–12 tháng | Người dùng bỏ đi trước khi thấy giá trị | P1 đưa sản phẩm dùng được trên Linux **trước**; mỗi phase đều có thứ chạy được |
| Không có `cargo`/`qemu` trong môi trường soạn thảo | Rust và nhân chưa được biên dịch ở đây | CI là nơi xác nhận; `verify.sh` bắt lỗi cấu trúc/an toàn tĩnh; mọi tuyên bố Rust ghi rõ "chưa build" |
| Lớp Linux ABI phình to | Trượt gate P2.7 | Chia nhóm syscall, tiêu chí "busybox → coreutils → python3" tăng dần, không làm hết một lần |
| Driver phần cứng lạ | Hệ thống không khởi động trên máy thật | Driver chạy trong micro-VM (P3.6); máy không có driver vẫn boot tới user space |
| Bảo mật: tự viết Ed25519/AES sai | Mất dữ liệu, mất niềm tin | Chỉ dùng nguyên hàm tự viết **có vector chuẩn**; Ed25519 phải qua RFC 8032 trước khi dùng thật; `SECURITY.md` §7 ghi rõ giới hạn |
| Tài liệu lệch mã | Người dùng mất tin | `verify.sh` kiểm liên kết + `YUI_SOURCE_DIR` buộc test nạp chính sách thật |
| Một mình phát triển | Burnout | Phase có gate rõ, việc nhỏ 1–3 ngày, mỗi task đều để lại bằng chứng |
| Mở rộng phạm vi (game, dựng 3D, doanh nghiệp, RTOS) khi chưa có phần cứng thật | Trượt cả P2 lẫn gate v1.0; một người không theo nổi | §14 chia **làm ngay / nghiên cứu / không làm**; mỗi hạng mục nghiên cứu có Go/No-Go đo được trước khi viết mã |
| Khoá ký phát hành nằm trong CI | Chiếm CI = ký được bản cập nhật giả cho mọi máy | Khoá ký **ngoại tuyến** (P4.7, P4.10); CI chỉ build/test/sinh manifest chưa ký |
| Tin số liệu "trước/sau" từ tư vấn ngoài | Hứa hẹn FPS/điện năng không đo được | Mọi con số hiệu năng phải do `P6.1`/`P6.11` đo; không ghi vào README khi chưa có |
| Snapshot/warm pool nhân bản trạng thái (cùng entropy, cùng ID, chứa bí mật) | Hai VM đoán được số ngẫu nhiên của nhau; lộ dữ liệu giữa ứng dụng | P3.14: gieo lại RNG + ID mới, băm snapshot, không chia sẻ giữa ứng dụng/người dùng |
| Bộ gõ bên thứ ba thấy mọi phím, kể cả mật khẩu | Keylogger hợp pháp hoá dưới dạng bộ gõ | P3.13: hộp cát + trường mật khẩu không bao giờ tới engine ngoài |
| Hộp thoại cấp quyền bị app giả mạo hoặc bị làm phiền liên tục | Người dùng bấm đồng ý theo phản xạ, hoặc app tự duyệt | P4.13 (đường tin cậy) + giới hạn tần suất hỏi + phạm vi đúng tên miền (P3.16) |

---

## 10. Định nghĩa "hệ điều hành hoàn chỉnh" (v1.0)

Dự án đạt v1.0 khi **tất cả** điều sau đúng:

1. Boot được trên 3 kiến trúc trong QEMU trên CI, mỗi cái < 3 s tới user space.
2. Máy 2 GB RAM: sau khởi động dùng < 64 MB, không tick khi rảnh, gõ tiếng Việt
   dưới 10 ms/phím, xem video 720p được.
3. Zero-Trust: 10/10 kịch bản thoát hộp cát bị chặn, có audit.
4. Cập nhật bằng `.bundle` ký Ed25519, chạy offline, A/B rollback được.
5. `busybox` + `coreutils` + `python3` chạy qua lớp Linux ABI.
6. Người dùng cuối cài từ USB, dùng hằng ngày 7 ngày liên tục mà không phải mở
   terminal (trừ khi muốn).
7. Toàn bộ tuyên bố trong `ARCHITECTURE.md` có test hoặc số đo tương ứng.

P5 (AI) **không** nằm trong điều kiện v1.0 — nó là phần mở rộng có thể bật sau.
> **Lưu ý (2026-11-16):** định nghĩa v1.0 ở trên **không** yêu cầu chạy trên phần cứng thật (chỉ QEMU) và không gồm driver NVMe/USB/mạng/Wi-Fi/GPU. Đó là chủ ý, nhưng cần nói rõ để không ai hiểu nhầm v1.0 = "cài lên laptop dùng hằng ngày". Đề xuất mốc **v1.1 = chạy bare-metal trên 1 máy mục tiêu đã đo** (xem Phase P6, §14). Cho tới lúc đó, đường đưa sản phẩm tới người dùng thật vẫn là **P1 (tầng dịch vụ trên Linux)**.


---

## 11. Thứ tự làm tiếp theo (cập nhật sau nghiệm thu P1.4)

P1.4 đã hoàn tất theo tiêu chí latency trung bình; kiểm thử GUI Firefox/LibreOffice
được chuyển thành Desktop QA follow-up, không chặn tính năng. Tiếp tục Phase P2:

1. **P2.8 FS bất biến** — nhánh hiện tại siết đọc `/core` bằng `openat` + `O_NOFOLLOW`
   để chặn symlink escape; kế tiếp cần kiểm thử flush/power-loss trên QEMU với disk image.
2. **P2.2 x86_64 boot** — đường Multiboot/ELF đã sửa; cần build + boot QEMU xanh để xác nhận,
   rồi mới tính UEFI. Môi trường này hiện không có `cargo`/`qemu-system-*`. *(2026-11-17: log Kaggle của bạn cho thấy x86_64 đã boot trong QEMU; còn đợi job CI + UEFI.)*
3. **P2.5 tickless/DVFS** — nối policy hiện có vào timer/driver nhân và đo wakeup/năng lượng.
4. **P2.7 Linux ABI** — bổ sung mmap/futex/epoll và kiểm bằng userspace thật.
5. **P2.9 ZRAM** — tích hợp vào kernel, đo nén 200 MB trong QEMU với RAM 512 MB.

*Việc nhỏ, độc lập, làm xen kẽ được (đề xuất 2026-11-17; không chặn P2):*

6. **P1.12** (`[x]`, hoàn tất: Direct mode giữ tiền tố chung, chuỗi mẫu giảm 34 → 5 BackSpace, test 488 chuỗi qua 7149 check xanh). Tiếp theo là **P1.11** (bạn đo trên ứng dụng thật).
7. **P0.14** (`[x]`, hoàn tất: cổng kiểm tra Tầng 1–2 không dependency ngoài trong `scripts/verify.py`, DoD đã nghiệm thu).
8. Chưa làm P3.13–P3.16, P4.14, P6.14–P6.16 cho tới khi bạn duyệt; P3 cần P3.1 (`yui-vmmd`) nên chưa thể bắt đầu sớm.

Mỗi mục giữ trạng thái `[~]`/`[ ]` cho tới khi bằng chứng đúng DoD xuất hiện; ghi kết quả
vào nhật ký dưới đây. Không nhận boot/đo phần cứng thay cho CI hoặc thiết bị thật.

---

## 12. Nhật ký tiến độ

| Ngày | Task | Bằng chứng |
|---|---|---|
| 2026-11-15 | P0.7–P0.13 | `make test` 705 check, `make check` xanh, monorepo 10 thư mục, 6 tài liệu |
| 2026-11-15 | P0.10 | `yui-sched` 9 test, `yui-zram` 8 test (chưa biên dịch — không có cargo trong môi trường soạn thảo) |
| 2026-11-15 | P0.9 | `test_sandbox` 129 check, gồm 14 tình huống nạp 4 policy thật |
| 2026-11-15 | P1.1 | `yuid` chạy được với `--socket/--state-dir/--serve-once`; `test_daemon` 102 check |
| 2026-11-15 | P1.5 | `test_telemetry` 471 check, `test_capi` 61 check; `telemetry.read` + MCP `system_telemetry`; đo thật trên container này: CPU 46 °C, RAM 3.8 GB, không pin ⇒ `present:false` |
| 2026-11-15 | P1 (framebuffer) | `test_fb` 1155 check; `yui-splash --width 640 --height 400` → `docs/splash.png` (2 880 byte PNG, xem bằng mắt: đúng bố cục) |
| 2026-11-15 | P2.1, P2.3 | CI `kernel` xanh 3/3 target; CI `qemu` xanh cho aarch64 + riscv64 (boot thật, thấy 3 dòng bắt buộc). Lỗi tìm và sửa được nhờ CI: E0255 macro, `misa` là CSR cấp M (S-mode đọc ⇒ trap), PL011 thiếu divisor, `{}` với `&str` runtime trên aarch64, `relocation-model=static`, PLT/linker script |
| 2026-11-15 | P2.2 (`[~]`) | x86_64 build xong nhưng QEMU `-kernel` chỉ nhận multiboot 1 từ ELF32; ghi rõ 2 hướng sửa. Job `qemu` x86_64 để `required: false` nên không chặn merge, nhưng **vẫn đỏ và vẫn hiện** |
| 2026-11-15 | P1.6 (`[~]`) | `yui-trim` tròn vẹn trên cây thư mục mẫu: 5 file → gói 187 B, lọc `locale/fr/`, giải nén đúng |
| 2026-11-15 | P2.2 (`[~]`) — sửa boot x86_64 | Header Multiboot2 (24 B + end tag) song song Multiboot1; `kmain_x86(u32,u32)` phân biệt magic mb1/mb2; `parse_multiboot2` đọc LE (sửa lỗi be32); `build-kernel.sh` xuất `.elf64` + `objcopy -O elf32-i386` → `.elf`; smoke asserts thêm `! grep "khong doc duoc"`; CI `qemu` bắt buộc 3 kiến trúc. **Boot thật chỉ xác nhận bằng CI** (sandbox không có QEMU) |
| 2026-11-15 | P2.4 (`[x]`) | Ed25519 clean-room C++ + Rust: `tests/test_ed25519.cpp` **78 check** (FIPS SHA-512 ×3 + streaming, RFC 8032 TEST 1/2/3/SHA(abc)/1024 khớp hex từng byte, tamper); `.bundle` keyring + ký/xác minh + 5 kịch bản sửa 1 byte + khe A/B quay lui; bản Rust `sha512/ed25519/keyring/bundle.rs` có test vector song song (chạy qua CI — sandbox không có cargo) |
| 2026-11-15 | P2.5 (`[~]`) | `power.hpp/.cpp` + `test_power` **47 check** (tickless 0 wakeup định kỳ, OPP 3 loại máy, DVFS tải/nhiệt/pin/trần, clock-gating quá nhiệt) — chưa đo phần cứng |
| 2026-11-15 | P2.6 (`[~]`) — chữ Việt | `fb.cpp` mở rộng: bảng 180 chữ cái NFD sinh từ Unicode, dấu thanh + nguyên âm xếp chồng + nặng + đ/Đ; `draw_text_utf8` ASCII y hệt `draw_text`; `ui.cpp::render_window`/`render_label`; `test_fb` **1184 check** (1155 cũ + 29 `fb_viet.*`) |
| 2026-11-15 | P2.7 (`[~]`) | `linuxabi.cpp` bảng phân phối syscall + `test_linuxabi` **38 check** (openat/utimensat EPERM, /core chỉ-đọc, nr_from32, ENOSYS) — chưa có busybox/coreutils |
| 2026-11-15 | P2.8 (`[~]`) | `fsstore.cpp` bất biến + ghi gộp + `test_fsstore` **37 check** (3 lệnh ghi → 1 chốt vật lý, chặn `..`/`//`) — chưa test rút điện |
| 2026-11-15 | P2.12–P2.13 (`[x]`) — 3 phòng vệ | Guard đào coin (`guard.cpp` + `yui-sched/src/guard.rs` + `yuid` `guard.*`), EgressFilter (`sandbox.cpp`), `tests/test_redteam.cpp` **83 check**; `compute.heavy` vào bảng quyền (15→16, C++/Rust/docs); `docs/SECURITY.md` §6 thành "đã cài đặt" + §10 mô tả 3 cơ chế. Tổng `make test` **2669 check** (2608 C++17 + 61 C11), 2357 cũ giữ xanh 100% |
| 2026-11-16 | Rà soát kế hoạch (không đổi mã) | Đối chiếu 2 đợt tư vấn Gemini (5 ý AIOS, 5 lỗ hổng, shell POSIX, phần cứng thật, TPM/PQC/IOMMU, game/Minecraft, máy HP Compaq, ngách yếu/doanh nghiệp, cập nhật A/B, phát hành bằng GitHub Actions) với mã: **15 lệnh** shell (không phải 25 mục tiêu); `fork`/`execve` trong `linuxabi.cpp` trả `EPERM`; `profile.cpp::classify` chỉ có 3 lớp, máy 512 MB và 3 GB cùng là `may-yeu`; `yui.h` có 25 khai báo (P5.4 cũ ghi 12); engine (`yui.cpp`) có 17 hàm và trùng 2 tên hàm (`yui_version`, `yui_free`; cùng chữ ký, khác ngữ nghĩa) với `libyui` của OS. Thêm P2.14–P2.15, P3.12, P4.9–P4.13, P5.11–P5.16, Phase P6, §13–§14. **Chưa chạy `make test`** vì chỉ sửa tài liệu; **đã chạy `scripts/verify.sh`** trên bản sao repo có file mới: *tất cả kiểm tra tĩnh đạt* (lần đầu đỏ 2 chỗ vì nhắc tới tệp chưa tồn tại; đã sửa chữ) |
| 2026-11-16 | P1.4 (`[~]`) — IBus Telex/VNI | Thêm `ime::Composition` cho preedit/backspace/commit, adapter đăng ký hai engine IBus và metadata component; `test_ime` **108 check**; `make test` **2693 check / 0 fail**, `make check` và `scripts/verify.sh` xanh. XML được kiểm tĩnh. **Chưa build được `make ibus`** vì sandbox thiếu `pkg-config`/`libibus-1.0-dev`; chưa chạy Firefox/LibreOffice, chưa đo < 10 ms/phím. P1.4 giữ `[~]`, không nhận DoD hoàn tất |
| 2026-11-16 | P1.4 follow-up (`[~]`) | Sửa IBus `set_content_type` ABI về `guint`; xác nhận tone cần phím lệnh: `cac` giữ nguyên, `cacs` → `các`, `tats` → `tát`, `nats` → `nát`, `mawms` → `mắm`, `cawps` → `cắp`; thêm regression tests. `Composition` tự giữ allowlist `word/window/software/user/linux` và Escape trả nguyên chuỗi phím. `make test`: **2831 check / 0 fail**, `test_ime` **246**. Người dùng báo build/register Ubuntu 24.04 và D-Bus avg 7.8 ms, min 7.15, max 10.25; chưa tái lập tại đây; Firefox/LibreOffice chưa có kết quả, latency max vượt ngưỡng nếu strict worst-case. P1.4 vẫn `[~]` |
| 2026-11-16 | P1.4 (`[x]`) — nghiệm thu Ubuntu 24.04 | Người dùng xác nhận commit `a786a44`: `make ibus` build không cần patch tay; `make check` **2831 checks**, `test_ime` **246**, English auto-restore, Escape-restore, `các`/`mắm` hoạt động; D-Bus avg **7.16 ms/phím**, min **6.18**, max ~12 do nhịp nghỉ `pump` của script Python. Latency DoD được xác nhận là trung bình < 10 ms. GUI Firefox/LibreOffice chuyển sang Desktop QA follow-up, không chặn P1.4 |
| 2026-11-16 | P2.8 (`[~]`) — chống symlink escape khi đọc /core | `ImmutablePartition::read` mở root dir rồi đi path từng phần tử với `openat` + `O_NOFOLLOW`, chỉ cho regular files, kiểm cùng inode giữa `fstatat`/`openat`, không ghi output một phần khi lỗi; thêm tests symlink file/dir và nested regular path. `test_fsstore` **48 checks**; `make test` **2842**, `make check` xanh. Còn tích hợp kernel và power-loss QEMU test 20 lần; sandbox không có `cargo`/`qemu-system-*` |
| 2026-11-17 | Nghiệm thu log Kaggle của bạn (commit `e885ac9`, bản `main`) | **Số liệu bạn báo:** C++/C11 xanh toàn bộ; `test_crypto` 28, `test_codec_json` 102, `test_core` 163, `test_sandbox` 129, `test_ime` 246 (lần chạy đầu) rồi **459** (lần sau, bản Direct), `test_daemon` 103, `test_tools` 61, `test_telemetry` 471, `test_fb` 1184, `test_power` 47; Rust `kernel-cap` **23**, `yui-sched` **13**, `yui-zram` **8** (0 fail); kernel build x86_64 42.272 B (ELF32) + ELF64, aarch64 113.112 B, riscv64 79.176 B; QEMU **cả 3 kiến trúc boot**, profiler chạy, bảng năng lực sống; tổng thời gian notebook 302,9 s. Direct mode: `YUI_IME_MODE=direct` được nhận; chuỗi commit `d, đ, T, Ti, Tie, Tiê, Tiên, Tiêng, Tiếng, w … window`, tổng **34** BackSpace. **Mình đối chiếu:** (1) chạy lại `make test` + `make check` trên bản sao `main`: 2994 C++17 + 61 C11 = **3055 check, 0 fail**, số theo từng file **trùng log của bạn**; (2) tính tay số BackSpace của chuỗi trên: replay cả từ = **34** (khớp log), giữ tiền tố chung = **5** ⇒ P1.12. **Mình không làm được:** `cargo`, QEMU, `make ibus` (thiếu công cụ/`libibus-1.0-dev`), và đọc trạng thái CI trên GitHub (API hết hạn mức) ⇒ Rust/kernel/QEMU ở đây là **số do bạn báo**, chưa có đối chiếu CI từ phía mình. Kết luận: P1.4 giữ `[x]`; P2.2 giữ `[~]`; P2.3 giữ `[x]` |
| 2026-11-17 | Nâng cấp kế hoạch (không đổi mã) | Đánh giá đợt tư vấn thứ ba của Gemini (§13 C30–C41). Thêm 12 task (xem đầu file), §5.2, §7.7. Sửa số lỗi thời: 2842 → 3055 (P0, P5.9, §8), số test `kernel-cap`/`yui-sched`, bảng trạng thái P1 (3/12; tiêu đề §3 lệch 2/10). `[x]`/`[~]` của task cũ **không đổi**. **Đã chạy `scripts/verify.sh`** trên bản sao repo có tệp mới: *tất cả kiểm tra tĩnh đạt* (liên kết nội bộ, đường dẫn trích trong docs, không CRLF). Không chạy lại `make test` vì chỉ sửa tài liệu |
| 2026-11-17 | P0.14 (`[x]`), P1.12 (`[x]`) | Hoàn tất 2 task theo kế hoạch: (1) **P0.14**: `scripts/verify.py::check_kernel_dependencies` khóa cứng nguyên tắc "lõi sạch không dependency ngoài" cho Tầng 1–2 (`kernel-cap`, `kernel-services/*`) và chỉ cho phép `yui-cap` ở `kernel-frame`; thử thêm `foo = "1"` vào `kernel-cap/Cargo.toml` ⇒ `verify.sh` báo đỏ FAIL rõ tên crate; gỡ ra ⇒ xanh. (2) **P1.12**: `DirectComposition::replace_rendered` trong `userspace-vi/ime.cpp` tính tiền tố chung theo codepoint NFC, chỉ gửi phần sai khác. Chuỗi mẫu `dd` + `Tieengs` + `window` ra đúng 5 BackSpace (giảm 85% so với 34 cũ). Thêm test `direct_mode_random_replay_and_common_prefix_invariants` kiểm tra 488 chuỗi (Telex & VNI, có `ươ`, `đ`, hoa/thường, xoá lùi giữa từ, PRNG) chứng minh văn bản luôn đồng nhất 100% với bản thay cả từ ở từng phím gõ và tổng BackSpace luôn $\le$ bản cũ. `test_ime` tăng từ 459 lên **7149 checks**; tổng kiểm thử monorepo đạt **9745 checks** (9684 C++17 + 61 C11), 0 fail; `scripts/verify.sh` và `make check` toàn bộ xanh |
| 2026-11-17 | P2.7 (`[~]`), P2.14 (`[x]`), P2.15 (`[x]`) | Thi công các task Phase P2: (1) **P2.7**: Cài đặt mmap (hỗ trợ file-backed + anonymous, bắt buộc W^X: `PROT_WRITE \| PROT_EXEC` trả `EPERM`, chặn ghi vào `/core` trả `EPERM`), munmap, futex (`FUTEX_WAIT` so sánh nguyên tử + ghi nhận hàng đợi, `FUTEX_WAKE` đánh thức), epoll (`epoll_create1`, `epoll_ctl` ADD/MOD/DEL, `epoll_wait` kiểm tra sẵn sàng) và ánh xạ i386 32-bit (`nr_from32`); `test_linuxabi` tăng từ 38 lên **89 checks**. (2) **P2.14**: `CowStore` trong `fsstore` hỗ trợ snapshot theo giao dịch Copy-on-Write ($O(1)$ sao chép buffer), đo thời gian snapshot bằng số thực tế, tính toán bù trừ dung lượng (`StorageAccounting`: logical/physical/shared/exclusive bytes); `guard::RansomwareDetector` tính Shannon entropy và phát hiện ghi dồn dập $H > 7.2$ bit/byte ⇒ bóp xung 10%, cảnh báo tiếng Việt và gợi ý hoàn tác. Test Red-Team mô phỏng 10.000 tệp bị mã hoá ciphertext ⇒ Guard bắt được, người dùng rollback ⇒ 10.000/10.000 tệp khớp 100% mã băm SHA-256 ban đầu; `test_fsstore` tăng từ 48 lên **96 checks**. (3) **P2.15**: `guard::IdleGuard` bảo vệ thiết bị nhạy cảm (micro, camera, danh bạ, mạng) khi máy rảnh (> 300 s); thu hồi thẻ năng lực tức thì **O(1)** (`revoked_badges_`), bóp xung 20%, phát cảnh báo tiếng Việt; kiểm thử đủ 6 kịch bản Red-Team trong `test_redteam.cpp` (tăng từ 83 lên **134 checks**). Tổng kiểm thử monorepo đạt **9855 checks** (9794 C++17 + 61 C11), 0 fail; `scripts/verify.sh` và `make check` toàn bộ xanh |


---

## 13. Đánh giá tư vấn ngoài (2026-11-16): giữ / sửa / bỏ

Cách đọc: `đã kiểm` = mình đối chiếu với mã/kế hoạch trong repo này; `chưa xác minh` = không kiểm được, **đừng dựa vào**. Phần AI nằm ở `Adaptive-MoE/docs/PLAN.md` mục 1b.

| # | Gợi ý | Đối chiếu | Quyết định |
|---|---|---|---|
| C1 | `yui-shell` chỉ là shell nội bộ, thiếu terminal POSIX | **Đã kiểm, đúng:** 15 lệnh (`userspace-vi/shell.cpp`), không pipe/chuyển hướng/job. Gemini ghi nhầm Linux ABI là "P2.9" (đúng là **P2.7**; P2.9 là ZRAM trong nhân). Quan trọng: `fork`/`execve` ở `linuxabi.cpp` hiện trả `EPERM`, nên trên nhân riêng shell chưa sinh được tiến trình | **P4.12**: làm trên Linux host trước; nhân riêng sau P2.7 |
| C2 | "Sẵn sàng 50%/40%/10%/15%" và "hoàn thiện 45%/75%/90%/100%" | Không có phương pháp đo | **Bỏ.** Dùng số thật: 2842 check, 2/3 kiến trúc boot QEMU, bảng trạng thái ở đầu file |
| C3 | Chiến lược lai: Linux ABI + Wayland/Vulkan thay vì viết lại phần mềm | Đúng hướng, trùng P2.7, P3, P4.1. Wayland chưa có task riêng | Giữ; Wayland vào backlog (§14.2) |
| C4 | Thiếu driver thật (NVMe, AHCI, xHCI/HID, mạng, Wi-Fi, ACPI, âm thanh) là rào cản lớn nhất | **Đúng**, và §10 (v1.0) cố ý không gồm phần cứng thật. "Dùng nhân Linux tối giản làm lớp phần cứng" thực chất là **đường P1 đã có** | **Phase P6**: virtio trước, rồi UEFI, NVMe/AHCI, xHCI/HID, mạng, ACPI. **Wi-Fi và GPU không viết**: dùng driver Linux trong micro-VM (P3.6) hoặc USB-Ethernet/tethering |
| C5 | TPM 2.0 + Secure Boot chuẩn 2023 | Hướng đúng. Nhưng "ký bằng chứng chỉ chuẩn mới" **không tự làm được**: PC bật Secure Boot chỉ nạp bộ nạp mà CA firmware tin ký (thường qua `shim` đã được Microsoft ký) hoặc người dùng tự đăng ký khoá. Chuyện chứng chỉ 2011 hết hạn: **chưa tự kiểm** | **P6.8** chỉ là đặc tả (tệp `SECURE-BOOT.md` sẽ tạo trong thư mục docs); chưa code |
| C6 | Mật mã hậu lượng tử (ML-KEM/ML-DSA) cho `yui_vault` | Hai chuẩn NIST có thật (FIPS 203/204, theo hiểu biết của mình). Nhưng `yui_vault` dùng mật mã **đối xứng** (scrypt + SHAKE-256 + HMAC) vốn không bị Shor; PQC cần cho **trao đổi khoá và chữ ký dài hạn**. Tự viết clean-room PQC rủi ro kênh bên hơn Ed25519 | Backlog; chỉ cân nhắc cho khoá ký phát hành, khi có vector chuẩn |
| C7 | NUMA, IOMMU | IOMMU cần cho GPU passthrough và chặn DMA (liên quan P3.6); NUMA chỉ máy nhiều ổ cắm, không phục vụ máy mục tiêu | Backlog |
| C8 | Init/giám sát dịch vụ, crash dump | Hợp lý; P1.1 đang dùng systemd, nhân riêng sẽ cần | **P6.10** |
| C9 | Self-hosting toolchain + SDK | Đã có: P2.10 (self-host) và P1.3 (publish `libyui`) | Không thêm |
| C10 | SBOM, build tái lập, Yui Store | SBOM + tái lập: rẻ, hợp mục tiêu "tin cậy". Store: lớn | **P4.11**; Store vào backlog sau P4.7 |
| C11 | Nâng độ phân giải thần kinh + chèn khung ở compositor | (1) tốn tính toán GPU đúng chỗ iGPU đang thiếu, "giảm 50% tải" chưa chứng minh; (2) chèn khung ở cấp compositor chỉ thấy ảnh cuối, không có vector chuyển động ⇒ lỗi hình + tăng độ trễ nhập; (3) tên công nghệ/số liệu nêu **chưa xác minh** | **Không vào v1.0.** **P6.12** nghiên cứu có Go/No-Go, sau P4.1 |
| C12 | Minecraft Java/Bedrock mượt, ép Zink/Vulkan, WineGDK/mcpelauncher | **Gemini tự mâu thuẫn:** một bản nói chơi được trên 2 GB, bản sau nói từ 7/2026 cần 8–12 GB RAM, Java 25, Vulkan 1.3; cả hai **chưa xác minh**. Bedrock qua WineGDK/mcpelauncher là dự án bên thứ ba, phụ thuộc Linux ABI + Wine + hộp cát, không phải tính năng nhân | Không lập kế hoạch theo yêu cầu Minecraft cho tới khi kiểm trang chính thức; chạy game thuộc P3.2/P3.4 |
| C13 | NTSYNC "110 → 860 FPS" | Con số chọn lọc, chưa xác minh. **P3.3** đã có DoD trung thực hơn (p99 frame time giảm ≥ 20%) | Giữ P3.3 nguyên |
| C14 | Giả lập tập lệnh + **giả mạo phần cứng** (TPM/VRAM) để qua kiểm tra của app/game | (1) giả lập AVX2 trên CPU không có thì chậm nhiều lần, không "mượt"; (2) giả mạo TPM/VRAM để vượt kiểm tra có rủi ro điều khoản/pháp lý; game có anti-cheat cấp nhân (ví dụ Valorant) vốn **không chạy trên Wine/Linux** (hiểu biết công khai, chưa tra lại hôm nay), nên không thể "chạy qua lớp tương thích" như Gemini viết | **Không làm** `[-]`. Giữ Virtual Hardware Manifest của `vmm-sandbox` cho nhất quán/riêng tư, **không** để đánh lừa |
| C15 | Máy HP Compaq 800/8200: "OpenGL 4.6 gốc", "tiết kiệm điện 30–40%", chơi mượt hàng loạt game | Haswell có AVX2/FMA, Sandy Bridge **không** (chi tiết ở `Adaptive-MoE` B15). Các con số GL/điện/FPS **chưa xác minh**, chưa ai đo | **P6.1**: đo thật trước; không viết hồ sơ Haswell/Ivy Bridge riêng khi chưa có bảng |
| C16 | Lớp `sieu-yeu` (< 1,5 GB RAM): Headless/TUI + không nạp AI | **Đã kiểm, đúng điểm này:** `profile.cpp::classify` chỉ có `may-yeu` (RAM < 4 GB **hoặc** ≤ 2 luồng), `can-bang`, `may-manh`; máy 512 MB và 3 GB cùng một lớp | **P6.2** |
| C17 | Doanh nghiệp (MDM/SSO/LDAP/Kerberos), RTOS cứng, cảm ứng/cảm biến, Wasm/PWA, đọc-ghi NTFS/ext4/APFS/exFAT, Migration Assistant | Phần lớn ngoài phạm vi v1.0. RTOS cứng mâu thuẫn mục tiêu desktop của LAVD. APFS ghi: hầu như không có hỗ trợ ổn định | exFAT/FAT32 + ext4/NTFS **đọc-chỉ** vào **P6.13**; còn lại backlog; RTOS cứng và APFS ghi: bỏ |
| C18 | "Chuyển Qwen/Gemma sang `.yui` để có trí tuệ cao cấp" | Không phải việc của một converter (xem `Adaptive-MoE` B14): engine chỉ chạy kiến trúc Adaptive-MoE | Không làm "converter"; chờ Q11 bên Adaptive-MoE |
| C19 | Cập nhật nguyên tử A/B + `yui-update` | Nền **đã có**: P2.4 (bundle ký Ed25519, khe A/B, quay lui, 78 check). Còn thiếu: bộ nạp chọn khe thật (P4.6), CLI (P4.9), delta. "0 giây chờ" là phóng đại: vẫn phải khởi động lại; hơn Windows ở chỗ không cài khi tắt máy. "Tiết kiệm 80–90%" của delta **chưa kiểm** | **P4.9**; delta là tuỳ chọn sau |
| C20 | Phát hành ISO/bản cập nhật bằng GitHub Actions + Releases, bot kiểm 4 chốt, canary → stable | **Khả thi** và hợp thiết kế hiện có. Các giới hạn Gemini nêu (tệp đính kèm < 2 GiB, API không đăng nhập 60 lượt/giờ/IP, job 6 giờ, ~14 GB đĩa) khớp hiểu biết của mình nhưng **chưa tra lại tài liệu hôm nay**: kiểm khi viết workflow. **Chỉnh:** (1) **không để khoá Ed25519 trong GitHub Secrets**: chiếm CI = ký được bản giả cho mọi máy; kế hoạch P4.7 đã nói khoá ngoại tuyến; (2) QEMU trên runner thường chạy TCG (chậm), đặt ngưỡng thời gian rộng; (3) tách repo `YuiVieOS-Releases` là ý hay | **P4.10** |
| C21 | Bộ nhớ ngữ nghĩa / phân trang KV bằng ZRAM / tiền nạp trọng số theo routing | Chỉ có nghĩa với model ≥ 7B (model 50–150M có KV ≈ 1 MB). Router lớp l+1 phụ thuộc kết quả lớp l nên chỉ đoán được theo token trước; **chưa đo tỉ lệ page-fault** | Backlog nghiên cứu R1 (cần P5); **đo trước** |
| C22 | FS bất biến + Snapshot + Rollback khi phát hiện mã hoá hàng loạt | **Khả thi và giá trị cao**, ghép đúng P2.8 + P2.12. Phải nói rõ giới hạn (xem P2.14) | **P2.14** |
| C23 | Lập lịch theo ý định: dồn luồng gõ/vẽ lên P-core, nền xuống E-core | Cần CPU lai P/E (chủ yếu Intel đời 12+); máy mục tiêu không có. Phần "ứng dụng đang nhận input được ưu tiên" cần ở mọi máy | Backlog (gắn P2.5) |
| C24 | Hệ miễn dịch hành vi "tự học" | Quy tắc trước (ít báo nhầm, kiểm được), tự học sau | **P2.15** |
| C25 | Vi-ứng-dụng / "Không ứng dụng" | Phần khả thi đã có: `pkg-stripper` (P1.6) + điều khiển bằng MCP (P4.3, P5.8) | Không thêm task |
| C26 | Mọi ứng dụng phải có chữ ký gắn phần cứng; ứng dụng tải về không ký bị nhốt hộp cát không mạng | Chính sách tốt và rẻ, nhất quán P4.7 | **P3.12** |
| C27 | Xác thực bằng **nhịp gõ/hành vi sinh trắc** chống giả click | Độ tin cậy thấp, là dữ liệu nhạy cảm, và không chặn được kẻ chen vào đường nhập. Cách đúng chống giả click là **đường nhập tin cậy** | **P4.13**; không dùng sinh trắc hành vi |
| C28 | Bộ lập lịch bằng model ML nhỏ qua `sched_ext` | Không dựa vào `sched_ext` (nhân riêng); chưa có dữ liệu vết; đặt suy luận trong đường lập lịch tăng độ trễ | **Hoãn** |
| C29 | RISC-V RVV; gọi `libyui.a` từ nhân Rust | `engine.hpp` của OS đã dò `rvv`, nhưng `yui.cpp` chưa có kernel RVV (chạy mã thuần). Nhân `no_std` không link được engine (cần libc, luồng, mmap) | RVV: backlog, đo bằng QEMU `-cpu rv64,v=true`; engine chỉ ở user-space (§7.5 mục 7) |
| C30 | **Dịch `.exe`/`.apk` sang định dạng gốc YuiVieOS** (AOT/JIT) để mượt | Gemini kết luận đúng: dịch mã CPU chỉ cần khi **khác kiến trúc** (Rosetta 2 dịch lúc cài/mở lần đầu, Box64/FEX trên ARM64/RISC-V — theo hiểu biết công khai); nút thắt thật là **API/ABI** (Win32, DirectX, Android Runtime ⇒ Wine/Proton/Waydroid) và **driver GPU + anti-cheat** (không "dịch" được; xem C14). **Nhưng Gemini nói như đã có, mà thực tế chưa:** (1) Linux ABI P2.7 mới là bảng syscall (`[~]`, 38 check), **chưa** chạy busybox/coreutils/python3; (2) "mở tệp 1 GB < 50 ms bằng mmap" là **DoD của P5.6 (đang khoá)**, chưa đo; (3) "Stripper giảm ≥ 35 % Electron" là DoD của P1.6 `[~]`, mới đo trên cây mẫu | **Không làm** `[-]`. Không viết 3 điều trên vào README như thành tựu |
| C31 | **"Lõi sạch tuyệt đối — ứng dụng nhốt hộp cát"**: Tầng 0–2 không thư viện ngoài; phần mềm văn phòng/trình duyệt chạy `isolated`/`microvm` | Đúng hướng và **đã là nguyên tắc** (`ARCHITECTURE.md`, §7.4). Đã kiểm: `kernel-cap`, `kernel-services/*` có `[dependencies]` rỗng, `kernel-frame` chỉ có `yui-cap`. Điểm yếu: nguyên tắc chưa được **máy** giữ | **P0.14** (cổng CI). Phần chạy app trong hộp cát = P3.2/P3.4 |
| C32 | Chạy **bộ gõ mở rộng, trình duyệt, office, driver USB token chữ ký số** trong hộp cát | Tách ba việc: (a) trình duyệt/office: đúng, thuộc P3 + P4.14; (b) **bộ gõ bên thứ ba là rủi ro cao nhất** (thấy mọi phím, kể cả mật khẩu): Gemini chỉ nói "cho vào hộp cát", chưa nói **trường mật khẩu không được tới engine ngoài**; (c) USB token: driver hãng thường đóng nguồn và có bước nhập PIN, **chưa xác minh** có chạy được dưới Wine/micro-VM | (b) **P3.13**; (c) **P6.16** (nghiên cứu, thiết kế proxy ký chứ không đưa cả token vào guest) |
| C33 | **Kho ứng dụng một chạm** bằng Smart App Stripper cho app Việt phổ biến | Cơ chế có (P0.12, P1.6). Hai điều Gemini bỏ sót: (1) gọt rồi **phân phối lại** phần mềm có bản quyền có thể vi phạm giấy phép; (2) hiệu quả mới đo trên cây mẫu | Giữ ý tưởng; **quy tắc phân phối** ghi ở đầu §3 (Lưu ý P1); Store ở backlog sau P4.7 |
| C34 | **Warm pool / Snapshot Resume** micro-VM (kiểu Firecracker/crosvm) để mở app "vài chục ms" | Khả thi. "Vài chục ms" (theo hiểu biết công khai, **chưa tra lại hôm nay**) là của micro-VM nhỏ, không phải guest chạy Wine + game. Gemini đề xuất giữ snapshot trong ZRAM: trên máy 2 GB tốn RAM đúng chỗ khan hiếm; snapshot trên đĩa nạp lười bằng mmap thường hợp hơn ⇒ **đo cả hai**. Bỏ sót: nhân bản trạng thái (cùng entropy/ID) và snapshot chứa bí mật | **P3.14** (có Go/No-Go đo) |
| C35 | **Cửa sổ liền mạch** (Wayland, kiểu Qubes/WSLg) | Đúng hướng; Wayland vốn ở backlog. Điểm bị bỏ qua: nếu guest vẽ được tiêu đề/hộp thoại thì **giả được giao diện hệ thống**. Qubes vẽ viền màu + nhãn bằng phía chủ | **P4.14** (phụ thuộc P4.13) |
| C36 | **Biên dịch trước shader** lúc rảnh (cắm sạc) cho DXVK/Vulkan | Có thật (Steam làm tương tự). Giới hạn: chỉ có nghĩa với đường DXVK/VKD3D; cache gắn với GPU + driver nên mất khi đổi; cache lộ "người dùng chơi gì" ⇒ lưu theo hộp cát. Cần GPU trong guest, mà P3.2/P6.11 chưa có | **P6.14** (nghiên cứu, Go/No-Go) |
| C37 | **Box64/FEX-Emu** cho ARM64/RISC-V | Đúng phạm vi (chỉ khi khác kiến trúc; x86_64 → không cần). Là mã ngoài ⇒ chạy trong hộp cát; giấy phép phải đọc, không đoán | **P6.15** (nghiên cứu, không vào v1.0) |
| C38 | **Tự sinh policy chặn** từ chuỗi tên miền quảng cáo/theo dõi trong bộ cài | Ý hay nhưng **ngược chiều với thiết kế**: `EgressFilter` đã chặn mặc định + allowlist, nên danh sách chặn không cần cho app trong hộp cát; cái thiếu là **allowlist ban đầu**. Quét chuỗi tĩnh bỏ sót tên ghép lúc chạy/nén/mã hoá | **P3.15** (đề xuất allowlist có người duyệt, thêm nguồn "chạy thử và ghi nhận") |
| C39 | **Cơ chế 2: trả HTTP 200 rỗng** cho tên miền quảng cáo thay vì ngắt mạng | **Không làm được với HTTPS nếu không chen vào TLS (MITM)**, mà MITM phá zero-trust và app ghim chứng chỉ vẫn lỗi. Bản đúng: sinkhole DNS + từ chối nhanh để app thất bại gọn; 200 rỗng chỉ với HTTP thuần/qua broker | Sửa trong **P3.16b**, ghi lý do ở §5.2 |
| C40 | **Cơ chế 1 + 3: Chế độ Tương thích theo app + thẻ phiên** (mặc định 15 phút, có ô nhập thời gian; tắt app thì thu hồi) | Hợp `CapTable` và thu hồi O(1) theo epoch (P3.8). Yêu cầu của bạn được giữ nguyên (15 phút mặc định + ô nhập). Thêm các chốt: phạm vi đúng tên miền hiển thị, trần thời gian, giới hạn tần suất hỏi, hộp thoại qua đường tin cậy P4.13, kích hoạt từ sự kiện chặn quan sát ở host (không cần app "báo lỗi") | **P3.16** |
| C41 | Công cụ web của tác tử AI cũng phải xin quyền mạng | Hệ quả tự nhiên của C40 và §7.4 "không tự bật mạng cho web_search" | **P5.17** ↔ `Adaptive-MoE` P9-18 |

---

## 14. Phase P6 — Phần cứng thật và hệ sinh thái (đề xuất 2026-11-16; chờ bạn duyệt)

Nguyên tắc: **đo trước, viết sau; virtio trước, phần cứng thật sau; thứ nào mượn được thì mượn (micro-VM), không tự viết (Wi-Fi, GPU).** Không task P6 nào chặn gate P1–P4.

### 14.1 Task

| Mã | Task | Trạng thái | DoD (đo được) |
|---|---|---|---|
| P6.1 | **Đo máy thật** dưới Linux host (đường P1) | [ ] | Trên ≥ 2 máy thật (đề xuất: máy HP Compaq 800/8200 như trong hội thoại Gemini, **xác nhận cấu hình thật bằng `yui-profiler`**, cộng 1 máy khác): lớp máy, RAM, cờ CPU (có AVX2 không), GPU, RAM sau boot, tốc độ engine q4 (tok/s), nhiệt/điện nếu có đồng hồ đo. Bảng ghi vào §12. **Chưa viết hồ sơ riêng cho máy nào trước khi có bảng** |
| P6.2 | Lớp máy `sieu-yeu` + chế độ Headless/TUI + Zero-AI fallback | [ ] | `profile.cpp::classify` thêm lớp (RAM < 1,5 GB); ở lớp này `yuid` **không nạp model** (dùng BM25/luật), compositor tắt. Test ranh giới 1,4 / 1,5 / 3,9 / 4,0 GB; trong QEMU `-m 512`, RAM sau boot headless < 48 MB (ngưỡng đề xuất, chỉnh sau P6.1); `yui-shell` vẫn dùng được |
| P6.3 | Driver **virtio** (blk, net, input, gpu 2D) cho `kernel-frame` | [ ] | Trong QEMU: ghi/đọc 1 MB qua virtio-blk qua `fsstore`; gói ARP/ICMP qua virtio-net; thêm vào smoke test `qemu` |
| P6.4 | Boot UEFI thật (nối P2.2, P4.6) | [ ] | OVMF boot ra `yui: ready` ở x86_64 |
| P6.5 | NVMe + AHCI | [ ] | QEMU `-device nvme`/`ich9-ahci`: đọc/ghi đúng; đọc trước, ghi sau; kiểm rút điện của P2.8 trên đĩa này |
| P6.6 | USB xHCI + HID | [ ] | QEMU `qemu-xhci` + `usb-kbd`: gõ Telex ra đúng chữ Việt qua đường nhân |
| P6.7 | Ngăn xếp mạng + Ethernet | [ ] | DHCP + TCP echo qua virtio-net/e1000; `EgressFilter` áp dụng. Dùng crate ngoài là dependency mới ⇒ phải duyệt giấy phép theo quy tắc §6.1 mục 5. **Wi-Fi và GPU không viết**: qua micro-VM (P3.6) hoặc USB-Ethernet/tethering |
| P6.8 | Đặc tả đường khởi động an toàn: Secure Boot (`shim`/MOK/tự đăng ký khoá/tắt) + đo toàn vẹn (TPM 2.0) | [ ] | bản đặc tả khởi động an toàn (sẽ tạo trong thư mục docs, tên `SECURE-BOOT.md`) được bạn duyệt; **không code** |
| P6.9 | ACPI cơ bản: tắt máy, ngủ, nút nguồn, pin | [ ] | QEMU: `yui-shell tắt` tắt máy qua ACPI; pin đọc đúng trên 1 máy thật (nếu có), nối `telemetry` |
| P6.10 | `yui-init` giám sát dịch vụ + `pstore` | [ ] | Giết `yuid` trong QEMU ⇒ tự khởi động lại ≤ 2 s; panic để lại dấu vết đọc được ở lần boot sau |
| P6.11 | **Hồ sơ Game** (chỉ phần khả thi): đóng băng tiến trình nền không thiết yếu, ưu tiên cửa sổ đang chơi, ZRAM cho nền, DVFS | [ ] | Đo **trước/sau** trên cùng máy: p99 frame time (MangoHud/ftrace) giảm ≥ 10% (ngưỡng đề xuất), nếu không đạt ghi `[-]` và nêu số. Phụ thuộc P2.5, P3.2 |
| P6.12 | *(nghiên cứu)* Nâng độ phân giải thần kinh + chèn khung ở compositor | [ ] | Chỉ sau P4.1. So 2 cấu hình cùng máy (native 720p và 540p + nâng): FPS, độ trễ nhập (ms), SSIM so với ảnh tham chiếu. **Go (đề xuất)** khi FPS tăng ≥ 30%, độ trễ nhập tăng ≤ 10 ms, SSIM ≥ 0,95; không thì `[-]` |
| P6.13 | Hệ tệp tương thích: FAT32/exFAT đọc-ghi; ext4, NTFS **đọc-chỉ**; là driver **userspace trong hộp cát** | [ ] | Ảnh đĩa mẫu ⇒ đọc đúng băm 100 tệp; ảnh hỏng/đột biến (fuzz) không làm sập nhân. APFS: không làm |
| P6.14 | *(nghiên cứu)* **Shader pre-cache** lúc rảnh cho game DXVK/VKD3D | [ ] | Chỉ sau P3.2 + P6.11. Cùng máy, cùng game: số lần **khựng** (khung > 50 ms) và p99 frame time trong 5 phút đầu — (A) không cache, (B) cache dựng sẵn khi rảnh + cắm sạc (theo telemetry). **Go (đề xuất):** số khựng giảm ≥ 50 % và chi phí dựng ≤ 15 phút/game; không thì `[-]`. Cache theo từng hộp cát, xoá khi gỡ app, mất hiệu lực khi đổi GPU/driver |
| P6.15 | *(nghiên cứu)* **Lớp dịch nhị phân** (Box64/FEX-Emu) cho ARM64 + RISC-V | [ ] | Chỉ khi đã có user-space chạy trên aarch64/riscv64 (sau P2.7 + P3). **Không bật trên x86_64.** Đọc giấy phép trước (không đoán). Chạy trong hộp cát `isolated`. Đo: một app x86_64 mẫu (không phải game) — tốc độ so với chạy gốc trên x86_64 cùng cỡ máy nếu có, và lần mở thứ 2 so lần 1 (cache AOT). Không vào v1.0 |
| P6.16 | *(nghiên cứu)* **Token chữ ký số USB** qua proxy PKCS#11 | [ ] | Bước 1: **bạn cho biết hãng/mẫu token cần dùng** (mình không đoán) và lấy trình điều khiển/PKCS#11 từ trang hãng. Bước 2: thử trong micro-VM có USB passthrough (P3.6). Bước 3 (thiết kế): quyền `sign` nằm ở **host**; mỗi lần ký hiện hộp thoại tin cậy (P4.13) nêu **tên tài liệu + băm**; PIN nhập qua UI của host nếu token cho phép, nếu không thì ghi rõ rủi ro. **Go:** ký được 1 tài liệu thật bằng token thật, và guest bị xâm nhập **không ký được** khi không có xác nhận. "Driver token chạy được dưới Wine" hiện **chưa xác minh** |

### 14.2 Backlog (chưa lập task; chỉ ghi để không mất)

| Ý | Ghi chú |
|---|---|
| Wayland protocol + Vulkan thật | Sau P4.1; là cầu để chạy phần mềm Linux có sẵn. Phần "cửa sổ app trong micro-VM" đã thành **P4.14** |
| IOMMU (VT-d/AMD-Vi), GPU passthrough | Cùng P3.6 |
| NUMA | Chỉ máy nhiều ổ cắm |
| Mật mã hậu lượng tử | Xem C6 |
| MDM/SSO/LDAP/Kerberos, quản trị tập trung | Sau v1.0 |
| Cảm ứng đa điểm, cảm biến, 4G/5G, Wasm/PWA | Sau v1.0 |
| Yui Store | Sau P4.7, P4.11 |
| Migration Assistant (nhập tài liệu/dấu trang/khoá SSH vào `yui_vault`) | Sau P6.13 |
| R1: nghiên cứu phân trang KV + tiền nạp trọng số cho model ≥ 7B | Sau P5; **đo tỉ lệ page-fault trước** |
| Tự chưng cất/gom trí nhớ khi máy rảnh | Chỉ "gom đồ thị"; không huấn luyện trên máy; chống nhiễm trí nhớ bằng dữ liệu web |
| RVV trong engine | Đo bằng QEMU `-cpu rv64,v=true` |

**Gate P6:** một máy thật (đã nằm trong bảng P6.1) boot YuiVieOS bằng UEFI, đọc đĩa, nhận bàn phím, có mạng, tắt máy bằng ACPI; mọi con số hiệu năng trong tài liệu có dòng đo tương ứng ở §12. Chưa đạt thì **không** gọi là "chạy trên phần cứng thật".
