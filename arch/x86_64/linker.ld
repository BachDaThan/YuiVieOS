/* YuiVieOS x86_64 higher-half kernel linker script.
 * The frame kernel is linked at the top 2 GiB so the microkernel's capability
 * model can use the low canonical range for user address spaces.
 */
OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)

KERNEL_BASE = 0xFFFFFFFF80000000;

SECTIONS
{
    . = KERNEL_BASE;
    .text : AT(ADDR(.text) - KERNEL_BASE) { *(.text.entry) *(.text*) }
    . = ALIGN(4096);
    .rodata : AT(ADDR(.rodata) - KERNEL_BASE) { *(.rodata*) }
    . = ALIGN(4096);
    .data : AT(ADDR(.data) - KERNEL_BASE) { *(.data*) }
    . = ALIGN(4096);
    .bss : AT(ADDR(.bss) - KERNEL_BASE) {
        *(COMMON)
        *(.bss*)
        . = ALIGN(16);
        __bss_end = .;
    }
    __kernel_end = .;
    /DISCARD/ : { *(.eh_frame) *(.comment) *(.note*) }
}
