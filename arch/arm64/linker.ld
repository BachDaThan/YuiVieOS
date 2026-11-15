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
    . = 0x40080000;
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
