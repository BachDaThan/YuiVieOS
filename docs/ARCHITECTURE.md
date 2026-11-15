# Kiến trúc YuiVieOS — Architecture

Tài liệu này đi từ trên xuống: người dùng thấy gì (Tầng 5), nó gọi ai (Tầng 4),
ai bảo vệ nó (Tầng 1–3), và chạy trên cái gì (Tầng 0). Mỗi mục ghi rõ **đã có**
hay **chưa có**, kèm đường dẫn mã nguồn.

```
           người dùng
               │
   ┌───────────▼───────────┐
   │ Tầng 5  giao diện      │  shell · bộ gõ · gọt tỉa app · bộ vẽ
   └───────────┬───────────┘
               │ JSON-RPC / C-ABI (năng lực)
   ┌───────────▼───────────┐
   │ Tầng 4  yuid daemon    │  engine · graph · vault · MCP · web · OS tools
   └───────────┬───────────┘
               │ capability token + hộp cát
   ┌───────────▼───────────┐
   │ Tầng 3  hộp cát        │  native · isolated · microvm
   └───────────┬───────────┘
               │ syscall/upcall
   ┌───────────▼───────────┐
   │ Tầng 2  frame kernel   │  lập lịch · ZRAM · I/O · DVFS · RAM-COW
   └───────────┬───────────┘
               │ IPC năng lực
   ┌───────────▼───────────┐
   │ Tầng 1  vi nhân        │  bảng năng lực · ký thẻ · IPC · profiler
   └───────────┬───────────┘
               │ boot protocol
   ┌───────────▼───────────┐
   │ Tầng 0  phần cứng      │  x86_64 (BIOS/UEFI) · ARM64 · RISC-V 64
   └───────────────────────┘
```

---

## Tầng 5 — Giao diện & trải nghiệm người dùng Việt

### 5.1 Shell song ngữ (`core/src/daemon/shell.cpp`)

Mỗi lệnh có **hai tên**: một tiếng Việt, một tiếng Anh. Không có chế độ "tiếng
Việt" riêng — cả hai luôn hoạt động, gõ cái nào cũng được.

| Tiếng Việt | English | Việc nó làm |
|---|---|---|
| `giúp` | `help` | danh sách lệnh |
| `thôngtin` | `info` | trạng thái hệ thống |
| `cấuhình` | `profile` | hồ sơ phần cứng + k/W |
| `tinhchỉnh` | `tune` | chỉnh k, W, luồng |
| `gõ` | `ime` | chuyển Telex/VNI → chữ Việt |
| `ghinhớ` | `remember` | thêm nút vào đồ thị tri thức |
| `nối` | `link` | nối hai nút |
| `tìm` | `search` | tìm trong đồ thị |
| `đường` | `path` | đường đi ngắn nhất |
| `thốngkê` | `stats` | thống kê đồ thị |
| `gọttỉa` | `trim` | gọt & nén thư mục app |
| `mởtệp` | `openpack` | mở gói `.yuip` |
| `hộpcát` | `sandbox` | kiểm tra một hành động có được phép |
| `niêmphong` | `seal` | niêm phong kho mã hoá |
| `thoát` | `exit` | thoát |

Shell là **client của daemon**, không phải một đống `if`: mỗi lệnh dựng một
`Request` JSON-RPC rồi gọi `Service::handle`, đúng đường mà `libyui` và MCP dùng.
Nhờ vậy shell có thể chạy trong cùng tiến trình (`yuid --shell`), qua socket, hoặc
trong hộp cát.

Tên lệnh được so khớp **sau khi chuẩn hoá Unicode** (`yui::text::fold`), nên
`thong tin`, `ThôngTin` và `THÔNGTIN` đều như nhau.

### 5.2 Bộ gõ Telex & VNI (`core/src/kernel/ime.cpp`, 84 test)

Yêu cầu thật của tiếng Việt mà các bộ gõ "dịch máy" hay làm sai:

- **Đặt dấu đúng vị trí**: `hoa` + `f` → `hòa` (kiểu hiện đại), `thuý` chứ không
  phải `thúy` khi có `y`; với `ươ` thì dấu đặt trên `ơ`.
- **Telex**: `aa â`, `aw ă`, `ee ê`, `oo ô`, `ow ơ`, `uw ư`, `dd đ`, `w` biến
  `uo` → `ươ` (cả hai chữ), `s sắc`, `f huyền`, `r hỏi`, `x ngã`, `j nặng`,
  `z xoá dấu`.
- **VNI**: `1 sắc`, `2 huyền`, `3 hỏi`, `4 ngã`, `5 nặng`, `6 dấu mũ`, `7 dấu
  móc`, `8 ă`, `9 đ`, `0 xoá dấu`.
- **Không phá chữ ASCII**: gõ `Xin` không được thành `Xín`; `as` ở cuối từ tiếng
  Anh không được biến thành dấu.
- Chỉ chuyển khi byte cuối là chữ ASCII để không mất ký tự trước đó.

Bộ gõ là **hàm thuần**: `(chuỗi, kiểu) → (chữ Việt, số dấu đã áp)`. Nhờ vậy nó
kiểm thử được không cần GUI, và chạy được ở cả tầng dịch vụ (Wayland/X11) lẫn
trong terminal.

### 5.3 Giao diện thích ứng (`core/src/kernel/ui.cpp`)

`ui::plan(Display{width, height, dpi}, profile)` trả về kế hoạch vẽ:

- **Máy yếu / RAM < 4 GB / pin thấp** → `Framebuffer`: vẽ trực tiếp vào
  framebuffer tuyến tính, không composite, không GPU.
- **Máy mạnh + GPU + RAM ≥ 4 GB + SIMD ≥ AVX2/NEON/SVE/RVV** → `GpuZeroCopy`:
  surface Vulkan/DRM chia sẻ bộ nhớ, không copy qua CPU.
- Ở giữa → `SoftwareComposite` (backing store + blit).
- Tỉ lệ điểm ảnh là **số nguyên** (1×, 2×, 3×) để chữ Việt có dấu không bị mờ.

Kết quả là một struct thuần dữ liệu (`Renderer`, `scale_px`, `columns`, `rows`,
lý do chọn) nên test được mà không cần màn hình.

### 5.4 Gọt tỉa & nén app (`core/src/kernel/apppack.cpp`, `tools/trim`)

```
thư mục app ──► 1. phân loại (VCS, node_modules, __pycache__, cache, build, docs, locale lạ)
            ──► 2. strip ELF (.debug_*, .comment, .note*, .gnu_debuglink → zero-fill)
            ──► 3. khử trùng lặp bằng SHA-256 (kind 'd' = trỏ tới bản gốc)
            ──► 4. đóng gói YUIA (đường dẫn tuyệt đối & ".." bị từ chối)
            ──► 5. nén YUI-LZ  ──►  .yuip  (+ SHA-256 để kiểm tra toàn vẹn)
```

- `keep_locales` mặc định `{vi, en}`: nhận diện `locale/`, `locales/`, `lang/`,
  `i18n/`, bỏ phần còn lại — đây thường là 30–60 % dung lượng của app Electron.
- Quét chữ ký số/DRM ("chủ quyền") *trước* khi gọt: file có dấu hiệu bảo vệ được
  đánh dấu và giữ nguyên, không bị cắt.
- Chạy thẳng trên RAM: gói được giải nén vào tmpfs và `exec` từ đó.

### 5.5 Chưa có ở Tầng 5

Bộ vẽ framebuffer/Vulkan thật, đầu vào bàn phím/chuột, app Flutter "Yui".
Hiện tại `ui::plan` + `apppack` + `ime` + `shell` là phần *logic* đã xong và
kiểm thử; phần *vẽ* cần một compositor, xem [`ROADMAP.md`](ROADMAP.md).

---

## Tầng 4 — `yuid`: cổng chờ & dịch vụ trí tuệ lõi

`yuid` là **một tiến trình, ba mặt tiền**:

```
                       ┌──────────────────────────────┐
   shell / app ───────►│                              │
   (JSON-RPC)          │           Service            │──► graph / engine / vault
   libyui.so ─────────►│  dispatch(Request) → Reply   │──► sandbox / ime / profile
   (C-ABI)             │                              │──► web_search / os tools
   MCP client ────────►│                              │
                       └──────────────────────────────┘
                                  ▲
                                  │ /run/yuid.sock (SOCK_STREAM, JSON dòng + Content-Length)
```

### 4.1 `Service` — một hàm dispatch duy nhất

`Service::handle(const Request&) → Reply` là **toàn bộ** bề mặt daemon. Mọi mặt
tiền (socket, C-ABI, MCP, shell) chỉ dựng `Request` rồi gọi hàm này. Lợi ích:

- một chỗ để kiểm tra năng lực, một chỗ để log audit;
- test gọi thẳng `handle` không cần socket;
- thêm mặt tiền mới (D-Bus, HTTP) không phải sửa nghiệp vụ.

### 4.2 Engine (`core/src/kernel/engine.cpp`)

| Thành phần | Việc | Ghi chú |
|---|---|---|
| lượng tử hoá `q4` | 4 bit/trọng số + scale mỗi 32 phần tử | sai số đo được trong test |
| `t2` | 2 bit/trọng số (ternary), cho máy rất yếu | |
| BITCOS | nén trọng số theo khối + cosin nén | tuỳ chọn |
| nhân SIMD | AVX2 / NEON / RVV, có nhánh vô hướng dự phòng | chọn lúc chạy từ `profile` |
| `auto_tune` | k ∈ {2,4,8}, W ∈ {1024,4096,8192} | theo RAM, số luồng, nhiệt, pin |

**Điều quan trọng**: repo này *không* chứa trọng số mô hình. `engine` là bộ máy
số học + chính sách tài nguyên; trọng số đến từ gói `.bundle` (Tầng 1) hoặc do
người dùng cung cấp. Nói cách khác: `yui-trim` nén *app*, `engine` chạy *phép
tính*, còn "trí tuệ" đến từ dữ liệu bạn cài.

### 4.3 Đồ thị tri thức (`graph.cpp`)

Không phải vector DB: đây là **đồ thị có nhãn** (node/edge có `kind`, `weight`,
`props`). Hỗ trợ `upsert`, `link`, `search` (fold, không phân biệt hoa/thường và
dấu), `path` (BFS ngắn nhất), `stats`, `export`/`import` JSON. Dùng làm bộ nhớ
dài hạn cho agent: "máy này pin yếu" → node `device:laptop` với prop.

### 4.4 Kho mã hoá (`vault.cpp`)

```
master  = scrypt(mật_khẩu, salt, N=2^log2_n, r, p, 64)          # chậm có chủ đích
enc_key = master[0..32]
nonce   = 16 byte ngẫu nhiên
body    = plaintext XOR SHAKE-256(enc_key ‖ nonce, len)          # stream cipher
tag     = HMAC-SHA-256(master, header ‖ body)                    # encrypt-then-MAC
blob    = header(salt, nonce, log2_n, r, p, len, tag) ‖ body
```

- `log2_n` mặc định 15 (≈ 32 MiB, ~0,2 s trên máy yếu); test dùng 10 cho nhanh.
- Không có "chế độ tắt xác thực": sai tag ⇒ từ chối, không trả plaintext.
- Đây là **kho cục bộ**, không phải giao thức mạng. Xem giới hạn ở
  [`SECURITY.md`](SECURITY.md).

### 4.5 MCP (`server.cpp`)

`yuid --mcp` nói giao thức Model Context Protocol `2024-11-05` qua stdio với
khung `Content-Length:`, phục vụ 8 công cụ (`yui.info`, `yui.graph.*`,
`yui.vault.*`, `yui.pack.*`, `yui.sandbox.check`, `yui.ime.convert`,
`yui.web.search`, `yui.os.*`). Nhờ vậy một agent bên ngoài (Claude Desktop, IDE,
script) dùng được YuiVieOS như một "bộ công cụ hệ điều hành" mà không cần biết
JSON-RPC nội bộ.

### 4.6 Web & công cụ OS

- `web_search`: nhiều bậc (tìm cục bộ trong đồ thị → cache → HTTP nếu có mạng).
  **Offline là trạng thái bình thường**, không phải lỗi: trả `backend:"offline"`
  kèm gợi ý từ đồ thị.
- `os.battery`, `os.thermal`, `os.power`: đọc từ profiler; trên máy không có pin
  trả `present:false` thay vì bịa số.

### 4.7 C-ABI (`core/include/yui/yui.h`, kiểm thử từ C11)

`libyui` xuất 12 hàm ổn định (`YUI_ABI_VERSION 1`), không có ngoại lệ C++ đi qua
biên, không có `std::string` trong chữ ký. Mã lỗi âm cố định
(`YUI_E_INVALID`…`YUI_E_TOO_SMALL`). `core/tests/test_capi.c` được biên dịch bằng
`cc -std=c11` để chứng minh header dùng được từ C thuần.

---

## Tầng 3 — Hộp cát zero-trust (`core/src/sandbox/sandbox.cpp`)

Ba bậc, mỗi bậc là một *tập con* quyền:

| Bậc | Dùng cho | Được phép |
|---|---|---|
| `native` | dịch vụ nội bộ đã ký | mọi năng lực trong thẻ |
| `isolated` | app bên thứ ba | vault, graph, model, net.connect, power, gpu, fs (chỉ trong scope) |
| `microvm` | app cũ, game, Android | vault, graph, model, net.connect, power, gpu — **không** syscall host, không fs, không input |

Quy tắc vàng:

1. **Chính sách lúc spawn là quyết định cuối**, thẻ năng lực chỉ có thể *thu hẹp*.
2. Phạm vi (`scopes`) là danh sách **cho phép** cho `fs.*`/`net.*`; quyền kiểu
   boolean (`vault`, `model`, `gpu`) do chính việc cấp quyền quyết định.
3. `deny` được kiểm tra **trước** `allow`, và một mục `deny` có thể là tên quyền
   (`fs.read`, `input`) để tắt cả một loại hành động.
4. Mọi quyết định đều ghi audit kèm `reason` bằng tiếng Việt — để người dùng đọc
   được vì sao bị chặn, chứ không phải "permission denied".

Chi tiết mối đe doạ: [`SECURITY.md`](SECURITY.md).

---

## Tầng 2 — Frame kernel (Rust, 100 % safe)

Hai crate độc lập, `#![deny(unsafe_code)]`, `no_std` + `alloc`:

### 2.1 `crates/yui-sched` — chính sách lập lịch LAVD

Tách **cơ chế** (chuyển ngữ cảnh, đồng hồ — thuộc nhân) khỏi **chính sách**
(crate này). Vì chính sách là Rust an toàn, nó hot-swap được và test được không
cần khởi động máy.

- `TaskClass::{Interactive, AiRender, Batch}` với trọng số 3/2/1 và slice 1/8/4 ms.
- `pick(core)` xét theo thứ tự: **ái lực lớp** (batch về E-core, game/AI về
  P-core) → **tín dụng đánh thức** (tác vụ tương tác vừa tỉnh được ưu tiên một
  lần) → **vruntime nhỏ nhất** → id nhỏ nhất (tất định để trace/test).
- `charge(id, delta)` chia theo trọng số; `scale_slice` co giãn theo W để máy yếu
  vẫn giữ độ trễ thấp.
- 9 test: thứ tự chọn, ái lực, đánh thức, công bằng (± 20 % sau 400 lượt),
  hot-swap không mất trạng thái.

### 2.2 `crates/yui-zram` — ZRAM

- `lz.rs`: YUI-LZ 64 KiB cửa sổ, token `(lit<<4)|(len-4)`, nibble 15 = chuỗi
  byte 0xFF — **byte-identical** với C++ (`core/src/codec.cpp`), nên trang nén
  bởi nhân đọc được bằng `yui-trim`.
- `store.rs`: `ZramStore` giữ trang đã nén, có **fallback verbatim** khi trang
  không nén được (không bao giờ phình), thống kê `bytes_in/bytes_stored/saved_ratio`.
- 8 test: văn bản lặp (>3×), dữ liệu giả ngẫu nhiên, kích thước biên 0..127,
  luồng hỏng bị từ chối, ghi đè/xoá, sức chứa.

### 2.3 Phần C++ của Tầng 2

`core/src/codec.cpp` là bản tham chiếu (dùng cho `.yuip`), `engine.cpp` giữ phần
SIMD và tự chỉnh. Cùng một định dạng, hai ngôn ngữ, có test hai phía.

### 2.4 Chưa có

Nhân thật (vòng đời tác vụ, ngắt, đồng hồ), `io_uring` zero-copy, coalescing
SSD, tickless thật, DVFS/giới hạn sạc ở mức driver. Phần *chính sách* đã xong và
đó là phần dễ sai nhất; phần *cơ chế* cần máy thật.

---

## Tầng 1 — Vi nhân năng lực (`crates/yui-cap`)

Lấy cảm hứng từ seL4 (nguyên tắc, **không** mã nguồn): quyền là **dữ liệu không
thể giả mạo**, không phải một bit trong một trường toàn cục.

- `rights`: 15 quyền có tên (`fs.read` … `admin`) trên một `u32`, có
  `rights_to_string`/`rights_from_string` để in và phân tích.
- `Capability { object, rights, badge }`: `derive` **giao** quyền (không bao giờ
  mở rộng), kết quả rỗng ⇒ `None`.
- `CapTable`: slot là handle tăng dần, có `parent`; `revoke()` đánh dấu cả cây
  con — thu hồi bắc cầu, đúng ngữ nghĩa "con không thể sống lâu hơn cha".
- `Token`: chuỗi chuẩn hoá `{"sub":…,"iss":"yui-cap","rights":…,"iat":…,"exp":…,
  "serial":…,"scopes":[…]}`, ký HMAC-SHA-256. Xác minh theo thứ tự **issuer →
  hết hạn → chưa hiệu lực (`iat > now+300`) → MAC**, trả lỗi có kiểu
  (`Expired`, `NotYetValid`, `BadMac`, `UnknownIssuer`) — không có "false" chung
  chung.
- MAC được kiểm tra **hằng thời gian** (`ct_eq`).
- `sha256.rs`: SHA-256/HMAC tự viết, có vector RFC 6234/4231 trong test.

Cùng định dạng token được `core/src/sandbox/sandbox.cpp` hiểu, nên thẻ do Rust
cấp kiểm được bằng C++ và ngược lại (test `token` hai phía dùng cùng chuỗi chuẩn).

### 1.1 Profiler & chế độ máy (`core/src/kernel/profile.cpp`)

Đọc CPUID/`/proc`-tương đương, RAM, pin, nhiệt, GPU; phân loại:

- `may-yeu` (máy yếu): RAM < 4 GB **hoặc** ≤ 2 luồng;
- `may-manh` (máy mạnh): RAM ≥ 16 GB **và** ≥ 8 luồng, hoặc ≥ 16 luồng **và** ≥ 8 GB;
- còn lại `can-bang`.

Kết quả là `k` (2/4/8) và `W` (1024/4096/8192), được Tầng 2 và Tầng 5 dùng lại.
Đây là chỗ hiện thực hoá yêu cầu "một hệ điều hành cho cả máy 2 GB và siêu máy tính".

### 1.2 Cập nhật ngoại tuyến có chữ ký (đặc tả)

`.bundle` = `manifest.json` (danh sách file + SHA-256 từng file) ‖ `signature`
(Ed25519 của khoá phát hành) ‖ payload. Cài đặt chỉ chấp nhận bundle có chữ ký
khớp khoá tin cậy trong `/etc/yui/trusted-keys/`, kiểm từng hash sau khi giải nén.
Repo chưa có Ed25519 (không có thư viện ngoài); xem roadmap để biết kế hoạch.

---

## Tầng 0 — Phần cứng đa kiến trúc

| Kiến trúc | Khởi động | Ngắt/đồng hồ | SIMD phát hiện |
|---|---|---|---|
| x86_64 | BIOS (limine-bios) hoặc UEFI (limine-uefi) | APIC, TSC deadline | `avx2`, `avx512f` |
| ARM64 | U-Boot/UEFI → EL2→EL1 | GICv3, generic timer | `neon`, `sve` |
| RISC-V 64 | OpenSBI → S-mode | PLIC/CLINT | `rvv` |

Cả ba cùng quy về một `YuiBootInfo` (memory map, framebuffer, ACPI/DT pointer,
cmdline), nhờ đó Tầng 1 chỉ có một đường khởi tạo. Linker script và luồng khởi
động: `arch/x86_64/linker.ld`, `arch/*/README.md`.

Hiện trạng: profiler và logic chọn chế độ **đã chạy** (kể cả trên Linux host để
test/CI); stub khởi động và driver **chưa có** — cần assembler cross và máy thật.

---

## Luồng dữ liệu của một câu lệnh

Ví dụ người dùng gõ trong `yui-shell`:

```
"ghinhớ os:yui \"hệ điều hành Việt\" concept"
   │ shell.cpp: fold() để so tên lệnh, tách tham số
   ▼
Request{ id, method:"graph.upsert", params:{id,label,kind}, token }
   │
   ▼
Service::handle
   │ 1. token hợp lệ? (issuer → exp → iat → MAC)
   │ 2. Enforcer::check(Policy, token, {kind:"graph", …}) → allowed?
   │ 3. Graph::upsert
   ▼
Reply{ ok:true, result:{id, label, kind} }
   │
   ▼
in ra terminal (và ghi audit nếu bị từ chối)
```

Mọi bước đều có test: `test_daemon.cpp` cho đường RPC, `test_sandbox.cpp` cho
quyết định, `test_core.cpp` cho đồ thị, `test_tools.cpp` cho shell end-to-end.
