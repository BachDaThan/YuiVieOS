/* YuiVieOS Beta — x86_64 multiboot 1 image.
 *
 * Multiboot 1 loads the image at 1 MiB physical and jumps to `_start` in 32-bit
 * protected mode, so this image is *low* memory, not higher-half: the frame
 * turns on its own 1 GiB identity map before any Rust code runs. Higher-half
 * linking returns in P2.2 together with the real page-table builder.
 *
 * The .multiboot section must start within the first 8192 bytes of the file,
 * which is why it is the very first output section.
 */
OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)

SECTIONS
{
    . = 1M;
    __kernel_start = .;

    .multiboot : { KEEP(*(.multiboot)) }
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
