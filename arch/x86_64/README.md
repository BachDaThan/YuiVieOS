# Tầng 0 — x86_64 (BIOS Legacy & UEFI)

## Đường khởi động

```
BIOS/UEFI → limine (hoặc GRUB2 cho BIOS Legacy) → yui-boot (32→64 bit stub)
   → yui-kernel (Tầng 1 microkernel) → yui-frame (Tầng 2) → yuid (Tầng 4)
```

`yui-boot` chịu trách nhiệm:

1. dò E820/UEFI memory map và chuyển cho kernel dưới dạng `YuiBootInfo`;
2. bật long mode (hoặc kiểm tra UEFI đã bật sẵn);
3. nạp `yui.cpio` (chứa `yuid`, `libyui.so`, policy, font) vào RAM;
4. chuyển quyền điều khiển vào `_start` với `rdi = &YuiBootInfo`.

## Vì sao hai đường BIOS + UEFI

Máy đời cổ (mục tiêu của YuiVieOS) thường chỉ có BIOS Legacy, trong khi máy mới
chỉ có UEFI. Ảnh cài đặt chứa cả hai: `boot/bios.img` (MBR + limine-bios) và
`EFI/BOOT/BOOTX64.EFI` (limine-uefi).

## Kiểm tra phần cứng lúc khởi động

`yui-profiler` (Tầng 1) chạy ngay sau khi kernel vào user space, đọc CPUID,
`/proc`-tương đương do kernel cung cấp, nhiệt độ qua ACPI, và ghi
`/var/lib/yui/profile.json`. Tầng 2 đọc file này để chọn chế độ máy yếu/máy mạnh.

## Trạng thái

- [x] `linker.ld`, quy ước `YuiBootInfo`, đặc tả luồng khởi động
- [x] Hardware profiler chạy được (trên Linux host, dùng cho CI và cho bản chạy trong hộp cát)
- [ ] `yui-boot` stub (cần assembler + toolchain cross, xem docs/BUILD.md)
- [ ] driver framebuffer/ACPI (Tầng 2/5)
