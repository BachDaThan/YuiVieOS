//! Chọn chế độ hiển thị gốc + hệ số phóng nguyên (P2.16) — bản phản chiếu
//! của `yui::display` trong `include/yui/display.hpp` /
//! `userspace-vi/display.cpp`.
//!
//! Chỉ số nguyên, không cấp phát, không chạm phần cứng: nhân (`kernel-frame`)
//! gọi [`choose_scale`] cho màn hình chào, và khi có đường GOP (P6.4) sẽ gọi
//! [`pick_mode`] với danh sách chế độ của firmware. Cả C++ lẫn Rust chạy cùng
//! tệp vector `tests/data/display_vectors.txt` (sinh bởi
//! `scripts/gen_display_vectors.py`, bản tham chiếu Python thứ ba), nên ba bản
//! không thể lệch nhau mà test không đỏ. `PickReason` được `scripts/verify.py`
//! so từng giá trị với bản C++.

const MAX_DIM: u32 = 16384;
const DPI_X10_MIN: u64 = 400;
const DPI_X10_MAX: u64 = 7000;
const EDID_HEADER: [u8; 8] = [0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00];

/// Một chế độ firmware cung cấp (GOP / VBE / simple-framebuffer).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Mode {
    pub width: u32,
    pub height: u32,
    pub bpp: u8,
    /// 0 = không rõ.
    pub refresh_hz: u16,
}

impl Mode {
    pub const fn new(width: u32, height: u32, bpp: u8, refresh_hz: u16) -> Mode {
        Mode {
            width,
            height,
            bpp,
            refresh_hz,
        }
    }

    fn area(&self) -> u64 {
        self.width as u64 * self.height as u64
    }

    fn usable(&self) -> bool {
        let depth_ok = self.bpp == 24 || self.bpp == 32;
        let w_ok = self.width >= 1 && self.width <= MAX_DIM;
        depth_ok && w_ok && self.height >= 1 && self.height <= MAX_DIM
    }
}

/// Phần của EDID mà việc chọn chế độ/hệ số cần.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Edid {
    pub valid: bool,
    pub vendor: [u8; 3],
    pub product: u16,
    pub has_native: bool,
    pub native_width: u32,
    pub native_height: u32,
    pub native_refresh_hz: u16,
    /// 0 = không rõ (máy chiếu, EDID thiếu hoặc số đo vô lý).
    pub width_mm: u16,
    pub height_mm: u16,
}

fn plausible(px: u32, mm: u32) -> bool {
    if mm == 0 || px == 0 {
        return false;
    }
    let d = px as u64 * 254 / mm as u64;
    (DPI_X10_MIN..=DPI_X10_MAX).contains(&d)
}

/// Đọc khối EDID cơ sở 128 byte (khối mở rộng bị bỏ qua).
pub fn parse_edid(d: &[u8]) -> Edid {
    let mut e = Edid::default();
    if d.len() < 128 {
        return e;
    }
    if d[..8] != EDID_HEADER {
        return e;
    }
    let sum = d[..128].iter().fold(0u8, |a, &b| a.wrapping_add(b));
    if sum != 0 || d[18] != 1 {
        return e;
    }
    let id = ((d[8] as u32) << 8) | d[9] as u32;
    for (i, letter) in e.vendor.iter_mut().enumerate() {
        let v = (id >> (10 - 5 * i)) & 0x1F;
        *letter = if (1..=26).contains(&v) {
            b'A' + (v - 1) as u8
        } else {
            b'?'
        };
    }
    e.product = d[10] as u16 | ((d[11] as u16) << 8);

    let (mut dtd_w_mm, mut dtd_h_mm) = (0u32, 0u32);
    for slot in 0..4 {
        let t = &d[54 + 18 * slot..72 + 18 * slot];
        let clock = t[0] as u64 | ((t[1] as u64) << 8);
        if clock == 0 || e.has_native {
            continue;
        }
        let ha = t[2] as u32 | ((t[4] as u32 & 0xF0) << 4);
        let hb = t[3] as u32 | ((t[4] as u32 & 0x0F) << 8);
        let va = t[5] as u32 | ((t[7] as u32 & 0xF0) << 4);
        let vb = t[6] as u32 | ((t[7] as u32 & 0x0F) << 8);
        if ha == 0 || va == 0 {
            continue;
        }
        let interlaced = t[17] & 0x80 != 0;
        e.has_native = true;
        e.native_width = ha;
        e.native_height = if interlaced { va * 2 } else { va };
        let total = (ha + hb) as u64 * (va + vb) as u64;
        let hz = (clock * 10000 + total / 2) / total;
        e.native_refresh_hz = if hz > 1000 { 0 } else { hz as u16 };
        dtd_w_mm = t[12] as u32 | ((t[14] as u32 & 0xF0) << 4);
        dtd_h_mm = t[13] as u32 | ((t[14] as u32 & 0x0F) << 8);
    }

    let (cm_w, cm_h) = (d[21] as u32 * 10, d[22] as u32 * 10);
    if e.has_native {
        let (nw, nh) = (e.native_width, e.native_height);
        if plausible(nw, dtd_w_mm) && plausible(nh, dtd_h_mm) {
            e.width_mm = dtd_w_mm as u16;
            e.height_mm = dtd_h_mm as u16;
        } else if plausible(nw, cm_w) && plausible(nh, cm_h) {
            e.width_mm = cm_w as u16;
            e.height_mm = cm_h as u16;
        }
    } else if cm_w != 0 && cm_h != 0 {
        e.width_mm = cm_w as u16;
        e.height_mm = cm_h as u16;
    }
    e.valid = true;
    e
}

/// Cùng tỉ lệ khung hình (sai lệch tương đối ≤ 1,5 %).
pub fn same_aspect(aw: u32, ah: u32, bw: u32, bh: u32) -> bool {
    if aw == 0 || ah == 0 || bw == 0 || bh == 0 {
        return false;
    }
    let x = aw as u64 * bh as u64;
    let y = bw as u64 * ah as u64;
    x.abs_diff(y) * 1000 <= 15 * x.max(y)
}

fn aspect_err(aw: u32, ah: u32, bw: u32, bh: u32) -> u64 {
    let x = aw as u64 * bh as u64;
    let y = bw as u64 * ah as u64;
    let mx = x.max(y);
    if mx == 0 {
        return 1_000_000;
    }
    x.abs_diff(y) * 1_000_000 / mx
}

fn key_better(a: &Mode, b: &Mode, native_hz: u16) -> bool {
    if a.area() != b.area() {
        return a.area() > b.area();
    }
    if a.bpp != b.bpp {
        return a.bpp > b.bpp;
    }
    let ha = native_hz != 0 && a.refresh_hz == native_hz;
    let hb = native_hz != 0 && b.refresh_hz == native_hz;
    ha && !hb
}

/// Lý do chọn — giá trị số khớp `yui::display::PickReason`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum PickReason {
    NoModes = 0,
    Native = 1,
    EdidAspect = 2,
    CurrentAspect = 3,
    KeepCurrent = 4,
    ClosestAspect = 5,
    Largest = 6,
}

impl PickReason {
    /// Mã chữ, giống `yui::display::reason_code`.
    pub fn code(self) -> &'static str {
        match self {
            PickReason::NoModes => "no_modes",
            PickReason::Native => "native",
            PickReason::EdidAspect => "edid_aspect",
            PickReason::CurrentAspect => "current_aspect",
            PickReason::KeepCurrent => "keep_current",
            PickReason::ClosestAspect => "closest_aspect",
            PickReason::Largest => "largest",
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Pick {
    /// `None` khi `NoModes`, hoặc `KeepCurrent` mà chế độ đang chạy không
    /// có trong danh sách (giữ nguyên, không đặt lại chế độ).
    pub index: Option<usize>,
    pub mode: Mode,
    pub reason: PickReason,
    /// Tỉ lệ chế độ ≠ tỉ lệ tấm nền ⇒ sẽ bị méo; giao diện phải hỏi.
    pub distorted: bool,
}

fn best_where<F: Fn(&Mode) -> bool>(modes: &[Mode], hz: u16, f: F) -> Option<usize> {
    let mut best: Option<usize> = None;
    for (i, m) in modes.iter().enumerate() {
        if !m.usable() || !f(m) {
            continue;
        }
        match best {
            Some(b) if !key_better(m, &modes[b], hz) => {}
            _ => best = Some(i),
        }
    }
    best
}

/// Cùng quy tắc với `yui::display::pick_mode` (xem chú thích ở tệp C++).
pub fn pick_mode(modes: &[Mode], edid: Option<&Edid>, current: Option<&Mode>) -> Pick {
    let native = edid.filter(|e| e.valid && e.has_native);
    let hz = native.map_or(0, |e| e.native_refresh_hz);
    let mut p = Pick {
        index: None,
        mode: Mode::default(),
        reason: PickReason::NoModes,
        distorted: false,
    };
    if !modes.iter().any(Mode::usable) {
        return p;
    }
    let take = |i: usize, reason: PickReason| Pick {
        index: Some(i),
        mode: modes[i],
        reason,
        distorted: false,
    };

    // 1. Đúng gốc.
    if let Some(e) = native {
        let (nw, nh) = (e.native_width, e.native_height);
        if let Some(i) = best_where(modes, hz, |m| m.width == nw && m.height == nh) {
            return take(i, PickReason::Native);
        }
    }

    // 2. Lớn nhất cùng tỉ lệ (có EDID: không vượt tấm nền nếu còn cách khác).
    let cur = current.filter(|c| c.width > 0 && c.height > 0);
    let reference = match (native, cur) {
        (Some(e), _) => Some((e.native_width, e.native_height)),
        (None, Some(c)) => Some((c.width, c.height)),
        (None, None) => None,
    };
    if let Some((rw, rh)) = reference {
        let limited = native.is_some();
        let fits = |m: &Mode| !limited || (m.width <= rw && m.height <= rh);
        let aspect = |m: &Mode| same_aspect(m.width, m.height, rw, rh);
        let mut best = best_where(modes, hz, |m| aspect(m) && fits(m));
        if best.is_none() && limited {
            for (i, m) in modes.iter().enumerate() {
                if !m.usable() || !aspect(m) {
                    continue;
                }
                let better = match best {
                    None => true,
                    Some(b) => {
                        let o = &modes[b];
                        m.area() < o.area() || (m.area() == o.area() && m.bpp > o.bpp)
                    }
                };
                if better {
                    best = Some(i);
                }
            }
        }
        if let Some(i) = best {
            let reason = if limited {
                PickReason::EdidAspect
            } else {
                PickReason::CurrentAspect
            };
            return take(i, reason);
        }
    }

    // 3. Không có chế độ cùng tỉ lệ.
    if let Some(c) = cur {
        let i = best_where(modes, hz, |m| m.width == c.width && m.height == c.height);
        p.index = i;
        p.mode = i.map_or(*c, |i| modes[i]);
        p.reason = PickReason::KeepCurrent;
        let skew = |e: &Edid| !same_aspect(c.width, c.height, e.native_width, e.native_height);
        p.distorted = native.is_some_and(skew);
        return p;
    }
    if let Some(e) = native {
        let (nw, nh) = (e.native_width, e.native_height);
        let mut best: Option<(usize, u64)> = None;
        for (i, m) in modes.iter().enumerate() {
            if !m.usable() {
                continue;
            }
            let err = aspect_err(m.width, m.height, nw, nh);
            let better = match best {
                None => true,
                Some((b, be)) => err < be || (err == be && key_better(m, &modes[b], hz)),
            };
            if better {
                best = Some((i, err));
            }
        }
        if let Some((i, _)) = best {
            let mut q = take(i, PickReason::ClosestAspect);
            q.distorted = true;
            return q;
        }
    }
    match best_where(modes, 0, |_| true) {
        Some(i) => take(i, PickReason::Largest),
        None => p,
    }
}

/// Đầu vào chọn hệ số phóng.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct ScaleInput {
    pub width_px: u32,
    pub height_px: u32,
    pub width_mm: u16,
    pub height_mm: u16,
    /// DPI bên gọi biết; 0 = không.
    pub dpi_hint: u32,
    /// 1–4; 0 = tự chọn.
    pub user_override: u8,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Scale {
    pub k: u8,
    pub dpi_x10: u32,
    pub dpi_measured: bool,
    pub dpi_guessed: bool,
    pub k_dpi: u8,
    pub k_fit: u8,
    pub cramped: bool,
    pub columns: u32,
    pub rows: u32,
}

fn clamp_k(k: u64) -> u8 {
    k.clamp(1, 4) as u8
}

/// Cùng quy tắc với `yui::display::choose_scale`: k ≈ DPI/96 làm tròn (ranh
/// giới 144/240/336 dpi), không phóng tới mức dưới 80 cột × 25 dòng.
pub fn choose_scale(input: &ScaleInput) -> Scale {
    let (w, h) = (input.width_px, input.height_px);
    let mut dpi_x10 = 960u64;
    let mut measured = false;
    if input.width_mm > 0 && input.height_mm > 0 && w > 0 && h > 0 {
        let dx = w as u64 * 254 / input.width_mm as u64;
        let dy = h as u64 * 254 / input.height_mm as u64;
        let avg = (dx + dy) / 2;
        if (DPI_X10_MIN..=DPI_X10_MAX).contains(&avg) {
            dpi_x10 = avg;
            measured = true;
        }
    }
    let mut guessed = false;
    if !measured {
        if (40..=700).contains(&input.dpi_hint) {
            dpi_x10 = input.dpi_hint as u64 * 10;
        } else {
            guessed = true;
            dpi_x10 = if h >= 2160 { 1920 } else { 960 };
        }
    }
    let k_dpi = clamp_k((dpi_x10 + 480) / 960);
    let k_fit = clamp_k((w / 480).min(h / 300) as u64);
    let (k, cramped) = if input.user_override >= 1 {
        let k = clamp_k(input.user_override as u64);
        (k, k > k_fit)
    } else {
        (k_dpi.min(k_fit), false)
    };
    Scale {
        k,
        dpi_x10: dpi_x10 as u32,
        dpi_measured: measured,
        dpi_guessed: guessed,
        k_dpi,
        k_fit,
        cramped,
        columns: w / (6 * k as u32),
        rows: h / (12 * k as u32),
    }
}

/// Khung nội dung khi phóng nguyên `src` vào `dst`, căn giữa, viền đen.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Letterbox {
    pub k: u32,
    pub x: u32,
    pub y: u32,
    pub w: u32,
    pub h: u32,
}

pub fn letterbox(src_w: u32, src_h: u32, dst_w: u32, dst_h: u32) -> Letterbox {
    if src_w == 0 || src_h == 0 || dst_w < src_w || dst_h < src_h {
        return Letterbox::default();
    }
    let k = (dst_w / src_w).min(dst_h / src_h);
    let (w, h) = (src_w * k, src_h * k);
    Letterbox {
        k,
        x: (dst_w - w) / 2,
        y: (dst_h - h) / 2,
        w,
        h,
    }
}

/// Ngân sách khung hình — tính riêng khỏi ngân sách < 64 MB của P2.11.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct FrameBudget {
    pub frame_bytes: u64,
    pub double_buffer: bool,
    pub dirty_rects: bool,
}

pub fn frame_budget(width: u32, height: u32) -> FrameBudget {
    let frame_bytes = width as u64 * height as u64 * 4;
    FrameBudget {
        frame_bytes,
        double_buffer: frame_bytes * 2 <= 16 << 20,
        dirty_rects: frame_bytes > 8 << 20,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const VECTORS: &str = include_str!("../../tests/data/display_vectors.txt");

    fn unhex(s: &str) -> Vec<u8> {
        if s == "-" {
            return Vec::new();
        }
        (0..s.len() / 2)
            .map(|i| u8::from_str_radix(&s[2 * i..2 * i + 2], 16).unwrap())
            .collect()
    }

    fn mode(s: &str) -> Mode {
        let (body, hz) = match s.split_once('@') {
            Some((b, h)) => (b, h.parse().unwrap()),
            None => (s, 0),
        };
        let p: Vec<u32> = body.split('x').map(|x| x.parse().unwrap()).collect();
        let bpp = if p.len() > 2 { p[2] as u8 } else { 32 };
        Mode::new(p[0], p[1], bpp, hz)
    }

    fn wh(w: u32, h: u32) -> String {
        format!("{w}x{h}")
    }

    #[test]
    fn shared_vectors_match_cpp_and_python() {
        let mut cases = 0;
        for line in VECTORS.lines() {
            if line.is_empty() || line.starts_with('#') {
                continue;
            }
            let c: Vec<&str> = line.split('|').collect();
            assert_eq!(c.len(), 17, "{line}");
            let raw = unhex(c[1]);
            let modes: Vec<Mode> = match c[2] {
                "-" => Vec::new(),
                s => s.split(',').map(mode).collect(),
            };
            let cur = if c[3] == "-" { None } else { Some(mode(c[3])) };
            let e = parse_edid(&raw);
            let name = c[0];
            assert_eq!(if e.valid { "1" } else { "0" }, c[6], "{name}: valid");
            let native = if e.has_native {
                wh(e.native_width, e.native_height)
            } else {
                String::from("-")
            };
            assert_eq!(native, c[7], "{name}: native");
            let mm = wh(e.width_mm as u32, e.height_mm as u32);
            assert_eq!(mm, c[8], "{name}: mm");
            let edid = e.valid.then_some(&e);
            let p = pick_mode(&modes, edid, cur.as_ref());
            let index = p.index.map_or(-1, |i| i as i64);
            assert_eq!(index.to_string(), c[9], "{name}: index");
            assert_eq!(p.reason.code(), c[10], "{name}: reason");
            assert_eq!(if p.distorted { "1" } else { "0" }, c[11], "{name}: distorted");
            let m = if p.reason == PickReason::NoModes {
                cur.unwrap_or_default()
            } else {
                p.mode
            };
            let input = ScaleInput {
                width_px: m.width,
                height_px: m.height,
                width_mm: if e.valid { e.width_mm } else { 0 },
                height_mm: if e.valid { e.height_mm } else { 0 },
                dpi_hint: c[5].parse().unwrap(),
                user_override: c[4].parse().unwrap(),
            };
            let s = choose_scale(&input);
            assert_eq!(s.k.to_string(), c[12], "{name}: k");
            assert_eq!(s.dpi_x10.to_string(), c[13], "{name}: dpi");
            assert_eq!(wh(m.width, m.height), c[14], "{name}: mode");
            assert_eq!(s.columns.to_string(), c[15], "{name}: cols");
            assert_eq!(s.rows.to_string(), c[16], "{name}: rows");
            cases += 1;
        }
        assert!(cases >= 12, "DoD P2.16: ít nhất 12 bảng chế độ/EDID");
    }

    #[test]
    fn same_width_different_size_gives_different_k() {
        let laptop = ScaleInput {
            width_px: 1920,
            height_px: 1080,
            width_mm: 309,
            height_mm: 174,
            ..ScaleInput::default()
        };
        let desk = ScaleInput {
            width_mm: 531,
            height_mm: 299,
            ..laptop
        };
        assert_eq!(choose_scale(&laptop).k, 2);
        assert_eq!(choose_scale(&desk).k, 1);
        let forced = ScaleInput {
            user_override: 4,
            ..desk
        };
        assert!(choose_scale(&forced).cramped);
    }

    #[test]
    fn letterbox_and_budget() {
        let lb = letterbox(640, 480, 1920, 1080);
        assert_eq!((lb.k, lb.x, lb.y, lb.w, lb.h), (2, 320, 60, 1280, 960));
        assert_eq!(letterbox(1920, 1080, 1280, 720).k, 0);
        let b = frame_budget(3840, 2160);
        assert_eq!(b.frame_bytes, 33_177_600);
        assert!(b.dirty_rects && !b.double_buffer);
        assert!(frame_budget(1920, 1080).double_buffer);
        assert!(same_aspect(1366, 768, 1920, 1080));
        assert!(!same_aspect(1920, 1200, 1920, 1080));
    }

    #[test]
    fn corrupt_edid_is_rejected_bit_by_bit() {
        let line = VECTORS
            .lines()
            .find(|l| l.starts_with("laptop14_1080p|"))
            .unwrap();
        let raw = unhex(line.split('|').nth(1).unwrap());
        let e = parse_edid(&raw);
        assert!(e.valid);
        assert_eq!(&e.vendor, b"YUI");
        assert_eq!(e.native_refresh_hz, 60);
        for i in 0..128 {
            let mut x = raw.clone();
            x[i] ^= 0x10;
            assert!(!parse_edid(&x).valid, "byte {i}");
        }
        assert!(!parse_edid(&raw[..127]).valid);
    }
}
