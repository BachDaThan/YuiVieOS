//! aarch64 — QEMU `virt` nạp ELF tại 0x4008_0000 và nhảy vào `_start` ở EL1
//! với `x0 =` con trỏ device tree. Không cần bật MMU cho pha khởi động; ngăn
//! xếp đặt trong .bss.

use crate::boot::RawBoot;

const PL011_DR: usize = 0x0900_0000;
const PL011_FR: usize = 0x0900_0018;

core::arch::global_asm!(
    r#"
.section .text.entry, "ax"
.global _start
_start:
    mov x19, x0               /* device tree pointer */
    ldr x1, =__yui_stack_top
    mov sp, x1
    mov x0, x19
    bl kmain_arm
1:
    wfe
    b 1b

.section .bss
.align 12
__yui_stack:
    .skip 65536
__yui_stack_top:
"#
);

pub fn init_console() {
    // QEMU cấu hình PL011 sẵn; trên bo mạch thật, phần này sẽ lập trình
    // IBRD/FBRD theo tinh thể (P2.2). Ghi CR để bật UART, TXE, RXE.
    unsafe { core::ptr::write_volatile(0x0900_0030 as *mut u32, (1 << 0) | (1 << 8) | (1 << 9)) }
}

pub fn serial_write(byte: u8) {
    unsafe {
        let mut spins = 0u32;
        // FR bit 5 = TXFF (transmit FIFO full)
        while core::ptr::read_volatile(PL011_FR as *const u32) & (1 << 5) != 0 {
            spins += 1;
            if spins > 10_000_000 {
                return;
            }
            core::hint::spin_loop();
        }
        core::ptr::write_volatile(PL011_DR as *mut u32, byte as u32);
    }
}

pub fn detect_simd() -> &'static str {
    // ARMv8-A bắt buộc có NEON. SVE được báo ở ID_AA64PFR0_EL1[35:32] != 0.
    let pfr0: u64;
    unsafe { core::arch::asm!("mrs {}, id_aa64pfr0_el1", out(reg) pfr0, options(nomem, nostack)) }
    if (pfr0 >> 32) & 0xF != 0 {
        "sve"
    } else {
        "neon"
    }
}

pub fn cpu_vendor() -> [u8; 12] {
    // MIDR_EL1 không phải chuỗi; in tên kiến trúc và để trống vendor.
    let mut v = [0u8; 12];
    v[0] = b'A';
    v[1] = b'R';
    v[2] = b'M';
    v
}

#[no_mangle]
pub extern "C" fn kmain_arm(dtb: usize) -> ! {
    crate::kmain(RawBoot::DeviceTree(dtb as u64))
}
