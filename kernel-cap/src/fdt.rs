//! Framebuffer từ Device Tree — P2.16 đường (c).
//!
//! Board ARM/RISC-V không có VBE/GOP: firmware (U-Boot, bootloader của board)
//! dựng sẵn khung hình rồi ghi địa chỉ, kích thước và định dạng vào một nút
//! `simple-framebuffer` của cây thiết bị (binding Linux
//! `display/simple-framebuffer.yaml`). Nhân đọc nút đó để có màn hình chào
//! trước khi có driver GPU.
//!
//! Đây là bộ đọc FDT (Devicetree Specification v0.4 §5) thuần, an toàn, không
//! cấp phát và không tin đầu vào: mọi offset/độ dài đều kiểm bằng `get`/
//! `checked_add`; DTB hỏng ở BẤT KỲ đâu ⇒ [`FdtReason::BadBlob`] và không dùng
//! nút nào. `kernel-frame` chỉ dựng lát cắt từ con trỏ firmware đưa (phần
//! `unsafe` duy nhất), còn lại là hàm này.
//!
//! Bản C++ `yui::display::parse_fdt_framebuffer` (userspace-vi/display.cpp)
//! làm cùng quy tắc; cả hai chạy bộ vector `tests/data/fdt_fb_vectors.txt`
//! sinh bởi bản tham chiếu Python `scripts/gen_fdt_vectors.py` (quy tắc chi
//! tiết ở đầu tệp đó).

/// Lý do — cùng số với `yui::display::FdtReason` (verify.py kiểm).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum FdtReason {
    Ok = 0,
    NoNode = 1,
    Disabled = 2,
    BadFormat = 3,
    BadGeometry = 4,
    BadReg = 5,
    BadBlob = 6,
}

impl FdtReason {
    /// Mã chữ, giống `yui::display::fdt_reason_code`.
    pub fn code(self) -> &'static str {
        match self {
            FdtReason::Ok => "ok",
            FdtReason::NoNode => "no_node",
            FdtReason::Disabled => "disabled",
            FdtReason::BadFormat => "bad_format",
            FdtReason::BadGeometry => "bad_geometry",
            FdtReason::BadReg => "bad_reg",
            FdtReason::BadBlob => "bad_blob",
        }
    }
}

/// Kết quả. Khi `reason != Ok` mọi trường số bằng 0 và `format` rỗng.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct FdtFramebuffer {
    pub reason: FdtReason,
    /// Số nút simple-framebuffer đã xét (0 nếu `BadBlob`).
    pub candidates: u32,
    pub addr: u64,
    pub size: u64,
    pub width: u32,
    pub height: u32,
    pub stride: u32,
    pub bpp: u8,
    pub red_shift: u8,
    pub green_shift: u8,
    pub blue_shift: u8,
    pub format: &'static str,
}

impl FdtFramebuffer {
    const fn empty(reason: FdtReason, candidates: u32) -> Self {
        FdtFramebuffer {
            reason,
            candidates,
            addr: 0,
            size: 0,
            width: 0,
            height: 0,
            stride: 0,
            bpp: 0,
            red_shift: 0,
            green_shift: 0,
            blue_shift: 0,
            format: "",
        }
    }
}

const FDT_MAGIC: u32 = 0xD00D_FEED;
const TOK_BEGIN: u32 = 1;
const TOK_END_NODE: u32 = 2;
const TOK_PROP: u32 = 3;
const TOK_NOP: u32 = 4;
const TOK_END: u32 = 9;
const MAX_DEPTH: usize = 32;
const MAX_DIM: u32 = 16384;
const MAX_STRIDE: u32 = 262_144;

/// (tên, bpp, dịch đỏ, dịch lục, dịch lam) — giá trị điểm ảnh little-endian.
const FORMATS: [(&str, u8, u8, u8, u8); 5] = [
    ("a8r8g8b8", 32, 16, 8, 0),
    ("x8r8g8b8", 32, 16, 8, 0),
    ("a8b8g8r8", 32, 0, 8, 16),
    ("x8b8g8r8", 32, 0, 8, 16),
    ("r8g8b8", 24, 16, 8, 0),
];

fn be32(b: &[u8], off: usize) -> Option<u32> {
    let s = b.get(off..off.checked_add(4)?)?;
    Some(u32::from_be_bytes([s[0], s[1], s[2], s[3]]))
}

/// Chuỗi DT: các byte tới NUL đầu tiên (hoặc cả giá trị nếu không có NUL).
fn cstr(v: &[u8]) -> &[u8] {
    match v.iter().position(|&b| b == 0) {
        Some(i) => &v[..i],
        None => v,
    }
}

/// stringlist: tách theo NUL, so khớp đúng từng mục.
fn list_has(v: &[u8], lit: &[u8]) -> bool {
    v.split(|&b| b == 0).any(|p| p == lit)
}

fn u32_prop(v: Option<&[u8]>) -> Option<u32> {
    match v {
        Some(b) if b.len() == 4 => Some(u32::from_be_bytes([b[0], b[1], b[2], b[3]])),
        _ => None,
    }
}

fn cell_count(v: Option<&[u8]>, default: u32) -> u32 {
    match v {
        None => default,
        Some(b) if b.len() == 4 => u32::from_be_bytes([b[0], b[1], b[2], b[3]]),
        Some(_) => 0,
    }
}

fn read_cells(b: &[u8], cells: usize) -> u64 {
    let mut v = 0u64;
    for i in 0..cells {
        v = (v << 32) | u64::from(be32(b, 4 * i).unwrap_or(0));
    }
    v
}

#[derive(Clone, Copy, Default)]
struct Props<'a> {
    compatible: Option<&'a [u8]>,
    status: Option<&'a [u8]>,
    format: Option<&'a [u8]>,
    width: Option<&'a [u8]>,
    height: Option<&'a [u8]>,
    stride: Option<&'a [u8]>,
    reg: Option<&'a [u8]>,
    address_cells: Option<&'a [u8]>,
    size_cells: Option<&'a [u8]>,
    had_child: bool,
}

fn judge(me: &Props<'_>, parent: Option<&Props<'_>>) -> Result<FdtFramebuffer, FdtReason> {
    if let Some(s) = me.status {
        let c = cstr(s);
        if c != b"okay" && c != b"ok" {
            return Err(FdtReason::Disabled);
        }
    }
    let fmt = match me.format {
        Some(f) => {
            let c = cstr(f);
            FORMATS.iter().find(|e| e.0.as_bytes() == c)
        }
        None => None,
    };
    let fmt = fmt.ok_or(FdtReason::BadFormat)?;
    let (w, h, stride) = match (u32_prop(me.width), u32_prop(me.height), u32_prop(me.stride)) {
        (Some(a), Some(b), Some(c)) => (a, b, c),
        _ => return Err(FdtReason::BadGeometry),
    };
    let bytes = u64::from(fmt.1 / 8);
    if !(1..=MAX_DIM).contains(&w)
        || !(1..=MAX_DIM).contains(&h)
        || u64::from(stride) < u64::from(w) * bytes
        || stride > MAX_STRIDE
    {
        return Err(FdtReason::BadGeometry);
    }
    let ac = cell_count(parent.and_then(|p| p.address_cells), 2);
    let sc = cell_count(parent.and_then(|p| p.size_cells), 1);
    if !matches!(ac, 1 | 2) || !matches!(sc, 1 | 2) {
        return Err(FdtReason::BadReg);
    }
    let reg = me.reg.ok_or(FdtReason::BadReg)?;
    let entry = 4 * (ac + sc) as usize;
    if reg.is_empty() || reg.len() % entry != 0 {
        return Err(FdtReason::BadReg);
    }
    let addr = read_cells(reg, ac as usize);
    let size = read_cells(&reg[4 * ac as usize..], sc as usize);
    if addr == 0 || size < u64::from(stride) * u64::from(h) || size > u64::MAX - addr {
        return Err(FdtReason::BadReg);
    }
    Ok(FdtFramebuffer {
        reason: FdtReason::Ok,
        candidates: 0,
        addr,
        size,
        width: w,
        height: h,
        stride,
        bpp: fmt.1,
        red_shift: fmt.2,
        green_shift: fmt.3,
        blue_shift: fmt.4,
        format: fmt.0,
    })
}

/// Tìm framebuffer đầu tiên dùng được trong một DTB (FDT v17). `data` là
/// đúng số byte có thật; header nói lớn hơn ⇒ `BadBlob`.
pub fn parse_fdt_framebuffer(data: &[u8]) -> FdtFramebuffer {
    walk(data).unwrap_or(FdtFramebuffer::empty(FdtReason::BadBlob, 0))
}

fn walk(data: &[u8]) -> Option<FdtFramebuffer> {
    if data.len() < 40 {
        return None;
    }
    let h = |i: usize| be32(data, 4 * i).map(u64::from);
    let (magic, total, off_st, off_str) = (h(0)?, h(1)?, h(2)?, h(3)?);
    let (ver, last, sz_str, sz_st) = (h(5)?, h(6)?, h(8)?, h(9)?);
    if magic != u64::from(FDT_MAGIC) || total < 40 || total > data.len() as u64 || ver < 17 || last > 17 {
        return None;
    }
    if off_st % 4 != 0 || sz_st % 4 != 0 || off_st < 40 || off_st + sz_st > total || off_str + sz_str > total {
        return None;
    }
    let st = data.get(off_st as usize..(off_st + sz_st) as usize)?;
    let strs = data.get(off_str as usize..(off_str + sz_str) as usize)?;

    let mut stack = [Props::default(); MAX_DEPTH];
    let mut depth = 0usize;
    let mut pos = 0usize;
    let mut count = 0u32;
    let mut first_ok: Option<FdtFramebuffer> = None;
    let mut first_bad: Option<FdtReason> = None;
    if be32(st, 0)? != TOK_BEGIN {
        return None;
    }
    loop {
        let tok = be32(st, pos)?;
        pos += 4;
        match tok {
            TOK_BEGIN => {
                let z = st.get(pos..)?.iter().position(|&b| b == 0)?;
                if depth > 0 {
                    stack[depth - 1].had_child = true;
                }
                if depth >= MAX_DEPTH {
                    return None;
                }
                stack[depth] = Props::default();
                depth += 1;
                pos = (pos + z + 1 + 3) & !3;
            }
            TOK_END_NODE => {
                if depth == 0 {
                    return None;
                }
                depth -= 1;
                let me = stack[depth];
                if me.compatible.map_or(false, |c| list_has(c, b"simple-framebuffer")) {
                    count += 1;
                    let parent = if depth > 0 { Some(&stack[depth - 1]) } else { None };
                    match judge(&me, parent) {
                        Ok(fb) => {
                            if first_ok.is_none() {
                                first_ok = Some(fb);
                            }
                        }
                        Err(r) => {
                            if first_bad.is_none() {
                                first_bad = Some(r);
                            }
                        }
                    }
                }
                if depth == 0 {
                    // Sau nút gốc chỉ được có NOP rồi END.
                    loop {
                        let t = be32(st, pos)?;
                        pos += 4;
                        if t == TOK_NOP {
                            continue;
                        }
                        if t != TOK_END {
                            return None;
                        }
                        break;
                    }
                    break;
                }
            }
            TOK_PROP => {
                if depth == 0 || stack[depth - 1].had_child {
                    return None;
                }
                let len = be32(st, pos)? as usize;
                let nameoff = be32(st, pos + 4)? as usize;
                pos += 8;
                let value = st.get(pos..pos.checked_add(len)?)?;
                let rest = strs.get(nameoff..)?;
                let name = &rest[..rest.iter().position(|&b| b == 0)?];
                let p = &mut stack[depth - 1];
                match name {
                    b"compatible" => p.compatible = Some(value),
                    b"status" => p.status = Some(value),
                    b"format" => p.format = Some(value),
                    b"width" => p.width = Some(value),
                    b"height" => p.height = Some(value),
                    b"stride" => p.stride = Some(value),
                    b"reg" => p.reg = Some(value),
                    b"#address-cells" => p.address_cells = Some(value),
                    b"#size-cells" => p.size_cells = Some(value),
                    _ => {}
                }
                pos = (pos + len + 3) & !3;
            }
            TOK_NOP => {}
            _ => return None,
        }
    }
    Some(match first_ok {
        Some(mut fb) => {
            fb.candidates = count;
            fb
        }
        None => FdtFramebuffer::empty(first_bad.unwrap_or(FdtReason::NoNode), count),
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    const VECTORS: &str = include_str!("../../tests/data/fdt_fb_vectors.txt");

    fn unhex(s: &str) -> Vec<u8> {
        (0..s.len() / 2)
            .map(|i| u8::from_str_radix(&s[2 * i..2 * i + 2], 16).unwrap())
            .collect()
    }

    fn cases() -> Vec<(String, Vec<u8>, Vec<String>)> {
        VECTORS
            .lines()
            .filter(|l| !l.is_empty() && !l.starts_with('#'))
            .map(|l| {
                let c: Vec<&str> = l.split('|').collect();
                assert_eq!(c.len(), 12, "{l}");
                (c[0].to_string(), unhex(c[1]), c[2..].iter().map(|s| s.to_string()).collect())
            })
            .collect()
    }

    fn fields(r: &FdtFramebuffer) -> Vec<String> {
        let mut v = vec![r.reason.code().to_string(), r.candidates.to_string()];
        if r.reason != FdtReason::Ok {
            v.extend((0..8).map(|_| String::from("-")));
            return v;
        }
        v.push(format!("{:x}", r.addr));
        v.push(format!("{:x}", r.size));
        v.push(r.width.to_string());
        v.push(r.height.to_string());
        v.push(r.stride.to_string());
        v.push(r.bpp.to_string());
        v.push(format!("{},{},{}", r.red_shift, r.green_shift, r.blue_shift));
        v.push(r.format.to_string());
        v
    }

    fn sane(r: &FdtFramebuffer) -> bool {
        if r.reason != FdtReason::Ok {
            return r.addr == 0 && r.width == 0 && r.format.is_empty();
        }
        let bytes = u64::from(r.bpp / 8);
        (r.bpp == 24 || r.bpp == 32)
            && r.addr != 0
            && (1..=MAX_DIM).contains(&r.width)
            && (1..=MAX_DIM).contains(&r.height)
            && u64::from(r.stride) >= u64::from(r.width) * bytes
            && r.size >= u64::from(r.stride) * u64::from(r.height)
            && r.size <= u64::MAX - r.addr
            && r.candidates >= 1
    }

    #[test]
    fn shared_vectors_match_cpp_and_python() {
        let all = cases();
        assert!(all.len() >= 12, "DoD: ít nhất 12 bảng");
        let (mut ok, mut bad) = (0, 0);
        for (name, blob, want) in &all {
            let r = parse_fdt_framebuffer(blob);
            assert_eq!(&fields(&r), want, "{name}");
            assert!(sane(&r), "{name}");
            ok += (r.reason == FdtReason::Ok) as u32;
            bad += (r.reason == FdtReason::BadBlob) as u32;
        }
        assert!(ok >= 10 && bad >= 10);
    }

    #[test]
    fn every_truncation_is_bad_blob() {
        for (name, blob, want) in cases() {
            if want[0] != "ok" {
                continue;
            }
            let total = u32::from_be_bytes([blob[4], blob[5], blob[6], blob[7]]) as usize;
            for n in 0..total {
                assert_eq!(parse_fdt_framebuffer(&blob[..n]).reason, FdtReason::BadBlob, "{name} cắt {n}");
            }
        }
    }

    #[test]
    fn bit_flips_and_random_bytes_never_break_invariants() {
        let all = cases();
        let base = &all.iter().find(|c| c.0 == "qemu_virt_800x600_argb").unwrap().1;
        let mut still_ok = 0;
        let mut total = 0;
        for i in 0..base.len() {
            for bit in 0..8 {
                let mut x = base.clone();
                x[i] ^= 1 << bit;
                let r = parse_fdt_framebuffer(&x);
                assert!(sane(&r), "byte {i} bit {bit}");
                still_ok += (r.reason == FdtReason::Ok) as u32;
                total += 1;
            }
        }
        assert!(still_ok < total);
        let mut seed: u32 = 0x5EED;
        for k in 0..4000 {
            let mut x = base.clone();
            for _ in 0..1 + k % 6 {
                seed = seed.wrapping_mul(1_664_525).wrapping_add(1_013_904_223);
                let at = 56 + (seed >> 8) as usize % (x.len() - 56);
                seed = seed.wrapping_mul(1_664_525).wrapping_add(1_013_904_223);
                x[at] = (seed >> 24) as u8;
            }
            assert!(sane(&parse_fdt_framebuffer(&x)), "vòng {k}");
        }
    }
}
