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

// Đo tốc độ ghi khung hình + write-combining (P2.16): hiện chỉ x86_64 có. Hai
// kiến trúc kia trả "không có" — không bịa số. (aarch64 sau này có thể dùng
// CNTVCT_EL0/CNTFRQ_EL0 và thuộc tính bộ nhớ Normal-NC của MMU.)
#[cfg(not(target_arch = "x86_64"))]
pub fn cycles() -> u64 {
    0
}

#[cfg(not(target_arch = "x86_64"))]
pub fn cycle_hz() -> Option<u64> {
    None
}

#[cfg(not(target_arch = "x86_64"))]
pub fn cpu_flags() -> (bool, bool) {
    (false, false)
}

#[cfg(not(target_arch = "x86_64"))]
pub fn set_write_combining(_first_2m: u64, _end_2m: u64) -> bool {
    false
}
