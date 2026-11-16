/* YuiVieOS Beta — aarch64 QEMU virt.
 *
 * QEMU's -kernel ELF loader honours the link addresses, and virt's RAM starts at
 * 0x40000000 with the payload at +0x80000. MMU stays off in the boot frame
 * (identity access, cache-aside), so physical == virtual here.
 */
OUTPUT_FORMAT(elf64-littleaarch64)
ENTRY(_start)

SECTIONS
{
    /* 0x4008_0000 để lại đúng 512 KiB dưới ảnh nhân — QUÁ HẸP: QEMU lặng lẽ
     * KHÔNG nạp DTB khi nó lớn hơn khoảng trống (hw/arm/arm_boot.c:
     * "Installing the device tree blob at addr would exceed addr_limit …
     * return 0 as size, i.e., no error"), và nhân chỉ thấy "khong doc duoc".
     * Dời ảnh lên 0x4100_0000 để lại 16 MiB cho DTB: dtb_limit của QEMU trở
     * thành 0x4100_0000, mọi DTB thực tế (~tens of KiB) đều lọt. */
    . = 0x41000000;
    __kernel_start = .;

    .text : {
        *(.text.entry)
        *(.text*)
    }
    . = ALIGN(4096);
    .rodata : { *(.rodata*) }
    . = ALIGN(4096);
    .data : { *(.data*) }
    . = ALIGN(4096);
    .bss : {
        __bss_start = .;
        *(COMMON)
        *(.bss*)
        . = ALIGN(16);
        __bss_end = .;
    }
    __kernel_end = .;

    /DISCARD/ : { *(.eh_frame) *(.comment) *(.note*) *(.rela*) }
}
