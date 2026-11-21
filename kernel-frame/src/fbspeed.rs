//! Tốc độ ghi khung hình trước/sau write-combining (P2.16, "đo MB/s").
//!
//! Khung hình firmware cấp (VBE/GOP) thường nằm ở vùng MMIO mà MTRR đánh dấu
//! *uncached*: mỗi lần ghi 4 byte là một giao dịch bus riêng, nên vẽ cả màn
//! hình chậm thấy rõ trên máy thật. Đánh dấu vùng đó *write-combining* (PAT)
//! cho phép CPU gộp nhiều lần ghi thành một cụm. Tệp này đo cả hai, bằng cùng
//! đường vẽ của màn chào, và in một dòng:
//!
//! `Ghi khung : truoc 37 MB/s, sau WC 1450 MB/s (8 MiB, 4 trang 2 MiB, TSC 2394 MHz)`
//!
//! Trên **máy ảo** con số không đại diện (QEMU không mô phỏng kiểu bộ nhớ), nên
//! dòng in thêm "may ao — so khong dai dien". Số thật chỉ có khi boot trên máy
//! thật — đó là phần DoD của P2.16 còn chờ.
//!
//! Sau khi bật WC không cần lệnh `sfence` riêng: dòng log in ngay sau khi vẽ
//! đi qua lệnh `out` của cổng nối tiếp, mà Intel SDM (8.2.5) bảo đảm lệnh
//! vào/ra chờ mọi lần ghi đang đệm (kể cả bộ đệm WC) xả xong trước khi chạy.
//!
//! Không có `unsafe` ở đây: phần chạm phần cứng nằm ở `arch::*` và
//! `splash::fill_bench`. Quyết định *có được bật WC không* (không chạm RAM,
//! không chạm ảnh nhân, có bản đồ bộ nhớ đầy đủ) là hàm thuần
//! `yui_cap::memrange::wc_pages`, có test trên host.

use crate::boot::{BootInfo, Framebuffer};
use crate::console::print_str_no_newline;
use yui_cap::memrange::wc_pages;

/// Đo tối đa 8 MiB mỗi lần (1080p 32 bit ≈ 7,9 MiB): giữ thời gian boot ngắn
/// cả khi vùng còn uncached (vài chục MB/s ⇒ < 0,3 s).
const BENCH_BYTES: u64 = 8 * 1024 * 1024;

fn mb_per_s(bytes: u64, cycles: u64, hz: u64) -> u64 {
    if cycles == 0 {
        return 0;
    }
    ((bytes as u128 * hz as u128) / (cycles as u128 * 1_000_000)) as u64
}

/// Đo, bật WC nếu an toàn, đo lại; in một dòng. Gọi **trước** khi vẽ màn
/// chào (phép đo tô nền, màn chào vẽ đè lên sau). Trả (MB/s trước, MB/s sau WC
/// nếu bật được, máy ảo) để màn chào in lại cho người không có cổng nối tiếp.
pub fn measure(fb: &Framebuffer, info: &BootInfo) -> Option<(u64, Option<u64>, bool)> {
    let hz = match crate::arch::cycle_hz() {
        Some(hz) if hz > 0 => hz,
        _ => {
            println!("  Ghi khung : khong do duoc (khong co dong ho chu ky / PIT)");
            return None;
        }
    };
    let (_, in_vm) = crate::arch::cpu_flags();
    let before = match crate::splash::fill_bench(fb, BENCH_BYTES) {
        Some(b) => b,
        None => {
            println!("  Ghi khung : khung hinh khong dung duoc, bo qua");
            return None;
        }
    };
    let before_mbs = mb_per_s(before.0, before.1, hz);
    let mut after_mbs = None;
    print!("  Ghi khung : truoc {} MB/s", before_mbs);

    let fb_len = fb.pitch as u64 * fb.height as u64;
    let ram = &info.ram_ranges[..info.ram_range_count];
    match wc_pages(fb.addr, fb_len, ram, info.ram_ranges_complete, crate::boot::kernel_image()) {
        Ok((first, end)) if crate::arch::set_write_combining(first, end) => {
            match crate::splash::fill_bench(fb, BENCH_BYTES) {
                Some(after) => {
                    let a = mb_per_s(after.0, after.1, hz);
                    after_mbs = Some(a);
                    print!(", sau WC {} MB/s ({} trang 2 MiB, ", a, end - first)
                }
                None => print!(", sau WC: khong do lai duoc ("),
            }
        }
        Ok(_) => print!(", WC: CPU khong co PAT ("),
        Err(why) => {
            print!(", WC: khong bat — ");
            print_str_no_newline(why.as_str());
            print!(" (");
        }
    }
    print!("{} KiB moi lan, TSC {} MHz", before.0 / 1024, hz / 1_000_000);
    if in_vm {
        print!(", may ao — so khong dai dien");
    }
    println!(")");
    Some((before_mbs, after_mbs, in_vm))
}
