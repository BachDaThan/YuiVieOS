//! x86_64 — BIOS Legacy (multiboot 1) → long mode → Rust.
//!
//! QEMU `-kernel` chỉ hiểu multiboot 1 cho x86, và multiboot 1 khởi động ở chế
//! độ 32-bit. Vì vậy khung này chứa một cầu nối 32→64 bit tối thiểu: bảng trang
//! nhận dạng 1 GiB với trang 2 MiB, bật PAE + LME + PG, nạp GDT 64-bit rồi nhảy
//! sang mã Rust. Toàn bộ phần đó là `global_asm!` — nghĩa là phần **duy nhất**
//! của dự án cần assembly, và nó nằm gọn trong một tệp.

use core::arch::x86_64::__cpuid;

use crate::boot::RawBoot;

const COM1: u16 = 0x3F8;

core::arch::global_asm!(
    r#"
.section .multiboot, "a"
.align 4
    .long 0x1BADB002          /* magic */
    .long 0x00000003          /* align modules + provide mem info */
    .long -(0x1BADB002 + 0x00000003)

.section .text.entry, "ax"
.code32
.global _start
_start:
    cli
    movl %eax, %esi           /* multiboot magic */
    movl %ebx, %edi           /* multiboot info (physical) */
    movl $stack32_top, %esp

    /* PML4[0] -> PDP[0] -> PD (identity map, 2 MiB pages) */
    xorl %ecx, %ecx
    leal pd, %eax
1:
    movl %ecx, %edx
    shll $21, %edx
    orl  $0x83, %edx
    movl %edx, (%eax,%ecx,8)
    incl %ecx
    cmpl $512, %ecx
    jne 1b

    leal pdpt, %eax
    leal pd, %edx
    orl  $3, %edx
    movl %edx, (%eax)
    movl $0, 4(%eax)

    leal pml4, %eax
    leal pdpt, %edx
    orl  $3, %edx
    movl %edx, (%eax)
    movl $0, 4(%eax)

    /* CR4.PAE */
    movl %cr4, %eax
    orl  $0x20, %eax
    movl %eax, %cr4

    /* EFER.LME */
    movl $0xC0000080, %ecx
    rdmsr
    orl  $0x100, %eax
    wrmsr

    /* CR0.PG | CR0.PE */
    movl %cr0, %eax
    orl  $0x80010000, %eax
    movl %eax, %cr0

    lgdt gdt64_ptr
    ljmp $0x08, $long_mode

.code64
long_mode:
    movq $stack64_top, %rsp
    xorq %rbp, %rbp
    movl %edi, %edi           /* zero-extend the multiboot pointer */
    movq %rdi, %rdi
    call kmain_x86
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
    .skip 4096
"#
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
#[no_mangle]
pub extern "C" fn kmain_x86(multiboot_info: u32) -> ! {
    crate::kmain(RawBoot::Multiboot(multiboot_info as u64))
}
