/* YuiVieOS Beta — riscv64 QEMU virt.
 *
 * OpenSBI (M-mode firmware) loads the S-mode payload at 0x80200000. paging is
 * off in Sv39 terms until P2.2, so the link address is the physical address.
 */
OUTPUT_FORMAT(elf64-littleriscv)
ENTRY(_start)

SECTIONS
{
    . = 0x80200000;
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
