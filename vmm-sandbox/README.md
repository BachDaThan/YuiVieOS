# vmm-sandbox — Tầng 3: Hộp Cát Zero-Trust (MicroVM / VMM)

Cách ly ba bậc: `native` → `isolated` → `microvm`. Chính sách lúc spawn là quyết
định cuối; thẻ năng lực chỉ thu hẹp thêm.

| Tệp | Nội dung |
|---|---|
| `sandbox.cpp` | `Policy`/`Action`/`Enforcer`: phân bậc, phạm vi `fs`/`net`, deny theo tên quyền, audit tiếng Việt |
| `policies/*.json` | 4 chính sách mẫu, **được nạp và kiểm trong `test_sandbox`** (14 tình huống) |
| `vmm/vm-manifest.schema.json` | Hợp đồng manifest micro-VM: mạng `none` mặc định, không đĩa host |
| `vmm/README.md` | Ba điểm nối với VMM thật |

## Điều tuyệt đối không xảy ra

- MicroVM **không** có syscall host, không `fs.*`, không `input`, không `spawn`.
- App cũ (Windows/Android) chỉ thấy GPU passthrough và các cổng dịch vụ của
  `yuid`; mọi thứ khác đi qua thẻ năng lực.
- Không có mạng trừ khi manifest khai `network` và capabilities có `net.connect`.

## Trạng thái

- [x] Bộ thực thi chính sách + 4 policy mẫu + schema manifest
- [ ] `yui-vmmd`: supervisor micro-VM trên KVM — Phase P3
- [ ] Lớp Windows (Wine/Proton + VKD3D) và Waydroid-lite trong micro-VM — Phase P3
- [ ] Bộ test "cố thoát hộp cát" (đọc `/etc/shadow`, mở socket, `ptrace`) — Phase P3
