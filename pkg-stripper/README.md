# pkg-stripper — Tầng 5: Trình Nén & Gọt Tỉa Ứng Dụng Thông Minh

`apppack.cpp` + CLI `tools/trim` (tên cài đặt: `yui-trim`).

## Quy trình (một chiều, có thể kiểm chứng)

```
thư mục app
  → 1. quét chữ ký bảo vệ (DRM/anti-tamper): thấy dấu hiệu ⇒ GIỮ NGUYÊN bản gốc
  → 2. phân loại rác: .git, node_modules, __pycache__, .cache, build, docs, locale lạ
  → 3. strip ELF: zero-fill .debug_*, .comment, .note*, .gnu_debuglink
  → 4. khử trùng lặp bằng SHA-256 (kind 'd', hardlink khi giải nén)
  → 5. đóng gói YUIA (từ chối đường dẫn tuyệt đối và "..")
  → 6. nén YUI-LZ → .yuip + SHA-256 toàn plaintext + CRC32 payload
```

`keep_locales` mặc định `{vi, en}`; nhận diện `locale/`, `locales/`, `lang/`,
`i18n/`. Giải nén vào tmpfs và chạy từ RAM.

## Tiêu chí đo (Phase P1.6)

- [x] Bộ test `test_tools` (61 check): tròn vẹn, tamper bị phát hiện, chống zip-slip, strip ELF
- [ ] Trên app Electron thật (VS Code / Zalo): giảm ≥ 35 % kích thước, chạy được sau khi gọt
- [ ] Trên app có DRM: `stripped=false` và **không** sửa một byte nào
- [ ] Thời gian gọt < 10 s cho app 500 MB trên máy yếu
