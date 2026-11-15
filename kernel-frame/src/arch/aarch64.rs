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
    // PL011 trên QEMU virt: UART_CLK = 24 MHz. Muốn 115200 8N1 thì
    //   IBRD = 24e6 / (16 * 115200) = 13, FBRD = 0.0208 * 64 ≈ 1
    // Ban đầu tôi chỉ ghi CR và bỏ qua divisor ⇒ sau vài chục byte TXFF kẹt và
    // serial_write rơi vào nhánh "quá số lần chờ" nên **mất chữ giữa dòng**
    // ("Kien truc : " rồi không có tên kiến trúc). Đó là lỗi, không phải ma.
    let uart = 0x0900_0000usize as *mut u32;
    unsafe {
        write(uart, 0x30, 0); // UARTEN = 0 trong lúc cấu hình
        write(uart, 0x24, 13); // IBRD
        write(uart, 0x28, 1); // FBRD
        write(uart, 0x2C, 0x70); // LCR_H: FIFO bật, 8 bit, không parity, 1 stop
        write(uart, 0x38, 0); // IMSC: không ngắt
        write(uart, 0x44, 0x7FF); // ICR: xoá mọi cờ ngắt
        write(uart, 0x30, 0x301); // CR: UARTEN | TXE | RXE
    }
}

unsafe fn write(uart: *mut u32, offset: usize, value: u32) {
    core::ptr::write_volatile(uart.add(offset / 4), value);
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
    // ARMv8-A **bắt buộc** có NEON nên không cần đọc thanh ghi nào.
    // (Trước đây tôi đọc ID_AA64PFR0_EL1 để dò SVE; lần chạy QEMU đầu tiên cho
    // thấy đọc thanh ghi ID ở EL1 có thể trap tuỳ cấu hình EL2 — không đáng để
    // mạo hiểm trong khung khởi động. Dò SVE sẽ làm ở P2.5 bằng bảng CPU.)
    "neon"
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
    let mut addr = dtb as u64;
    if addr == 0 {
        // QEMU `-kernel` với ảnh ELF: bootloader QEMU coi đây là "ELF không
        // phải Linux" nên nhảy thẳng vào _start mà KHÔNG đặt x0 (xem
        // hw/arm/boot.c: "Assume that raw images are linux kernels, and ELF
        // images are not"), nhưng nó vẫn sinh DTB và đặt ngay tại đầu RAM
        // (`loader_start` = 0x4000_0000 trên QEMU virt, dưới ảnh nhân ở
        // 0x4008_0000). U-Boot/UEFI thì truyền x0 đúng chuẩn. Khi x0 = 0,
        // quét cửa sổ đầu RAM để tìm magic FDT — không thấy thì để 0 và báo
        // "khong doc duoc", tuyệt đối không bịa số.
        addr = crate::boot::find_fdt(0x4000_0000, 0x8_0000);
    }
    crate::kmain(RawBoot::DeviceTree(addr))
}
