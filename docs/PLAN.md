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

### Trạng thái hiện tại (2026-11-15)

| Phase | Nội dung | Trạng thái |
|---|---|---|
| **P0** | Nền móng: monorepo, hợp đồng cổng chờ, test C++/C11, CI | ✅ xong (670 check lúc đó, nay 2669) |
| **P1** | Chạy thật trên Linux host: `yuid` service, IBus, stripper, telemetry, OS MCP | 🟡 4/10 task (P1.5 xong) |
| **P2** | Rust framekernel + 3 kiến trúc + QEMU + năng lượng + FS + Linux ABI | 🟡 khung nhân **boot thật** trên aarch64 + riscv64 (CI xanh); x86_64 chặn bởi QEMU multiboot |
| **P3** | VMM sandbox (Wine/VKD3D, Waydroid-lite) | ⬜ chưa bắt đầu |
| **P4** | UX & phát hành (compositor, installer, ký số) | ⬜ chưa bắt đầu |
| **P5** | *(dự trữ)* Tích hợp AI vào hệ điều hành | 🔒 chờ lệnh kích hoạt |

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

---

## 2. Phase P0 — Nền móng (✅ xong)

| Mã | Task | Trạng thái | Bằng chứng |
|---|---|---|---|
| P0.7 | Monorepo sạch: `docs/ kernel-cap/ kernel-services/ vmm-sandbox/ ai-bridge-yuid/ pkg-stripper/ userspace-vi/ tools/ arch/ include/ tests/ scripts/ .github/workflows/` | [x] | `ls -F` |
| P0.8 | Hợp đồng cổng chờ: JSON-RPC `/run/yuid.sock`, C-ABI `libyui`, MCP `2024-11-05` | [x] | `tests/test_daemon.cpp` (102), `tests/test_capi.c` (36) |
| P0.9 | Capability: quyền, dẫn xuất, thu hồi, thẻ ký HMAC (Rust + C++ đối chiếu) | [x] | `kernel-cap/src/lib.rs` (6 test), `tests/test_sandbox.cpp` (129) |
| P0.10 | Chính sách lập lịch LAVD + ZRAM (Rust `no_std`, deny unsafe) | [x] | `kernel-services/yui-{sched,zram}` (9 + 8 test) |
| P0.11 | Vietnamese: Telex/VNI, fold, shell song ngữ, kế hoạch UI | [x] | `tests/test_ime.cpp` (84), `tests/test_tools.cpp` (61) |
| P0.12 | Smart App Stripper: strip ELF, khử trùng lặp, lọc locale, chống zip-slip | [x] | `pkg-stripper/apppack.cpp`, `tools/trim` |
| P0.13 | Tài liệu: `ARCHITECTURE.md`, `SECURITY.md`, `PROTOCOL.md`, `FORMAT-YUIP.md`, `BUILD.md`, `PLAN.md` | [x] | `docs/` |

**Gate P0:** `make check` + `verify.sh` xanh, cả 6 tầng có ít nhất một vật thể chạy
được hoặc một đặc tả kiểm được. ✅

---

## 3. Phase P1 — Chạy thật trên Linux host (🟡 3/10)

Mục tiêu chiến lược: **có người dùng thật trước khi có nhân riêng.** YuiVieOS Beta
chạy như một tầng dịch vụ trên Linux: bộ gõ, stripper, trợ lý, quản lý năng lượng.
Đây là phase tạo giá trị và tạo phản hồi cho P2.

| Mã | Task | Trạng thái | DoD (đo được) |
|---|---|---|---|
| P1.1 | `yuid` chạy như systemd service + socket activation | [x] | `systemctl start yuid` xanh; socket `0600`; `yuid --serve-once` vẫn qua test |
| P1.2 | ZRAM dùng thật qua `/dev/zram0` (module có sẵn của host) | [ ] | 200 MB dữ liệu văn phòng → < 80 MB; đọc lại **đúng từng byte**; `zramctl` xác nhận |
| P1.3 | `libyui` publish: `pkg-config`, `soname` ổn định, ví dụ C + Python | [ ] | `pkg-config --cflags --libs yui` chạy ngoài cây; ví dụ Python gọi `ime.convert` |
| P1.4 | Bộ gõ thành IBus/Fcitx5 engine | [ ] | Gõ Telex trong Firefox **và** LibreOffice ra chữ Việt đúng dấu; độ trễ < 10 ms/phím |
| P1.5 | Telemetry Hook (P4.2 của `ARCHITECTURE.md` §6.3) | [x] | `telemetry.read` + `system_telemetry` MCP đọc sysfs thật; máy này không có pin ⇒ `present:false`; `tests/test_telemetry.cpp` **471 check** (ranh giới 20 % / 75 °C, pin thấp, GPU nóng, quét toàn miền k/W, RPC + MCP); `test_capi` 61 check phủ `yui_telemetry`/`yui_telemetry_advise`. **Chưa** đối chiếu được trên máy có pin thật — ghi rõ, không tô hồng |
| P1.6 | Smart Stripper trên app thật | [~] | Electron ≥ 35 % nhỏ hơn và **chạy được**; app có DRM: `stripped=false`, 0 byte bị sửa |
| P1.7 | Đóng gói `.deb` + AppImage/Flatpak | [ ] | Cài trên Ubuntu 22.04 sạch, `yui-shell` chạy, gỡ sạch |
| P1.8 | MCP dùng thật với một client | [ ] | Agent ngoài gọi `graph_upsert` → `graph_query` thấy dữ liệu; log chứng minh |
| P1.9 | Audit log người đọc được | [ ] | `audit.log` có ≥ 3 mục thật; mỗi mục có lý do tiếng Việt và **không** chứa bí mật |
| P1.10 | Đo nền: thời gian khởi động dịch vụ, RAM của `yuid` | [ ] | `yuid` < 8 MB RSS khi rảnh; khởi động < 200 ms (đo bằng `systemd-analyze` + `/proc/pid/status`) |

**Gate P1:** một người không phải lập trình viên gõ tiếng Việt được bằng YuiVieOS
trên máy thật, và một app được gọt chạy bình thường. Ghi lại ảnh + số đo.

---

## 4. Phase P2 — Rust framekernel, 3 kiến trúc, đo được

| Mã | Task | Trạng thái | DoD (đo được) |
|---|---|---|---|
| P2.0 | Workspace Rust `kernel-cap` + `kernel-services/*`, `deny(unsafe_code)` | [x] | `cargo test --workspace` (CI); `grep unsafe` rỗng; `kernel-frame` **ngoài** workspace để `--workspace` vẫn chỉ chạy trên host |
| P2.1 | `yui-frame`: khung duy nhất chứa `unsafe` (asm, MMU, ngắt, MMIO) | [x] | `kernel-frame/` là crate **duy nhất** có `unsafe`: 29 khối, tất cả nằm trong 7 tệp có tên + lý do ở `scripts/verify.py::FRAME_UNSAFE_FILES` (tệp khác có `unsafe` ⇒ `verify.sh` đỏ); mọi crate khác vẫn `deny(unsafe_code)`. Boot thật trong QEMU trên aarch64 + riscv64 — job CI `qemu` xanh |
| P2.2 | `yui-boot` + nhân tối thiểu, x86_64 (BIOS Legacy + UEFI) | [~] | **Đã sửa đường boot QEMU x86_64**: header Multiboot2 (24 byte + end tag, checksum `-(0xE85250D6+24)`) đặt cạnh Multiboot1; cầu 32→64 mở rộng edi/esi thành `kmain_x86(u32,u32)` phân biệt magic `0x2BADB002`/`0x36D76289`; `parse_multiboot2` đọc **little-endian** (đã sửa lỗi be32); `build-kernel.sh` xuất `.elf64` + `objcopy -O elf32-i386` ra `.elf` cho QEMU `-kernel`; job CI `qemu` **bắt buộc** cả 3 kiến trúc. **Còn đợi:** CI `kernel`+`qemu` xanh xác nhận boot thật (sandbox dev không có QEMU — không claim thay). UEFI chưa làm |
| P2.3 | ARM64 + RISC-V 64 ở mức "ready" | [x] | Cả hai **boot thật trong QEMU** (job CI `qemu` xanh): in `yui: ready`, `Phan loai`, `Tang 1`. riscv64 đọc RAM từ device tree; aarch64 nhận `x0 = 0` (QEMU không truyền DTB cho ELF `-kernel`) nên tự quét FDT (`find_fdt`) trong cửa sổ `0x4000_0000±512 KiB`, không có thì in `RAM: 0 MiB — khong doc duoc`, **không bịa số** |
| P2.4 | Sovereign Keyring + Ed25519 clean-room cho `.bundle` | [x] | **Xong, có bằng chứng test**: Ed25519 clean-room (`kernel-services/ed25519.cpp` ~600 dòng + `kernel-cap/src/ed25519.rs`) khớp **vector RFC 8032 §7.1** (TEST 1/2/3/SHA(abc)/1024 — pub + sig khớp từng hex) và SHA-512 khớp FIPS 180-4: `tests/test_ed25519.cpp` **78 check xanh**; kho khoá + `.bundle` ký/xác minh ngoại tuyến + 5 kịch bản sửa 1 byte đều từ chối + cài khe A/B + quay lui (`bundle.*`); bản Rust `keyring.rs`/`bundle.rs` có test song song. **Còn thiếu (ngoài phạm vi bản này):** cài bundle thật trong QEMU |
| P2.5 | Năng lượng: full tickless + DVFS mỗi nhân + clock gating | [~] | **Chính sách đã cài + test 47 check** (`include/yui/power.hpp`, `kernel-services/power.cpp`, `tests/test_power.cpp`): tickless 0 wakeup định kỳ khi rảnh, kế hoạch ngủ theo deadline (halt/deep/suspend), bảng OPP 3 loại máy, DVFS theo tải/nhiệt/pin/trần tiến trình, clock-gating GPU/NPU khi quá nhiệt 75 °C, bóp xung per-task. Nguồn dữ liệu = `telemetry.cpp`. **Còn thiếu:** đo trên nhân thật (0 wakeup/10 s, ≥ 20 % W gói) — không có phần cứng trong sandbox, không claim thay |
| P2.6 | Hiển thị: framebuffer trực tiếp + DRM/Vulkan zero-copy | [~] | Bộ vẽ pixel tuyến tính + font 5×7 + màn hình chào: `userspace-vi/fb.cpp`, `tests/test_fb.cpp` (**1184 check**), `tools/splash` → `docs/splash.png`; bản kernel `kernel-frame/src/splash.rs`. **Đã thêm chữ Việt có dấu**: 180 chữ cái tiền tổ hợp (bảng NFD sinh từ Unicode), dấu thanh + dấu nguyên âm xếp chồng + dấu nặng + đ/Đ, `draw_text_utf8` vẽ ASCII **giống hệt** `draw_text` cùng toạ độ; khung cửa sổ + nhãn tiếng Việt (`userspace-vi/ui.cpp::render_window`) — test `fb_viet.*` (29 check). **Còn thiếu:** đường DRM/Vulkan, và đo fps |
| P2.7 | Lớp Linux ABI: syscall nhóm 1 (file, mmap, futex, epoll) | [~] | **Bảng phân phối syscall đã có** (`kernel-services/linuxabi.cpp`, `tests/test_linuxabi.cpp` **38 check**): read/write/open/close/openat/exit/exit_group đúng ngữ nghĩa, `/core` chỉ-đọc (EPERM), `utimensat` → EPERM (chặn timestomping), fork/execve/socket/connect → EPERM, `nr_from32` quy đổi số i386, không rõ ⇒ ENOSYS. **Còn thiếu:** mmap/futex/epoll + busybox/coreutils/python3 thật |
| P2.8 | FS bất biến + write coalescing | [~] | **Đã cài + test 37 check** (`kernel-services/fsstore.cpp`, `tests/test_fsstore.cpp`): phân vùng lõi từ chối mọi ghi (lý do tiếng Việt), `WriteCoalescer` gộp ghi cùng trang (3 lệnh → 1 chốt vật lý), chốt lỗi giữ trang để thử lại, chặn `..`/`//`/đường dẫn tuyệt đối. **Còn thiếu:** test rút điện 20 lần trên QEMU với disk image thật |
| P2.9 | ZRAM trong nhân thật | [ ] | 200 MB văn phòng → < 80 MB trong QEMU với RAM 512 MB; đọc lại đúng |
| P2.10 | Self-hosting: build `yuid` trên chính YuiVieOS Beta | [ ] | Trong QEMU: `cargo build` (hoặc rustc bootstrap) của `yuid` thành công; chạy `yuid --info` từ binary vừa build |
| P2.11 | Ngân sách RAM máy yếu | [ ] | Sau boot (nhân + dịch vụ bắt buộc): **< 64 MB** RSS tổng, đo bằng `free`/bộ đếm trong nhân |
| P2.12 | Guard chống bất thường nền (đào coin ngầm) — `docs/SECURITY.md` §10.1 | [x] | Quy tắc 70 %/15 s/headless/không `compute.heavy` ⇒ hạ Idle + bóp xung 20 % + audit + hỏi "tiêu diệt / gỡ bỏ / để yên" qua `yuid` (`guard.decide` bắt buộc `confirm`). Cài: `kernel-services/guard.cpp`, `kernel-services/yui-sched/src/guard.rs`, `ai-bridge-yuid/daemon/service.cpp` (`guard.observe/alerts/decide`). Test: `tests/test_redteam.cpp` (kịch bản miner, tải nhấp nháy, tiến trình hợp lệ bất khả xâm phạm, chuỗi quyết định qua daemon) |
| P2.13 | Bộ lọc egress theo năng lực + test Red-Team — `docs/SECURITY.md` §10.2–10.3 | [x] | `isolated`/`microvm` mặc định chặn egress; chỉ đích trong `policies/*.json` (host/wildcard/CIDR + cổng) đi được, deny thắng allow, có lý do tiếng Việt. Cài: `vmm-sandbox/sandbox.cpp::EgressFilter` (+ `compute.heavy` vào bảng quyền, docs 15→16). Test: `tests/test_redteam.cpp` (mặc định chặn, khoá hậu tố giả mạo, sai cổng, ngoài CIDR, exfil bị chặn ở cả EgressFilter lẫn Enforcer + audit) |

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

### 5.1 Trình tự hardening Tầng 3 — chỉ lập kế hoạch, chưa triển khai

P2.13 hiện tại đã có `EgressFilter` allowlist cơ bản; các việc P3.7–P3.11 là
**mở rộng mới**, không được ghi nhận nhầm là đã có transitive revocation, freeze
phiên micro-VM, lọc DNS theo entropy hay secretless broker. Thứ tự bắt buộc là
P3.7 → P3.8 → P3.9 → P3.10 → P3.11; không nhảy thẳng tới tuyên bố hoàn tất.

**Gate P3:** 10/10 kịch bản thoát hộp cát thất bại; game chạy có số fps; Android app chạy. Với hardening mới, chỉ qua gate khi P3.7–P3.11 có bằng chứng riêng, toàn bộ kịch bản red-team đạt và containment Lớp 2 được kiểm trên micro-VM thật (P3.1), không chỉ bằng mock.

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

### 7.2 Runbook (thực hiện tuần tự, có kiểm sau mỗi bước)

| Bước | Việc | Lệnh / tiêu chí |
|---|---|---|
| P5.1 | Kéo mã lõi AI | `git clone https://github.com/BachDaThan/Adaptive-MoE vendor/adaptive-moe` và `git clone https://github.com/BachDaThan/Y-App vendor/y-app` — kiểm `vendor/*/LICENSE` |
| P5.2 | Đóng băng phiên bản | ghi commit SHA vào `vendor/VERSIONS.lock` (không dùng `main` trôi nổi) |
| P5.3 | Biên dịch `libyui.so` đa kiến trúc | `x86_64`, `aarch64`, `riscv64` — mỗi cái có `sha256` và được ghi vào `vendor/VERSIONS.lock` |
| P5.4 | Kiểm khớp C-ABI | `nm -D libyui.so` phải có đủ 12 hàm trong `include/yui/yui.h`; chạy `tests/test_capi.c` link với **lib thật** (không phải bản tham chiếu) |
| P5.5 | Nối vào bridge | trỏ `yuid` sang `dlopen` `libyui.so` thật; giữ `reference/` làm fallback có cờ `--reference-engine` |
| P5.6 | Nạp `.yui v3` | `mmap` tệp qua `yuid`, kiểm `fs.read` đúng đường dẫn; test: tệp 1 GB mở < 50 ms, RAM tăng < 5 MB |
| P5.7 | Bật dịch vụ nền `yuid` | `systemctl enable --now yuid`, socket `0600`, `yuid --info` liệt kê engine thật |
| P5.8 | Bật điều khiển tiếng Việt qua MCP | test 5 công cụ với câu tiếng Việt có dấu **và** không dấu; mỗi hành động có audit + xác nhận |
| P5.9 | Kiểm hồi quy toàn bộ | `make test` 2669 check xanh (2608 C++17 + 61 C11); thêm test tích hợp: 1 câu lệnh tiếng Việt → hành động OS thật |
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

---

## 8. CI: ma trận, QEMU, tiêu chí pass

File: `.github/workflows/ci.yml`. Sáu job, chạy song song trên mọi push/PR.

| Job | Chạy gì | Pass khi |
|---|---|---|
| `cpp` | `make -j`, `make test`, `make check`, `make install` vào prefix tạm, link `libyui.a` từ chương trình C ngoài cây | **2669** check pass (2608 C++17 + 61 C11); 9 binary tồn tại; chương trình C in `abi=1` |
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

---

## 11. Thứ tự làm tiếp theo (ngay bây giờ)

1. **P1.5 Telemetry** — biến số liệu pin/nhiệt/RAM thành API thật. Không phụ thuộc
   gì bên ngoài, dùng được ngay, và là điều kiện vào của P5.
2. **P1.4 IBus engine** — đây là thứ khiến người Việt dùng hệ điều hành này.
3. **P1.3 publish `libyui`** — mở đường cho app bên ngoài.
4. **P1.2 ZRAM thật** — số đo đầu tiên về "máy yếu chạy mượt".
5. **P2.0 → P2.1** — bắt đầu `yui-frame` khi có máy có `cargo` + `qemu`.

Mỗi mục trên đều có DoD đo được ở §3; làm xong thì sửa `[ ]` → `[x]` và thêm dòng
vào nhật ký dưới đây.

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
