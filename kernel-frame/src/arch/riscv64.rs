//! riscv64 — QEMU `virt` nạp ELF tại 0x8020_0000 và nhảy vào `_start` ở S-mode
//! với `a0 = hartid`, `a1 =` con trỏ device tree. Console đi qua SBI
//! (legacy console_putchar, EID 1) nên không cần biết UART của bo mạch.

use crate::boot::RawBoot;

core::arch::global_asm!(
    r#"
.section .text.entry, "ax"
.global _start
_start:
    la sp, __yui_stack_top
    call kmain_riscv
1:
    wfi
    j 1b

.section .bss
.align 12
__yui_stack:
    .skip 65536
__yui_stack_top:
"#
);

pub fn init_console() {
    // SBI đã sẵn sàng (OpenSBI giữ M-mode); không cần khởi tạo gì.
}

pub fn serial_write(byte: u8) {
    // legacy SBI: a7 = 1 (console_putchar), a0 = ký tự
    unsafe {
        core::arch::asm!(
            "ecall",
            inlateout("a0") byte as usize => _,
            in("a7") 1usize,
            options(nostack)
        )
    }
}

pub fn detect_simd() -> &'static str {
    // KHÔNG đọc `misa`: đó là CSR cấp M (0x301), S-mode đọc sẽ trap — đây chính
    // là lỗi làm nhân dừng ngay sau dòng "Kien truc" ở lần boot QEMU đầu tiên.
    // Vector extension được phát hiện qua chuỗi `riscv,isa` trong device tree
    // (xem boot::parse_fdt), mặc định là scalar.
    "scalar"
}

pub fn cpu_vendor() -> [u8; 12] {
    let mut v = [0u8; 12];
    v[0] = b'R';
    v[1] = b'I';
    v[2] = b'S';
    v[3] = b'C';
    v[4] = b'-';
    v[5] = b'V';
    v[6] = b'6';
    v[7] = b'4';
    v
}

#[no_mangle]
pub extern "C" fn kmain_riscv(hartid: usize, dtb: usize) -> ! {
    let _ = hartid; // hart phụ được đánh thức ở P2.2 (PSCI/SBI HSM)
    crate::kmain(RawBoot::DeviceTree(dtb as u64))
}
