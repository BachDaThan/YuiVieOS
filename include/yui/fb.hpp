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
long render_splash(const Surface& s, const profile::HostProfile& host);

/// Encodes the surface as a binary PPM (P6), handy for screenshots in CI.
std::string to_ppm(const Surface& s);

/// ASCII-only uppercase copy of a Vietnamese string (the 5x7 font has no
/// diacritics; documents and the shell use the real thing).
std::string ascii_upper(const std::string& utf8);

}  // namespace yui::fb

#endif  // YUI_FB_HPP
