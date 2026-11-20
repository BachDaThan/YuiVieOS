//! x86_64 — BIOS Legacy (multiboot 1/2) → long mode → Rust.
//!
//! QEMU `-kernel` chỉ nạp multiboot 1 **từ ELF32** ("Cannot load x86-64 image,
//! give a 32bit one"), còn GRUB2 hiểu multiboot 2. Vì vậy ảnh khởi động có cả
//! hai header multiboot trong `.multiboot`, và `scripts/build-kernel.sh` đóng
//! gói ELF64 thành container ELF32 (`objcopy -O elf32-i386`) cho QEMU — nội
//! dung mã không đổi, chỉ thay "vỏ" ELF. Multiboot 1 lẫn 2 đều nhảy vào
//! `_start` ở chế độ 32-bit, nên cầu nối 32→64 bit dưới đây dùng chung cho cả
//! hai đường khởi động. Toàn bộ phần assembly là `global_asm!` — nghĩa là phần
//! **duy nhất** của dự án cần assembly, và nó nằm gọn trong một tệp.

// Rust dùng cú pháp Intel cho asm!; cầu nối dưới đây viết theo AT&T nên phải
// yêu cầu tường minh. Đổi cú pháp cả tệp là một lựa chọn khác, nhưng dễ sai hơn.
#![allow(unused_unsafe)]

use core::arch::x86_64::__cpuid;

use crate::boot::RawBoot;

const COM1: u16 = 0x3F8;

core::arch::global_asm!(
    r#"
.section .multiboot, "a"
.align 4
    .long 0x1BADB002          /* magic multiboot 1 */
    .long 0x00000003          /* align modules + provide mem info */
    .long -(0x1BADB002 + 0x00000003)

/* Header multiboot 2 (GRUB2 `multiboot2`): cùng mục vào _start 32-bit.
 * checksum = -(magic + arch + length) mod 2^32; end tag bắt buộc. */
.align 8
mb2_start:
    .long 0xE85250D6          /* magic multiboot 2 */
    .long 0x00000000          /* architecture: i386 */
    .long mb2_end - mb2_start /* header_length */
    .long -(0xE85250D6 + 0x00000000 + (mb2_end - mb2_start))
    /* Thẻ yêu cầu framebuffer (type 5, cờ 1 = tuỳ chọn): GRUB dựng chế độ
     * đồ hoạ tuyến tính và trả thẻ framebuffer (type 8) — P2.16. Độ phân
     * giải ở đây chỉ là gợi ý; `set gfxpayload=WxHxD` trong grub.cfg thắng. */
    .word 5
    .word 1
    .long 20
    .long 1024
    .long 768
    .long 32
    .long 0                   /* đệm: thẻ kế tiếp phải tròn 8 byte (16+20+4 = 40) */
    .word 0                   /* end tag: type = 0 */
    .word 0
    .long 8
mb2_end:

.section .text.entry, "ax"
.code32
.global _start
_start:
    cli
    movl %eax, %esi           /* multiboot magic (0x2BADB002 hoặc 0x36D76289) */
    movl %ebx, %edi           /* multiboot info (physical) */
    movl $stack32_top, %esp

    /* PML4[0] -> PDPT[0..3] -> PD0..PD3: ánh xạ đồng nhất 4 GiB đầu bằng
     * trang 2 MiB (P2.16). 1 GiB là đủ cho RAM khi boot, nhưng framebuffer
     * tuyến tính của VBE/GOP nằm ở lỗ PCI dưới 4 GiB (QEMU stdvga:
     * 0xFD000000) — thiếu ánh xạ thì vẽ màn hình chào là page fault. */
    xorl %ecx, %ecx
    leal pd, %eax
1:
    movl %ecx, %edx
    shll $21, %edx
    orl  $0x83, %edx
    movl %edx, (%eax,%ecx,8)
    movl $0, 4(%eax,%ecx,8)
    incl %ecx
    cmpl $2048, %ecx
    jne 1b

    leal pdpt, %eax
    leal pd, %edx
    orl  $3, %edx
    xorl %ecx, %ecx
3:
    movl %edx, (%eax,%ecx,8)
    movl $0, 4(%eax,%ecx,8)
    addl $4096, %edx
    incl %ecx
    cmpl $4, %ecx
    jne 3b

    leal pml4, %eax
    leal pdpt, %edx
    orl  $3, %edx
    movl %edx, (%eax)
    movl $0, 4(%eax)

    /* CR3 = &pml4 — BẮT BUỘC trước khi bật CR0.PG: thiếu bước này CPU dò
     * bảng trang tại địa chỉ 0 (giá trị reset), hỏng ngay ở lệnh kế tiếp. */
    movl $pml4, %eax
    movl %eax, %cr3

    /* CR4.PAE */
    movl %cr4, %eax
    orl  $0x20, %eax
    movl %eax, %cr4

    /* EFER.LME */
    movl $0xC0000080, %ecx
    rdmsr
    orl  $0x100, %eax
    wrmsr

    /* CR0.PG | CR0.PE (PE đã = 1 từ protected mode, OR lại cho đúng chuẩn) */
    movl %cr0, %eax
    orl  $0x80000001, %eax
    movl %eax, %cr0

    lgdt gdt64_ptr
    ljmp $0x08, $long_mode

.code64
long_mode:
    movq $stack64_top, %rsp
    xorq %rbp, %rbp
    movl %edi, %edi           /* zero-extend the multiboot pointer */
    movl %esi, %esi           /* zero-extend the multiboot magic */
    movq %rdi, %rdi
    call kmain_x86            /* rdi = info, rsi = magic */
2:
    hlt
    jmp 2b

.section .rodata
.align 8
gdt64:
    .quad 0
    .quad 0x00AF9A000000FFFF  /* 64-bit code segment, ring 0 */
gdt64_ptr:
    .word gdt64_ptr - gdt64 - 1
    .quad gdt64

.section .bss
.align 16
stack32:
    .skip 8192
stack32_top:
.align 16
stack64:
    .skip 65536
stack64_top:
.align 4096
pml4:
    .skip 4096
pdpt:
    .skip 4096
pd:
    .skip 16384               /* 4 bảng PD liên tiếp: 4 × 512 × 2 MiB = 4 GiB */
"#,
    options(att_syntax)
);

/// Khởi tạo cổng nối tiếp COM1 ở 115200 8N1 (QEMU không cần baud nhưng phần cứng thật cần).
pub fn init_console() {
    unsafe {
        outb(COM1 + 1, 0x00); // tắt ngắt
        outb(COM1 + 3, 0x80); // bật DLAB
        outb(COM1 + 0, 0x03); // divisor = 3 (38400) — an toàn với mọi tinh thể
        outb(COM1 + 1, 0x00);
        outb(COM1 + 3, 0x03); // 8N1
        outb(COM1 + 2, 0xC7); // FIFO bật, xoá, 14 byte
        outb(COM1 + 4, 0x0B); // DTR + RTS + OUT2
    }
}

pub fn serial_write(byte: u8) {
    unsafe {
        // Chờ thanh ghi truyền rỗng (LSR bit 5), có giới hạn để không treo máy.
        let mut spins = 0u32;
        while inb(COM1 + 5) & 0x20 == 0 {
            spins += 1;
            if spins > 10_000_000 {
                return;
            }
            core::hint::spin_loop();
        }
        outb(COM1, byte);
    }
}

unsafe fn outb(port: u16, value: u8) {
    core::arch::asm!("out dx, al", in("dx") port, in("al") value, options(nomem, nostack, preserves_flags));
}

unsafe fn inb(port: u16) -> u8 {
    let value: u8;
    core::arch::asm!("in al, dx", in("dx") port, out("al") value, options(nomem, nostack, preserves_flags));
    value
}

pub fn detect_simd() -> &'static str {
    // CPUID là lệnh an toàn về mặt bộ nhớ; wrapper trong core là `unsafe` vì nó
    // phụ thuộc CPU, nên nằm trong khối unsafe có chú thích.
    let leaf1 = unsafe { __cpuid(1) };
    let has_avx = leaf1.ecx & (1 << 28) != 0;
    let leaf7 = unsafe { __cpuid(7) };
    let avx2 = leaf7.ebx & (1 << 5) != 0;
    let avx512f = leaf7.ebx & (1 << 16) != 0;
    if avx512f && has_avx {
        "avx512f"
    } else if avx2 {
        "avx2"
    } else {
        "scalar"
    }
}

pub fn cpu_vendor() -> [u8; 12] {
    let leaf0 = unsafe { __cpuid(0) };
    let mut vendor = [0u8; 12];
    vendor[0..4].copy_from_slice(&leaf0.ebx.to_le_bytes());
    vendor[4..8].copy_from_slice(&leaf0.edx.to_le_bytes());
    vendor[8..12].copy_from_slice(&leaf0.ecx.to_le_bytes());
    vendor
}

/// Điểm vào từ assembly (long mode, ngăn xếp đã dựng).
///
/// `magic` phân biệt hai giao thức multiboot: `0x2BADB002` = multiboot 1
/// (QEMU `-kernel`), `0x36D76289` = multiboot 2 (GRUB2). Cả hai đặt con trỏ
/// info vào tham số đầu; cấu trúc info khác nhau nên `boot::parse` rẽ nhánh.
#[no_mangle]
pub extern "C" fn kmain_x86(multiboot_info: u32, magic: u32) -> ! {
    crate::kmain(RawBoot::Multiboot(multiboot_info as u64, magic))
}
