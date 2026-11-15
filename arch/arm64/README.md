# Tầng 0 — ARM64 (điện thoại / bo mạch)

## Đường khởi động

```
BootROM → U-Boot / systemd-boot (UEFI) → yui-boot (EL2 → EL1) → yui-kernel
```

Ở ARM64, YuiVieOS dùng:

- `PSCI` cho khởi động phụ (secondary cores) và reset;
- `GICv3` cho ngắt, `ARM generic timer` cho tickless;
- device tree (hoặc ACPI trên máy UEFI) để dò phần cứng — cùng một
  `YuiBootInfo` như x86_64 sau khi parse;
- NEON là bắt buộc từ ARMv8-A nên `detect_simd()` trả `neon` (SVE nếu có).

## Điện thoại

Điện thoại Android khóa bootloader thường không cho nạp nhân ngoài. Hai đường
thực tế:

1. **Bo mạch mở** (Raspberry Pi, Rockchip, Pine64): cài trực tiếp như một distro.
2. **Máy đã mở khoá**: dùng làm *host* cho tầng 3 (hộp cát Android) thay vì thay
   toàn bộ hệ điều hành.

## Trạng thái

- [x] `detect_simd()` NEON/SVE, profiler đọc được `/proc` của ARM64 Linux
- [x] hộp cát Android (Waydroid-lite) ở mức chính sách
- [ ] `yui-boot` EL2→EL1 stub, driver GICv3
