//! Màn hình chào trên framebuffer tuyến tính — bản bare-metal.
//!
//! Cùng bố cục với `userspace-vi/fb.cpp` (`render_splash`): dải đỏ trên cùng,
//! chữ "YUIVIEOS BETA", phụ đề, bảng thông số máy, dòng chân trang. Ở đây chỉ
//! có font 5x7 tối giản vì không có bộ cấp phát ảnh.
//!
//! P2.16: mọi kích thước (cỡ chữ, khoảng cách, bảng) nhân với hệ số HiDPI
//! nguyên `k` do `yui_cap::display::choose_scale` chọn — đúng hàm mà bản C++
//! và bộ vector chung kiểm. Chữ chỉ được phóng bằng khối k×k (không nội suy).
//! Firmware không cho kích thước vật lý ở đây (EDID đến sau, cùng GOP — P6.4),
//! nên DPI là ĐOÁN theo độ phân giải: 4K ⇒ k = 2, còn lại k = 1.
//!
//! Toàn bộ `unsafe` của tệp này nằm ở hàm `put`: nó ghi vào vùng nhớ do
//! firmware cấp, đúng loại việc mà khung phần cứng phải làm.

use alloc::string::String;
use core::fmt::Write;

use yui_cap::display;

use crate::boot::{BootInfo, Framebuffer};
use crate::profiler;

const BG: u32 = 0xFF0B_1020;
const INK: u32 = 0xFFF5_F7FF;
const ACCENT: u32 = 0xFFDA_251D;
const SOFT: u32 = 0xFF8A_94B8;
const PANEL: u32 = 0xFF15_1B33;

/// Kết quả vẽ: số pixel và hệ số k đã dùng.
pub struct Drawn {
    pub pixels: u64,
    pub k: u8,
}

fn surface(fb: &Framebuffer) -> Option<Surface> {
    if fb.addr == 0 || fb.width < 120 || fb.height < 80 || fb.bpp < 24 {
        return None;
    }
    let bytes_per_pixel = (fb.bpp as u32) / 8;
    let stride = if fb.pitch >= fb.width * bytes_per_pixel {
        fb.pitch
    } else {
        fb.width * bytes_per_pixel
    };
    Some(Surface {
        base: fb.addr as *mut u8,
        width: fb.width as i32,
        height: fb.height as i32,
        stride,
        bytes_per_pixel,
        shifts: (fb.red_shift, fb.green_shift, fb.blue_shift),
    })
}

/// Đo ghi khung hình (P2.16): tô nền các dòng đầu sao cho ≤ `max_bytes`, bằng
/// đúng đường `put` mà màn chào dùng. Trả (số byte đã ghi, số chu kỳ) —
/// `fbspeed.rs` đổi ra MB/s. Không thêm `unsafe`: đi qua `Surface::put`.
pub fn fill_bench(fb: &Framebuffer, max_bytes: u64) -> Option<(u64, u64)> {
    let surface = surface(fb)?;
    let row_bytes = surface.width as u64 * surface.bytes_per_pixel as u64;
    let rows = (max_bytes / row_bytes.max(1)).clamp(1, surface.height as u64) as i32;
    let t0 = crate::arch::cycles();
    surface.fill(0, 0, surface.width, rows, BG);
    let t1 = crate::arch::cycles();
    Some((row_bytes * rows as u64, t1.wrapping_sub(t0)))
}

/// Trả về None nếu framebuffer không dùng được.
///
/// `speed` = (MB/s trước, MB/s sau WC nếu bật được, chạy trong máy ảo) từ
/// `fbspeed::measure`: in thành dòng thứ ba của bảng thông số để người thử trên
/// **máy thật không có cổng nối tiếp** vẫn đọc được số đo (P2.16).
pub fn render(fb: &Framebuffer, info: &BootInfo, speed: Option<(u64, Option<u64>, bool)>) -> Option<Drawn> {
    let surface = surface(fb)?;
    let (w, h) = (surface.width, surface.height);

    let scale = display::choose_scale(&display::ScaleInput {
        width_px: fb.width,
        height_px: fb.height,
        ..display::ScaleInput::default()
    });
    let k = scale.k as i32;

    // Nền trước, dải đỏ sau (thứ tự ngược lại thì dải bị nền đè mất).
    surface.fill(0, 0, w, h, BG);
    surface.fill(0, 0, w, h / 30 + 2, ACCENT);

    let title = "YUIVIEOS BETA";
    let mut title_scale = (if w >= 640 { 4 } else if w >= 400 { 3 } else { 2 }) * k;
    while title_scale > 1 && text_width(title, title_scale) > w - 16 {
        title_scale -= 1;
    }
    surface.centered(title, title_scale, h / 3, INK);
    let sub_scale = if title_scale > k { title_scale - k } else { 1 };
    surface.centered("HE DIEU HANH VIET NAM", sub_scale, h / 3 + 7 * title_scale + 6 * k, SOFT);

    let c = profiler::classify(info.ram_mb, info.harts);
    // Chuỗi ghép bằng push_str, chỉ số nguyên đi qua `core::fmt`: định dạng
    // biến `&str` bằng `{}` treo nhân aarch64 (xem console.rs).
    let mut line1 = String::new();
    line1.push_str(c.tier);
    let _ = write!(line1, "  {} HARTS  {} MB", info.harts, info.ram_mb);
    let mut line2 = String::new();
    let _ = write!(line2, "K={}  W={}  ", c.k, c.window);
    line2.push_str(info.simd);
    let mut line3 = String::new();
    if let Some((before, after, in_vm)) = speed {
        let _ = write!(line3, "GHI KHUNG {} MB/S", before);
        if let Some(a) = after {
            let _ = write!(line3, "  WC {} MB/S", a);
        }
        if in_vm {
            line3.push_str("  MAY AO");
        }
    }
    let lines = if line3.is_empty() { 2 } else { 3 };
    let panel_x = w / 20;
    let panel_w = w - 2 * panel_x;
    let inner = panel_w - 22 * k;
    let mut panel_scale = 2 * k;
    while panel_scale > 1
        && (text_width(&line1, panel_scale) > inner
            || text_width(&line2, panel_scale) > inner
            || text_width(&line3, panel_scale) > inner)
    {
        panel_scale -= 1;
    }
    let line_gap = 6 * k;
    let panel_h = lines * 7 * panel_scale + (lines - 1) * line_gap + 16 * k;
    let panel_y = h - panel_h - 7 * k - 24 * k;
    surface.fill(panel_x, panel_y, panel_w, panel_h, PANEL);
    surface.fill(panel_x, panel_y, 4 * k, panel_h, ACCENT);
    let (tx, ty) = (panel_x + 14 * k, panel_y + 8 * k);
    surface.text(tx, ty, &line1, INK, panel_scale);
    surface.text(tx, ty + 7 * panel_scale + line_gap, &line2, SOFT, panel_scale);
    if lines == 3 {
        surface.text(tx, ty + 2 * (7 * panel_scale + line_gap), &line3, SOFT, panel_scale);
    }

    surface.centered("CLEAN-ROOM  -  SAFE RUST KERNEL", k, h - 7 * k - 8 * k, SOFT);

    Some(Drawn {
        pixels: w as u64 * h as u64,
        k: scale.k,
    })
}

struct Surface {
    base: *mut u8,
    width: i32,
    height: i32,
    stride: u32,
    bytes_per_pixel: u32,
    shifts: (u8, u8, u8),
}

impl Surface {
    fn put(&self, x: i32, y: i32, argb: u32) {
        if x < 0 || y < 0 || x >= self.width || y >= self.height {
            return;
        }
        let offset = y as u32 * self.stride + x as u32 * self.bytes_per_pixel;
        // ARGB của bố cục → bố cục kênh màu firmware báo (thường trùng XRGB).
        let (rs, gs, bs) = self.shifts;
        let native = (((argb >> 16) & 0xFF) << rs) | (((argb >> 8) & 0xFF) << gs) | ((argb & 0xFF) << bs);
        // An toàn: offset đã bị chặn trong (pitch × height) do kiểm tra ở trên,
        // và vùng nhớ thuộc framebuffer firmware cấp cho nhân (boot.rs đã kiểm
        // nó nằm trong vùng ánh xạ đồng nhất).
        unsafe {
            let pixel = self.base.add(offset as usize);
            if self.bytes_per_pixel >= 4 {
                core::ptr::write_volatile(pixel as *mut u32, native);
            } else {
                core::ptr::write_volatile(pixel, (native & 0xFF) as u8);
                core::ptr::write_volatile(pixel.add(1), ((native >> 8) & 0xFF) as u8);
                core::ptr::write_volatile(pixel.add(2), ((native >> 16) & 0xFF) as u8);
            }
        }
    }

    fn centered(&self, text: &str, scale: i32, y: i32, argb: u32) {
        let x = (self.width - text_width(text, scale)) / 2;
        self.text(x.max(0), y, text, argb, scale);
    }

    fn fill(&self, x: i32, y: i32, w: i32, h: i32, argb: u32) {
        let mut yy = y.max(0);
        while yy < (y + h).min(self.height) {
            let mut xx = x.max(0);
            while xx < (x + w).min(self.width) {
                self.put(xx, yy, argb);
                xx += 1;
            }
            yy += 1;
        }
    }

    fn text(&self, x: i32, y: i32, text: &str, argb: u32, scale: i32) {
        let mut cx = x;
        for ch in text.bytes() {
            let glyph = glyph_for(ch).or_else(|| glyph_for(b'?'));
            if let Some(rows) = glyph {
                let mut row = 0;
                while row < 7 {
                    let bits = rows[row];
                    let mut col = 0;
                    while col < 5 {
                        if bits & (1 << (4 - col)) != 0 {
                            self.fill(cx + col as i32 * scale, y + row as i32 * scale, scale, scale, argb);
                        }
                        col += 1;
                    }
                    row += 1;
                }
            }
            cx += 6 * scale;
        }
    }
}

fn text_width(text: &str, scale: i32) -> i32 {
    let chars = text.bytes().count() as i32;
    if chars == 0 {
        0
    } else {
        chars * 6 * scale - scale
    }
}

fn glyph_for(c: u8) -> Option<&'static [u8; 7]> {
    let up = if c.is_ascii_lowercase() { c - 32 } else { c };
    Some(match up {
        b' ' => &FONT_SPACE,
        b'A' => &FONT_A,
        b'B' => &FONT_B,
        b'C' => &FONT_C,
        b'D' => &FONT_D,
        b'E' => &FONT_E,
        b'F' => &FONT_F,
        b'G' => &FONT_G,
        b'H' => &FONT_H,
        b'I' => &FONT_I,
        b'J' => &FONT_J,
        b'K' => &FONT_K,
        b'L' => &FONT_L,
        b'M' => &FONT_M,
        b'N' => &FONT_N,
        b'O' => &FONT_O,
        b'P' => &FONT_P,
        b'Q' => &FONT_Q,
        b'R' => &FONT_R,
        b'S' => &FONT_S,
        b'T' => &FONT_T,
        b'U' => &FONT_U,
        b'V' => &FONT_V,
        b'W' => &FONT_W,
        b'X' => &FONT_X,
        b'Y' => &FONT_Y,
        b'Z' => &FONT_Z,
        b'0' => &FONT_0,
        b'1' => &FONT_1,
        b'2' => &FONT_2,
        b'3' => &FONT_3,
        b'4' => &FONT_4,
        b'5' => &FONT_5,
        b'6' => &FONT_6,
        b'7' => &FONT_7,
        b'8' => &FONT_8,
        b'9' => &FONT_9,
        b'-' => &FONT_DASH,
        b'.' => &FONT_DOT,
        b'=' => &FONT_EQ,
        b'?' => &FONT_QUESTION,
        b'/' => &FONT_SLASH,
        _ => return None,
    })
}

const FONT_SPACE: [u8; 7] = [0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00];
const FONT_A: [u8; 7] = [0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11];
const FONT_B: [u8; 7] = [0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E];
const FONT_C: [u8; 7] = [0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E];
const FONT_D: [u8; 7] = [0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E];
const FONT_E: [u8; 7] = [0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F];
const FONT_F: [u8; 7] = [0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10];
const FONT_G: [u8; 7] = [0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F];
const FONT_H: [u8; 7] = [0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11];
const FONT_I: [u8; 7] = [0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E];
const FONT_J: [u8; 7] = [0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C];
const FONT_K: [u8; 7] = [0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11];
const FONT_L: [u8; 7] = [0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F];
const FONT_M: [u8; 7] = [0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11];
const FONT_N: [u8; 7] = [0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11];
const FONT_O: [u8; 7] = [0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E];
const FONT_P: [u8; 7] = [0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10];
const FONT_Q: [u8; 7] = [0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D];
const FONT_R: [u8; 7] = [0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11];
const FONT_S: [u8; 7] = [0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E];
const FONT_T: [u8; 7] = [0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04];
const FONT_U: [u8; 7] = [0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E];
const FONT_V: [u8; 7] = [0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04];
const FONT_W: [u8; 7] = [0x11, 0x11, 0x11, 0x15, 0x15, 0x1B, 0x11];
const FONT_X: [u8; 7] = [0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11];
const FONT_Y: [u8; 7] = [0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04];
const FONT_Z: [u8; 7] = [0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F];
const FONT_0: [u8; 7] = [0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E];
const FONT_1: [u8; 7] = [0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E];
const FONT_2: [u8; 7] = [0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F];
const FONT_3: [u8; 7] = [0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E];
const FONT_4: [u8; 7] = [0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02];
const FONT_5: [u8; 7] = [0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E];
const FONT_6: [u8; 7] = [0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E];
const FONT_7: [u8; 7] = [0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08];
const FONT_8: [u8; 7] = [0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E];
const FONT_9: [u8; 7] = [0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C];
const FONT_DASH: [u8; 7] = [0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00];
const FONT_DOT: [u8; 7] = [0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C];
const FONT_EQ: [u8; 7] = [0x00, 0x00, 0x1F, 0x00, 0x1F, 0x00, 0x00];
const FONT_QUESTION: [u8; 7] = [0x0E, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04];
const FONT_SLASH: [u8; 7] = [0x01, 0x01, 0x02, 0x04, 0x08, 0x10, 0x10];
