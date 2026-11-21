//! Chọn dải trang 2 MiB để bật write-combining cho khung hình (P2.16).
//!
//! Hàm thuần, chạy `cargo test` trên host; nhân (`kernel-frame/src/fbspeed.rs`)
//! gọi nó trước khi đổi kiểu bộ nhớ. Quy tắc: dải trang **làm tròn ra ngoài**
//! phủ khung hình phải nằm trong 4 GiB đã ánh xạ đồng nhất, và **không chạm**
//! ảnh nhân lẫn bất kỳ vùng RAM `available` nào của bản đồ bộ nhớ. Bản đồ
//! không đầy đủ ⇒ không biết ⇒ không bật (đánh dấu RAM là WC làm mất thứ tự ghi
//! và làm đọc chậm — thà chậm vẽ còn hơn hỏng bộ nhớ).

pub const TWO_MIB: u64 = 2 * 1024 * 1024;
pub const MAPPED_LIMIT: u64 = 4 * 1024 * 1024 * 1024;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum WcRefusal {
    Empty,
    Overflow,
    AboveMappedLimit,
    NoCompleteMap,
    TouchesKernel,
    TouchesRam,
}

impl WcRefusal {
    /// Lý do in ra cổng nối tiếp (ASCII, không dấu: phông 5×7/serial).
    pub const fn as_str(self) -> &'static str {
        match self {
            WcRefusal::Empty => "khung hinh rong",
            WcRefusal::Overflow => "khung hinh tran so",
            WcRefusal::AboveMappedLimit => "khung hinh ngoai 4 GiB da anh xa",
            WcRefusal::NoCompleteMap => "khong co ban do bo nho day du",
            WcRefusal::TouchesKernel => "dai trang cham anh nhan",
            WcRefusal::TouchesRam => "dai trang cham RAM",
        }
    }
}

/// Dải chỉ số trang 2 MiB `[first, end)` phủ `[fb_addr, fb_addr + fb_len)`.
pub fn wc_pages(
    fb_addr: u64,
    fb_len: u64,
    ram: &[(u64, u64)],
    ram_complete: bool,
    kernel: (u64, u64),
) -> Result<(u64, u64), WcRefusal> {
    if fb_addr == 0 || fb_len == 0 {
        return Err(WcRefusal::Empty);
    }
    let end_addr = fb_addr.checked_add(fb_len).ok_or(WcRefusal::Overflow)?;
    let first = fb_addr / TWO_MIB;
    let end = end_addr.checked_add(TWO_MIB - 1).ok_or(WcRefusal::Overflow)? / TWO_MIB;
    let (lo, hi) = (first * TWO_MIB, end.saturating_mul(TWO_MIB));
    if hi > MAPPED_LIMIT {
        return Err(WcRefusal::AboveMappedLimit);
    }
    if !ram_complete {
        return Err(WcRefusal::NoCompleteMap);
    }
    let overlaps = |a: u64, b: u64| a < hi && lo < b;
    if kernel.1 > kernel.0 && overlaps(kernel.0, kernel.1) {
        return Err(WcRefusal::TouchesKernel);
    }
    for &(start, len) in ram {
        if len != 0 && overlaps(start, start.saturating_add(len)) {
            return Err(WcRefusal::TouchesRam);
        }
    }
    Ok((first, end))
}

#[cfg(test)]
mod tests {
    use super::*;

    const MIB: u64 = 1024 * 1024;
    // Bản đồ kiểu QEMU 1 GiB: 0–639 KiB và 1 MiB–1 GiB là RAM.
    const RAM: [(u64, u64); 2] = [(0, 639 * 1024), (MIB, 1023 * MIB)];
    const KERNEL: (u64, u64) = (MIB, MIB + 300 * 1024);

    #[test]
    fn stdvga_pci_hole_ok() {
        // QEMU stdvga (pc): 0xFD000000, 1920×1080×4.
        let len = 1920 * 4 * 1080;
        assert_eq!(wc_pages(0xFD00_0000, len, &RAM, true, KERNEL), Ok((0x7E8, 0x7EC)));
        // OVMF q35: 0x80000000, 800×600×4 ⇒ một trang.
        assert_eq!(wc_pages(0x8000_0000, 800 * 4 * 600, &RAM, true, KERNEL), Ok((1024, 1025)));
    }

    #[test]
    fn rounded_page_touching_ram_is_refused() {
        // Khung hình bắt đầu ngay sau RAM nhưng không căn 2 MiB: trang làm tròn
        // xuống chứa đuôi RAM ⇒ từ chối.
        let ram = [(MIB, 0x3FF0_0000 - MIB)]; // RAM tới 0x3FF00000
        assert_eq!(wc_pages(0x3FF0_0000, 4 * MIB, &ram, true, KERNEL), Err(WcRefusal::TouchesRam));
        // Bắt đầu ở biên 2 MiB kế tiếp ⇒ được.
        assert_eq!(wc_pages(0x4000_0000, 4 * MIB, &ram, true, KERNEL), Ok((512, 514)));
    }

    #[test]
    fn framebuffer_inside_ram_is_refused() {
        assert_eq!(wc_pages(64 * MIB, 8 * MIB, &RAM, true, KERNEL), Err(WcRefusal::TouchesRam));
    }

    #[test]
    fn kernel_image_is_refused_even_without_ram_entry() {
        assert_eq!(wc_pages(MIB, MIB, &[], true, KERNEL), Err(WcRefusal::TouchesKernel));
    }

    #[test]
    fn needs_complete_map() {
        assert_eq!(wc_pages(0xFD00_0000, 4 * MIB, &RAM, false, KERNEL), Err(WcRefusal::NoCompleteMap));
    }

    #[test]
    fn above_identity_map_is_refused() {
        // Chạm đúng biên 4 GiB vẫn được; vượt 1 byte thì không.
        assert_eq!(wc_pages(MAPPED_LIMIT - 2 * MIB, 2 * MIB, &RAM, true, KERNEL), Ok((2047, 2048)));
        assert_eq!(
            wc_pages(MAPPED_LIMIT - 2 * MIB, 2 * MIB + 1, &RAM, true, KERNEL),
            Err(WcRefusal::AboveMappedLimit)
        );
        assert_eq!(wc_pages(0x1_0000_0000_0, 4 * MIB, &RAM, true, KERNEL), Err(WcRefusal::AboveMappedLimit));
    }

    #[test]
    fn empty_and_overflow() {
        assert_eq!(wc_pages(0, 4 * MIB, &RAM, true, KERNEL), Err(WcRefusal::Empty));
        assert_eq!(wc_pages(0xFD00_0000, 0, &RAM, true, KERNEL), Err(WcRefusal::Empty));
        assert_eq!(wc_pages(u64::MAX - 10, 100, &RAM, true, KERNEL), Err(WcRefusal::Overflow));
        assert_eq!(wc_pages(u64::MAX - TWO_MIB, 10, &RAM, true, KERNEL), Err(WcRefusal::Overflow));
    }

    #[test]
    fn zero_length_ram_entries_ignored() {
        let ram = [(0xFD00_0000, 0)];
        assert_eq!(wc_pages(0xFD00_0000, 4 * MIB, &ram, true, KERNEL), Ok((0x7E8, 0x7EA)));
    }
}
