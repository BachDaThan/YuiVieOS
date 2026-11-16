# YuiVieOS Beta — Bản vẽ kỹ thuật 6 tầng

**Hệ điều hành thuần Việt, tự chủ hoàn toàn** · Clean-Room Design · MIT
Kho: `github.com/BachDaThan/YuiVieOS` (tên sản phẩm: **YuiVieOS Beta**)

Tài liệu này là bản vẽ thi công: mỗi tầng nói **nó chịu trách nhiệm gì**, **ai gọi
ai bằng giao diện nào**, **cái gì đã có trong repo hôm nay** và **cái gì thuộc
phase nào** (xem [`PLAN.md`](PLAN.md)). Mọi khẳng định "đã có" đều kèm đường dẫn
tệp và số kiểm thử; không có tầng nào được mô tả bằng tính từ.

---

## 0. Nguyên tắc kiến trúc

| # | Nguyên tắc | Hệ quả kỹ thuật |
|---|---|---|
| 1 | **Clean-room 100 %** | Không một dòng mã nào từ Linux/seL4/Asterinas. Chỉ dùng *nguyên lý* công khai (capability, microkernel, framekernel) và chuẩn giao tiếp (POSIX, Linux ABI, MCP, JSON-RPC). Giấy phép MIT, không nghĩa vụ kế thừa GPL. |
| 2 | **Rust an toàn cho toàn bộ dịch vụ nhân** | `#![deny(unsafe_code)]` ở mọi crate thuộc Tầng 1–3. Mã giao tiếp phần cứng cấp thấp (asm, MMU, ngắt) bị **cô lập trong một khung duy nhất** (`yui-frame`), có audit riêng, không lan ra dịch vụ. |
| 3 | **Máy đời tống là công dân hạng nhất** | Cùng một binary: profiler chọn `may-yeu` (framebuffer trực tiếp, ZRAM, < 64 MB RAM sau boot) hay `may-manh` (DRM/Vulkan, zero-copy, AVX-512/NEON/RVV). Không có bản "Lite" tách rời. |
| 4 | **Zero-Trust theo mặc định** | Không có root; quyền là thẻ năng lực ký số. App lạ chạy trong micro-VM không có syscall host. |
| 5 | **Đo được thì mới tuyên bố** | Mỗi mục tiêu trong `PLAN.md` có số đo và cách đo. Con số chưa đo được ghi là *mục tiêu*, không phải kết quả. |
| 6 | **AI là khách, không phải nhân** | Lõi AI (`yui.cpp`, `.yui v3`) phát triển độc lập. Hệ điều hành chỉ dựng **cổng chờ** `/ai-bridge-yuid` với hợp đồng ổn định; lõi được cắm vào ở Phase P5, không thể làm sập nhân. |

### Ranh giới "safe Rust" nói chính xác

```
Tầng 1  kernel-cap/          Rust, no_std, deny(unsafe_code)          ✅ hiện có
Tầng 2  kernel-services/     Rust, no_std, deny(unsafe_code)          ✅ hiện có
Tầng 3  vmm-sandbox/         Rust (P3); bản chính sách hiện tại: C++  🟡
Tầng 4  ai-bridge-yuid/      Rust host + C-ABI shim (P4); hiện tại C++  🟡
        + libyui.so          C++ từ Adaptive-MoE — nằm ngoài nhân       (P5)
yui-frame/                   **nơi duy nhất** được có unsafe          (P2)
```

Bản C++ đang chạy trong repo **không phải** nhân: nó là bản tham chiếu của cổng
chờ, dùng để chứng minh hợp đồng trước khi có Rust/P5. Khi `yui-frame` chạy, mọi
thứ trong bảng trên trừ `libyui.so` đều là Rust an toàn. Điều này được ghi lại
thành tiêu chí P2.1 trong `PLAN.md`, không phải lời hứa suông.

---

## 1. Bản đồ 6 tầng

```
┌──────────────────────────────────────────────────────────────────────────────┐
│ TẦNG 5 — TRẢI NGHIỆM NGƯỜI DÙNG VIỆT                          userspace-vi/ │
│  Smart App Stripper (pkg-stripper/) │ Shell VI–EN │ Bộ gõ Telex/VNI tại dịch  │
│  App Yui (Flutter)  │ Giao diện thích ứng: Framebuffer ⇄ Vulkan/DRM          │
└───────────────┬──────────────────────────────────────────────────────────────┘
                │ JSON-RPC /run/yuid.sock · C-ABI libyui.so · MCP (có xác nhận)
┌───────────────▼──────────────────────────────────────────────────────────────┐
│ TẦNG 4 — CỔNG CHỜ DỊCH VỤ TRÍ TUỆ LÕI                 ai-bridge-yuid/       │
│  ① IPC & C-ABI  ② Telemetry Hook  ③ OS Control MCP  ④ nạp .yui v3 (mmap)     │
│  ── đường nối Phase P5 ──► libyui.so (Adaptive-MoE + Y-App), KHÔNG ở repo này │
└───────────────┬──────────────────────────────────────────────────────────────┘
                │ thẻ năng lực + quyết định hộp cát
┌───────────────▼──────────────────────────────────────────────────────────────┐
│ TẦNG 3 — HỘP CÁT ZERO-TRUST (MicroVM/VMM)                    vmm-sandbox/   │
│  native │ isolated │ microvm (Wine/Proton+VKD3D, Waydroid-lite, driver lạ)   │
└───────────────┬──────────────────────────────────────────────────────────────┘
                │ upcall qua cổng năng lực
┌───────────────▼──────────────────────────────────────────────────────────────┐
│ TẦNG 2 — NHÂN DỊCH VỤ (Safe Rust Framekernel)             kernel-services/   │
│  Lập lịch chuyển đổi nóng │ ZRAM (YUI-LZ/Zstd) │ FS bất biến │ DVFS/clock-gate │
│  Full tickless │ Write coalescing │ Linux ABI │ zero-copy I/O                │
└───────────────┬──────────────────────────────────────────────────────────────┘
                │ IPC năng lực (không syscall toàn cục)
┌───────────────▼──────────────────────────────────────────────────────────────┐
│ TẦNG 1 — LÕI GIÁM SÁT BẢO MẬT (Capability Microkernel)          kernel-cap/  │
│  CapTable · Thẻ năng lực ký HMAC · Thu hồi bắc cầu · Sovereign Keyring       │
└───────────────┬──────────────────────────────────────────────────────────────┘
                │ YuiBootInfo (memory map, framebuffer, ACPI/DT, cmdline)
┌───────────────▼──────────────────────────────────────────────────────────────┐
│ TẦNG 0 — PHẦN CỨNG ĐA KIẾN TRÚC                              arch/           │
│  x86_64 (BIOS Legacy + UEFI) │ ARM64 (GICv3/NEON/SVE) │ RISC-V 64 (PLIC/RVV)  │
└──────────────────────────────────────────────────────────────────────────────┘
```

### Ánh xạ thư mục → tầng

| Thư mục | Tầng | Nội dung |
|---|---|---|
| `arch/` | 0 | Luồng khởi động 3 kiến trúc, linker script x86_64 |
| `kernel-cap/` | 1 | Rust: quyền, thẻ, bảng năng lực, SHA-256/HMAC |
| `kernel-services/` | 2 | Rust: `yui-sched`, `yui-zram`; C++ tham chiếu: `codec`, `crypto`, `profile` |
| `vmm-sandbox/` | 3 | Chính sách hộp cát, policy mẫu, schema manifest micro-VM |
| `ai-bridge-yuid/` | 4 | Daemon, JSON-RPC, MCP, C-ABI, telemetry, `reference/` |
| `pkg-stripper/` | 5 | Trình gọt tỉa & nén app |
| `userspace-vi/` | 5 | `text`, `ime`, `ui`, `shell`, `userspace-vi/ui/app` (Flutter) |
| `include/yui/` | 1–5 | API công khai dùng chung + header C `yui.h` |
| `tests/` | 1–5 | 7 bộ test C++17 + 1 bộ test C11 |
| `tools/` | 5 | `yui-profiler`, `yui-trim`, `yui-shell` |
| `docs/` | — | Tài liệu này, `PLAN.md`, `SECURITY.md`, `PROTOCOL.md`, `FORMAT-YUIP.md`, `BUILD.md` |

---

## 2. Tầng 0 — Phần cứng

### 2.1 Luồng khởi động (một `YuiBootInfo` cho cả ba kiến trúc)

```
x86_64 : BIOS/UEFI → limine → yui-boot (32→64 bit) ┐
ARM64  : U-Boot/UEFI → yui-boot (EL2→EL1)          ├→ YuiBootInfo → yui-frame → yui-cap → …
RISC-V : OpenSBI (M-mode) → yui-boot (S-mode)      ┘
```

`YuiBootInfo` chứa: bản đồ bộ nhớ, framebuffer (địa chỉ, sải, định dạng), con trỏ
ACPI/DT, `cmdline`, seed ngẫu nhiên từ phần cứng, và **kết quả profiler** nếu
firmware đã chạy nó. Nhờ vậy Tầng 1 chỉ có *một* đường khởi tạo.

### 2.2 Hardware Auto-Profiler (quyết định hình dạng hệ thống)

Chạy ngay sau khi nhân vào user space, ghi `/var/lib/yui/profile.json`:

| Nhóm | Đọc từ | Dùng để |
|---|---|---|
| CPU | CPUID/`MIDR_EL1`/`mvendorid`, số nhân P/E, tập lệnh | chọn nhân SIMD, lập lịch |
| RAM | bản đồ bộ nhớ, RAM trống | chọn k/W, bật ZRAM mạnh/tắt |
| GPU/NPU | PCI class, DT `compatible`, DRM node | Framebuffer vs Vulkan/DRM |
| Ổ cứng | `/sys/block/*/queue/rotational`, loại NVMe | write coalescing, fsync policy |
| Nguồn | pin, sạc, nhiệt độ | hạ k/W, giới hạn sạc, DVFS |

Phân loại (đã cài đặt, có test):

| Chế độ | Điều kiện | Hệ quả |
|---|---|---|
| `may-yeu` | RAM < 4 GB **hoặc** ≤ 2 luồng | framebuffer trực tiếp, `k=2`, `W=1024`, ZRAM bật, không composite |
| `can-bang` | còn lại | `k=4`, `W=4096`, composite phần mềm |
| `may-manh` | RAM ≥ 16 GB **và** ≥ 8 luồng (hoặc ≥ 16 luồng **và** ≥ 8 GB) | `k=8`, `W=8192`, DRM/Vulkan zero-copy, AVX-512/NEON/RVV |

Máy đời tống (512 MB–2 GB) nằm sâu trong `may-yeu`; mục tiêu **< 64 MB RAM sau
khởi động** là tiêu chí P2.5 trong `PLAN.md` (chưa đo trên phần cứng thật).

### 2.3 Trạng thái

- [x] `profile.cpp` + `yui-profiler` chạy được (Linux host & trong test)
- [x] Phát hiện SIMD `avx2 / avx512f / neon / sve / rvv / scalar`
- [x] Linker script x86_64, luồng khởi động 3 kiến trúc (`arch/*/README.md`)
- [ ] `yui-boot` + `yui-frame` thật — Phase P2

---

## 3. Tầng 1 — Lõi Giám Sát Bảo Mật (Capability Microkernel)

### 3.1 Vì sao capability chứ không phải uid/gid

POSIX dùng *danh tính*: "ai chạy" quyết định "được làm gì". Điều đó nghĩa là một
chương trình chạy bằng bạn có toàn bộ quyền của bạn. YuiVieOS dùng *năng lực*:
"được làm gì trên đối tượng nào" nằm trong một thẻ, và thẻ chỉ có thể bị **thu
hẹp** khi trao cho tiến trình con.

### 3.2 Vật thể

| Khái niệm | Định nghĩa | Cài đặt |
|---|---|---|
| Quyền | 15 tên trên `u64`: `fs.read`…`admin` | `kernel-cap/src/lib.rs` |
| `Capability` | `{object, rights, badge}` — badge phân biệt các bản sao trong audit | `Capability::derive` (giao quyền) |
| `CapTable` | slot ổn định + liên kết cha, thu hồi bắc cầu | `CapTable::revoke` |
| `Token` | dạng truyền đi, ký HMAC-SHA-256 | `Token::canonical` + `Authority` |
| `CapError` / `TokenError` | lỗi có kiểu, không có "false" chung chung | 6 unit test |

### 3.3 Chuỗi chuẩn hoá & xác minh

```
{"sub":"app:yui","iss":"yui-cap","rights":"fs.read,graph,vault",
 "iat":1700000000,"exp":1700003600,"serial":42,"scopes":["fs:/home/user"]}
```

- MAC = `hex(HMAC-SHA-256(key, chuỗi trên))`.
- Bên nhận **dựng lại** chuỗi từ các trường rồi so MAC — không ký lại chuỗi nhận
  được, nên không có lỗ hổng canonicalisation.
- Thứ tự kiểm: `issuer` → `exp` → `iat ≤ now+300` → MAC. `exp = 0` ⇒ không hết hạn.
- So sánh MAC hằng thời gian (`ct_eq`).

### 3.4 Sovereign Keyring & cập nhật ngoại tuyến (`.bundle`)

```
bundle/  manifest.json (danh sách file + SHA-256)  ·  manifest.sig (Ed25519)  ·  payload/
```

Khoá phát hành nằm trong keyring chủ quyền (`/etc/yui/trusted-keys/`), cài một
lần bằng USB và **không** bao giờ lấy từ mạng. Cài đặt: xác minh chữ ký manifest →
kiểm từng SHA-256 sau giải nén → ghi vào phân vùng A/B → đổi con trỏ boot. Không
có mạng, quy trình y hệt.

> **Chưa có Ed25519** trong repo (không thư viện ngoài, tự viết đường cong là rủi
> ro cao). Đây là P2.4 và **không** được coi là an toàn trước khi có vector RFC
> 8032. Ghi rõ trong [`SECURITY.md`](SECURITY.md) §6.

### 3.5 Trạng thái

- [x] Quyền, dẫn xuất, thu hồi, kiểm quyền (Rust + bản C++ đối chiếu)
- [x] Thẻ ký HMAC, xác minh có kiểu lỗi, so sánh hằng thời gian
- [x] SHA-256/HMAC tự viết + vector RFC
- [ ] Sovereign Keyring + Ed25519 (P2.4)
- [ ] IPC năng lực ở mức nhân (P2.1)

---

## 4. Tầng 2 — Nhân Dịch Vụ (Safe Rust Framekernel)

### 4.1 Framekernel là gì

Một khung duy nhất (`yui-frame`) giữ toàn bộ `unsafe`: chuyển ngữ cảnh, bảng
trang, xử lý ngắt, truy cập MMIO. Trên khung đó, mọi dịch vụ là Rust an toàn gọi
hàm trực tiếp (**không** syscall, không copy qua biên). Đây là mô hình
framekernel: hiệu năng gần monolithic, an toàn bộ nhớ gần microkernel.

### 4.2 Bộ lập lịch chuyển đổi nóng (Programmable Task-Aware Scheduler)

Ba lớp tác vụ, chính sách hot-swap được vì nó là crate Rust an toàn:

| Lớp | Trọng số | Slice | Ái lực | Dùng cho |
|---|---|---|---|---|
| `Interactive` | 3 | 1 ms | P-core | game, âm thanh, nhập liệu |
| `AiRender` | 2 | 8 ms | P-core (ghim) | suy luận cục bộ, render |
| `Batch` | 1 | 4 ms | E-core | biên dịch, tải, index |

Thứ tự chọn: **ái lực lớp** → **tín dụng đánh thức** (tác vụ tương tác vừa tỉnh
được ưu tiên một lượt, bù `wakeup_boost_ns`) → **vruntime nhỏ nhất** → id nhỏ nhất
(tất định để so sánh trace). Công bằng: 4 tác vụ cùng lớp chia đều ±20 % qua 400
lượt chọn (có test). `scale_slice(class, W)` co slice theo cửa sổ ngữ cảnh.

Đồng bộ tầng nhân kiểu **NTSYNC** (P2.2): đối tượng chờ/đánh thức nằm trong nhân
thay vì giả lập qua server userspace, để game Windows dưới Wine không bị "khựng"
do mỗi lần chờ phải qua nhiều lần chuyển ngữ cảnh.

### 4.3 ZRAM & bộ nhớ

- Codec **YUI-LZ** (64 KiB cửa sổ, token nibble) — byte-identical giữa Rust
  (`kernel-services/yui-zram/src/lz.rs`) và C++ (`kernel-services/codec.cpp`).
- Kho trang có **fallback verbatim**: trang không nén được thì lưu nguyên, không
  bao giờ phình.
- Backend **Zstd** là tuỳ chọn cho trang lạnh (`Codec::Zstd` đã dành chỗ); mặc
  định không phụ thuộc thư viện ngoài để hệ thống khởi động được trên máy trắng.
- Mục tiêu: ≥ 2,5× trên dữ liệu văn phòng, đọc lại đúng từng byte (tiêu chí P1.2).

### 4.4 Năng lượng: tickless + DVFS + clock gating

| Cơ chế | Việc | Trạng thái |
|---|---|---|
| Full tickless | khi rảnh, không có timer định kỳ; chỉ đánh thức theo deadline | đặc tả, P2.5 |
| Adaptive DVFS | mỗi nhân có OPP riêng, chọn theo tải + nhiệt + chế độ máy | chính sách có trong profiler/engine; driver P2.5 |
| Clock gating | tắt xung nhịp khối rảnh (GPU, NPU, USB) | P2.5 |
| Giới hạn sạc | dừng sạc ở 85 % khi người dùng chọn | `os.power` trả chính sách, driver P2.5 |

Con số **35–54 % tiết kiệm điện** trong yêu cầu là *mục tiêu cần đo*, không phải
kết quả đã đo — nó phụ thuộc phần cứng. `PLAN.md` P2.5 quy định cách đo (công
suất gói qua RAPL/tương đương, cùng workload, so với governor mặc định).

### 4.5 Hệ thống tệp bất biến & write coalescing

- Lõi hệ thống **chỉ đọc** (squashfs/erofs hoặc ảnh đã ký): không thể sửa nhân
  bằng một `sudo` lỡ tay; cập nhật là thay ảnh có chữ ký (A/B).
- Dữ liệu người dùng ở phân vùng riêng, ghi qua lớp **coalescing**: gom trang nhỏ
  thành lần ghi lớn, fsync theo chu kỳ, để mất điện không mất dữ liệu đã báo
  "đã lưu" và giảm số lần ghi lên SSD/eMMC.
- Hiện có: `pack` ghi nguyên tử (`graph.json` qua `.tmp` → `fsync` → `rename`).

### 4.6 Tương thích Linux ABI

Ứng dụng Linux chạy được mà không cần biên dịch lại: lớp chuyển `open/read/mmap/
futex/epoll/io_uring-ish` sang cổng năng lực của YuiVieOS. Đây là **bề mặt lớn**
nhất của dự án; `PLAN.md` P2.7 chia nhỏ theo nhóm syscall và đặt tiêu chí "chạy
được `busybox`, `coreutils`, `python3`" trước khi nghĩ tới desktop.

### 4.7 Trạng thái

- [x] `yui-sched` (9 test) và `yui-zram` (8 test) — Rust an toàn
- [x] Codec đối xứng Rust ↔ C++, container YUIP có SHA-256 + CRC32
- [x] Chính sách k/W và phân loại máy
- [ ] `yui-frame`, tickless, DVFS, FS bất biến, Linux ABI — Phase P2

---

## 5. Tầng 3 — Hộp Cát Zero-Trust (VMM)

### 5.1 Ba bậc

| Bậc | Dùng cho | Được phép |
|---|---|---|
| `native` | dịch vụ nội bộ đã ký | mọi năng lực trong thẻ |
| `isolated` | app bên thứ ba | `vault`, `graph`, `model`, `net.connect`, `power`, `gpu`, `fs` **trong scope** |
| `microvm` | app Windows/Android, driver lạ | `vault`, `graph`, `model`, `net.connect`, `power`, `gpu` — **không** syscall host, **không** `fs`, **không** `input`, **không** `spawn` |

Quy tắc thực thi (đã có, 129 check):

1. `deny` thắng tuyệt đối, và có thể là tên quyền (`"deny": ["input"]`).
2. Bậc là **trần cứng**: micro-VM không có `fs.*` dù thẻ nói gì.
3. Phạm vi `fs`/`net` là danh sách cho phép; quyền boolean do việc cấp quyền quyết định.
4. So đường dẫn theo **thành phần** (`/home/user` không bao `/home/user2`), `..`
   được chuẩn hoá trước khi so.
5. Mọi từ chối trả `reason` tiếng Việt + ghi audit.

### 5.2 MicroVM

Manifest (`vmm-sandbox/vmm/vm-manifest.schema.json`) trả lời bốn câu: chạy gì, được
bao nhiêu RAM/CPU, nối ra ngoài bằng gì (mặc định `network: none`), được cấp quyền
gì qua `yuid`. VMM thật (KVM/crosvm) là Phase P3; điểm nối đã cố định nên không
phải sửa tầng trên khi cắm vào.

### 5.3 Lớp tương thích

- **Windows**: Wine/Proton + VKD3D-Proton/DXVK trong micro-VM, đồng bộ NTSYNC của
  Tầng 2 để giảm khựng giật.
- **Android**: Waydroid-lite, mạng chỉ qua `net.connect` của `yuid`.
- **Driver lạ**: chạy trong micro-VM riêng; hỏng driver chỉ làm chết VM đó.

---

## 6. Tầng 4 — Cổng chờ `/ai-bridge-yuid`

> **Đây là phần quan trọng nhất của tài liệu này**, vì nó là hợp đồng mà lõi AI
> (`Adaptive-MoE`, `Y-App`) phải tuân khi được cắm vào ở Phase P5.

### 6.1 Ranh giới trách nhiệm

| Thuộc về YuiVieOS Beta (repo này) | Thuộc về lõi AI (repo ngoài) |
|---|---|
| Socket, framing JSON-RPC, mã lỗi | Số học mô hình, lượng tử hoá q4/t2/BITCOS |
| C-ABI ổn định (`yui.h`), quản lý bộ nhớ đệm ra | Đọc/giải mã trọng số `.yui v3` |
| Telemetry (pin/nhiệt/RAM) và khuyến nghị k/W | Quyết định dùng k/W thế nào |
| Kiểm quyền, hộp cát, audit, xác nhận người dùng | Suy luận, agent, prompt |
| Đồ thị/khóa tham chiếu (tạm) | `yui_graph`, `yui_vault` thật |

Nguyên tắc: **nhân không bao giờ gọi thẳng lõi AI**, và lõi AI không bao giờ có
quyền hệ điều hành trừ khi đi qua công cụ MCP có xác nhận.

### 6.2 ① IPC & C-ABI

**Unix socket** `/run/yuid.sock`, `SOCK_STREAM`, quyền `0600`, một đối tượng
JSON-RPC 2.0 mỗi dòng:

```console
$ printf '{"jsonrpc":"2.0","id":1,"method":"yuid.profile"}\n' | nc -U /run/yuid.sock
{"jsonrpc":"2.0","id":1,"result":{"class":"may-manh","k":8,"window":8192,…}}
```

**C-ABI** `libyui.so` / `libyui.a` (`include/yui/yui.h`, `YUI_ABI_VERSION 1`):

- Không trạng thái: không `open/close`, chỉ hàm thuần + `yui_graph*`.
- Không ngoại lệ C++, không `std::string` qua biên; bộ đệm do người gọi cấp.
- Mã lỗi âm cố định `YUI_E_*`; hàm trả chuỗi trả **độ dài byte** (≥ 0).
- Kiểm từ C11: `tests/test_capi.c` (36 check).

**Nạp mô hình `.yui v3`**: `libyui` `mmap` trực tiếp tệp (không copy vào heap),
bridge chỉ chuyển `(path, offset, length, flags)`; trọng số không bao giờ đi qua
socket. Header `.yui v3` do repo AI định nghĩa — bridge coi là **mờ** (opaque) và
chỉ kiểm: tệp tồn tại, kích thước khớp, chữ ký/hash nếu có, quyền `fs.read` trên
đúng đường dẫn đó.

### 6.3 ② Telemetry Hook

```json
// JSON-RPC: telemetry.read
{"battery":{"present":true,"percent":37,"charging":false},
 "thermal":{"cpu_c":71.5,"gpu_c":64.0,"throttle":true},
 "memory":{"total_mb":1843,"free_mb":402,"zram_ratio":2.7},
 "machine":{"class":"may-yeu","simd":"avx2","threads":2},
 "advice":{"k":1,"window":16,"reason":"Pin thấp và nhiệt cao: giảm k/W để giữ máy mát"}}
```

C-ABI tương ứng:

```c
typedef struct yui_telemetry {
  int battery_percent;   /* -1 nếu không có pin */
  int charging;
  float cpu_c, gpu_c;
  int ram_free_mb, ram_total_mb;
  char machine_class[16]; /* may-yeu | can-bang | may-manh */
  int advised_k, advised_window;
} yui_telemetry;
int yui_telemetry(yui_telemetry* out);
```

Quy tắc khuyến nghị (bridge quyết, lõi AI tuân hoặc ghi đè có lý do):

| Điều kiện | k | W |
|---|---|---|
| Máy yếu **hoặc** pin < 20 % **hoặc** nhiệt > 75 °C | 1 | 16 |
| Máy cân bằng, không sạc | 2 | 1024 |
| Cắm sạc **và** máy mạnh | 2–4 (tối đa 8 nếu `may-manh` + mát) | 4096–8192 |

Bridge **không** tự đổi cấu hình mô hình; nó công bố sự thật phần cứng và khuyến
nghị, để lõi AI — nơi hiểu chi phí suy luận — quyết định.

### 6.4 ③ OS Control MCP (có phân quyền & xác nhận)

`yuid --mcp` nói MCP `2024-11-05` trên stdio (khung `Content-Length:`). Ngoài 8
công cụ dữ liệu hiện có, bridge **dành sẵn** 5 công cụ điều khiển hệ điều hành:

| Công cụ | Việc | Quyền cần | Xác nhận |
|---|---|---|---|
| `sys_power` | đổi governor, giới hạn sạc, chế độ tiết kiệm | `power` | có (trừ khi policy cho phép) |
| `sys_scheduler` | đổi profile lập lịch (game / AI-render / batch) | `admin` **hoặc** `power` | có |
| `sys_app_stripper` | gọt & nén một app, hoặc bỏ gọt | `fs.write` + `fs.read` trong scope | có (hiện báo cáo trước) |
| `sys_proc` | liệt kê/dừng tiến trình trong hộp cát | `admin` | có |
| `sys_search_vault` | tìm trong kho mã hoá (không trả plaintext nếu chưa mở khoá) | `vault` | không |

Quy tắc bắt buộc:

1. **Không có công cụ nào tự chạy**: mỗi lời gọi mang token; thiếu quyền ⇒ từ chối
   kèm lý do tiếng Việt.
2. **Mặc định cần xác nhận**: `tools/call` phải kèm `confirm: true` do người dùng
   bấm; thiếu ⇒ trả `needs_confirmation: true` + mô tả hành động.
3. **Audit bắt buộc**: mỗi hành động ghi `audit.log` (thời gian, chủ thể, hành
   động, đối số, kết quả).
4. **Tiếng Việt cả hai chiều**: tham số nhận cả chuỗi có dấu và không dấu
   (`tắt mạng`, `tat mang`, `disable network`), chuẩn hoá bằng `text::fold`.

### 6.5 Trạng thái & đường nối P5

- [x] Socket JSON-RPC, 25+ phương thức, mã lỗi, `--serve-once` cho test
- [x] MCP 8 công cụ dữ liệu (`graph_*`, `ime_convert`, `vault_seal`, `sandbox_check`, `web_search`, `system_profile`)
- [x] C-ABI 1.0, kiểm từ C11
- [x] Cài đặt tham chiếu `graph/vault/engine` + 102 check RPC
- [ ] `yui_telemetry` C-ABI + `telemetry.read` (P4.2)
- [ ] 5 công cụ OS MCP + xác nhận người dùng (P4.3)
- [ ] Nạp `.yui v3` qua `mmap` + kiểm quyền (P5 bước 4)
- [ ] Thay `reference/` bằng `libyui.so` thật (P5)

Chi tiết từng bước cắm AI: [`PLAN.md`](PLAN.md) §P5.

---

## 7. Tầng 5 — Trải nghiệm người dùng Việt

### 7.1 Smart App Stripper (đã có, 61 check)

Xem `pkg-stripper/README.md`. Điểm quan trọng về ***chủ quyền***: nếu app có dấu
hiệu DRM/anti-tamper, stripper **không sửa một byte** và báo `stripped=false`.
Người dùng luôn thấy dung lượng trước/sau và lý do.

### 7.2 Bộ gõ tại tầng dịch vụ (Telex/VNI + adapter IBus)

Telex **và** VNI, đặt dấu theo chính tả hiện đại (`hoa` + `f` → `hòa`), không phá
chữ ASCII (`Xin` không thành `Xín`). Hàm chuyển đổi thuần không qua IPC; buffer
composition hỗ trợ preedit/backspace/commit, tự khôi phục một allowlist nhỏ trong
Telex/VNI và Escape commit nguyên văn mọi chuỗi phím. Adapter IBus đăng ký `yui-telex`
và `yui-vni` (`userspace-vi/ibus/`); kiểu trường mật khẩu/PIN/số/URL/email đi qua
nguyên trạng. Người dùng báo đã build/đăng ký trên Ubuntu 24.04 và đo D-Bus trung
bình 7.8 ms/phím (min 7.15, max 10.25). Chưa có xác nhận gõ trong **cả Firefox
và LibreOffice**; xem P1.4.

### 7.3 Shell song ngữ (đã có, 15 lệnh)

Mỗi lệnh hai tên: `giúp/help`, `thôngtin/info`, `gõ/ime`, `ghinhớ/remember`,
`gọttỉa/trim`, `hộpcát/sandbox`, `niêmphong/seal`… Shell là client của `yuid`,
không phải một đống `if`.

### 7.4 Giao diện thích ứng (logic đã có, bộ vẽ chưa)

`ui::plan()` trả `Framebuffer` / `SoftwareComposite` / `GpuZeroCopy` theo phần cứng
thật, tỉ lệ điểm ảnh nguyên (1×/2×/3×) để chữ Việt có dấu không bị nhoè. Bộ vẽ
thật và compositor là P2.6/P4.

### 7.5 App Yui

Khung sườn Flutter (`userspace-vi/ui/app`): 3 màn hình, nói chuyện với `yuid` qua
Unix socket. **Chưa build** trong CI (không có Flutter SDK) — ghi rõ trong
`userspace-vi/ui/README.md`.

---

## 8. Hai luồng dữ liệu tiêu biểu

### 8.1 Người dùng gõ một câu lệnh tiếng Việt

```
"ghinhớ os:yui "hệ điều hành Việt" concept"
  │ shell.cpp: fold() so tên lệnh (có dấu/không dấu/hoa-thường), tách tham số
  ▼
Request{id, method:"graph.upsert", params, token}
  │ Service::handle: 1) token hợp lệ?  2) Enforcer::check({kind:"graph"})
  ▼                                  3) Graph::upsert
Reply{ok, result}  ──► terminal  (+ audit nếu bị từ chối)
```

### 8.2 Trợ lý AI điều khiển hệ điều hành (P5)

```
Người dùng: "tắt chế độ hiệu năng cao, tiết kiệm pin"
  │ agent (Y-App) → MCP tools/call sys_power {mode:"tiet_kiem", confirm:true}
  ▼
yuid bridge: 1) token có quyền "power"?  2) policy cho phép mode này?
  │          3) ghi audit  4) gọi dịch vụ năng lượng của Tầng 2
  ▼
Reply: {"ok":true,"applied":"governor=powersave, charge_limit=85%"}
```

Nếu thiếu `confirm`, bridge trả `needs_confirmation` + câu tiếng Việt mô tả tác
động; agent phải hỏi người dùng. **Lõi AI không bao giờ có đường tắt.**

---

## 9. Những gì đã kiểm chứng (2026-11-16)

```console
$ make test
✓ test_ime 246 checks; toàn bộ suite: 2831 checks (2770 C++17 + 61 C11), 0 failed

$ make check
✓ test suite + scripts/verify.sh + CLI smoke runs
all checks passed
```

Adapter IBus, callback ABI `guint` và metadata Telex/VNI hiện có; XML được kiểm tĩnh.
Sandbox này vẫn thiếu `pkg-config`/`libibus-1.0-dev`, nên chưa build lại adapter tại
đây. Người dùng báo Ubuntu 24.04 đã build/đăng ký sau khi áp dụng bản sửa callback và đo
D-Bus avg 7.8 ms/phím (min 7.15, max 10.25); đây là kết quả bên ngoài, chưa tái
lập. Chưa có báo cáo Firefox **và** LibreOffice; vì max vượt 10 ms nếu DoD tính
worst-case, P1.4 vẫn chưa được nhận hoàn tất. Cũng chưa kiểm chứng Rust/QEMU/Flutter
ở môi trường cục bộ này.

---

## 10. Quyết định thiết kế & đánh đổi

| Quyết định | Đánh đổi | Vì sao vẫn chọn |
|---|---|---|
| Capability thay vì POSIX uid/gid ở nhân | Phải viết lớp Linux ABI riêng (P2.7) | Zero-Trust thật; app cũ vẫn chạy qua lớp ABI |
| Chính sách lập lịch bằng Rust, hot-swap | Thêm một lớp gián tiếp | Đo và thay thuật toán mà không reboot; test không cần máy thật |
| Codec tự viết thay Zstd mặc định | Tỉ lệ nén thấp hơn Zstd ở mức cao | Không phụ thuộc phiên bản; byte-identical Rust↔C++; Zstd vẫn là tuỳ chọn |
| AI là tiến trình ngoài (bridge) | Tốn một lần qua socket | Nhân không thể bị lõi AI làm sập; thay mô hình không cần build lại HĐH |
| Tài liệu song ngữ, Việt trước | Tốn công viết | Đây là hệ điều hành Việt, không phải bản dịch |
| Ghi rõ 7 điều chưa bảo vệ (`SECURITY.md` §7) | Trông "yếu" hơn marketing | Người dùng cần biết giới hạn thật để không mất dữ liệu |
