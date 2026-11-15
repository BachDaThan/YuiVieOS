# App Yui (Flutter) — Tầng 5

Trạng thái: **khung sườn (skeleton)**. Mã trong `app/` viết xong phần giao tiếp
và ba màn hình cơ bản, nhưng **chưa được build** trong môi trường phát triển của
repo (không có Flutter SDK). Đừng coi nó là "đã chạy" — xem `docs/PLAN.md`
mục E1.

## Chạy thử (khi bạn có Flutter ≥ 3.19)

```bash
cd userspace-vi/ui/app
flutter pub get
flutter run -d linux        # hoặc -d android / -d macos
```

App nói chuyện với `yuid` qua **Unix socket** `/run/yuid.sock` (một đối tượng
JSON mỗi dòng). Không cần plugin FFI: `dart:io` có sẵn `Socket.connect`.

```bash
./build/bin/yuid --socket /tmp/yui.sock --state-dir /tmp/yui-state &
```

rồi trong app, màn hình "Kết nối" đổi đường dẫn socket thành `/tmp/yui.sock`.

## Ba màn hình

| Màn hình | Gọi `yuid` | Việc |
|---|---|---|
| Trò chuyện / Lệnh | `ime.convert`, `graph.query`, `web.search` | gõ tiếng Việt, hỏi đáp, tìm trong tri thức |
| Đồ thị | `graph.upsert`, `graph.link`, `graph.query`, `graph.stats` | thêm nút, nối cạnh, xem thống kê |
| Khoá | `vault.seal`, `vault.open` | niêm phong/mở dữ liệu bằng mật khẩu |

## Vì sao socket chứ không FFI

- Chạy được trên điện thoại/desktop mà không cần biên dịch `libyui` cho từng
  kiến trúc (Android arm64, macOS arm64, Linux x86_64…).
- Không crash app khi daemon chết: chỉ mất kết nối, hiện thông báo tiếng Việt.
- FFI (`libyui`) vẫn là đường cho app cần độ trễ thấp; cả hai chia cùng một
  nghiệp vụ nên không lệch hành vi.

## Còn thiếu

- Build thật + ảnh chụp màn hình trong README.
- Lưu mật khẩu vào khoá hệ điều hành (chưa có TPM/keychain tích hợp).
- Bàn phím Telex trong app: hiện gõ qua `ime.convert` theo từng từ, chưa phải
  bộ gõ từng phím như IBus.
