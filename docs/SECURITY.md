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

- 15 quyền có tên trên một `u32`: `fs.read`, `fs.write`, `fs.exec`, `net.connect`,
  `net.listen`, `input`, `camera`, `mic`, `gpu`, `model`, `graph`, `vault`,
  `spawn`, `power`, `admin`.
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

Ví dụ chính sách thật trong repo: `sandbox/policies/yui-app.policy.json`,
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

## 6. Cập nhật hệ thống (đặc tả, chưa cài đặt)

```
bundle/
  manifest.json     { "version": "...", "files": [{"path": "...", "sha256": "...", "mode": "0644"}] }
  manifest.sig      Ed25519(manifest.json)  — khoá phát hành YuiVieOS
  payload/          file thật
```

Cài đặt: xác minh chữ ký manifest bằng khoá trong `/etc/yui/trusted-keys/` →
kiểm từng SHA-256 sau khi giải nén → chỉ ghi vào phân vùng A/B → đổi con trỏ
boot. Không có mạng: bundle chép qua USB, vẫn xác minh y hệt.

**Chưa có Ed25519** trong repo (không có thư viện, tự viết đường cong là rủi ro
cao). Kế hoạch: implement Ed25519 clean-room kèm vector RFC 8032, hoặc dùng
kernel keyring nếu chạy trên Linux host. Cho tới lúc đó, **đừng** coi `.bundle`
là đã an toàn — nó mới là đặc tả.

## 7. Những gì chưa được bảo vệ

Danh sách này cố ý đầy đủ, vì biết giới hạn quan trọng hơn tin vào khẩu hiệu:

1. **Chưa có nhân thật**: Tầng 1/2 là crate chạy trong tiến trình test, không có
   MMU/ring cách ly. Hộp cát hiện là *quyết định chính sách* + kiểm tra, chưa
   phải cách ly phần cứng.
2. **Chưa có micro-VM**: `sandbox/vmm` là schema + đặc tả. Cách ly game/Android
   ở mức KVM **chưa** tồn tại trong repo.
3. **Chưa có chữ ký số** cho cập nhật (mục 6).
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

1. Không thêm `unsafe` vào `crates/` (`#![deny(unsafe_code)]` cố ý).
2. Quyền mới phải có tên trong `rights`, không dùng số trần.
3. Mọi hàm giải nén/nạp dữ liệu phải: kiểm độ dài → kiểm toàn vẹn → mới parse.
4. Mọi quyết định cho/không phải có `reason` đọc được bằng tiếng Việt.
5. Mật mã mới phải kèm vector kiểm thử chuẩn và dòng "giới hạn đã biết" ở mục 4.
