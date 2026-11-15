# Build — tự biên dịch & kiểm thử

## 1. Yêu cầu tối thiểu

| Thành phần | Bắt buộc | Ghi chú |
|---|---|---|
| `make` (GNU) | có | chỉ dùng biến thường, không có gì đặc biệt |
| trình biên dịch C++17 | có | `g++ ≥ 10` hoặc `clang++ ≥ 12` |
| trình biên dịch C11 | để chạy `test_capi` | `cc`, cùng máy |
| `rustc`/`cargo` ≥ 1.70 | tuỳ chọn | chỉ cần cho `` (Tầng 1–2) |
| libzstd, GTK, Qt | **không** | repo không cần thư viện ngoài nào |

Đã kiểm trên Debian 12 / g++ 12.2.0 / glibc 2.36 (x86_64). Không dùng GNU
extension nào ngoài `-D_GNU_SOURCE` (cho `strdup`, socket).

---

## 2. Build cơ bản

```bash
git clone <repo> YuiVieOS && cd YuiVieOS
make -j"$(nproc)"        # → build/bin/{yuid,libyui.so,libyui.a,yui-profiler,yui-trim,yui-shell}
make test                # 705 kiểm thử (669 C++17 + 36 C11 ABI) — xem mục 4
make check               # test + smoke test 4 CLI
```

Biến thường dùng:

```bash
make CXX=clang++                 # đổi trình biên dịch
make CXXFLAGS="-std=c++17 -O2"   # ghi đè (nhớ giữ -std và -fPIC)
make BUILD=/tmp/yui-build        # thư mục trung gian khác
make PREFIX=$HOME/.local install # cài vào $HOME/.local/{bin,lib,include}
make DESTDIR=/tmp/pkg install    # đóng gói
make clean
```

Cờ mặc định (giữ nguyên nếu bạn tự đặt `CXXFLAGS`):

```
-std=c++17 -O2 -g -Wall -Wextra -Wno-unused-parameter
-Iinclude -Itests -Ikernel-services -Iuserspace-vi -D_GNU_SOURCE -fPIC
```

`-fPIC` là bắt buộc vì `libyui.so` được link từ cùng object với `libyui.a`.

### Cài đặt

```
$PREFIX/bin/{yuid,yui-profiler,yui-trim,yui-shell}
$PREFIX/lib/libyui.so.1, libyui.a, libyui.so → libyui.so.1
$PREFIX/include/yui/{yui.h,crypto.hpp,…}
```

---

## 3. Rust (Tầng 1 & 2)

```bash
make rust      # cargo test --workspace, rồi build release
```

hoặc thủ công:

```bash
cd kernel-cap   && cargo test
cd kernel-services/yui-sched && cargo test
cd kernel-services/yui-zram  && cargo test
```

Ba crate đều `#![no_std]` + `alloc` khi **không** ở chế độ `test`, và dùng `std`
khi chạy test (để có `Vec`, `format!`, harness). `#![deny(unsafe_code)]` ở cả ba:
nếu bạn thêm `unsafe`, build sẽ **thất bại** — đó là chủ ý, không phải tai nạn.

Ràng buộc:

- `panic = "abort"`, `lto = true`, `codegen-units = 1` trong profile release
  (xem `Cargo.toml`) — hợp với mã nhân, nơi không có unwinding.
- Không có dependency ngoài. `Cargo.lock` được commit cho tái lập.
- MSRV: 1.70 (do dùng `let … else` và `core::cmp` ổn định từ lâu).

Nếu máy bạn không có Rust, mọi thứ C++ vẫn build và test được; `make rust` chỉ
báo "cargo không có trong PATH" và thoát khác 0.

---

## 4. Kiểm thử

```bash
make test                          # tất cả
./build/obj/test/test_core         # một bộ (dùng YUI_TEST_MAIN, in ✓/FAIL từng check)
./build/obj/test/test_sandbox -v   # (tuỳ bộ) in cả check thành công
```

| Bộ | Số check | Kiểm gì |
|---|---|---|
| `test_crypto` | 28 | SHA-256/HMAC/SHAKE/scrypt với vector RFC, vault tròn vẹn, sai khoá/hỏng tag |
| `test_codec_json` | 102 | YUI-LZ tròn vẹn, biên kích thước, JSON parse/serialize, CRC/SHA |
| `test_core` | 163 | đồ thị, engine q4/t2 + SIMD, text tiếng Việt, profiler, apppack |
| `test_sandbox` | 129 | bảng năng lực, thu hồi, token, hộp cát 3 bậc, **4 chính sách mẫu** |
| `test_ime` | 84 | Telex, VNI, đặt dấu, chữ ASCII không bị phá |
| `test_daemon` | 102 | JSON-RPC, MCP, vault qua RPC, web offline, os.* |
| `test_tools` | 61 | `yui-trim` tròn vẹn + chống zip-slip, strip ELF, ui::plan, shell |
| `test_capi` (C11) | 36 | toàn bộ bề mặt `libyui` gọi từ C thuần, kể cả `YUI_E_TOO_SMALL` và hộp cát |

Test dùng `YUI_SOURCE_DIR` (Makefile chèn `-DYUI_SOURCE_DIR='"$(CURDIR)"'`) để
nạp chính sách thật trong `vmm-sandbox/policies/` — nhờ vậy tài liệu và mã không thể
lệch nhau mà test vẫn xanh.

### Chạy test trong môi trường tối thiểu

Không cần mạng, không cần root, không cần `/run`. `test_daemon` tạo socket trong
thư mục tạm; `test_tools` tạo cây thư mục tạm và tự dọn.

---

## 5. Cross-compile (Tầng 0, chưa có trong repo)

Khi bắt đầu viết `yui-boot`/`yui-kernel`, cần toolchain đích:

```bash
# x86_64
rustup target add x86_64-unknown-none
cargo build -p yui-kernel --target x86_64-unknown-none --release

# ARM64
rustup target add aarch64-unknown-none

# RISC-V 64
rustup target add riscv64gc-unknown-none-elf
```

Liên kết với script trong `arch/`:

```bash
ld.lld -T arch/x86_64/linker.ld -nostdlib -o yui-kernel.elf <objects>
```

Cần thêm: `limine` (ISO ảnh khởi động), `qemu-system-x86_64` (thử), `xorriso`.
Không có trong sandbox phát triển hiện tại — xem `docs/PLAN.md`.

---

## 6. CI

`.github/workflows/ci.yml` chạy hai job song song:

1. **cpp** — Ubuntu: `make -j`, `make test`, `make check`, cài vào `$RUNNER_TEMP`
   rồi biên dịch một chương trình C nhỏ chống lại `libyui.a` để chắc header C
   vẫn dùng được từ ngoài.
2. **rust** — Ubuntu + `cargo` có sẵn: `cargo test --workspace`, `cargo fmt
   --check`, `cargo clippy -- -D warnings` (clippy chỉ cảnh báo, không chặn).

Cả hai chạy trên mọi push và PR. Không job nào cần mạng sau bước checkout.

---

## 7. Sự cố thường gặp

| Triệu chứng | Nguyên nhân | Cách sửa |
|---|---|---|
| `undefined reference to lroundf` khi link C | thiếu `-lm` sau `-lstdc++` | dùng `$(LDLIBS)` (đã có `-lm`), đừng viết danh sách lib tay |
| `-Wstringop-overflow` trong `server.cpp` | đọc 1 byte vào `char` | đã sửa: dùng đệm 512 byte |
| `error: 'Edge' was not declared` | viết `Graph::Edge` ngoài lớp | viết `Edge` (xem `graph.cpp`) |
| test fail vì kích thước gói | dữ liệu nhỏ hơn header 78 byte | dùng ≥ 1 KiB dữ liệu lặp |
| `cargo: not found` | chưa cài Rust | `make` (C++) vẫn đủ; cài rustup nếu cần phần Rust |
| test_sandbox không tìm thấy policy | thiếu `-DYUI_SOURCE_DIR` | dùng `make test`, đừng tự biên dịch tay |
