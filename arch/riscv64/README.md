# Tầng 0 — RISC-V 64 (chip mở)

## Đường khởi động

```
ZSBL → OpenSBI (M-mode) → U-Boot → yui-boot (S-mode) → yui-kernel
```

- OpenSBI giữ M-mode; YuiVieOS chạy ở S-mode và **không** giả định có H-extension
  (nếu có, tầng 3 có thể dùng hypervisor để cô lập micro-VM mạnh hơn).
- `RVV` (vector) được dò qua `hwprobe`; nếu có, `detect_simd()` trả `rvv`.
- Device tree là nguồn thông tin phần cứng duy nhất.

## Trạng thái

- [x] `detect_simd()` nhánh RVV, profiler chạy được trên board Linux RISC-V
- [ ] `yui-boot` S-mode stub, driver PLIC/CLINT
