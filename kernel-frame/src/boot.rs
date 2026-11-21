//! YuiBootInfo — dữ liệu khởi động thô, chuẩn hoá cho cả ba kiến trúc.
//!
//! x86_64 nhận con trỏ multiboot (physical, dưới 4 GiB); ARM64/RISC-V nhận con
//! trỏ device tree (FDT). Từ hai định dạng khác nhau, hàm `parse` tạo ra **một**
//! `BootInfo` duy nhất — nhờ đó profiler và Tầng 1 chỉ có một đường khởi tạo.

use alloc::string::String;

/// Framebuffer tuyến tính do firmware cấp (multiboot 1 flag bit 12, thẻ
/// framebuffer type 8 của multiboot 2, nút `simple-framebuffer` của device
/// tree — P2.16; GOP là P6.4).
#[derive(Clone, Copy, Debug)]
pub struct Framebuffer {
    pub addr: u64,
    pub width: u32,
    pub height: u32,
    pub pitch: u32,
    pub bpp: u8,
    /// Vị trí bit của từng kênh màu trong một điểm ảnh (mặc định 16/8/0 =
    /// XRGB, kiểu VBE/GOP phổ biến nhất; firmware báo khác thì theo firmware).
    pub red_shift: u8,
    pub green_shift: u8,
    pub blue_shift: u8,
    /// Nguồn: "multiboot1" / "multiboot2" / "devicetree" — in ra log để biết
    /// đường nào chạy.
    pub source: &'static str,
}

/// Kiểm một framebuffer firmware báo: chỉ nhận RGB trực tiếp 24/32 bit, kích
/// thước hợp lý, 32 bit thì địa chỉ + pitch chia hết cho 4 (aarch64 chạy với
/// MMU tắt + strict-align: ghi u32 lệch là lỗi căn), và nằm trọn trong vùng
/// nhân chạm được: x86_64 = 4 GiB đầu mà bảng trang khởi động ánh xạ đồng
/// nhất; aarch64/riscv64 chưa bật MMU nên là địa chỉ vật lý < 2⁴⁸. Sai ở đâu
/// thì in ra và trả None — không vẽ bừa.
#[allow(clippy::too_many_arguments)]
fn checked_fb(
    addr: u64,
    pitch: u32,
    width: u32,
    height: u32,
    bpp: u8,
    fb_type: u8,
    shifts: (u8, u8, u8),
    source: &'static str,
) -> Option<Framebuffer> {
    if fb_type != 1 {
        fb_skip(source);
        println!("type {} (khong phai RGB truc tiep), bo qua", fb_type);
        return None;
    }
    if addr == 0 || width == 0 || height == 0 || width > 16384 || height > 16384 {
        fb_skip(source);
        println!("kich thuoc vo ly {}x{}, bo qua", width, height);
        return None;
    }
    if bpp != 24 && bpp != 32 {
        fb_skip(source);
        println!("{} bit/pixel chua ho tro, bo qua", bpp);
        return None;
    }
    let min_pitch = width as u64 * (bpp as u64 / 8);
    if (pitch as u64) < min_pitch {
        fb_skip(source);
        println!("pitch {} < {}, bo qua", pitch, min_pitch);
        return None;
    }
    if bpp == 32 && (addr % 4 != 0 || pitch % 4 != 0) {
        fb_skip(source);
        println!("dia chi/pitch khong can 4 byte, bo qua");
        return None;
    }
    let limit: u64 = if cfg!(target_arch = "x86_64") { 4u64 << 30 } else { 1u64 << 48 };
    let end = addr.checked_add(pitch as u64 * height as u64);
    if end.map_or(true, |e| e > limit) {
        fb_skip(source);
        println!("nam ngoai vung nhan cham duoc ({:#x}), bo qua", limit);
        return None;
    }
    let (r, g, b) = shifts;
    let sane = |v: u8| v % 8 == 0 && (v as u32) < bpp as u32;
    let (r, g, b) = if sane(r) && sane(g) && sane(b) && r != g && g != b && r != b {
        (r, g, b)
    } else {
        (16, 8, 0)
    };
    Some(Framebuffer {
        addr,
        width,
        height,
        pitch,
        bpp,
        red_shift: r,
        green_shift: g,
        blue_shift: b,
        source,
    })
}

/// Tiền tố "    - framebuffer <nguồn>: " in thẳng, KHÔNG qua `core::fmt`: trên
/// aarch64, định dạng một biến `&str` bằng `{}` làm nhân dừng giữa dòng (xem
/// console.rs — `print_str_no_newline`).
fn fb_skip(source: &str) {
    print!("    - framebuffer ");
    crate::console::print_str_no_newline(source);
    print!(": ");
}

/// Thông tin khởi động thô: hoặc multiboot info (x86_64) hoặc DTB (ARM/RISC-V).
///
/// `Multiboot` mang kèm magic để phân biệt hai giao thức: `0x2BADB002` là
/// multiboot 1 (QEMU `-kernel`), `0x36D76289` là multiboot 2 (GRUB2) — hai
/// định dạng info khác nhau nên `parse` rẽ nhánh theo magic.
#[derive(Clone, Copy)]
pub enum RawBoot {
    Multiboot(u64, u32),
    DeviceTree(u64),
    None,
}

#[derive(Debug)]
pub struct BootInfo {
    pub arch: &'static str,
    pub vendor: [u8; 12],
    pub cpu_name: String,
    pub simd: &'static str,
    pub harts: u32,
    pub ram_mb: u64,
    pub ram_source: &'static str,
    pub framebuffer: Option<Framebuffer>,
}

impl BootInfo {
    pub fn cpu_display(&self) -> String {
        let mut name = String::new();
        for &b in self.vendor.iter() {
            if b == 0 {
                break;
            }
            if b.is_ascii_graphic() || b == b' ' {
                name.push(b as char);
            }
        }
        if !self.cpu_name.is_empty() {
            name.push(' ');
            name.push_str(&self.cpu_name);
        }
        if name.is_empty() {
            name.push_str(self.arch);
        }
        name
    }
}

pub fn arch_name() -> &'static str {
    if cfg!(target_arch = "x86_64") {
        "x86_64"
    } else if cfg!(target_arch = "aarch64") {
        "aarch64"
    } else if cfg!(target_arch = "riscv64") {
        "riscv64"
    } else {
        "unknown"
    }
}

pub fn parse(raw: RawBoot) -> BootInfo {
    let mut info = BootInfo {
        arch: arch_name(),
        vendor: crate::arch::cpu_vendor(),
        cpu_name: String::new(),
        simd: crate::arch::detect_simd(),
        harts: 1,
        ram_mb: 0,
        ram_source: "khong doc duoc",
        framebuffer: None,
    };

    print!("    - simd: ");
    println_str!(info.simd);
    match raw {
        RawBoot::Multiboot(addr, 0x36D7_6289) => parse_multiboot2(&mut info, addr),
        RawBoot::Multiboot(addr, _) => parse_multiboot(&mut info, addr),
        RawBoot::DeviceTree(addr) => parse_fdt(&mut info, addr),
        RawBoot::None => {}
    }

    if info.ram_mb == 0 {
        // Không bịa số: nếu firmware không cho biết, in 0 và ghi rõ nguồn.
        info.ram_source = "khong doc duoc (firmware khong cung cap)";
    }
    info
}

// ---------------------------------------------------------------- x86_64 ---
//
// Multiboot 1: cấu trúc tại địa chỉ vật lý `addr`. Chỉ cần `flags`, `mem_lower`
// và `mem_upper`; framebuffer nếu firmware đặt bit 12.
fn parse_multiboot(info: &mut BootInfo, addr: u64) {
    if addr == 0 {
        return;
    }
    // Multiboot 1 nằm dưới 4 GiB theo đặc tả; địa chỉ cao hơn chắc chắn sai.
    if addr > u32::MAX as u64 {
        return;
    }
    let flags = unsafe { core::ptr::read_volatile((addr as *const u32).add(0)) };
    let mem_lower = unsafe { core::ptr::read_volatile((addr as *const u32).add(1)) };
    let mem_upper = unsafe { core::ptr::read_volatile((addr as *const u32).add(2)) };

    if flags & (1 << 0) != 0 {
        info.ram_mb = ((mem_lower as u64 + mem_upper as u64) + 1023) / 1024;
        info.ram_source = "multiboot mem_lower+mem_upper";
    }

    // flags bit 6 = mmap, bit 12 = framebuffer (multiboot 1.6 extension):
    // layout: flags(0), mem_lower(4), mem_upper(8), boot_device(12), cmdline(16),
    // mods_count(20), mods_addr(24), syms[16](28..44), mmap_length(44),
    // mmap_addr(48), drives_length(52), drives_addr(56), config_table(60),
    // boot_loader_name(64), apm_table(68), vbe_control_info(72), vbe_mode_info(76),
    // vbe_mode(80), vbe_interface_seg(82), vbe_interface_off(84),
    // vbe_interface_len(86), framebuffer_addr(88), framebuffer_pitch(96),
    // framebuffer_width(100), framebuffer_height(104), framebuffer_bpp(108),
    // framebuffer_type(109)
    if flags & (1 << 12) != 0 {
        let fb_addr = unsafe { core::ptr::read_unaligned((addr as *const u64).add(11)) };
        let pitch = unsafe { core::ptr::read_unaligned((addr as *const u32).add(24)) };
        let width = unsafe { core::ptr::read_unaligned((addr as *const u32).add(25)) };
        let height = unsafe { core::ptr::read_unaligned((addr as *const u32).add(26)) };
        let bpp = unsafe { core::ptr::read_volatile((addr as *const u8).add(108)) };
        let fb_type = unsafe { core::ptr::read_volatile((addr as *const u8).add(109)) };
        // color_info (type 1): red_pos(110) red_size(111) green_pos(112)
        // green_size(113) blue_pos(114) blue_size(115).
        let byte = |off: usize| unsafe { core::ptr::read_volatile((addr as *const u8).add(off)) };
        let shifts = (byte(110), byte(112), byte(114));
        info.framebuffer =
            checked_fb(fb_addr, pitch, width, height, bpp, fb_type, shifts, "multiboot1");
    }

    // Số nhân: multiboot 1 không có; lấy từ ACPI MADT là P2.2. In 1 và nói rõ.
    info.harts = 1;
}

// ---------------------------------------------------------------- multiboot 2
//
// GRUB2 `multiboot2` truyền một **luồng thẻ** (tag stream) thay vì bảng cố
// định: [total_size u32][reserved u32], rồi lần lượt các thẻ
// [type u32][size u32][payload…, tròn 8 byte], kết thúc bởi thẻ type 0.
// Thẻ type 4 (basic meminfo) có cùng ngữ nghĩa mem_lower/mem_upper như
// multiboot 1 — chỉ đọc nó, không bịa gì thêm.
fn parse_multiboot2(info: &mut BootInfo, addr: u64) {
    if addr == 0 || addr > u32::MAX as u64 {
        return;
    }
    let base = addr as usize;
    let total_size = unsafe { core::ptr::read_unaligned(base as *const u32) } as usize;
    if total_size < 16 || total_size > 1024 * 1024 {
        println!("    - multiboot2 info vo ly ({} byte), bo qua", total_size);
        return;
    }
    let blob = unsafe { core::slice::from_raw_parts(base as *const u8, total_size) };

    let mut pos = 8usize;  // sau header total_size + reserved
    let mut guard = 0u32;
    // Tổng vùng `available` (type 1) của thẻ mmap (type 6), tính bằng byte.
    // Dưới UEFI, `mem_upper` của thẻ meminfo chỉ đếm vùng LIỀN từ 1 MiB tới lỗ
    // đầu tiên OVMF giữ lại, nên có thể nhỏ hơn RAM thật nhiều lần ⇒ có mmap
    // thì dùng mmap (P2.2/P6.4).
    let mut mmap_avail: u64 = 0;
    while pos + 8 <= blob.len() && guard < 10_000 {
        guard += 1;
        // Multiboot2: mọi trường là LITTLE-endian (khác FDT).
        let tag_type = le32(blob, pos);
        let tag_size = le32(blob, pos + 4) as usize;
        if tag_size < 8 || pos + tag_size > blob.len() {
            return;  // thẻ hỏng: dừng, không đọc bừa
        }
        match tag_type {
            0 => break,  // end tag
            4 => {
                // basic meminfo: mem_lower (KB dưới 1 MiB), mem_upper (KB trên 1 MiB)
                if tag_size >= 16 {
                    let mem_lower = le32(blob, pos + 8) as u64;
                    let mem_upper = le32(blob, pos + 12) as u64;
                    info.ram_mb = (mem_lower + mem_upper + 1023) / 1024;
                    info.ram_source = "multiboot2 meminfo";
                }
            }
            2 => {
                // bootloader name tag: in tên bootloader ASCII để log tự khai.
                if tag_size > 8 {
                    let name = &blob[pos + 8..pos + tag_size];
                    let mut end = 0;
                    while end < name.len() && name[end] != 0 {
                        end += 1;
                    }
                    print!("    - bootloader: ");
                    for &b in &name[..end.min(64)] {
                        if b.is_ascii_graphic() || b == b' ' {
                            print!("{}", b as char);
                        }
                    }
                    println!();
                }
            }
            8 => {
                // framebuffer info (P2.16). Bố cục theo GRUB (multiboot2.h):
                // addr u64 (8), pitch (16), width (20), height (24), bpp u8 (28),
                // type u8 (29), reserved u16 (30), color_info từ 32: với type 1
                // là red_pos, red_size, green_pos, green_size, blue_pos, blue_size.
                if tag_size >= 32 {
                    let fb_addr = le32(blob, pos + 8) as u64 | ((le32(blob, pos + 12) as u64) << 32);
                    let pitch = le32(blob, pos + 16);
                    let width = le32(blob, pos + 20);
                    let height = le32(blob, pos + 24);
                    let bpp = blob[pos + 28];
                    let fb_type = blob[pos + 29];
                    let shifts = if tag_size >= 38 {
                        (blob[pos + 32], blob[pos + 34], blob[pos + 36])
                    } else {
                        (16, 8, 0)
                    };
                    info.framebuffer =
                        checked_fb(fb_addr, pitch, width, height, bpp, fb_type, shifts, "multiboot2");
                }
            }
            6 => {
                // memory map: entry_size (8), entry_version (12), từ 16 là các
                // mục { base u64, len u64, type u32, reserved u32 }.
                if tag_size >= 16 {
                    let entry_size = le32(blob, pos + 8) as usize;
                    if entry_size >= 24 {
                        let mut e = pos + 16;
                        while e + 24 <= pos + tag_size {
                            let len = le32(blob, e + 8) as u64 | ((le32(blob, e + 12) as u64) << 32);
                            if le32(blob, e + 16) == 1 {
                                mmap_avail = mmap_avail.saturating_add(len);
                            }
                            e += entry_size;
                        }
                    }
                }
            }
            11 | 12 if tag_size >= 12 => {
                // Con trỏ bảng hệ thống EFI 32/64 bit: chỉ có khi bộ nạp chạy
                // trên UEFI. Đây là bằng chứng firmware là UEFI, không đoán.
                let ptr = if tag_type == 12 && tag_size >= 16 {
                    le32(blob, pos + 8) as u64 | ((le32(blob, pos + 12) as u64) << 32)
                } else {
                    le32(blob, pos + 8) as u64
                };
                print!("    - firmware: UEFI (bang he thong EFI");
                if tag_type == 12 {
                    print!("64");
                } else {
                    print!("32");
                }
                println!(" @ {:#x})", ptr);
            }
            _ => {}  // thẻ khác (ACPI, EFI mmap…) — chưa dùng, bỏ qua
        }
        pos = (pos + tag_size + 7) & !7;
    }
    if mmap_avail >= 1024 * 1024 {
        info.ram_mb = (mmap_avail + (1 << 20) - 1) >> 20;
        info.ram_source = "multiboot2 mmap (vung available)";
    }
    info.harts = 1;
}

/// Tìm FDT header (magic `0xd00dfeed` đọc big-endian) trong cửa sổ
/// `[start, start+window)`, bước tròn 8 byte theo yêu cầu alignment của FDT.
///
/// Dùng khi firmware **không** truyền con trỏ DTB trong x0 — QEMU đặt DTB
/// sinh tự động ngay tại đầu RAM (`loader_start`) khi nạp nhân ELF
/// (`is_linux = false`, không set x0). Trả về 0 nếu không thấy: không bịa.
pub fn find_fdt(start: u64, window: u64) -> u64 {
    if start == 0 {
        return 0;
    }
    let mut addr = start;
    let end = start + window;
    while addr + 4 <= end {
        let magic = unsafe { core::ptr::read_unaligned(addr as *const u32) }.swap_bytes();
        if magic == FDT_MAGIC {
            return addr;
        }
        addr += 8;
    }
    0
}

// ------------------------------------------------------------ ARM / RISC-V --
//
// Bộ đọc device tree tối giản: duyệt khối cấu trúc, tìm node "memory" và đọc
// thuộc tính `reg` (base, size). Không cấp phát, không đệ quy.
const FDT_MAGIC: u32 = 0xd00d_feed;

fn be32(bytes: &[u8], offset: usize) -> u32 {
    let mut v = 0u32;
    let mut i = 0;
    while i < 4 {
        v = (v << 8) | bytes[offset + i] as u32;
        i += 1;
    }
    v
}

// Multiboot2 (khác FDT): trường u32 little-endian.
fn le32(bytes: &[u8], offset: usize) -> u32 {
    let mut v = 0u32;
    let mut i = 0;
    while i < 4 {
        v |= (bytes[offset + i] as u32) << (8 * i);
        i += 1;
    }
    v
}

fn parse_fdt(info: &mut BootInfo, addr: u64) {
    if addr == 0 {
        println!("    - khong co device tree");
        return;
    }
    let base = addr as usize;
    // Chặn con trỏ rác trước khi chạm bộ nhớ: một địa chỉ ngoài RAM sẽ gây lỗi
    // trang và khung không còn cơ hội in ra manh mối nào.
    if !(0x1000..(1usize << 48)).contains(&base) {
        println!("    - device tree @ {:#x} vo ly, bo qua", base);
        return;
    }
    let magic = unsafe { core::ptr::read_unaligned(base as *const u32) }.swap_bytes();
    println!("    - device tree @ {:#x}, magic {:#x}", base, magic);
    if magic != FDT_MAGIC {
        println!("    - khong phai FDT (can 0xd00dfeed), bo qua");
        return;
    }
    let header = unsafe { core::slice::from_raw_parts(base as *const u8, 40) };
    if be32(header, 0) != FDT_MAGIC {
        return;
    }
    let totalsize = be32(header, 4) as usize;
    let off_struct = be32(header, 8) as usize;
    let off_strings = be32(header, 12) as usize;
    if totalsize < 40 || totalsize > 64 * 1024 * 1024 {
        return;
    }
    let blob = unsafe { core::slice::from_raw_parts(base as *const u8, totalsize) };

    let mut pos = off_struct;
    let mut depth = 0usize;
    let mut in_memory = 0usize;
    let mut ram_bytes = 0u64;
    let mut harts = 0u32;

    // Giới hạn số vòng để một DTB hỏng không treo nhân.
    let mut guard = 0u32;
    while pos + 4 <= blob.len() && guard < 100_000 {
        guard += 1;
        let token = be32(blob, pos);
        pos += 4;
        match token {
            1 => {
                // FDT_BEGIN_NODE: tên node, kết thúc bằng NUL, căn 4 byte.
                let start = pos;
                let mut end = start;
                while end < blob.len() && blob[end] != 0 {
                    end += 1;
                }
                let name = &blob[start..end];
                if name.starts_with(b"memory") && depth == 1 {
                    in_memory = depth + 1;
                } else if name.starts_with(b"cpu@") || name == &b"cpu"[..] {
                    if depth == 2 {
                        harts = harts.saturating_add(1);
                    }
                }
                depth += 1;
                pos = (end + 4) & !3;
            }
            2 => {
                // FDT_END_NODE
                depth = depth.saturating_sub(1);
                if in_memory > 0 && depth < in_memory {
                    in_memory = 0;
                }
            }
            3 => {
                // FDT_PROP: len(4) nameoff(4) value[...]
                if pos + 8 > blob.len() {
                    return;
                }
                let len = be32(blob, pos) as usize;
                let nameoff = be32(blob, pos + 4) as usize;
                pos += 8;
                if pos + len > blob.len() {
                    return;
                }
                if off_strings + nameoff < blob.len() {
                    let name_bytes = &blob[off_strings + nameoff..];
                    // RISC-V: chuỗi ISA của hart (rv64imafdcv...) là cách duy nhất
                    // tin được từ S-mode để biết có vector extension.
                    if name_bytes.starts_with(b"riscv,isa") && len > 0 && pos + len <= blob.len() {
                        let isa = &blob[pos..pos + len];
                        if isa.iter().any(|&b| b == b'v' || b == b'V') {
                            info.simd = "rvv";
                        }
                    }
                }
                if in_memory > 0 && len >= 8 && off_strings + nameoff < blob.len() {
                    let prop_name = &blob[off_strings + nameoff..];
                    if prop_name.starts_with(b"reg") {
                        let value = &blob[pos..pos + len];
                        // #address-cells/#size-cells = 2/2 trên QEMU virt; nếu là
                        // 1/1 thì đọc theo 32-bit. Nhận cả hai cho an toàn.
                        if len >= 16 {
                            let size = u64::from(be32(value, 8)) << 32 | be32(value, 12) as u64;
                            ram_bytes = ram_bytes.saturating_add(size);
                        } else if len >= 8 {
                            ram_bytes = ram_bytes.saturating_add(be32(value, 4) as u64);
                        }
                    }
                }
                pos = (pos + len + 3) & !3;
            }
            4 => {}
            9 => break,
            _ => return,
        }
    }

    if ram_bytes > 0 {
        info.ram_mb = ram_bytes / (1024 * 1024);
        info.ram_source = "device tree /memory/reg";
    }
    info.harts = if harts > 0 { harts } else { 1 };

    fdt_framebuffer(info, blob);
}

/// P2.16 (c): nút `simple-framebuffer`. Toàn bộ việc đọc nằm ở hàm an toàn
/// `yui_cap::fdt::parse_fdt_framebuffer` (cùng vector với bản C++ + Python);
/// ở đây chỉ in kết quả, kiểm như mọi framebuffer khác, và chặn trường hợp
/// vùng khung hình đè lên ảnh nhân hoặc chính DTB (vẽ vào đó là tự phá mình).
fn fdt_framebuffer(info: &mut BootInfo, blob: &[u8]) {
    use yui_cap::fdt::{parse_fdt_framebuffer, FdtReason};
    let r = parse_fdt_framebuffer(blob);
    if r.reason != FdtReason::Ok {
        print!("    - device tree: khong co framebuffer dung duoc (");
        crate::console::print_str_no_newline(r.reason.code());
        println!(", {} nut simple-framebuffer)", r.candidates);
        return;
    }
    print!("    - device tree: simple-framebuffer {}x{} ", r.width, r.height);
    crate::console::print_str_no_newline(r.format);
    println!(" stride {} @ {:#x} (+{:#x})", r.stride, r.addr, r.size);
    let fb_end = r.addr.saturating_add(r.size);
    let dtb = (blob.as_ptr() as u64, blob.as_ptr() as u64 + blob.len() as u64);
    let image = kernel_image();
    for (what, (lo, hi)) in [("anh nhan", image), ("device tree", dtb)] {
        if lo < hi && r.addr < hi && lo < fb_end {
            print!("    - framebuffer devicetree: de len ");
            crate::console::print_str_no_newline(what);
            println!(" [{:#x}, {:#x}), bo qua", lo, hi);
            return;
        }
    }
    let shifts = (r.red_shift, r.green_shift, r.blue_shift);
    info.framebuffer = checked_fb(r.addr, r.stride, r.width, r.height, r.bpp, 1, shifts, "devicetree");
}

/// Vùng ảnh nhân [__kernel_start, __kernel_end) theo linker script (gồm cả
/// .bss, nơi đặt ngăn xếp và đống cấp phát).
fn kernel_image() -> (u64, u64) {
    extern "C" {
        static __kernel_start: u8;
        static __kernel_end: u8;
    }
    // An toàn: chỉ lấy ĐỊA CHỈ của hai ký hiệu linker, không đọc nội dung.
    unsafe {
        (
            core::ptr::addr_of!(__kernel_start) as u64,
            core::ptr::addr_of!(__kernel_end) as u64,
        )
    }
}
