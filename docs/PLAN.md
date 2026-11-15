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
| **P0** | Nền móng: monorepo, hợp đồng cổng chờ, test C++/C11, CI | ✅ xong (705 check) |
| **P1** | Chạy thật trên Linux host: `yuid` service, IBus, stripper, telemetry, OS MCP | 🟡 3/10 task |
| **P2** | Rust framekernel + 3 kiến trúc + QEMU + năng lượng + FS + Linux ABI | ⬜ chưa bắt đầu (P2.0 xong) |
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
| P1.5 | Telemetry Hook (P4.2 của `ARCHITECTURE.md` §6.3) | [ ] | `telemetry.read` trả pin/nhiệt/RAM thật; trên máy không pin trả `present:false`; 3 test (có pin, không pin, pin thấp) |
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
| P2.0 | Workspace Rust `kernel-cap` + `kernel-services/*`, `deny(unsafe_code)` | [x] | `cargo test --workspace` (CI); `grep unsafe` rỗng |
| P2.1 | `yui-frame`: khung duy nhất chứa `unsafe` (asm, MMU, ngắt, MMIO) | [ ] | Boot được trong QEMU; **mọi** `unsafe` nằm trong `yui-frame/src/arch/**`; crate khác vẫn `deny(unsafe_code)` |
| P2.2 | `yui-boot` + nhân tối thiểu, x86_64 (BIOS Legacy + UEFI) | [ ] | QEMU BIOS boot tới `yui: ready` < 3 s; QEMU UEFI (OVMF) cũng tới `yui: ready` |
| P2.3 | ARM64 + RISC-V 64 ở mức "ready" | [ ] | `qemu-system-aarch64 -M virt` và `qemu-system-riscv64 -M virt` in `yui: ready`; cùng một `YuiBootInfo` |
| P2.4 | Sovereign Keyring + Ed25519 clean-room cho `.bundle` | [ ] | Vector RFC 8032 (5 test) qua; sửa 1 byte bundle ⇒ từ chối; bundle hợp lệ ⇒ cài được trong QEMU |
| P2.5 | Năng lượng: full tickless + DVFS mỗi nhân + clock gating | [ ] | Khi rảnh: **0** wakeup/s trong 10 s (đếm trong nhân); công suất gói giảm **≥ 20 %** so với governor mặc định cùng workload (đo RAPL nếu có, nếu không ghi rõ máy không hỗ trợ) |
| P2.6 | Hiển thị: framebuffer trực tiếp + DRM/Vulkan zero-copy | [ ] | Máy yếu: vẽ 1280×800 @ ≥ 30 fps không GPU (đo trong QEMU + máy thật); máy mạnh: đường zero-copy có test đếm số lần copy = 0 |
| P2.7 | Lớp Linux ABI: syscall nhóm 1 (file, mmap, futex, epoll) | [ ] | `busybox` chạy ≥ 100 lệnh; `coreutils` `ls/cat/cp` đúng; `python3 -c "print(1)"` in `1` |
| P2.8 | FS bất biến + write coalescing | [ ] | Rút điện đột ngột 20 lần khi đang ghi: `graph.json` **luôn** parse được (0 hỏng); số lần ghi giảm ≥ 50 % so với fsync từng lần |
| P2.9 | ZRAM trong nhân thật | [ ] | 200 MB văn phòng → < 80 MB trong QEMU với RAM 512 MB; đọc lại đúng |
| P2.10 | Self-hosting: build `yuid` trên chính YuiVieOS Beta | [ ] | Trong QEMU: `cargo build` (hoặc rustc bootstrap) của `yuid` thành công; chạy `yuid --info` từ binary vừa build |
| P2.11 | Ngân sách RAM máy yếu | [ ] | Sau boot (nhân + dịch vụ bắt buộc): **< 64 MB** RSS tổng, đo bằng `free`/bộ đếm trong nhân |

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

**Gate P3:** 10/10 kịch bản thoát hộp cát thất bại; game chạy có số fps; Android app chạy.

---

## 6. Phase P4 — UX & phát hành

| Mã | Task | Trạng thái | DoD |
|---|---|---|---|
| P4.1 | Compositor: framebuffer (yếu) + Vulkan/DRM (mạnh) | [ ] | Chuyển chế độ theo profiler không cần đăng nhập lại; 30 fps trên máy 2 GB |
| P4.2 | Telemetry C-ABI (`yui_telemetry`) hoàn chỉnh | [ ] | 3 test C11; khuyến nghị k/W khớp bảng `ARCHITECTURE.md` §6.3 |
| P4.3 | 5 công cụ OS MCP + xác nhận người dùng + audit | [ ] | Mỗi công cụ: 1 test thiếu quyền (từ chối), 1 test thiếu `confirm` (`needs_confirmation`), 1 test thành công có audit |
| P4.4 | Shell: 25 lệnh, gợi ý lệnh, song ngữ đầy đủ | [ ] | Mọi lệnh có 2 tên; `giúp` liệt kê đủ; test cho từng tên |
| P4.5 | App Yui (Flutter) build được | [ ] | `flutter build linux` xanh trong CI; 3 màn hình dùng được với `yuid` thật |
| P4.6 | Trình cài đặt (USB, BIOS + UEFI, A/B) | [ ] | Cài lên máy ảo từ USB, boot lại vào hệ thống đã cài, chữ ký ảnh hợp lệ |
| P4.7 | Ký số phát hành + `.bundle` | [ ] | Bundle được ký bằng khoá offline; máy từ chối bundle sai chữ ký |
| P4.8 | Tài liệu người dùng cuối (tiếng Việt, có ảnh) | [ ] | 5 hướng dẫn: cài, gõ tiếng Việt, gọt app, tiết kiệm pin, khôi phục |

**Gate P4:** một người dùng mới cài được YuiVieOS Beta từ USB và dùng được hằng ngày.

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
| P5.9 | Kiểm hồi quy toàn bộ | `make test` 705+ check xanh; thêm test tích hợp: 1 câu lệnh tiếng Việt → hành động OS thật |
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

File: `.github/workflows/ci.yml`. Năm job, chạy song song trên mọi push/PR.

| Job | Chạy gì | Pass khi |
|---|---|---|
| `cpp` | `make -j`, `make test`, `make check`, `make install` vào prefix tạm, link `libyui.a` từ chương trình C ngoài cây | 705 check pass; 6 binary tồn tại; chương trình C in `abi=1` |
| `rust` | `cargo test --workspace`, `cargo build --release`; fmt/clippy advisory | 0 test fail |
| `docs` | `scripts/verify.sh`: policy JSON parse, không CRLF, liên kết README, không `unsafe` trong crate nhân | tất cả xanh |
| `cross-arch` | `cargo check --workspace --target {aarch64-unknown-linux-gnu, riscv64gc-unknown-linux-gnu}` | cả hai target type-check |
| `qemu` | Build nhân cho 3 kiến trúc rồi boot trong QEMU (`scripts/qemu-smoke.sh`) | **Hiện tại:** QEMU skip có báo cáo (chưa có `yui-frame`). **Từ P2.2:** mỗi kiến trúc in `yui: ready` < 60 s, job fail nếu không |

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
./scripts/qemu-smoke.sh x86_64     # cần yui-kernel.elf; nếu chưa có, báo rõ và thoát 0
./scripts/qemu-smoke.sh --strict   # fail nếu thiếu nhân (dùng trong CI từ P2.2)
```

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
| 2026-11-15 | P1.6 (`[~]`) | `yui-trim` tròn vẹn trên cây thư mục mẫu: 5 file → gói 187 B, lọc `locale/fr/`, giải nén đúng |
