# YuiVieOS 🪷

**Hệ điều hành Việt Nam — máy yếu chạy mượt, máy mạnh chạy tối đa.**
*A Vietnamese-first operating system: smooth on a 2 GB netbook, unrestricted on a workstation.*

```
====================================================================================================
 TẦNG 5: GIAO DIỆN & TRẢI NGHIỆM NGƯỜI DÙNG VIỆT (USER-SPACE SHELL & APPS)
 ┌───────────────────────────────────┐ ┌───────────────────────────────────┐ ┌─────────────────────┐
 │  Giao diện Thích ứng (Adaptive UI)│ │   Trình Gọt Tỉa & Nén App (Zstd)  │ │   Hệ thống Gõ & AI  │
 │  • Máy yếu: Direct Framebuffer    │ │  • Quét DRM/Chữ ký số chủ quyền   │ │  • Bộ gõ Telex/VNI  │
 │  • Máy mạnh: Vulkan / DRM Zero-copy│ │  • Cắt mã chết/ngôn ngữ thừa      │ │    tầng dịch vụ     │
 │  • Shell lệnh song ngữ Việt - Anh │ │  • Nén Zstd + Chạy thẳng trên RAM │ │  • App Yui (Flutter)│
 └─────────────────┬─────────────────┘ └─────────────────┬─────────────────┘ └──────────┬──────────┘
===================╪=====================================╪==============================╪===========
 TẦNG 4: CỔNG CHỜ & DỊCH VỤ TRÍ TUỆ LÕI (`yuid` CORE AI DAEMON — CẮM TỪ Y-APP & ADAPTIVE-MOE)
 ┌─────────────────────────────────────────────────────────────────────────────────────────────────┐
 │  Giao tiếp qua Unix Socket (`/run/yuid.sock`) & C-ABI FFI (`libyui.so` / `libyui.a`)            │
 │  ┌──────────────────────────┐ ┌───────────────────────────┐ ┌─────────────────────────────────┐ │
 │  │ Engine C++ (`yui.cpp`)   │ │ Bộ nhớ & Đồ thị Tri thức  │ │ Hệ thống Agent & Công cụ OS     │ │
 │  │ • SIMD: AVX2/NEON/RVV    │ │ • Graphify (`yui_graph`)  │ │ • Giao thức MCP Server/Client   │ │
 │  │ • Nén q4 / t2 / BITCOS   │ │ • Kho mã hóa (`yui_vault`)│ │ • Sandbox 3 bậc (`yui_sandbox`) │ │
 │  └──────────────────────────┘ └───────────────────────────┘ └─────────────────────────────────┘ │
 ═════════════════════════════════════╪════════════════════════════════════════════════════════════
 TẦNG 3: HỘP CÁT TƯƠNG THÍCH ỨNG DỤNG CŨ (ZERO-TRUST SANDBOX & MICRO-VM)
 TẦNG 2: NHÂN DỊCH VỤ HIỆU NĂNG CAO (CLEAN-ROOM RUST FRAMEKERNEL — SAFE RUST 100%)
 TẦNG 1: LÕI GIÁM SÁT BẢO MẬT & CẤP QUYỀN (CAPABILITY MICROKERNEL)
 TẦNG 0: PHẦN CỨNG ĐA KIẾN TRÚC — x86_64 (BIOS+UEFI) · ARM64 · RISC-V 64
====================================================================================================
```

Xem sơ đồ đầy đủ và trạng thái từng ô ở [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md).

---

## Chạy thử trong 60 giây

Yêu cầu: `g++` ≥ 10 (C++17), `make`, POSIX. Không cần thư viện ngoài, không cần mạng.

```bash
make              # build yuid, libyui.so/.a, yui-profiler, yui-trim, yui-shell
make test         # 705 kiểm thử (C++17 + C11 ABI)
make check        # test + smoke test các CLI
```

Thử ngay:

```bash
./build/bin/yui-profiler              # máy bạn yếu hay mạnh? k/W tự chỉnh là bao nhiêu?
./build/bin/yuid --info               # daemon biết làm gì
./build/bin/yui-shell                 # shell song ngữ Việt–Anh
printf 'gõ telex Tieengs Vieetj\nthoát\n' | ./build/bin/yui-shell
```

```console
$ ./build/bin/yui-profiler
YuiVieOS · Hồ sơ phần cứng
  Kiến trúc : x86_64 (avx512f, 512 bit)
  CPU       : Intel(R) Xeon(R) Processor @ 2.60GHz — 2 luồng / 1 nhân
  RAM       : 3.8 GB (trống 3.6 GB)
  Phân loại : may-yeu
  Tự chỉnh  : k=2, cửa sổ=1024, luồng=2 (may-yeu)
  Ghi chú   : Máy yếu / pin thấp: giảm k xuống 2 và cửa sổ ngữ cảnh 1024 để giữ RAM và nhiệt.

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

## Trong repo có gì (và chưa có gì)

Đây là một repo **chạy được thật**, không phải bản mô tả. Mọi thứ trong cột
"Đã có" đều được biên dịch và kiểm thử tự động; mọi thứ ở cột "Chưa có" đều
được ghi rõ là chưa có, kèm đặc tả để làm tiếp.

| Tầng | Thành phần | Trạng thái | Bằng chứng |
|---|---|---|---|
| 0 | Profiler phần cứng (CPU/RAM/GPU/pin/nhiệt → máy yếu/mạnh) | ✅ chạy | `tools/profiler`, `core/src/kernel/profile.cpp` |
| 0 | Stub khởi động + linker script x86_64/ARM64/RISC-V | 🟡 đặc tả | `arch/*/` |
| 1 | Vi nhân năng lực: bảng capability, dẫn xuất, thu hồi | ✅ Rust | `crates/yui-cap` |
| 1 | Thẻ năng lực ký HMAC-SHA-256 (khớp C++ ↔ Rust) | ✅ hai phía | `crates/yui-cap/src/lib.rs`, `core/src/sandbox/sandbox.cpp` |
| 1 | SHA-256 / HMAC / SHAKE-256 / scrypt tự viết, không lib ngoài | ✅ có vector chuẩn | `core/src/crypto/`, `core/tests/test_crypto.cpp` |
| 2 | Chính sách lập lịch LAVD (P/E-core, ưu tiên độ trễ, công bằng) | ✅ Rust | `crates/yui-sched` |
| 2 | ZRAM: codec YUI-LZ + kho trang nén + thống kê | ✅ Rust + C++ | `crates/yui-zram`, `core/src/codec.cpp` |
| 2 | Lượng tử hoá q4/t2 + nhân SIMD (AVX2/NEON/RVV) | ✅ C++ | `core/src/kernel/engine.cpp` |
| 2 | Tự chỉnh k/W theo RAM, nhiệt, pin, GPU | ✅ C++ | `engine::auto_tune` |
| 3 | Hộp cát 3 bậc (native/isolated/microvm) + audit | ✅ C++ | `core/src/sandbox/` |
| 3 | Chính sách mẫu cho app Yui, trim, game cũ | ✅ có test | `sandbox/policies/*.json` |
| 3 | Manifest micro-VM (JSON Schema) | 🟡 đặc tả | `sandbox/vmm/` |
| 3 | VMM thật (KVM/crosvm), lớp dịch Windows, Waydroid | ❌ chưa có | roadmap |
| 4 | `yuid` daemon: socket Unix, JSON-RPC 2.0 | ✅ chạy | `core/src/daemon/` |
| 4 | MCP server (Content-Length framing, tools/list, tools/call) | ✅ chạy | `tools/list` 8 công cụ |
| 4 | `libyui.so` / `.a` + header C, kiểm thử từ **C11** | ✅ chạy | `core/include/yui/yui.h`, `core/tests/test_capi.c` |
| 4 | Đồ thị tri thức (thêm/nối/tìm/đường đi/BFS/JSON) | ✅ chạy | `core/src/kernel/graph.cpp` |
| 4 | Kho mã hoá: scrypt + SHAKE-256 + HMAC (encrypt-then-MAC) | ✅ chạy | `core/src/crypto/vault.cpp` |
| 4 | Trọng số mô hình/engine suy luận thật | ❌ chưa có | cần bundle ngoài |
| 5 | Bộ gõ Telex **và** VNI, đặt dấu đúng chính tả | ✅ 84 test | `core/src/kernel/ime.cpp` |
| 5 | Shell song ngữ Việt–Anh (mọi lệnh có 2 tên) | ✅ chạy | `core/src/daemon/shell.cpp` |
| 5 | Giao diện thích ứng: chọn framebuffer/Vulkan theo phần cứng | ✅ logic + test | `core/src/kernel/ui.cpp` |
| 5 | Trình gọt tỉa & nén app (.yuip) | ✅ chạy | `core/src/kernel/apppack.cpp`, `tools/trim` |
| 5 | Bộ vẽ framebuffer/Vulkan thật, app Flutter Yui | ❌ chưa có | roadmap |

Ghi chú về chữ **Zstd** trong sơ đồ: codec mặc định của repo là **YUI-LZ** tự
viết (không phụ thuộc), đã đạt >3× trên văn bản lặp và chạy được ngay sau khi
cài. Backend Zstd là tuỳ chọn (`Codec::Zstd` đã dành chỗ) cho trang lạnh; xem
[`docs/FORMAT-YUIP.md`](docs/FORMAT-YUIP.md).

---

## Cấu trúc repo

```
core/
  include/yui/      API công khai: crypto, codec, vault, graph, engine,
                    sandbox, service, server, shell, apppack, ime, text, ui, yui.h (C ABI)
  src/crypto/       SHA-256, HMAC, SHAKE-256, scrypt, vault
  src/kernel/       graph, engine (q4/t2 + SIMD), text (tiếng Việt), ime, profile,
                    apppack, ui
  src/sandbox/      capability token + hộp cát 3 bậc + audit
  src/daemon/       service (dispatch), server (socket/MCP), shell, main (yuid)
  src/capi/         lớp C-ABI của libyui
  tests/            7 bộ test C++ + 1 bộ test C11
crates/             Rust no_std: yui-cap (Tầng 1), yui-sched, yui-zram (Tầng 2)
tools/              profiler, trim, shell (CLI cho người dùng)
sandbox/policies/   chính sách hộp cát mẫu (có test)
sandbox/vmm/        đặc tả manifest micro-VM
arch/               x86_64, arm64, riscv64 (linker + luồng khởi động)
docs/               kiến trúc, bảo mật, giao thức, định dạng, build, roadmap
```

## Tài liệu

- [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) — từng tầng làm gì, ai gọi ai, vì sao.
- [`docs/SECURITY.md`](docs/SECURITY.md) — mô hình mối đe doạ, mô hình năng lực, giới hạn thật.
- [`docs/PROTOCOL.md`](docs/PROTOCOL.md) — JSON-RPC qua `/run/yuid.sock`, MCP, C-ABI, định dạng token.
- [`docs/FORMAT-YUIP.md`](docs/FORMAT-YUIP.md) — YUI-LZ, container YUIP, kho YUIA, blob vault.
- [`docs/BUILD.md`](docs/BUILD.md) — build, cross-compile, toolchain Rust, CI.
- [`docs/ROADMAP.md`](docs/ROADMAP.md) — cái gì tiếp theo và cần gì để làm.

## Nguyên tắc

1. **Tiếng Việt là công dân hạng nhất**, không phải bản dịch: bộ gõ, tìm kiếm
   không phân biệt hoa/thường có dấu, shell hai tên lệnh, thông báo lỗi tiếng Việt.
2. **Máy yếu không phải phiên bản cắt bớt**: cùng một binary, cùng một chính sách,
   chỉ khác k/W và đường vẽ.
3. **Không hứa những gì chưa kiểm thử**: mỗi tuyên bố trong README này đều trỏ
   tới file và test cụ thể.
4. **Mặc định từ chối**: không có năng lực thì không có quyền, kể cả với app nội bộ.

## Giấy phép

MIT — xem [`LICENSE`](LICENSE). Clean-room: không mã nguồn GPL/closed nào được
sao chép; các thuật toán (SHA-2, SHA-3/SHAKE, scrypt, LZ77, q4) được cài đặt lại
từ đặc tả công khai và đối chiếu với vector kiểm thử chuẩn.
