# Micro-VM supervisor (Tầng 3)

Thư mục này mô tả và kiểm tra *hợp đồng* của micro-VM, không phải một VMM đầy đủ.
Một VMM thật (ví dụ một bản fork tối giản của crosvm/rust-vmm) cắm vào đây qua
ba điểm nối đã cố định:

| Thành phần | Trách nhiệm | Trạng thái trong repo |
|---|---|---|
| `yui-vmmd` (chưa có trong repo) | tạo VM, gán PCI passthrough, vòng đời | spec trong `vm-manifest.schema.json` |
| `libyui` | cầu JSON-RPC qua virtio-vsock tới `yuid` | đã có (`core/src/capi`) |
| Hộp cát chính sách | quyết định trước khi VM được tạo | đã có (`core/src/sandbox`) |

## Vì sao chỉ có đặc tả ở đây

Một micro-VM đúng nghĩa cần KVM/HAXM/đặc quyền phần cứng. Repo này cố tình
không giả lập điều đó: thay vào đó nó đặc tả manifest, kiểm tra manifest, và
đảm bảo mọi yêu cầu từ VM đều đi qua capability token — phần còn lại là công
việc của driver VMM khi chạy trên máy thật.

## Manifest

Xem `vm-manifest.schema.json`. Mỗi manifest trả lời bốn câu hỏi:

1. VM này chạy cái gì? (`image`, `cmdline`)
2. Nó được thấy bao nhiêu RAM/CPU? (`memory_mb`, `vcpus`)
3. Nó được nối ra ngoài bằng gì? (`vsock`, `gpu`, `network = "none"` mặc định)
4. Nó được cấp quyền gì qua `yuid`? (`capabilities`, đi qua `sandbox.check`)

Mặc định là **không có mạng, không có ổ đĩa host, không có thiết bị USB**.
