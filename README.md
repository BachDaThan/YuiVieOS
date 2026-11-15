# YuiVieOS Beta 🪷

**Hệ điều hành thuần Việt, tự chủ hoàn toàn — clean-room 100 %, nhân bằng Rust an toàn.**
*A Vietnamese-first operating system: smooth on a 2 GB netbook, unrestricted on a workstation.*

```
┌──────────────────────────────────────────────────────────────────────────────┐
│ TẦNG 5  TRẢI NGHIỆM NGƯỜI DÙNG VIỆT                       userspace-vi/      │
│   Smart App Stripper │ Shell VI–EN │ Telex/VNI tại dịch vụ │ App Yui (Flutter)│
├──────────────────────────────────────────────────────────────────────────────┤
│ TẦNG 4  CỔNG CHỜ TRÍ TUỆ LÕI                            ai-bridge-yuid/      │
│   ①IPC & C-ABI  ②Telemetry  ③OS Control MCP  ④nạp .yui v3 (mmap)            │
│   ── đường nối Phase P5 ──► libyui.so (Adaptive-MoE + Y-App, ngoài repo)     │
├──────────────────────────────────────────────────────────────────────────────┤
│ TẦNG 3  HỘP CÁT ZERO-TRUST (MicroVM)                     vmm-sandbox/        │
│   native │ isolated │ microvm (Wine/Proton+VKD3D, Waydroid-lite)             │
├──────────────────────────────────────────────────────────────────────────────┤
│ TẦNG 2  NHÂN DỊCH VỤ (Safe Rust Framekernel)            kernel-services/     │
│   Lập lịch chuyển đổi nóng │ ZRAM │ FS bất biến │ DVFS │ tickless │ Linux ABI│
├──────────────────────────────────────────────────────────────────────────────┤
│ TẦNG 1  LÕI GIÁM SÁT BẢO MẬT (Capability Microkernel)   kernel-cap/          │
│   CapTable │ Thẻ năng lực ký HMAC │ Thu hồi bắc cầu │ Sovereign Keyring      │
├──────────────────────────────────────────────────────────────────────────────┤
│ TẦNG 0  PHẦN CỨNG ĐA KIẾN TRÚC                          arch/                │
│   x86_64 (BIOS Legacy + UEFI) │ ARM64 │ RISC-V 64                            │
└──────────────────────────────────────────────────────────────────────────────┘
```

Bản vẽ đầy đủ + giao thức cổng chờ: [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md).
Kế hoạch thi công P0–P5, tiêu chí đo được, CI QEMU 3 kiến trúc: [`docs/PLAN.md`](docs/PLAN.md).

---

## Chạy thử trong 60 giây

Yêu cầu: `g++` ≥ 10 (C++17), `make`, `python3`, POSIX. **Không** cần mạng, không
cần thư viện ngoài, không cần Rust.

```bash
make              # → build/bin/{yuid,libyui.so,libyui.a,yui-profiler,yui-trim,yui-shell}
make test         # 705 kiểm thử (669 C++17 + 36 C11 ABI)
make check        # test + 4 smoke run + kiểm tra tĩnh (scripts/verify.sh)
```

```console
$ ./build/bin/yui-profiler
YuiVieOS · Hồ sơ phần cứng
  Kiến trúc : x86_64 (avx512f, 512 bit)
  CPU       : Intel(R) Xeon(R) Processor @ 2.60GHz — 2 luồng / 1 nhân
  RAM       : 3.8 GB (trống 3.6 GB)
  Phân loại : may-yeu
  Tự chỉnh  : k=2, cửa sổ=1024, luồng=2 (may-yeu)

$ ./build/bin/yui-shell
› gõ telex Tieengs Vieetj
Tiếng Việt
› ghinhớ os:yui "YuiVieOS — hệ điều hành Việt" concept
Đã ghi nhớ: os:yui
› tìm yui
· os:yui — YuiVieOS — hệ điều hành Việt [concept]
› thoát
Tạm biệt — hẹn gặp lại ở YuiVieOS. / Goodbye.
```

---

## Cấu trúc monorepo

```
docs/                ARCHITECTURE · PLAN · SECURITY · PROTOCOL · FORMAT-YUIP · BUILD
kernel-cap/          Tầng 1 (Rust no_std): quyền, CapTable, thẻ HMAC, SHA-256
kernel-services/     Tầng 2: yui-sched/ (LAVD) · yui-zram/ (ZRAM) + codec/crypto/profile
vmm-sandbox/         Tầng 3: bộ thực thi chính sách · policies/ · vmm/ (manifest schema)
ai-bridge-yuid/      Tầng 4: daemon/ · capi.cpp · reference/ (chờ thay ở P5)
pkg-stripper/        Tầng 5: Smart App Stripper (apppack)
userspace-vi/        Tầng 5: text · ime · ui · shell · ui/app (Flutter)
include/yui/         API công khai + header C `yui.h`
tests/               7 bộ test C++17 + 1 bộ test C11
tools/               yui-profiler · yui-trim · yui-shell
arch/                x86_64 (linker script) · arm64 · riscv64
scripts/             verify.sh · verify.py · qemu-smoke.sh
.github/workflows/   CI: cpp · rust · docs · cross-arch · qemu
```

Chi tiết từng thư mục: mỗi thư mục có `README.md` riêng nói nó làm gì, đã xong
phần nào, còn thiếu phần nào.

---

## Trạng thái thật (không tô hồng)

Mỗi dòng "đã có" đều trỏ tới tệp và test cụ thể; mỗi dòng "chưa" đều trỏ tới
task trong [`docs/PLAN.md`](docs/PLAN.md).

| Tầng | Thành phần | Trạng thái | Bằng chứng |
|---|---|---|---|
| 0 | Hardware Auto-Profiler: CPU/RAM/GPU/pin/nhiệt → `may-yeu`/`can-bang`/`may-manh` + k/W | ✅ chạy | `kernel-services/profile.cpp`, `tools/profiler` |
| 0 | Luồng khởi động 3 kiến trúc + linker script x86_64 | 🟡 đặc tả | `arch/*/README.md`, `arch/x86_64/linker.ld` |
| 1 | Bảng năng lực: 15 quyền, dẫn xuất chỉ thu hẹp, thu hồi bắc cầu | ✅ Rust | `kernel-cap/src/lib.rs` |
| 1 | Thẻ năng lực ký HMAC-SHA-256, xác minh hằng thời gian, lỗi có kiểu | ✅ hai phía | `kernel-cap/src/lib.rs`, `vmm-sandbox/sandbox.cpp` |
| 1 | SHA-256 / HMAC / SHAKE-256 / scrypt tự viết, có vector RFC | ✅ | `kernel-services/crypto.cpp`, `tests/test_crypto.cpp` |
| 1 | Sovereign Keyring + `.bundle` ký Ed25519 | ❌ chưa có | P2.4 |
| 2 | Lập lịch chuyển đổi nóng LAVD (P/E-core, tín dụng đánh thức, công bằng) | ✅ Rust | `kernel-services/yui-sched` (9 test) |
| 2 | ZRAM: codec YUI-LZ + kho trang nén, fallback verbatim | ✅ Rust + C++ | `kernel-services/yui-zram` (8 test), `kernel-services/codec.cpp` |
| 2 | `yui-frame` (khung unsafe duy nhất), tickless, DVFS, FS bất biến, Linux ABI | ❌ chưa có | P2.1–P2.9 |
| 3 | Hộp cát 3 bậc + audit + 4 policy mẫu có test | ✅ | `vmm-sandbox/` (129 check) |
| 3 | Manifest micro-VM (JSON Schema) | 🟡 đặc tả | `vmm-sandbox/vmm/` |
| 3 | VMM thật (KVM), Wine/VKD3D, Waydroid-lite | ❌ chưa có | P3 |
| 4 | `yuid`: socket `/run/yuid.sock`, JSON-RPC 2.0 | ✅ chạy | `ai-bridge-yuid/daemon/` (102 check) |
| 4 | C-ABI `libyui.so/.a` + header C, kiểm từ **C11** | ✅ chạy | `include/yui/yui.h`, `tests/test_capi.c` (36 check) |
| 4 | MCP `2024-11-05`, 8 công cụ dữ liệu | ✅ chạy | `ai-bridge-yuid/daemon/server.cpp` |
| 4 | Telemetry Hook + 5 công cụ OS MCP có xác nhận | ❌ chưa có | P1.5, P4.3 |
| 4 | Cài đặt tham chiếu graph/vault/engine (**sẽ thay ở P5**) | 🟡 tạm | `ai-bridge-yuid/reference/` |
| 5 | Smart App Stripper: strip ELF, khử trùng lặp, lọc locale, chống zip-slip | ✅ chạy | `pkg-stripper/apppack.cpp`, `tools/trim` (61 check) |
| 5 | Bộ gõ Telex **và** VNI, đặt dấu đúng chính tả | ✅ 84 test | `userspace-vi/ime.cpp` |
| 5 | Shell song ngữ Việt–Anh, 15 lệnh | ✅ chạy | `userspace-vi/shell.cpp` |
| 5 | Kế hoạch giao diện thích ứng (Framebuffer ⇄ Vulkan/DRM) | ✅ logic + test | `userspace-vi/ui.cpp` |
| 5 | Bộ vẽ/compositor thật, IBus engine, App Yui build được | ❌ chưa có | P1.4, P2.6, P4.1, P4.5 |

**Chưa kiểm chứng được trong môi trường soạn thảo này**: biên dịch Rust (không có
`cargo`), boot QEMU (chưa có nhân), build Flutter (không có SDK). Cả ba đều có
job CI và task tương ứng trong `PLAN.md` — chúng tôi **không** tuyên bố chúng đã chạy.

---

## Phase P5 — dự trữ, chưa kích hoạt

Lõi AI (`yui.cpp`, `.yui v3`, `yui_graph`, `yui_vault`, `Y-App`) phát triển ở hai
repo riêng. Repo này chỉ dựng **cổng chờ** với hợp đồng ổn định:

- `telemetry.read` + `yui_telemetry()` — pin, nhiệt, RAM, khuyến nghị k/W
  (`k=1,W=16` khi yếu/pin thấp; `k=2..4` khi cắm sạc/máy mạnh)
- `sys_power`, `sys_scheduler`, `sys_app_stripper`, `sys_proc`, `sys_search_vault`
  qua MCP, mỗi hành động cần quyền + xác nhận người dùng + audit
- `mmap` trực tiếp tệp `.yui v3` (trọng số không bao giờ đi qua socket)

Khi bạn ra lệnh **"Tích hợp AI vào hệ điều hành"**, quy trình ở
[`docs/PLAN.md`](docs/PLAN.md) §P5 được thực hiện tuần tự: kéo mã → đóng băng SHA →
build `libyui.so` 3 kiến trúc → kiểm C-ABI → cắm vào bridge → bật `yuid` → bật
điều khiển tiếng Việt. Có đường rollback một lệnh (`--reference-engine`).

---

## Nguyên tắc

1. **Clean-room**: không một dòng mã từ Linux/seL4/Asterinas; chỉ dùng nguyên lý
   công khai + chuẩn giao tiếp. MIT, không nghĩa vụ GPL.
2. **Rust an toàn**: `#![deny(unsafe_code)]` cho mọi crate nhân; `unsafe` chỉ được
   tồn tại trong khung phần cứng `yui-frame` (P2.1) và được `scripts/verify.sh` canh.
3. **Tiếng Việt hạng nhất**: tìm không dấu, bộ gõ đúng chính tả, shell hai tên lệnh.
4. **Máy yếu không phải bản cắt bớt**: cùng binary, khác chế độ vận hành.
5. **Không hứa điều chưa đo**: mỗi mục tiêu có số đo trong `PLAN.md`; giới hạn bảo
   mật được liệt kê thẳng ở [`docs/SECURITY.md`](docs/SECURITY.md) §7.

## Giấy phép

MIT — xem [`LICENSE`](LICENSE).
