// Linear framebuffer rasteriser (Tầng 5 / Tầng 2 display).
//
// The same 32-bit linear surface the kernel hands to user space on weak
// machines: no GPU, no compositor, just pixels. The boot splash and the
// Direct-Framebuffer mode both draw through this file, which is why it has no
// dependency other than <cstdint>.
//
// Pixels are 0xAARRGGBB stored little-endian (the format of both the QEMU
// `-display gtk` framebuffer and a typical UEFI GOP mode), so writing a 32-bit
// word per pixel produces B,G,R,X in memory — exactly what the hardware reads.
#ifndef YUI_FB_HPP
#define YUI_FB_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "yui/engine.hpp"
#include "yui/profile.hpp"

namespace yui::fb {

struct Surface {
  int width = 0;
  int height = 0;
  uint32_t* pixels = nullptr;  // width*height pixels, row-major, stride == width
};

struct Rect {
  int x = 0;
  int y = 0;
  int w = 0;
  int h = 0;
};

/// True when the surface can be drawn to (non-null and positive size).
bool valid(const Surface& s);

void clear(const Surface& s, uint32_t argb);
void fill_rect(const Surface& s, Rect r, uint32_t argb);
/// Clipped single pixel; out-of-bounds coordinates are ignored, never written.
void put_pixel(const Surface& s, int x, int y, uint32_t argb);

/// Width in pixels of `text` when drawn at `scale` (1 = 5x7 font).
int text_width(const std::string& text, int scale);
/// Height in pixels of a line at `scale`.
int text_height(int scale);
/// Draws `text` with the built-in 5x7 font; unknown characters render as '?'.
/// Non-ASCII bytes are dropped (the splashed text is transliterated first).
void draw_text(const Surface& s, int x, int y, const std::string& text, uint32_t argb, int scale);

/// Renders the boot splash: product name, machine class, and the tuned k/W.
/// Returns the number of pixels drawn (0 if the surface is invalid/too small).
/// `scale_k` = hệ số HiDPI nguyên 1–4; 0 = tự chọn theo độ phân giải (P2.16).
long render_splash(const Surface& s, const profile::HostProfile& host, int scale_k = 0);

/// Encodes the surface as a binary PPM (P6), handy for screenshots in CI.
std::string to_ppm(const Surface& s);

// Đưa một mặt vẽ logic `src` lên màn hình thật `dst` bằng hệ số NGUYÊN lớn
// nhất vừa khung (mỗi điểm ảnh nguồn thành đúng một khối k×k — không nội suy,
// không kéo giãn sang tỉ lệ khác), căn giữa, phần thừa tô đen (P2.16).
// Trả về k; 0 nếu `src` lớn hơn `dst` (khi đó không vẽ gì).
int present_scaled(const Surface& dst, const Surface& src);

/// ASCII-only uppercase copy of a Vietnamese string (the 5x7 font has no
/// diacritics; documents and the shell use the real thing).
std::string ascii_upper(const std::string& utf8);

// ------------------------------------------------------- Vietnamese UTF-8 --
// Chữ Việt có dấu trên framebuffer — "ngôn ngữ hiển thị", P2.10.
//
// Ô chữ mở rộng: glyph 5×7 gốc + dải dấu 3 hàng phía trên + 1 hàng dấu nặng
// phía dưới (tổng 11 hàng mỗi dòng ở scale = 1). Mỗi chữ cái tiếng Việt tiền
// tổ hợp được tách thành chữ gốc Latin + mặt nạ dấu (NFD) — xem bảng
// kViet trong fb.cpp, sinh tự động từ Unicode nên không sót chữ nào.
// Chuỗi ASCII vẽ giống hệt draw_text (cùng glyph, cùng bước 6*scale).

/// Số điểm ảnh của một dòng chữ Việt ở `scale` (7 chữ + 3 dấu + 1 dấu nặng).
int text_height_utf8(int scale);
/// Bề rộng pixel của `utf8` ở `scale` (cùng công thức với text_width).
int text_width_utf8(const std::string& utf8, int scale);
/// Giải mã UTF-8 → codepoint; byte lỗi trả U+FFFD (thử nghiệm dùng).
std::vector<uint32_t> decode_utf8(const std::string& utf8);
/// Vẽ chuỗi UTF-8 (tiếng Việt đầy đủ dấu + ASCII). `y` là ĐỈNH CHỮ CÁI —
/// đúng tọa độ của draw_text, nên chuỗi ASCII thuần vẽ GIỐNG HỆT draw_text
/// (cùng glyph, cùng vị trí). Dải dấu vẽ phía trên `y` (3×scale px) và dấu
/// nặng phía dưới (1×scale px) — người gọi chừa chỗ theo text_height_utf8.
/// Chữ không có trong bảng vẽ thành '?' — không bao giờ bịa glyph.
void draw_text_utf8(const Surface& s, int x, int y, const std::string& utf8, uint32_t argb,
                    int scale);

}  // namespace yui::fb

#endif  // YUI_FB_HPP
