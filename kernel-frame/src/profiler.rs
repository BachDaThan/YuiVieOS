//! Hardware Auto-Profiler (Tầng 1) — bản chạy trên bare metal.
//!
//! Phân loại máy và chọn k/W. Ngưỡng **giống hệt** `kernel-services/profile.cpp`
//! để bản kernel và bản user-space không bao giờ nói hai điều khác nhau:
//!   * `may-yeu`  : RAM < 4 GiB hoặc ≤ 2 luồng
//!   * `may-manh` : RAM ≥ 16 GiB và ≥ 8 luồng
//!   * `can-bang` : còn lại

use crate::console::print_str_no_newline;
use crate::allocator;
use crate::boot::BootInfo;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Classification {
    pub tier: &'static str,
    pub k: u32,
    pub window: u32,
    pub reason: &'static str,
}

/// Hàm thuần: cùng đầu vào ⇒ cùng phân loại, kiểm thử được không cần máy thật.
pub fn classify(ram_mb: u64, harts: u32) -> Classification {
    if ram_mb < 4096 || harts <= 2 {
        Classification {
            tier: "may-yeu",
            k: 2,
            window: 1024,
            reason: "May yeu (RAM nho hoac it nhan): k=2, W=1024 de giu RAM va nhiet",
        }
    } else if ram_mb >= 16384 && harts >= 8 {
        Classification {
            tier: "may-manh",
            k: 8,
            window: 8192,
            reason: "May manh: k=8, W=8192 cho suy luan sau",
        }
    } else {
        Classification {
            tier: "can-bang",
            k: 4,
            window: 4096,
            reason: "May can bang: k=4, W=4096",
        }
    }
}

/// In hồ sơ phần cứng ra cổng nối tiếp (đây là "kết quả" của bước P1.2/P1.3).
pub fn report(info: &BootInfo) {
    let c = classify(info.ram_mb, info.harts);
    let cpu = info.cpu_display();

    // Kỹ thuật: **không** dùng `println!("{}", chuoi_runtime)` trong khung này.
    // Trên aarch64, `{}` với một `&str` tính lúc chạy làm nhân dừng giữa dòng
    // (cùng chuỗi dạng hằng thì in được — đã tái hiện qua log QEMU), còn
    // `print_str` ghi byte trực tiếp thì luôn đúng. Số và nhãn hằng vẫn dùng
    // `println!` bình thường.
    label_str("  CPU       : ", &cpu);
    label_str("  SIMD      : ", info.simd);
    println!("  So nhan   : {}", info.harts);
    print!("  RAM       : {} MiB — ", info.ram_mb);
    println_str!(info.ram_source);
    label_str("  Phan loai : ", c.tier);
    print!("  Tu chinh  : k={}, W={}", c.k, c.window);
    println!();
    label_str("  Ghi chu   : ", c.reason);
    println!("  Bo nho    : {} byte da cap phat cho khung + bang nang luc", allocator::used_bytes());
}

/// In `nhãn + chuỗi` mà không đi qua bộ máy `core::fmt` cho phần chuỗi.
fn label_str(label: &str, value: &str) {
    print_str_no_newline(label);
    println_str!(value);
}
