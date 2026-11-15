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

/// Ghi một chuỗi ra cổng nối tiếp, có khoá.
pub fn print(args: fmt::Arguments<'_>) {
    while LOCK
        .compare_exchange(false, true, Ordering::Acquire, Ordering::Relaxed)
        .is_err()
    {
        core::hint::spin_loop();
    }
    let mut serial = Serial;
    let _ = serial.write_fmt(args);
    LOCK.store(false, Ordering::Release);
}

#[macro_export]
macro_rules! print {
    ($($arg:tt)*) => {
        $crate::console::print(format_args!($($arg)*))
    };
}

#[macro_export]
macro_rules! println {
    () => {
        $crate::print!("\n")
    };
    ($($arg:tt)*) => {
        $crate::print!("{}\n", format_args!($($arg)*))
    };
}
