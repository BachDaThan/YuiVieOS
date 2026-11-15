# Giao thức — Protocol reference

Ba mặt tiền nói cùng một ngôn ngữ nghiệp vụ: `Daemon::handle(method, params) → result`.
Dưới đây là hợp đồng đầy đủ, đúng như mã nguồn đang chạy.

---

## 1. Unix socket: `/run/yuid.sock`

- `SOCK_STREAM`, quyền `0600`, chủ sở hữu là người chạy `yuid`.
- Mặc định `--socket /run/yuid.sock`, đổi được bằng `--socket PATH`.
- **Khung dòng**: một đối tượng JSON trên một dòng, kết thúc `\n`. Đây là đường
  dùng cho shell và script (`nc -U`, `socat`).
- Mã lỗi ở mức JSON-RPC 2.0 (`-32700` parse, `-32600` invalid request,
  `-32601` method not found, `-32602` invalid params, `-32603` internal).

```console
$ printf '{"jsonrpc":"2.0","id":1,"method":"yuid.version"}\n' | nc -U /run/yuid.sock
{"jsonrpc":"2.0","id":1,"result":{"version":"0.1.0"}}
```

Một yêu cầu, một trả lời, không trạng thái. Không có đăng ký sự kiện trong bản
này (xem roadmap).

### Chạy thử không cần root

```bash
./build/bin/yuid --socket /tmp/yui.sock --state-dir /tmp/yui-state --serve-once &
printf '{"jsonrpc":"2.0","id":1,"method":"yuid.profile"}\n' | nc -U /tmp/yui.sock
```

`--serve-once` nhận đúng một kết nối rồi thoát — dùng cho test và cho CI.

---

## 2. Phương thức JSON-RPC

`yuid.info` là bí danh của `ping`. Mọi phương thức đều trả `result` là object
(trừ khi ghi chú khác) và không bao giờ trả `null` im lặng — lỗi phải là `error`.

### 2.1 Daemon

| Phương thức | Tham số | Trả về |
|---|---|---|
| `yuid.info` / `ping` | — | `{version, arch, simd, profile:{class, k, window, threads}, methods:[…], tools:[…]}` |
| `yuid.version` | — | `{version}` |
| `yuid.profile` | — | hồ sơ phần cứng đầy đủ + gợi ý k/W |
| `yuid.tune` / `engine.tune` | `{k?, window?, threads?, class?}` | cấu hình sau khi chỉnh, kiểm miền giá trị |

`yuid.tune` chỉ chấp nhận `k ∈ {2,4,8}`, `window ∈ {1024,4096,8192}`,
`threads ∈ [1, 256]`; ngoài miền ⇒ `-32602` kèm `data` nêu giá trị hợp lệ.

### 2.2 Đồ thị — `graph.*`

| Phương thức | Tham số | Trả về |
|---|---|---|
| `graph.upsert` | `{id, label?, kind?, props?}` | `{created, id}` |
| `graph.link` | `{from, rel, to, weight?, props?}` | `{linked:true}` |
| `graph.query` | `{q, kind?, expand_from?, depth?, rel?}` | `{hits:[{id,label,kind}], expanded?:[id]}` |
| `graph.path` | `{from, to}` | `{path:[id,…]}` (rỗng nếu không nối) |
| `graph.stats` | — | `{nodes, edges, kinds:{…}}` |
| `graph.export` | — | toàn bộ đồ thị dạng JSON |
| `graph.import` | `{nodes, edges}` | `{nodes, edges}` đã nạp |

`kind` ∈ `file app entity concept device capability person place note unknown`.
`q` được chuẩn hoá bằng `text::fold` ⇒ **tìm được không dấu**: `he dieu hanh`
khớp "hệ điều hành".

`graph.link` trả `-32602` nếu một đầu không tồn tại hoặc `rel` rỗng — không tự
tạo nút ma.

### 2.3 Kho mã hoá — `vault.*`

| Phương thức | Tham số | Trả về |
|---|---|---|
| `vault.seal` | `{plain: base64, key?, log2_n?}` | `{blob: base64, bytes}` |
| `vault.open` | `{blob: base64, key?, log2_n?}` | `{plain: base64}` hoặc lỗi `-32602` "sai khoá hoặc dữ liệu hỏng" |

Khoá lấy theo thứ tự: tham số `key` → `YUI_VAULT_KEY` → `--vault-key`. Không có
khoá ⇒ `-32602`. `vault.open` **không** phân biệt "sai khoá" với "hỏng dữ liệu"
ở thông điệp (tránh oracle), nhưng mã lỗi khác nhau: `-32602` cho sai khoá/hỏng,
`-32603` cho lỗi nội bộ.

### 2.4 Nén — `pack.*`

| Phương thức | Tham số | Trả về |
|---|---|---|
| `pack.compress` | `{data: base64, label?}` | `{pack: base64, orig_size, payload_size, ratio}` |
| `pack.decompress` | `{pack: base64}` | `{data: base64, orig_size}` |

`pack.decompress` kiểm SHA-256 và CRC32 trước khi trả dữ liệu; sai ⇒ `-32602`.

### 2.5 Hộp cát — `sandbox.check`

```json
{"jsonrpc":"2.0","id":7,"method":"sandbox.check",
 "params":{"policy":{…},"kind":"fs.read","target":"/home/user/.ssh/id_ed25519",
           "token":"<chuỗi token>","now":1700000000}}
```

Trả `{allowed, reason, tier, matched_scope?}`. `reason` là câu tiếng Việt giải
thích quyết định — dùng được để hiển thị trực tiếp cho người dùng.

### 2.6 Bộ gõ — `ime.convert`

```json
{"method":"ime.convert","params":{"text":"Tieengs Vieetj","method":"telex"}}
→ {"result":{"text":"Tiếng Việt","tones":2}}
```

`method` ∈ `telex` (mặc định) | `vni`. Chuỗi vào là phím gõ, không phải chữ đã
có dấu; hàm thuần nên gọi lặp lại cho cùng kết quả.

### 2.7 Tìm kiếm — `web.search`

`{q, limit?, allow_network?}` → `{backend, results:[{title,url,snippet}], note}`.

`backend` ∈ `graph` (tìm thấy trong đồ thị cục bộ) | `cache` | `http` | `offline`.
Ngoại tuyến **không phải lỗi**: trả `backend:"offline"` + `note` gợi ý. Chỉ gọi
mạng khi `--enable-net` và `allow_network` cùng bật.

### 2.8 Công cụ hệ điều hành — `os.*`

| Phương thức | Trả về |
|---|---|
| `os.battery` | `{present, percent, charging, seconds_left?}` (máy bàn: `present:false`) |
| `os.thermal` | `{celsius, throttle, source}` |
| `os.power` | `{governor, charge_limit_percent, note}` |

Giá trị không đọc được thì trả `present:false`/`source:"none"`, **không bịa số**.

### 2.9 `telemetry.read` — đã cài đặt (task P1.5)

Hợp đồng đã chốt để lõi AI đọc được số liệu tài nguyên thật:

```json
{"jsonrpc":"2.0","id":9,"method":"telemetry.read","result":{
  "battery":{"present":true,"percent":37,"charging":false},
  "thermal":{"cpu_c":71.5,"gpu_c":64.0,"throttle":true},
  "memory":{"total_mb":1843,"free_mb":402,"zram_ratio":2.7},
  "machine":{"class":"may-yeu","simd":"avx2","threads":2},
  "advice":{"k":1,"window":16,"reason":"Pin thấp và nhiệt cao: giảm k/W để giữ máy mát"}}}
```

Khuyến nghị `k/W` do bridge quyết (bảng ở
[`ARCHITECTURE.md`](ARCHITECTURE.md) §6.3); lõi AI có thể ghi đè nhưng phải ghi lý do.

Nguồn số liệu thật (`kernel-services/telemetry.cpp`):

| Trường | Đọc từ | Khi không có |
|---|---|---|
| `battery.percent` / `charging` | `/sys/class/power_supply/BAT*/capacity`, `status` | `present:false`, `percent:-1` |
| `thermal.cpu_c` | `/sys/class/thermal/thermal_zone*/temp` (ưu tiên `type` chứa `pkg`/`cpu`/`soc`) | `-1` |
| `thermal.gpu_c` | `/sys/class/drm/card*/device/hwmon/hwmon*/temp1_input` | `-1` |
| `memory.total_mb` / `free_mb` | `/proc/meminfo` | `0` |
| `machine.*` | `kernel-services/profile.cpp` (cùng ngưỡng với nhân) | luôn có |

`throttle` bật khi `max(cpu_c, gpu_c) > 75 °C`. **Quy tắc vàng: không bịa số.**
Cảm biến không đọc được thì trả `-1`, và `advice.mode` khi đó chỉ dựa trên những
gì thật sự biết. Không có `zram_ratio` trong bản này (P2.9 mới có số liệu thật;
hiện trả `0`).

Mã lỗi: `-32602` nếu tham số sai kiểu; method này **không** yêu cầu thẻ năng lực
vì dữ liệu chỉ đọc và không chứa bí mật.

---

## 3. MCP (Model Context Protocol) — `yuid --mcp`

- Phiên bản giao thức: `2024-11-05`.
- Vận chuyển: stdio, khung `Content-Length: N\r\n\r\n` + JSON (giống LSP).
- `initialize` → `{protocolVersion, capabilities:{tools:{listChanged:false}},
  serverInfo:{name:"yuid", version}, instructions}`.
- `tools/list` → 8 công cụ dưới đây. `tools/call` → `{content:[{type:"text",text}]}`.
- Thông báo (`notifications/*`) không có `id` ⇒ không trả lời.

| Công cụ | Tham số bắt buộc | Việc |
|---|---|---|
| `graph_query` | `q` | tìm trong đồ thị tri thức |
| `graph_upsert` | `id` | thêm/cập nhật nút (`label`, `kind`) |
| `graph_link` | `from`, `rel`, `to` | nối hai nút |
| `ime_convert` | `text` | Telex/VNI → chữ Việt |
| `vault_seal` | `plain` (base64) | niêm phong dữ liệu |
| `sandbox_check` | `policy`, `kind` | hành động có được phép? |
| `web_search` | `q` | tìm kiếm (có thể ngoại tuyến) |
| `system_profile` | — | phần cứng + k/W |

Ánh xạ MCP → JSON-RPC là 1–1: `graph_query` gọi `graph.query`, v.v. Không có
đường tắt nào bỏ qua kiểm tra năng lực.

### 3.1 Năm công cụ điều khiển hệ điều hành — **dành sẵn, chưa cài đặt** (task P4.3)

| Công cụ | Quyền cần | Xác nhận | Trạng thái |
|---|---|---|---|
| `sys_power` | `power` | có | ⬜ P4.3 |
| `sys_scheduler` | `admin` hoặc `power` | có | ⬜ P4.3 |
| `sys_app_stripper` | `fs.read` + `fs.write` trong scope | có | ⬜ P4.3 |
| `sys_proc` | `admin` | có | ⬜ P4.3 |
| `sys_search_vault` | `vault` | không | ⬜ P4.3 |

Quy tắc (đã ghi trong `ARCHITECTURE.md` §6.4): thiếu quyền ⇒ từ chối kèm lý do
tiếng Việt; thiếu `confirm: true` ⇒ trả `needs_confirmation` + mô tả tác động;
mọi hành động ghi `audit.log`. Gọi `tools/list` hôm nay **không** thấy 5 công cụ
này — đó là hành vi đúng của bản hiện tại, không phải lỗi.

Ví dụ khung trên stdio:

```
Content-Length: 121\r\n\r\n{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"ime_convert","arguments":{"text":"Vieetj"}}}
```

---

## 4. C-ABI — `libyui.so` / `libyui.a`

Header: `include/yui/yui.h`, `YUI_ABI_VERSION 1`. Dùng được từ C11
(`tests/test_capi.c` là bằng chứng: 36 check, biên dịch bằng `cc -std=c11`).
Không có ngoại lệ C++ hay `std::string` đi qua biên.

**Thiết kế không trạng thái**: không có `yui_open`/`yui_close`. Hàm thuần nhận
đầu vào, trả mã lỗi; đối tượng có trạng thái duy nhất là `yui_graph*`, do
`yui_graph_new`/`yui_graph_free` quản lý. Nhờ vậy `libyui` không có "phiên" để
rò rỉ, và cùng một hàm gọi được từ nhiều luồng.

| Nhóm | Hàm |
|---|---|
| Danh tính | `yui_version()`, `yui_arch()`, `yui_abi_version()` — chuỗi tĩnh, **không** free |
| SIMD | `yui_simd(out, cap)` → `"avx2"`, `"avx512f"`, `"neon"`, `"sve"`, `"rvv"`, `"scalar"` |
| Băm | `yui_sha256(data, len, out_hex[65])` |
| Nén | `yui_pack(data, len, level, yui_buf*)`, `yui_unpack(…, err, err_cap)`, `yui_buf_free` |
| Tự chỉnh | `yui_tune_auto(ram_decigb, temp_c, on_battery, threads, has_gpu, yui_tune*)` |
| Đồ thị | `yui_graph_new/free`, `upsert`, `link`, `dump`, `import`, `nodes`, `bfs` |
| Bộ gõ | `yui_telex(keys, out, cap)`, `yui_vni(keys, out, cap)` — trả **độ dài byte** |
| Kho | `yui_vault_seal(pw, plain, len, log2n, yui_buf*)`, `yui_vault_open(…, err, err_cap)` |
| Hộp cát | `yui_sandbox_check(policy_json, kind, target, reason, reason_cap)` → **1 cho, 0 không** |
| Bộ nhớ | `yui_free(ptr)` cho chuỗi do `yui_graph_dump` cấp |

```c
#include <stdio.h>
#include <yui/yui.h>

int main(void) {
  char text[256];
  int n = yui_telex("Tieengs Vieetj", text, sizeof text);   /* n = độ dài byte */
  if (n >= 0) puts(text);                                    /* Tiếng Việt */

  char hex[65];
  yui_sha256("abc", 3, hex);                 /* ba mươi hai byte → 64 hex */

  yui_buf packed = {0};
  if (yui_pack("du lieu", 7, 1, &packed) == YUI_OK) yui_buf_free(&packed);

  char reason[256];
  int allowed = yui_sandbox_check(
      "{\"app\":\"a\",\"tier\":\"isolated\",\"capabilities\":[\"fs.read\",\"fs:/home/u:ro\"]}",
      "fs.read", "/home/u/doc.txt", reason, sizeof reason);
  printf("%d: %s\n", allowed, reason);

  yui_graph* g = yui_graph_new();
  yui_graph_upsert(g, "os:yui", "YuiVieOS", "concept");
  char* dump = yui_graph_dump(g);   /* malloc'd JSON */
  puts(dump);
  yui_free(dump);
  yui_graph_free(g);
  return 0;
}
```

Mã trả về:

| Mã | Giá trị | Nghĩa |
|---|---|---|
| `YUI_OK` | 0 | thành công (với hàm trả chuỗi: kết quả rỗng) |
| `YUI_E_INVALID` | −1 | tham số sai (NULL, chuỗi hỏng, level ngoài miền) |
| `YUI_E_NOMEM` | −2 | hết bộ nhớ |
| `YUI_E_AUTH` | −3 | thiếu/sai khoá (vault) |
| `YUI_E_CORRUPT` | −4 | dữ liệu hỏng (SHA/CRC/tag/độ dài sai) |
| `YUI_E_UNAVAILABLE` | −5 | tính năng không có trên máy này (ví dụ SIMD không hỗ trợ) |
| `YUI_E_TOO_SMALL` | −6 | bộ đệm ra không đủ |

**Hàm trả chuỗi trả độ dài byte** (≥ 0) chứ không phải 0: `yui_telex("Việt")`
trả 6 vì "Việt" là 6 byte UTF-8. Luôn kiểm `rc >= 0`, đừng kiểm `rc == 0` — đây là
lỗi dễ mắc nhất khi dùng ABI này, và chính `test_capi.c` khẳng định `> 0`.

`cap` được truyền **theo giá trị** nên hàm **không** báo được độ dài cần. Quy ước:
cấp bộ đệm ≥ 4 × `strlen(keys)` cho bộ gõ (mỗi ký tự có thể thành 3 byte UTF-8),
và ≥ 2 × chuỗi JSON cho `yui_graph_bfs`. Nếu nhận `YUI_E_TOO_SMALL`, cấp đôi rồi
gọi lại — hàm không có tác dụng phụ nên gọi lại luôn an toàn.

**Cấp phát**: mọi thứ hàm trả về qua `yui_buf`/`char*` đều thuộc về người gọi:
`yui_buf_free` cho `yui_buf`, `yui_free` cho chuỗi. Chuỗi từ `yui_version`,
`yui_arch`, `yui_abi_version` là tĩnh — **không** free.

## 5. Định dạng thẻ năng lực

Chuỗi chuẩn hoá (phải đúng từng byte, không khoảng trắng thừa, thứ tự khoá cố
định, `scopes` là mảng chuỗi ASCII tăng dần theo thứ tự cấp):

```
{"sub":"<chủ thể>","iss":"yui-cap","rights":"<quyền,phân,cách,bằng,dấu,phẩy>","iat":<u64>,"exp":<u64>,"serial":<u64>,"scopes":["<a>","<b>"]}
```

- MAC = `hex(HMAC-SHA-256(khoá, chuỗi_trên))`, 64 ký tự hex thường.
- Thẻ truyền đi dưới dạng object JSON có thêm trường `mac`; bên nhận **dựng lại**
  chuỗi từ các trường (không ký lại chuỗi nhận được).
- `exp = 0` ⇒ không hết hạn. `iat > now + 300` ⇒ `NotYetValid` (chống đồng hồ
  chạy nhanh hơn 5 phút).
- Quyền phải là tên trong danh sách 15 quyền; tên lạ ⇒ thẻ không hợp lệ, không
  bị bỏ qua im lặng.

Chi tiết ngữ nghĩa: [`SECURITY.md`](SECURITY.md) mục 2.

---

## 6. Phiên bản hoá

- JSON-RPC: thêm phương thức mới = thay đổi tương thích; đổi tham số = phải
  thêm phương thức mới (`.v2`), không phá cái cũ.
- MCP: giữ `protocolVersion` khai báo; client lạ vẫn nhận `2024-11-05`.
- C-ABI: `YUI_ABI_VERSION` chỉ tăng khi phá vỡ; thêm hàm thì chỉ tăng minor.
- Định dạng gói: `YUIP` có trường `version`; `YUIA` có `version`; thẻ có `iss`.
