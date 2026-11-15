# kernel-cap — Tầng 1: Lõi Giám Sát Bảo Mật (Capability Microkernel)

Rust, `no_std` + `alloc`, **`#![deny(unsafe_code)]`**.

| Tệp | Nội dung |
|---|---|
| `src/lib.rs` | 15 quyền có tên, `Capability` (dẫn xuất chỉ thu hẹp), `CapTable` (thu hồi bắc cầu), `Token` ký HMAC-SHA-256, `Authority` cấp/kiểm thẻ |
| `src/sha256.rs` | SHA-256, HMAC-SHA-256, so sánh hằng thời gian — tự viết, có vector RFC |

## Nguyên lý (lấy cảm hứng seL4, mã nguồn viết mới)

1. **Không có thẩm quyền môi trường**: không có "root", chỉ có thẻ năng lực.
2. **Dẫn xuất chỉ thu hẹp**: `parent.derive(x) = parent.rights & x`; rỗng ⇒ `None`.
3. **Thu hồi bắc cầu**: thu hồi một slot là thu hồi cả cây con của nó.
4. **Thẻ là dữ liệu ký**: HMAC-SHA-256 trên chuỗi chuẩn hoá cố định; bên nhận
   **dựng lại** chuỗi rồi so, không ký lại chuỗi nhận được.

Chuỗi chuẩn hoá và thứ tự kiểm tra: [`docs/PROTOCOL.md`](../docs/PROTOCOL.md) §5,
[`docs/SECURITY.md`](../docs/SECURITY.md) §2.

## Trạng thái

- [x] Bảng năng lực, dẫn xuất, thu hồi, kiểm quyền (6 unit test)
- [x] Thẻ ký HMAC + xác minh có kiểu lỗi (`Expired`, `NotYetValid`, `BadMac`, `UnknownIssuer`)
- [x] SHA-256/HMAC tự viết, vector RFC 6234/4231
- [ ] Khung phần cứng (MMU, IPC, chuyển ngữ cảnh) — Phase P2, `arch/` + `kernel-frame`
- [ ] Sovereign Keyring + Ed25519 cho gói `.bundle` — Phase P2 (xem `docs/PLAN.md` P2.4)
