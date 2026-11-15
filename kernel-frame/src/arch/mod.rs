//! Lớp kiến trúc — mỗi tệp con chỉ làm bốn việc: dựng ngăn xếp, nói chuyện với
//! cổng nối tiếp, dựng `RawBoot` từ thông tin firmware, và dừng máy.
//!
//! Chỉ **một** mô-đun được biên dịch cho mỗi target, nên tên hàm giống nhau ở
//! cả ba nhánh là an toàn và giúp phần còn lại của nhân không cần `cfg`.

#[cfg(target_arch = "x86_64")]
mod x86_64;
#[cfg(target_arch = "aarch64")]
mod aarch64;
#[cfg(target_arch = "riscv64")]
mod riscv64;

#[cfg(target_arch = "x86_64")]
pub use x86_64::*;
#[cfg(target_arch = "aarch64")]
pub use aarch64::*;
#[cfg(target_arch = "riscv64")]
pub use riscv64::*;

/// Dừng nhân vĩnh viễn. Không bật ngắt: dừng nghĩa là dừng.
pub fn halt_forever() -> ! {
    loop {
        halt_once();
    }
}

#[cfg(target_arch = "x86_64")]
fn halt_once() {
    unsafe { core::arch::asm!("hlt", options(nomem, nostack, preserves_flags)) }
}

#[cfg(any(target_arch = "aarch64", target_arch = "riscv64"))]
fn halt_once() {
    unsafe { core::arch::asm!("wfi", options(nomem, nostack)) }
}
