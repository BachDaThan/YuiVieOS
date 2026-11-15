//! Màn hình chào trên framebuffer tuyến tính — bản bare-metal.
//!
//! Cùng bố cục với `userspace-vi/fb.cpp` (đã kiểm thử 1155 check): dải đỏ trên
//! cùng, chữ "YUIVIEOS BETA", bảng thông số máy, dòng chân trang. Ở đây chỉ có
//! 32-bit màu và font 5x7 tối giản vì không có bộ cấp phát ảnh.
//!
//! Toàn bộ `unsafe` của tệp này nằm ở hai hàm `put`/`fill`: chúng ghi vào vùng
//! nhớ do firmware cấp, đúng loại việc mà khung phần cứng phải làm.

use alloc::format;

use crate::boot::{BootInfo, Framebuffer};
use crate::profiler;

const BG: u32 = 0xFF0B_1020;
const INK: u32 = 0xFFF5_F7FF;
const ACCENT: u32 = 0xFFDA_251D;
const SOFT: u32 = 0xFF8A_94B8;
const PANEL: u32 = 0xFF15_1B33;

/// Trả về số pixel đã vẽ, hoặc None nếu framebuffer không dùng được.
pub fn render(fb: &Framebuffer, info: &BootInfo) -> Option<u64> {
    if fb.addr == 0 || fb.width < 120 || fb.height < 80 || fb.bpp < 24 {
        return None;
    }
    let bytes_per_pixel = (fb.bpp as u32) / 8;
    let stride = if fb.pitch >= fb.width * bytes_per_pixel {
        fb.pitch
    } else {
        fb.width * bytes_per_pixel
    };

    let surface = Surface {
        base: fb.addr as *mut u8,
        width: fb.width as i32,
        height: fb.height as i32,
        stride,
        bytes_per_pixel,
    };

    surface.fill(0, 0, surface.width, surface.height / 30 + 2, ACCENT);
    surface.fill(0, 0, surface.width, surface.height, BG);

    let title_scale = if surface.width >= 640 { 4 } else { 3 };
    let title = "YUIVIEOS BETA";
    let tw = text_width(title, title_scale);
    surface.text((surface.width - tw) / 2, surface.height / 3, title, INK, title_scale);

    let sub = "HE DIEU HANH VIET NAM";
    let sw = text_width(sub, 2);
    surface.text((surface.width - sw) / 2, surface.height / 3 + 7 * title_scale + 8, sub, SOFT, 2);

    let c = profiler::classify(info.ram_mb, info.harts);
    let panel_w = surface.width - surface.width / 10;
    let panel_x = surface.width / 20;
    let panel_y = surface.height - 64;
    surface.fill(panel_x, panel_y, panel_w, 48, PANEL);
    surface.fill(panel_x, panel_y, 4, 48, ACCENT);

    let line1 = format!("{}  {} HARTS  {} MB", c.tier, info.harts, info.ram_mb);
    let line2 = format!("K={}  W={}  {}", c.k, c.window, info.simd);
    surface.text(panel_x + 14, panel_y + 8, &line1, INK, 1);
    surface.text(panel_x + 14, panel_y + 24, &line2, SOFT, 1);

    Some(surface.width as u64 * surface.height as u64)
}

struct Surface {
    base: *mut u8,
    width: i32,
    height: i32,
    stride: u32,
    bytes_per_pixel: u32,
}

impl Surface {
    fn put(&self, x: i32, y: i32, argb: u32) {
        if x < 0 || y < 0 || x >= self.width || y >= self.height {
            return;
        }
        let offset = y as u32 * self.stride + x as u32 * self.bytes_per_pixel;
        // An toàn: offset đã bị chặn trong (pitch × height) do kiểm tra ở trên,
        // và vùng nhớ thuộc framebuffer firmware cấp cho nhân.
        unsafe {
            let pixel = self.base.add(offset as usize);
            if self.bytes_per_pixel >= 4 {
                core::ptr::write_volatile(pixel as *mut u32, argb);
            } else {
                core::ptr::write_volatile(pixel, (argb & 0xFF) as u8);
                core::ptr::write_volatile(pixel.add(1), ((argb >> 8) & 0xFF) as u8);
                core::ptr::write_volatile(pixel.add(2), ((argb >> 16) & 0xFF) as u8);
            }
        }
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
