//! Cổng nối tiếp: kênh log duy nhất trong pha khởi động (QEMU `-serial stdio`).
//!
//! Không dùng `print!` của std (không có std). Khoá bằng spinlock nguyên tử vì
//! sau này nhiều hart/nhân có thể cùng in.

use core::fmt::{self, Write};
use core::sync::atomic::{AtomicBool, Ordering};

static LOCK: AtomicBool = AtomicBool::new(false);

struct Serial;

impl Write for Serial {
    fn write_str(&mut self, s: &str) -> fmt::Result {
        for byte in s.bytes() {
            crate::arch::serial_write(byte);
        }
        Ok(())
    }
}

/// In một `&str` **không** đi qua `core::fmt`.
///
/// Vì sao cần: trên aarch64, một dòng `println!("...{}", ten)` với `ten` là biến
/// `&'static str` (len 7, con trỏ hợp lệ nằm trong .rodata) làm nhân dừng ngay
/// giữa dòng, trong khi cùng chuỗi đó dưới dạng hằng thì in được. Đường in trực
/// tiếp vừa tránh được chỗ chết đó, vừa bỏ hẳn bộ máy `Arguments` cho văn bản
/// thuần — rẻ hơn trên máy yếu.
pub fn print_str(text: &str) {
    lock();
    for byte in text.bytes() {
        crate::arch::serial_write(byte);
    }
    crate::arch::serial_write(b'\n');
    unlock();
}

fn lock() {
    while LOCK
        .compare_exchange(false, true, Ordering::Acquire, Ordering::Relaxed)
        .is_err()
    {
        core::hint::spin_loop();
    }
}

fn unlock() {
    LOCK.store(false, Ordering::Release);
}

/// Ghi một chuỗi ra cổng nối tiếp, có khoá.
pub fn print(args: fmt::Arguments<'_>) {
    lock();
    let mut serial = Serial;
    let _ = serial.write_fmt(args);
    unlock();
}

/// Hai macro dưới đây sống trong phạm vi văn bản của crate (`#[macro_use] mod
/// console;` đứng đầu `lib.rs`), **không** `#[macro_export]` — nếu export thì
/// tên `print`/`println` sẽ tồn tại đồng thời ở gốc crate và ở đây (lỗi E0255).
macro_rules! print {
    ($($arg:tt)*) => {
        $crate::console::print(format_args!($($arg)*))
    };
}

macro_rules! println_str {
    ($s:expr) => {
        $crate::console::print_str($s)
    };
}

macro_rules! println {
    () => {
        print!("\n")
    };
    ($($arg:tt)*) => {
        print!("{}\n", format_args!($($arg)*))
    };
}
