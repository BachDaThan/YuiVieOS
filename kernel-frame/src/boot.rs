//! YuiBootInfo — dữ liệu khởi động thô, chuẩn hoá cho cả ba kiến trúc.
//!
//! x86_64 nhận con trỏ multiboot (physical, dưới 4 GiB); ARM64/RISC-V nhận con
//! trỏ device tree (FDT). Từ hai định dạng khác nhau, hàm `parse` tạo ra **một**
//! `BootInfo` duy nhất — nhờ đó profiler và Tầng 1 chỉ có một đường khởi tạo.

use alloc::string::String;

/// Framebuffer tuyến tính do firmware cấp (multiboot flag bit 12 / simple-framebuffer).
#[derive(Clone, Copy, Debug)]
pub struct Framebuffer {
    pub addr: u64,
    pub width: u32,
    pub height: u32,
    pub pitch: u32,
    pub bpp: u8,
}

/// Thông tin khởi động thô: hoặc multiboot info (x86_64) hoặc DTB (ARM/RISC-V).
#[derive(Clone, Copy)]
pub enum RawBoot {
    Multiboot(u64),
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
        RawBoot::Multiboot(addr) => parse_multiboot(&mut info, addr),
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
        if fb_addr != 0 && width > 0 && height > 0 && bpp >= 24 {
            info.framebuffer = Some(Framebuffer {
                addr: fb_addr,
                width,
                height,
                pitch,
                bpp,
            });
        }
    }

    // Số nhân: multiboot 1 không có; lấy từ ACPI MADT là P2.2. In 1 và nói rõ.
    info.harts = 1;
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
}
