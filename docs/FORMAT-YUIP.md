# Định dạng dữ liệu — Format reference

Mọi định dạng ở đây **byte-identical giữa C++ và Rust** (khi cả hai cài đặt một
định dạng), và có test hai phía. Số nguyên nhiều byte là **little-endian**.

---

## 1. YUI-LZ

Codec LZ77 theo byte, cửa sổ 64 KiB, không có bảng Huffman (giữ đơn giản, giải mã
nhanh, không cần bộ nhớ phụ).

### 1.1 Token

Mỗi chuỗi gồm: `token` (1 byte) → literals → distance (2 byte) → phần mở rộng độ dài khớp.

```
token = (lit_len << 4) | (match_len - 4)
```

- `lit_len` 0…15; nếu đúng 15 thì đọc thêm chuỗi byte: cộng dồn từng byte, byte
  `0xFF` nghĩa là "còn nữa". Giá trị cuối = `15 + Σ`.
- `match_len` 4…18 (`token & 0x0F` = 0…14); nếu nibble = 15 thì đọc chuỗi mở rộng
  cùng quy tắc, `match_len = 4 + 15 + Σ`.
- `distance` = 2 byte LE, `1…65535`, trỏ về **sau** (khoảng cách lùi).
- **Chuỗi cuối cùng** chỉ có literals và **không** có `distance`: bộ giải mã dừng
  khi gặp cuối luồng sau khi đọc literals.

### 1.2 Ghi chú cài đặt

- Bảng băm 4 byte (`65536` bucket) + chuỗi `prev` để tìm khớp; tối đa 4 lần thử
  mỗi vị trí (nhanh, đủ tốt cho trang RAM và app bundle).
- Bộ nén đăng ký **mọi** vị trí mà một khớp đã nhảy qua, nhờ vậy khớp sau tốt hơn.
- Bộ giải mã kiểm tra: `ip` không vượt `input`, `distance ≤ out.len()`, đầu ra
  không vượt `out_len` dự kiến, và **cuối cùng `out.len() == out_len`**. Sai một
  điều ⇒ `None`/`false`, không bao giờ trả dữ liệu nửa vời.
- Khớp có thể **chồng lấn** (distance < match_len): giải mã copy từng byte.

### 1.3 Ví dụ

`"aaaaaaaa"` (8 byte) →

```
0x04              token: lit_len=0, match_len-4=4  → khớp 8 byte
0x01 0x00         distance = 1
0x20              token cuối: 2 literals? không — xem ghi chú
```

Chính xác hơn, với `"aaaaaaaa"`: khớp 7 byte ở distance 1 (`a` + 7 bản sao), rồi
1 literal cuối. Đọc `tests/test_codec_json.cpp` để có vector chính xác thay
vì suy đoán — đây là lý do định dạng có test vector thay vì chỉ có tài liệu.

### 1.4 Kích thước tối đa

`max_compressed_len(n) = n + n/255 + 16`. Đây là biên trên thực tế: mỗi 255 byte
mở rộng chỉ tốn 1 byte.

---

## 2. Container YUIP

```
offset  size  field
0       4     magic = 'Y' 'U' 'I' 'P'
4       1     version = 1
5       1     codec_id (1 = YUI-LZ, 0 = stored)
6       2     flags (bit 0 = có checksum, hiện luôn 1)
8       8     orig_size    (u64, kích thước plaintext)
16      8     payload_size (u64, kích thước sau nén)
24      32    sha256(plaintext)
56      4     crc32(payload)
60      …     payload
```

Tổng header = **78 byte**. `codec_id = 0` nghĩa là payload chính là plaintext
(dùng khi nén không có lợi — bộ nén không bao giờ trả gói phình).

Thứ tự kiểm khi đọc: magic → version → `payload_size` khớp độ dài thật →
**CRC32 payload** (rẻ, bắt hỏng bit) → giải nén → `orig_size` khớp chính xác →
**SHA-256 plaintext** (bắt sửa đổi có chủ đích).

---

## 3. Kho YUIA (bên trong YUIP)

```
offset  size  field
0       4     magic = 'Y' 'U' 'I' 'A'
4       1     version = 1
5       3     reserved = 0
8       4     count (u32, số mục)
12      …     mục × count
```

Mỗi mục:

```
u16  name_len
u8[] name            (UTF-8, không có NUL, KHÔNG được tuyệt đối, KHÔNG chứa "..")
u8   kind            (0 = file, 1 = thư mục, 'd' = bản sao trùng lặp)
u32  mode            (bit quyền POSIX)
u64  size
u8[] data            (size byte; kind='d' ⇒ size = 0, dữ liệu lấy từ bản gốc)
```

`kind='d'` là kết quả của bước khử trùng lặp bằng SHA-256: mục này trỏ tới mục
cùng hash đã ghi trước đó (hardlink khi giải nén, copy nếu filesystem không hỗ
trợ). Nhờ vậy app có 10 bản `libnode.so` chỉ tốn một bản.

Giải nén **từ chối** tuyệt đối đường dẫn và `..` — đây là biện pháp chống
"zip-slip", có test trong `test_tools.cpp`.

---

## 4. Blob vault

```
offset  size  field
0       4     magic = 'Y' 'U' 'I' 'V'
4       1     version = 1
5       1     log2_n     (scrypt N = 1 << log2_n; mặc định 15)
6       1     r          (mặc định 8)
7       1     p          (mặc định 1)
8       16    salt
24      16    nonce
40      8     plain_len (u64)
48      32    tag = HMAC-SHA-256(master, bytes[0..48] ‖ ciphertext)
80      …     ciphertext
```

`master = scrypt(password, salt, 1<<log2_n, r, p, 64)`; `enc_key = master[0..32]`;
`ciphertext = plain XOR SHAKE-256(enc_key ‖ nonce ‖ counter)` (SHAKE-256 ở chế độ
XOF, lấy đúng `plain_len` byte).

Khi mở: kiểm tag **trước** khi giải mã (encrypt-then-MAC), rồi mới giải mã. Sai
tag ⇒ từ chối. Bộ nhớ cho scrypt được cấp phát theo `log2_n`; `log2_n > 20` bị
từ chối để một blob độc hại không làm cạn RAM (tấn công từ chối dịch vụ).

---

## 5. Gói cập nhật `.bundle` (đặc tả)

```
bundle.yuib = zip (không nén) chứa:
  manifest.json      { "version": "0.2.0", "channel": "stable",
                       "files": [ {"path": "usr/bin/yuid", "sha256": "…", "mode": "0755"} ] }
  manifest.sig       Ed25519(manifest.json) — 64 byte
  payload/…          file thật
```

Chưa cài đặt; xem [`SECURITY.md`](SECURITY.md) mục 6 để biết vì sao và
[`PLAN.md`](PLAN.md) để biết kế hoạch.

---

## 6. Tệp trạng thái

`--state-dir` (mặc định `/var/lib/yui`) chứa:

| Tệp | Nội dung |
|---|---|
| `graph.json` | đồ thị tri thức (`{"nodes":[…],"edges":[…]}`, cùng schema với `graph.export`) |
| `profile.json` | hồ sơ phần cứng + k/W đã chốt (để lần khởi động sau không dò lại) |
| `audit.log` | mỗi dòng một quyết định hộp cát bị từ chối: thời gian, chủ thể, hành động, lý do |

Ghi nguyên tử: ghi `<tên>.tmp` → `fsync` → `rename`. Mất điện giữa chừng không
làm hỏng `graph.json`.

---

## 7. Vì sao chọn "tự viết" thay vì Zstd/gzip

1. **Không phụ thuộc**: hệ điều hành cài trên máy 2 GB không nên cần libzstd đúng
   phiên bản để khởi động.
2. **Đối xứng hai ngôn ngữ**: cùng một codec trong Rust (`no_std`) và C++ mà
   không cần FFI — FFI ở đây chính là chỗ dễ sinh lỗi an toàn bộ nhớ nhất.
3. **Đủ tốt cho mục tiêu**: trang RAM lặp nhiều (≥ 3×), app bundle giảm 30–60 %.
4. **Kiểm chứng được**: 78 byte header + CRC + SHA khiến hỏng dữ liệu thành lỗi
   rõ ràng, không thành crash.

Backend Zstd vẫn được dành chỗ (`codec_id` và `Codec::Zstd`) cho trang lạnh —
nhưng nó là **tuỳ chọn**, không phải điều kiện để hệ thống chạy.
