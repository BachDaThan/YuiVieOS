# pkg-stripper — Tầng 5: Trình Nén & Gọt Tỉa Ứng Dụng Thông Minh

`apppack.cpp` + CLI `tools/trim` (tên cài đặt: `yui-trim`).

## Quy trình (một chiều, có thể kiểm chứng)

```
thư mục app
  → 1. quét dấu hiệu bảo vệ (DRM/anti-tamper): thấy ⇒ stripped=false, GIỮ NGUYÊN 100% byte
  → 2. phân loại rác: .git/.hg/.svn, __pycache__, .cache, *.debug, locale lạ, docs (tuỳ chọn);
       node_modules/.venv/target/build chỉ khi --strip-build-dirs (cây mã nguồn)
  → 3. strip ELF: zero-fill .debug_*, .comment, .note*, .gnu_debuglink
  → 4. khử trùng lặp bằng SHA-256 (kind 'd', hardlink khi giải nén)
  → 5. đóng gói YUIA (từ chối đường dẫn tuyệt đối và "..")
  → 6. nén YUI-LZ → .yuip + SHA-256 toàn plaintext + CRC32 payload
```

`keep_locales` mặc định `{vi, en}`; nhận diện `locale/`, `locales/`, `lang/`,
`i18n/` (cả thư mục `locale/fr/` lẫn tệp `locales/fr.pak` kiểu Electron).
Giải nén vào tmpfs và chạy từ RAM.

### Dấu hiệu bảo vệ (bước 1, `detect_protection`)

| Dấu hiệu | Ví dụ |
|---|---|
| Widevine CDM | thư mục `WidevineCdm/`, `libwidevinecdm.so`, `widevinecdm.dll` |
| Chữ ký toàn vẹn / VMP | `<tệp>.sig` nằm cạnh chính `<tệp>` thực thi/thư viện (castlabs ECS) |
| Section của trình bảo vệ (ELF/PE) | `.vmp0/.vmp1/.vmp2` VMProtect, `.themida`, `.winlice`, `.enigma1/2`, `.bind` SteamStub |

Khi gặp: không bỏ tệp nào, không zero-fill ELF, không khử trùng lặp; báo cáo in
`stripped=false` + lý do; gói chỉ là bản nén không mất dữ liệu. Sửa những tệp
này thường làm app hỏng và có thể vi phạm giấy phép. `--verbatim` ép chế độ này
bằng tay. Liên kết mềm / tệp đặc biệt không được lưu — có cảnh báo, không im lặng.

## Tiêu chí đo (Phase P1.6)

- [x] Bộ test `tests/test_tools.cpp` (`apppack.*`): tròn vẹn, tamper bị phát hiện, chống zip-slip, strip ELF,
  đọc section ELF/PE an toàn, bố cục Electron (giữ `node_modules`, gọt `.pak`), 6 kiểu app có bảo vệ
  ⇒ `stripped=false` + bung ra trùng từng byte, bung vào đích tương đối
- [x] App Electron thật — job CI `realapp` (`scripts/realapp-trim.sh`, 2026-11-21, run 44921693171):
  Electron 33.2.1 chính thức + app thử: 261 MiB → gói 107 MiB (**nhỏ hơn 59 %**), sau gọt chạy lại
  ra kết quả **giống hệt** (renderer + `node_modules` + `--lang=vi`; `--lang=fr` đã gọt thì lùi về en-US);
  VSCodium 1.143.17232: 602 MiB → 223 MiB (**nhỏ hơn 63 %**), CLI chạy được trước/sau như nhau.
  *Nói thật:* phần lớn là nhờ nén — cây sau khi bung chỉ nhỏ hơn 11–15 %, và gói `.yuip` vẫn
  **lớn hơn** zip/tar.gz chính thức 6–9 %.
- [x] App có DRM — Google Chrome 158 (kèm Widevine CDM): `stripped=false`, bung ra 253/253 tệp trùng
  đường dẫn + quyền + SHA-256 (**0 byte bị sửa**), bản bung chạy headless được
- [ ] Thời gian gọt < 10 s cho app 500 MB trên **máy yếu** — mới đo trên runner CI: Electron 261 MiB mất 4,7 s
