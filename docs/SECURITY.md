# Bảo mật — Security model

YuiVieOS không tuyên bố "an toàn tuyệt đối". Tài liệu này nói rõ: chống được
ai, chống bằng cách nào, và **cái gì chưa** chống được. Mọi điểm yếu đã biết đều
được liệt kê — người dùng cần biết trước khi lưu bí mật vào đây.

## 1. Mô hình mối đe doạ

| Kẻ tấn công | Mục tiêu | YuiVieOS chống bằng |
|---|---|---|
| App bên thứ ba chạy trên máy | đọc file riêng, gọi mạng lén, leo thang | năng lực theo phạm vi + hộp cát 3 bậc + audit |
| App Android / game Windows độc hại | thoát khỏi lớp tương thích | micro-VM không có syscall host, không fs, mạng = `none` mặc định |
| Ai đó sửa token năng lực | tự cấp quyền | HMAC-SHA-256 trên chuỗi chuẩn hoá, so sánh hằng thời gian |
| Ai đó chèn/sửa gói cập nhật | chạy mã lạ | `.bundle` có chữ ký + SHA-256 từng file (đặc tả) |
| Ai đó sửa file `.yuip` | đầu độc app đã gọt | SHA-256 toàn plaintext + CRC32 payload |
| Ai đó sửa kho mã hoá | đọc/sửa bí mật | scrypt + encrypt-then-MAC, sai tag ⇒ từ chối |
| Chương trình của bạn bị lỗi | mất dữ liệu im lặng | mọi giải nén/nạp đều kiểm độ dài, kiểm tag trước khi dùng |

**Ngoài phạm vi**: kẻ có quyền root/ring-0 trên máy đang chạy, tấn công kênh bên
(timing/cache/power) ở mức phần cứng, firmware độc hại, và mọi thứ liên quan đến
việc bạn tự đưa mã độc vào chính sách của mình.

## 2. Mô hình năng lực

Quyền là **dữ liệu**, không phải một bit toàn cục:

```rust
Capability { object: u32, rights: u32, badge: u32 }
```

- 16 quyền có tên trên một `u32`: `fs.read`, `fs.write`, `fs.exec`, `net.connect`,
  `net.listen`, `input`, `camera`, `mic`, `gpu`, `model`, `graph`, `vault`,
  `spawn`, `power`, `admin`, `compute.heavy` (được phép tải CPU/GPU cao kéo
  dài — Guard nền dùng nó để phân biệt tiến trình tính toán hợp lệ với đào
  coin ngầm, mục 10).
- **Dẫn xuất chỉ thu hẹp**: `parent.derive(child)` = `parent.rights & child.rights`.
  Rỗng ⇒ `None`. Không có phép toán nào trong crate mở rộng quyền.
- **Thu hồi bắc cầu**: `revoke(slot)` đánh dấu cả cây con. Một tiến trình bị thu
  hồi không thể dùng slot con để sống tiếp.
- Badge là không gian tên cho phép nhiều bản sao của cùng một đối tượng có
  quyền khác nhau (giống badge của seL4) mà không cần cấp slot mới cho đối tượng.

### Thẻ (token) trên dây

```
{"sub":"app:yui","iss":"yui-cap","rights":"fs.read,graph,vault",
 "iat":1700000000,"exp":1700003600,"serial":42,"scopes":["fs:/home/user"]}
```

Đúng chuỗi trên (không khoảng trắng thừa, thứ tự khoá cố định) được ký
HMAC-SHA-256. Bên nhận **tự dựng lại** chuỗi từ các trường rồi so MAC — không
bao giờ ký lại chuỗi nhận được, nên không có lỗ hổng "canonicalisation".

Thứ tự kiểm tra (cố ý): `issuer` → `exp` → `iat > now + 300` → `MAC`. Kiểm tra
rẻ trước, kiểm tra mật mã sau, và **luôn** kiểm MAC trước khi tin bất cứ trường
nào khác về mặt nghiệp vụ.

`exp == 0` nghĩa là **không hết hạn** — chỉ dùng cho thẻ do hệ thống tạo lúc cài
đặt, không dùng cho app.

## 3. Hộp cát

Ba bậc, mỗi bậc là tập con của bậc trên. Chính sách lúc spawn là **quyết định
cuối**; thẻ chỉ thu hẹp thêm.

Quy tắc thực thi trong `Enforcer::check`:

1. `deny` thắng tuyệt đối (và có thể là tên quyền: `"deny": ["input"]`).
2. Bậc `microvm` không có `fs.*`, `input`, `spawn`, `admin` — bất kể thẻ nói gì.
3. Với `fs.*`/`net.*`, phạm vi là danh sách cho phép; không có phạm vi ⇒ chỉ
   đúng đường dẫn/host đã cấp.
4. Mọi từ chối trả `reason` tiếng Việt và được ghi audit.

Ví dụ chính sách thật trong repo: `vmm-sandbox/policies/yui-app.policy.json`,
`trim.policy.json`, `legacy-game.policy.json`, `first-party-ipc.policy.json` —
cả bốn đều được nạp và kiểm trong `test_sandbox` (14 tình huống cho/không cho).

### Phạm vi đường dẫn

`/home/user` bao `/home/user/a`, nhưng **không** bao `/home/user2` (so sánh theo
thành phần đường dẫn, không phải tiền tố chuỗi). `..` được chuẩn hoá **trước**
khi so. Đường dẫn tuyệt đối và `..` bị từ chối khi giải nén `.yuip`.

### Mạng

`net:*.yui.vn` khớp mọi host con; `net:api.github.com:443` khớp đúng host và
cổng. So khớp không phân biệt hoa/thường, và host được tách khỏi cổng trước khi so.

## 4. Mật mã — dùng gì, vì sao, và giới hạn

| Thành phần | Cài đặt | Vector kiểm thử | Giới hạn đã biết |
|---|---|---|---|
| SHA-256 | tự viết (`sha256.cpp`) | RFC 6234, chuỗi rỗng, "abc", triệu chữ 'a' | không có |
| HMAC-SHA-256 | tự viết | RFC 4231 case 1–7 | không có |
| SHAKE-256 | Keccak tự viết | vector NIST nhúng trong test | chỉ dùng làm keystream, **không** làm MAC |
| scrypt | tự viết (`romix`/`blockmix`) | vector RFC 7914 | không có |
| So sánh MAC | `ct_eq` | test | phụ thuộc trình biên dịch tối ưu; đây là biện pháp giảm thiểu, không phải bảo đảm phần cứng |

### Vault: vì sao "stream cipher + HMAC" chứ không AES

Repo không được phép kéo thư viện ngoài và tự viết AES-GCM là việc *dễ sai*.
Lựa chọn có chủ đích:

```
keystream_i = SHAKE-256(enc_key ‖ nonce ‖ counter)      # không bao giờ tái dùng (nonce ngẫu nhiên 16 B)
ciphertext  = plaintext XOR keystream
tag         = HMAC-SHA-256(master, header ‖ ciphertext)
```

- **Encrypt-then-MAC**: xác thực *toàn bộ* header + ciphertext trước khi giải mã,
  nên không có oracle giải mã.
- **Nonce 16 byte ngẫu nhiên** từ `/dev/urandom`; trùng nonce với cùng khoá là
  thảm hoạ duy nhất — xác suất 2⁻¹²⁸ cho 2³² lần niêm phong.
- Không có "chế độ tắt xác thực": sai tag ⇒ `false`, không trả plaintext.

**Giới hạn thật**: đây là mật mã phần mềm thuần, không chống được tấn công kênh
bên; và vì `master` nằm trong RAM khi mở kho, một tiến trình đọc được bộ nhớ của
`yuid` đọc được bí mật. Muốn mạnh hơn cần TPM/Secure Enclave — chưa có.

### Vì sao scrypt chứ không Argon2

scrypt có đặc tả RFC 7914 ngắn, dễ cài đúng và dễ kiểm bằng vector chuẩn. Argon2
cần BLAKE2b và cấu trúc phức tạp hơn — roadmap, không phải bản này.

## 5. Toàn vẹn dữ liệu

`.yuip` (app đã gọt):

```
YUIP | version | codec_id | flags | orig_size | payload_size | sha256(plain) | crc32(payload) | payload
```

Kiểm **hai lớp**: CRC32 bắt lỗi truyền/hỏng bit nhanh, SHA-256 bắt sửa đổi có
chủ đích. Giải nén so `out_len` chính xác với `orig_size`; thiếu/thừa byte đều
bị từ chối. Đường dẫn trong kho YUIA bị kiểm: tuyệt đối và `..` ⇒ lỗi.

**Không** có chữ ký số trong `.yuip`: gói app là *dữ liệu người dùng tự tạo*,
không phải gói cập nhật hệ thống. Chữ ký nằm ở `.bundle` (mục 6).

## 6. Cập nhật hệ thống (đã cài đặt — clean-room)

```
bundle/
  manifest.json     bản chuẩn (canonical) — ĐÂY là thứ được ký:
                    {"files":[{"mode":…,"path":…,"sha256":…},…],"key_id":…,"version":…}
                    (tệp sắp theo path, thứ tự khoá cố định; người ký dựng
                    manifest dạng chuẩn, bên xác minh TỰ DỰNG LẠI từ các
                    trường đã phân tích — không bao giờ ký lại chuỗi nhận được)
  manifest.sig      Ed25519(canonical) 64 byte — khoá phát hành YuiVieOS
  payload/          file thật
```

Cài đặt: tra khoá trong kho `/etc/yui/trusted-keys/` (`<key_id>.pub` = 64 hex)
→ xác minh chữ ký Ed25519 trên **dạng chuẩn dựng lại** → kiểm SHA-256 từng
`payload/<path>` → chỉ ghi vào **phân vùng bất biến + khe A/B** → đổi con trỏ
boot bằng ghi nguyên tử (tmp + fsync + rename). Không có mạng: bundle chép qua
USB, vẫn xác minh y hệt. Đường dẫn trong manifest bị kiểm trước khi dùng:
tuyệt đối, `..`, `//` ⇒ từ chối với lý do tiếng Việt.

**Ed25519 đã được cài đặt clean-room** (không dùng thư viện mật mã nào):
`kernel-services/ed25519.cpp` + `kernel-cap/src/ed25519.rs` — SHA-512
(FIPS 180-4), số học GF(2^255−19) limb 32-bit, cộng đầy đủ twisted Edwards
(RFC 8032 §5.1.4). Kiểm chứng bằng **vector chính thức RFC 8032 §7.1**
(TEST 1/2/3, SHA(abc), TEST 1024 — khóa công khai và chữ ký khớp từng hex) +
vector FIPS cho SHA-512: `tests/test_ed25519.cpp` (78 check),
`kernel-cap/src/ed25519.rs` (5 test). Bộ test `.bundle` kèm theo:
kho khoá CRUD + bền vững, ký/xác minh ngoại tuyến, **5 kịch bản sửa 1 byte**
(tệp, chữ ký, phiên bản, đường dẫn thoát gốc, khoá lạ) đều bị từ chối, và
cài đặt khe A/B + quay lui (`tests/test_ed25519.cpp::bundle.*`).

Giới hạn đã biết: Ed25519 bản C++/Rust này tối ưu cho đúng chứ không cho tốc
độ (trừ dài theo bit); không có chống đo thời gian cho toàn bộ đường cong
(riêng so MAC dùng so khớp hằng thời gian); sinh số ngẫu nhiên cho khoá do
phần gọi đảm nhiệm (seed 32 byte).

## 7. Những gì chưa được bảo vệ

Danh sách này cố ý đầy đủ, vì biết giới hạn quan trọng hơn tin vào khẩu hiệu:

1. **Chưa có nhân thật**: Tầng 1/2 là crate chạy trong tiến trình test, không có
   MMU/ring cách ly. Hộp cát hiện là *quyết định chính sách* + kiểm tra, chưa
   phải cách ly phần cứng.
2. **Chưa có micro-VM**: `vmm-sandbox/vmm` là schema + đặc tả. Cách ly game/Android
   ở mức KVM **chưa** tồn tại trong repo.
3. **Chữ ký cập nhật đã có** (mục 6) nhưng khoá sinh ngẫu nhiên vẫn do phần
   gọi đảm nhiệm; hạ tầng phân phối khoá (xoay khoá, thu hồi) chưa có.
4. **Không chống kênh bên**: mọi so sánh mật mã đều hằng thời gian ở mức mã
   nguồn, nhưng CPU có thể vẫn rò rỉ.
5. **`yuid` tin tiến trình cùng UID**: ai chạy được `yuid` (hoặc thay được
   `/run/yuid.sock`) thì điều khiển được nó. Socket có quyền `0600`.
6. **Không có sandbox cho chính `yuid`**: daemon cần đọc/ghi file để làm việc;
   giới hạn duy nhất là quyền Unix của người chạy nó.
7. **`web_search` không kiểm nội dung**: khi có mạng, nó trả dữ liệu từ nguồn
   ngoài, có thể chứa prompt injection. Agent dùng nó phải coi kết quả là *dữ
   liệu*, không phải mệnh lệnh.

## 8. Báo lỗi bảo mật

Gửi email tới người bảo trì (xem `LICENSE`/commit author) với mô tả, cách tái
hiện, và mức ảnh hưởng. Vui lòng **không** mở issue công khai cho lỗ hổng chưa vá.

## 9. Nguyên tắc khi sửa mã

1. Không thêm `unsafe` vào `kernel-cap/` hay `kernel-services/` (`#![deny(unsafe_code)]` cố ý).
2. Quyền mới phải có tên trong `rights`, không dùng số trần.
3. Mọi hàm giải nén/nạp dữ liệu phải: kiểm độ dài → kiểm toàn vẹn → mới parse.
4. Mọi quyết định cho/không phải có `reason` đọc được bằng tiếng Việt.
5. Mật mã mới phải kèm vector kiểm thử chuẩn và dòng "giới hạn đã biết" ở mục 4.

## 10. Phòng vệ chủ động (đã cài đặt)

Ba cơ chế dưới đây đều là **chính sách thuần** (Rust an toàn / C++ đối chiếu,
kiểm thử không cần phần cứng thật) — telemetry thật chỉ là nguồn dữ liệu đầu vào.

### 10.1 Guard chống bất thường nền (đào coin ngầm)

Quy tắc: tiến trình **không có cửa sổ** (headless) **và** không giữ quyền
`compute.heavy` (CAP_COMPUTE_HEAVY) mà tải CPU hoặc GPU **vượt 70 %** liên tục
**≥ 15 s** ⇒ bị nghi ngờ chạy ngầm. Hệ thống tự động:

1. hạ xuống lớp ưu tiên đáy (`Idle`/`Background` trong yui-sched);
2. clock-gating + bóp xung nhịp về **20 %** (`yui-power`, giải phóng khi người
   dùng chọn "để yên");
3. ghi audit tiếng Việt;
4. hỏi người dùng qua `yuid`: **"Bạn muốn tiêu diệt, gỡ bỏ, hay để yên?"** —
   `guard.decide` **bắt buộc `confirm: true`**, thiếu thì trả
   `needs_confirmation`, không bao giờ hành động ngầm.

Chuỗi dữ liệu: `telemetry` (tải từng tiến trình) → `guard::Guard`
(`kernel-services/guard.cpp`, `kernel-services/yui-sched/src/guard.rs`) →
`yui-power` + `yuid` (`guard.observe`/`guard.alerts`/`guard.decide`).
Test: `tests/test_redteam.cpp` (kịch bản đào coin, tải nhấp nháy dưới ngưỡng,
tiến trình có cửa sổ/quyền không bị ảnh hưởng, chuỗi quyết định qua `yuid`).

### 10.2 Bộ lọc egress theo năng lực, Zero-Trust và Secretless Broker (P3.10)

Tiến trình ở chế độ `isolated`/`microvm` **mặc định bị chặn** mọi kết nối ra
ngoài; chỉ đích nằm trong allowlist của `policies/*.json` (`net:host[:port]`,
`net:*.suffix`, CIDR IPv4) mới đi được. `deny` **thắng** `allow`. Cổng bị kiểm:
rule chỉ định cổng thì chỉ đúng cổng đó. Mọi quyết định kèm lý do tiếng Việt
để audit ("chặn kết nối tới … — ngoài phạm vi cho phép của chính sách").

Các cơ chế phòng vệ mở rộng (P3.10):
1. **East-West Zero-Trust**: Giao tiếp ngang giữa các sandbox (`is_east_west`) mặc
   định bị từ chối (`allow_east_west = false`). Các container/micro-VM không thể
   tự do giao tiếp nội bộ qua mạng nếu không được khai báo tường minh.
2. **Chống rò rỉ qua DNS (Anti-DNS-Tunneling)**:
   - Cấm DNS trực tiếp ra Internet trên cổng 53 (`allow_direct_dns = false`). Mọi
     truy vấn tên miền bắt buộc phải qua resolver/broker nội bộ có kiểm soát.
   - Kiểm tra chuẩn tên miền RFC 1035: Tên miền không vượt quá 253 octet, mỗi nhãn
     không vượt quá 63 octet, ký tự chỉ gồm chữ cái, chữ số và gạch nối (không
     bắt đầu/kết thúc bằng gạch nối).
   - Đo lường **Shannon Entropy**: Tính toán $H = -\sum p_i \log_2(p_i)$ trên các
     nhãn tên miền (độ dài $\ge 12$). Bất kỳ nhãn nào có entropy vượt ngưỡng hiệu
     chuẩn $H > 4.2$ bit/ký tự (dấu hiệu điển hình của payload mã hoá base32/base36
     từ dnscat2, iodine...) đều bị từ chối lập tức với lý do cảnh báo nguy cơ DNS tunneling.
3. **Secretless Broker**:
   - Mọi khoá xác thực, API token và bí mật ngoại vi được quản lý tập trung ở vòng
     ngoài (`SecretlessBroker`), hoàn toàn cách ly khỏi bộ nhớ guest micro-VM.
   - Ứng dụng guest chỉ gửi yêu cầu ý định kèm thẻ năng lực hợp lệ (`kRightNetConnect`).
   - Broker thẩm định thẻ năng lực, kiểm tra allowlist egress, tự động gắn bí mật
     ngoài biên (`[SECRETLESS_BROKER_ATTACHED]`) trước khi gửi lên upstream.
   - Bí mật thô tuyệt đối không được đưa vào guest, biến môi trường hay nhật ký audit.

Cài đặt: `vmm-sandbox/sandbox.cpp::EgressFilter` và `SecretlessBroker`. Test:
`tests/test_sandbox.cpp` và `tests/test_redteam.cpp`.

### 10.3 Test Red-Team

`tests/test_redteam.cpp` **mô phỏng tấn công thật** thay vì kiểm API từng phần:
kịch bản đào coin ngầm (phát hiện + bóp xung + hỏi người dùng + xác nhận tiêu
diệt), kịch bản rò rỉ dữ liệu (cho phép đúng đích trong chính sách, chặn đích
xấu ở cả hai lớp: EgressFilter và Enforcer, có audit), kịch bản mã độc tống tiền
mã hoá 10.000 tệp (phát hiện entropy cao + bóp xung + rollback CoW khớp 100%
băm SHA-256), và 6 kịch bản truy cập lén khi máy rảnh. Đây là test hồi quy
bảo vệ các cơ chế phòng vệ chủ động — sửa Guard/EgressFilter mà test đỏ nghĩa là
đã làm hỏng một hàng phòng thủ.

Giới hạn đã biết: tải CPU/GPU trong test là **mẫu do test nạp vào**, chưa phải
cảm biến thật; ngưỡng 70 %/15 s là hằng số cấu hình, chưa tinh chỉnh theo máy.

### 10.4 Snapshot CoW & Hoàn tác chống mã độc tống tiền (P2.14)

Để bảo vệ phân vùng dữ liệu trạng thái (`/state`), hệ thống sử dụng kho lưu trữ
Copy-on-Write (`include/yui/fsstore.hpp`, `kernel-services/fsstore.cpp::CowStore`):

1. **Snapshot giao dịch CoW**: Mỗi lần tạo điểm khôi phục, hệ thống chỉ sao chép
   bảng ánh xạ con trỏ bất biến (`std::shared_ptr<const std::vector<uint8_t>>`),
   đạt chi phí bộ nhớ $O(1)$ cho dữ liệu byte thực tế. Thời gian tạo snapshot được đo
   bằng đồng hồ thực (`high_resolution_clock`).
2. **Bù trừ dung lượng (Storage Accounting)**: Đo lường chính xác dung lượng logic,
   dung lượng vật lý thực tế, dung lượng chia sẻ (shared) và riêng biệt (exclusive),
   tính toán tỉ lệ tiết kiệm không gian đĩa tự động.
3. **Phát hiện Ransomware theo Shannon Entropy**: `guard::RansomwareDetector`
   theo dõi các thao tác ghi tệp. Khi một tiến trình ghi dồn dập nhiều tệp với
   entropy cao liên tục ($H \ge 7.2$ bit/byte, đặc trưng của ciphertext hoặc dữ
   liệu ngẫu nhiên) trong cửa sổ thời gian ngắn:
   - Tự động hạ ưu tiên tiến trình xuống Idle;
   - Bóp xung nhịp còn 10%;
   - Ghi audit tiếng Việt và kích hoạt cảnh báo tới người dùng qua daemon;
   - Gợi ý hoàn tác (Rollback) về snapshot sạch gần nhất.
4. **Kiểm thử thực nghiệm (10.000 tệp)**: `tests/test_redteam.cpp` kiểm chứng
   kịch bản tấn công trên 10.000 tệp: sau khi bị mã độc mã hoá toàn bộ, lệnh rollback
   khôi phục lại 10.000/10.000 tệp khớp 100% mã băm SHA-256 ban đầu.
5. **Giới hạn đã biết**: Cơ chế không chống được ransomware chạy với quyền `fs.write`
   hợp lệ nếu kẻ tấn công cố tình ghi chậm dưới ngưỡng tốc độ hoặc ghi đè dữ liệu có
   entropy thấp (như xoá bằng số 0). Bảo vệ toàn diện đòi hỏi kết hợp phân quyền
   hộp cát tối thiểu.

### 10.5 Guard mở rộng khi máy rảnh (Idle Guard — P2.15)

Ngăn chặn phần mềm độc hại hoặc ứng dụng nền lợi dụng lúc người dùng không ngồi
trước máy tính để thu thập dữ liệu nhạy cảm (`include/yui/guard.hpp`,
`kernel-services/guard.cpp::IdleGuard`):

1. **Quy tắc máy rảnh**: Theo dõi thời gian rảnh dựa trên sự kiện thao tác của
   người dùng (bàn phím, chuột, cảm ứng). Khi máy rảnh vượt ngưỡng (mặc định 5 phút / 300 s):
   - Mọi tiến trình chạy nền không có cửa sổ hiển thị (`has_window == false`) và
     không có tương tác chủ động của người dùng (`user_initiated == false`) mà
     cố tình mở micro (ghi âm), camera, đọc danh bạ/tệp nhạy cảm hoặc gửi gói tin ra mạng
     đều bị coi là hành vi bất thường.
2. **Thu hồi quyền tức thì $O(1)$**: Badge của thẻ năng lực liên quan bị đưa vào danh
   sách thu hồi tức thì (`revoked_badges_` băm $O(1)$) để vô hiệu hoá ngay lập tức
   mọi yêu cầu kế tiếp; tiến trình bị bóp xung về 20%.
3. **Thông báo và audit**: Phát cảnh báo tiếng Việt ghi rõ tiến trình, tài nguyên
   và thời gian máy đã rảnh để người dùng quyết định tiếp tục chặn hay cho phép.
4. **Kiểm thử Red-Team 6 kịch bản**: Đã kiểm thử đầy đủ các tình huống:
   - Rảnh + mở micro lén ⇒ chặn + thu hồi thẻ $O(1)$;
   - Đang gõ phím + gọi video call ⇒ cho phép hợp lệ;
   - Rảnh + đọc trộm danh bạ ⇒ chặn;
   - Rảnh + phát tán dữ liệu ra mạng ⇒ chặn;
   - Ứng dụng có cửa sổ đang chạy (nghe nhạc/xem phim) ⇒ không báo nhầm;
   - Thao tác chập chờn trước ngưỡng thời gian rảnh ⇒ không kích hoạt giả mạo,
     chỉ kích hoạt khi thực sự đủ điều kiện rảnh.

### 10.6 An ninh Bộ gõ bên thứ ba (Third-Party IME Sandbox — P3.13)

Bộ gõ (IME) là một trong những thành phần có nguy cơ rò rỉ dữ liệu cao nhất vì
tiếp nhận mọi phím gõ của người dùng. Để bảo vệ an toàn nhưng vẫn cho phép người
dùng cài đặt các engine bộ gõ mở rộng từ bên thứ ba (IBus/Fcitx5):

1. **Hộp cát cô lập bắt buộc**: Engine IME bên thứ ba luôn chạy trong hộp cát `isolated`
   với quyền năng lực tối thiểu duy nhất là `input.compose` (`kRightInputCompose`),
   chỉ được đọc/ghi trong thư mục cấu hình riêng (`~/.config/<ime-id>`),
   **tuyệt đối không có quyền mạng** (`net:*` trong deny) và không có quyền `admin`.
2. **Quy tắc cứng ở tầng OS bảo vệ trường nhạy cảm**: Trình môi giới `ThirdPartyImeHost`
   tại tầng OS chặn đứng phím gõ trước khi chuyển tới engine ngoài:
   - Khi con trỏ nằm ở các trường nhạy cảm: **mật khẩu (Password)**, **mã PIN (Pin)**,
     **số thẻ / OTP (DigitsOnly)**, **địa chỉ web (Url)**, và **thư điện tử (Email)**:
     Hệ thống chuyển **0** phím tới engine ngoài (engine hoàn toàn không nhận được phím,
     không có dữ liệu để ghi lén hay học từ điển).
   - Tự động ghi nhật ký kiểm toán (audit log) tiếng Việt nêu rõ lý do bảo vệ.
3. **Phòng vệ Red-Team**: Ngăn chặn toàn diện các hành vi của keylogger đội lốt bộ gõ:
   - Cố mở socket mạng exfiltration ra ngoài C2 ⇒ bị Enforcer chặn ngay lập tức;
   - Cố đọc trộm khoá bí mật SSH (`~/.ssh/id_ed25519`) hay tài liệu người dùng ⇒ bị chặn;
   - Cố ghi nhật ký phím lén ra ngoài thư mục riêng ⇒ bị chặn.

### 10.7 Thẩm định chữ ký gói cài đặt và hộp cát mặc định (App Signature Gate — P3.12)

Áp dụng mô hình bảo vệ zero-trust đối với mọi phần mềm cài đặt vào hệ thống:

1. **Gói ký hợp lệ bởi khoá chủ quyền (Sovereign Keyring)**:
   Xác minh chữ ký số Ed25519 trên chuỗi canonical dựng lại của manifest.
   Nếu hợp lệ, gói được cài đặt và áp dụng chính sách riêng đã qua thẩm duyệt.
2. **Gói ký sai hoặc bị can thiệp (Tampered / Invalid Signature)**:
   Nếu chữ ký không khớp hoặc nội dung manifest bị sửa dù chỉ 1 byte:
   TỪ CHỐI cài đặt ngay lập tức, ghi rõ lý do cảnh báo can thiệp.
3. **Gói không có chữ ký hoặc ký bằng khoá lạ (Unsigned / Untrusted Key)**:
   Bắt buộc người dùng phải xác nhận qua hộp thoại tin cậy. Nếu người dùng đồng ý:
   Gói bị đưa vào hộp cát `isolated` ở chế độ ngắt mạng hoàn toàn (`net: none`),
   tước toàn bộ quyền `admin` và `power`, chỉ cho phép đọc/ghi trong thư mục dữ liệu riêng
   (`/home/user/app-data/<app_id>`).

