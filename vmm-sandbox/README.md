# vmm-sandbox — Tầng 3: Hộp Cát Zero-Trust (MicroVM / VMM)

Cách ly ba bậc: `native` → `isolated` → `microvm`. Chính sách lúc spawn là quyết
định cuối; thẻ năng lực chỉ thu hẹp thêm.

| Tệp | Nội dung |
|---|---|
| `sandbox.cpp` | `Policy`/`Action`/`Enforcer`: phân bậc, phạm vi `fs`/`net`, deny theo tên quyền, audit tiếng Việt |
| `policies/*.json` | 4 chính sách mẫu (version 1, east-west deny, DNS direct deny), **được nạp và kiểm trong `test_sandbox`** |
| `vmm/vm-manifest.schema.json` | Hợp đồng manifest micro-VM: mạng `none` mặc định, không đĩa host, hỗ trợ trường zero-trust |
| `vmm/README.md` | Ba điểm nối với VMM thật |

## Vòng đời & Cách ly Micro-VM (P3.9)

- `MicroVmSupervisor` và `MicroVmSession`:
  - Trạng thái vòng đời: `Running` → `Quarantined` (đóng băng/cách ly) → `Destroyed` (tiêu huỷ).
  - Bắt buộc qua bước đóng băng/cách ly trước khi tiêu huỷ phiên.
  - Khi phát hiện vi phạm Lớp 1 hoặc thẻ năng lực bị thu hồi, phiên tương ứng bị cách ly và tiêu huỷ lập tức; các phiên độc lập khác không bị ảnh hưởng.
  - Mọi yêu cầu guest tới phiên đã đóng băng hoặc đã huỷ đều bị phủ quyết fail-closed.

## Egress Zero-Trust, Anti-DNS-Tunneling & Secretless Broker (P3.10)

- **East-West Zero-Trust**: Giao tiếp ngang giữa các sandbox mặc định `deny` (`allow_east_west: false`).
- **Chặn DNS trực tiếp**: Cấm cổng 53 trực tiếp ra Internet (`allow_direct_dns: false`), chỉ qua resolver/broker nội bộ.
- **Anti-DNS-Tunneling**: Kiểm tra RFC 1035 (tên $\le 253$ octet, nhãn $\le 63$ octet) và đo lường Shannon entropy nhãn ($H \le 4.2$ bit/ký tự); ngăn chặn các công cụ đào hầm exfiltration như dnscat2 / iodine.
- **Secretless Broker**: Quản lý bí mật dịch vụ ngoài biên micro-VM; gắn header xác thực ngoài biên mà không rò rỉ token/secret vào guest, môi trường hay nhật ký audit.

## Điều tuyệt đối không xảy ra

- MicroVM **không** có syscall host, không `fs.*`, không `input`, không `spawn`.
- App cũ (Windows/Android) chỉ thấy GPU passthrough và các cổng dịch vụ của
  `yuid`; mọi thứ khác đi qua thẻ năng lực.
- Không có mạng trừ khi manifest khai `network` và capabilities có `net.connect`.

## Trạng thái

- [x] Bộ thực thi chính sách + 4 policy mẫu + schema manifest
- [x] Vòng đời Micro-VM `MicroVmSession` / `MicroVmSupervisor` (P3.9)
- [x] Egress Zero-Trust, Anti-DNS-Tunneling & Secretless Broker (P3.10)
- [ ] `yui-vmmd`: supervisor micro-VM trên KVM — Phase P3
- [ ] Lớp Windows (Wine/Proton + VKD3D) và Waydroid-lite trong micro-VM — Phase P3
- [ ] Bộ test "cố thoát hộp cát" (đọc `/etc/shadow`, mở socket, `ptrace`) — Phase P3
