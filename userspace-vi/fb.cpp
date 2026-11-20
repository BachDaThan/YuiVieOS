// Linear framebuffer rasteriser — see include/yui/fb.hpp.
#include "yui/fb.hpp"
#include "yui/display.hpp"

#include <cstdio>
#include <cstring>


namespace yui::fb {
namespace {

// 5x7 bitmap font, one byte per row, bit 4 = leftmost column.
// Only the glyphs the boot splash needs: A-Z, 0-9 and a little punctuation.
// Anything else renders as '?' on purpose — we never invent a glyph.
struct Glyph {
  char ch;
  uint8_t rows[7];
};

constexpr Glyph kFont[] = {
    {' ', {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
    {'A', {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'B', {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}},
    {'C', {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}},
    {'D', {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E}},
    {'E', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}},
    {'F', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}},
    {'G', {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F}},
    {'H', {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'I', {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'J', {0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C}},
    {'K', {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}},
    {'L', {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}},
    {'M', {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}},
    {'N', {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11}},
    {'O', {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
    {'P', {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}},
    {'Q', {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D}},
    {'R', {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}},
    {'S', {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}},
    {'T', {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}},
    {'U', {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
    {'V', {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04}},
    {'W', {0x11, 0x11, 0x11, 0x15, 0x15, 0x1B, 0x11}},
    {'X', {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11}},
    {'Y', {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04}},
    {'Z', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F}},
    {'0', {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}},
    {'1', {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'2', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}},
    {'3', {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E}},
    {'4', {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}},
    {'5', {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}},
    {'6', {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}},
    {'7', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}},
    {'8', {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}},
    {'9', {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}},
    {'.', {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C}},
    {',', {0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C, 0x08}},
    {':', {0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00}},
    {'-', {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00}},
    {'/', {0x01, 0x02, 0x02, 0x04, 0x08, 0x08, 0x10}},
    {'%', {0x19, 0x1A, 0x02, 0x04, 0x08, 0x0B, 0x13}},
    {'=', {0x00, 0x00, 0x1F, 0x00, 0x1F, 0x00, 0x00}},
    {'+', {0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00}},
    {'!', {0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x04}},
    {'(', {0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02}},
    {')', {0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08}},
    {'?', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04}},
    {'_', {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1F}},
};

const Glyph* glyph_for(char c) {
  if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
  for (const auto& g : kFont) {
    if (g.ch == c) return &g;
  }
  return nullptr;
}

// Colours (0xAARRGGBB).
constexpr uint32_t kBg = 0xFF0B1020;
constexpr uint32_t kInk = 0xFFF5F7FF;
constexpr uint32_t kAccent = 0xFFDA251D;  // Vietnamese red
constexpr uint32_t kSoft = 0xFF8A94B8;
constexpr uint32_t kPanel = 0xFF151B33;

// One lotus petal, drawn from rectangles so the splash never needs a bitmap.
void petal(const Surface& s, int cx, int cy, int size, uint32_t argb) {
  for (int i = 0; i < size; ++i) {
    const int half = size - i;
    fill_rect(s, Rect{cx - half, cy + size - i, half * 2 + 1, 1}, argb);
  }
}

void centered(const Surface& s, const std::string& text, int scale, int y, uint32_t argb) {
  const int w = text_width(text, scale);
  int x = (s.width - w) / 2;
  if (x < 0) x = 0;
  draw_text(s, x, y, text, argb, scale);
}

}  // namespace

bool valid(const Surface& s) { return s.pixels != nullptr && s.width > 0 && s.height > 0; }

void put_pixel(const Surface& s, int x, int y, uint32_t argb) {
  if (!valid(s)) return;
  if (x < 0 || y < 0 || x >= s.width || y >= s.height) return;
  s.pixels[y * s.width + x] = argb;
}

void clear(const Surface& s, uint32_t argb) {
  if (!valid(s)) return;
  const long n = long(s.width) * long(s.height);
  for (long i = 0; i < n; ++i) s.pixels[i] = argb;
}

int present_scaled(const Surface& dst, const Surface& src) {
  if (!valid(dst) || !valid(src)) return 0;
  const auto lb = display::letterbox(uint32_t(src.width), uint32_t(src.height),
                                     uint32_t(dst.width), uint32_t(dst.height));
  if (lb.k == 0) return 0;
  const int k = int(lb.k), ox = int(lb.x), oy = int(lb.y);
  const int ow = int(lb.w), oh = int(lb.h);
  for (int y = 0; y < dst.height; ++y) {
    uint32_t* row = dst.pixels + long(y) * dst.width;
    if (y < oy || y >= oy + oh) {
      for (int x = 0; x < dst.width; ++x) row[x] = 0xFF000000u;
      continue;
    }
    const uint32_t* srow = src.pixels + long((y - oy) / k) * src.width;
    for (int x = 0; x < ox; ++x) row[x] = 0xFF000000u;
    for (int x = 0; x < ow; ++x) row[ox + x] = srow[x / k];
    for (int x = ox + ow; x < dst.width; ++x) row[x] = 0xFF000000u;
  }
  return k;
}

void fill_rect(const Surface& s, Rect r, uint32_t argb) {
  if (!valid(s) || r.w <= 0 || r.h <= 0) return;
  int x0 = r.x < 0 ? 0 : r.x;
  int y0 = r.y < 0 ? 0 : r.y;
  int x1 = r.x + r.w;
  int y1 = r.y + r.h;
  if (x1 > s.width) x1 = s.width;
  if (y1 > s.height) y1 = s.height;
  for (int y = y0; y < y1; ++y) {
    uint32_t* row = s.pixels + long(y) * s.width;
    for (int x = x0; x < x1; ++x) row[x] = argb;
  }
}

int text_height(int scale) { return scale < 1 ? 7 : 7 * scale; }

int text_width(const std::string& text, int scale) {
  if (scale < 1) scale = 1;
  int chars = 0;
  for (char c : text) {
    if (c == '\n') break;
    ++chars;
  }
  if (chars == 0) return 0;
  return chars * 6 * scale - scale;  // 5 columns + 1 gap, minus the trailing gap
}

void draw_text(const Surface& s, int x, int y, const std::string& text, uint32_t argb, int scale) {
  if (!valid(s) || scale < 1) return;
  int cx = x;
  for (char c : text) {
    if (c == '\n') continue;  // single-line rasteriser; callers position each line
    const Glyph* g = glyph_for(c);
    if (!g) g = glyph_for('?');  // documented behaviour: unknown → '?'
    if (g) {
      for (int row = 0; row < 7; ++row) {
        const uint8_t bits = g->rows[row];
        for (int col = 0; col < 5; ++col) {
          if ((bits >> (4 - col)) & 1u) {
            fill_rect(s, Rect{cx + col * scale, y + row * scale, scale, scale}, argb);
          }
        }
      }
    }
    cx += 6 * scale;
  }
}

std::string ascii_upper(const std::string& utf8) {
  // The 5x7 font only knows ASCII. Vietnamese diacritics need the Unicode font
  // that arrives with the real compositor (P2.6), so here every non-ASCII byte
  // becomes a space instead of being silently mangled. The splash strings are
  // written transliterated on purpose ("HE DIEU HANH VIET NAM"), which is also
  // what a user reads on a 320x200 screen on a 512 MB machine.
  std::string out;
  out.reserve(utf8.size());
  for (unsigned char c : utf8) {
    if (c >= 0x80) {
      out.push_back(' ');
    } else if (c >= 'a' && c <= 'z') {
      out.push_back(char(c - 'a' + 'A'));
    } else if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' || c == '.' ||
               c == ',' || c == ':' || c == '-' || c == '/' || c == '%' || c == '=' || c == '+' ||
               c == '!' || c == '(' || c == ')' || c == '?' || c == '_') {
      out.push_back(char(c));
    } else {
      out.push_back(' ');
    }
  }
  return out;
}

long render_splash(const Surface& s, const profile::HostProfile& host, int scale_k) {
  if (!valid(s) || s.width < 120 || s.height < 80) return 0;

  clear(s, kBg);
  // Accent band on top — the red of the flag, thin enough to keep contrast.
  fill_rect(s, Rect{0, 0, s.width, s.height / 30 + 2}, kAccent);

  // Lotus motif (three petals) above the title.
  const int cx = s.width / 2;
  const int size = s.height / 18 < 6 ? 6 : s.height / 18;
  petal(s, cx, s.height / 5, size, kAccent);
  petal(s, cx - size, s.height / 5 + size / 2, size - 2, kSoft);
  petal(s, cx + size, s.height / 5 + size / 2, size - 2, kSoft);

  // Hệ số HiDPI nguyên (P2.16): splash không biết kích thước vật lý nên
  // dùng DPI đoán theo độ phân giải (4K ⇒ 2), bên gọi có thể ép `scale_k`.
  int k = scale_k;
  if (k < 1 || k > 4) {
    display::ScaleInput si;
    si.width_px = uint32_t(s.width);
    si.height_px = uint32_t(s.height);
    k = display::choose_scale(si).k;
  }
  int title_scale = (s.width >= 640 ? 4 : (s.width >= 400 ? 3 : 2)) * k;
  while (title_scale > 1 && text_width("YUIVIEOS BETA", title_scale) > s.width - 16) --title_scale;
  centered(s, "YUIVIEOS BETA", title_scale, s.height / 3, kInk);
  const int sub_scale = title_scale > k ? title_scale - k : 1;
  centered(s, "HE DIEU HANH VIET NAM", sub_scale, s.height / 3 + text_height(title_scale) + 6 * k,
           kSoft);

  // Machine panel: what the profiler decided, so the boot screen is also a
  // diagnostic (a technician can read it from a photo).
  char line1[128];
  std::snprintf(line1, sizeof line1, "%s  %d THREADS  %.1f GB",
                ascii_upper(host.tier.empty() ? "can-bang" : host.tier).c_str(),
                host.threads > 0 ? host.threads : 1, host.ram_gb);
  char line2[128];
  std::snprintf(line2, sizeof line2, "K=%d  W=%d  %s", host.tune.k, host.tune.window,
                ascii_upper(host.simd.best).c_str());

  const int panel_x = s.width / 20;
  const int panel_w = s.width - 2 * panel_x;
  const int inner = panel_w - 22 * k;  // accent bar + padding

  // Never overflow the panel: shrink from 2k to 1x before clipping anything.
  int panel_scale = 2 * k;
  while (panel_scale > 1 &&
         (text_width(line1, panel_scale) > inner || text_width(line2, panel_scale) > inner)) {
    --panel_scale;
  }
  const int line_gap = 6 * k;
  const int panel_h = 2 * text_height(panel_scale) + line_gap + 16 * k;
  const int panel_y = s.height - panel_h - text_height(k) - 24 * k;
  fill_rect(s, Rect{panel_x, panel_y, panel_w, panel_h}, kPanel);
  fill_rect(s, Rect{panel_x, panel_y, 4 * k, panel_h}, kAccent);

  const int tx = panel_x + 14 * k;
  const int ty = panel_y + 8 * k;
  draw_text(s, tx, ty, line1, kInk, panel_scale);
  draw_text(s, tx, ty + text_height(panel_scale) + line_gap, line2, kSoft, panel_scale);

  // Footer: one honest line about what this build is.
  centered(s, "clean-room  -  safe rust kernel", k, s.height - text_height(k) - 8 * k, kSoft);
  return long(s.width) * s.height;
}

std::string to_ppm(const Surface& s) {
  if (!valid(s)) return std::string();
  std::string out = "P6\n" + std::to_string(s.width) + " " + std::to_string(s.height) + "\n255\n";
  out.reserve(out.size() + size_t(s.width) * size_t(s.height) * 3);
  for (long i = 0; i < long(s.width) * long(s.height); ++i) {
    const uint32_t p = s.pixels[i];
    out.push_back(char((p >> 16) & 0xFF));  // R
    out.push_back(char((p >> 8) & 0xFF));   // G
    out.push_back(char(p & 0xFF));          // B
  }
  return out;
}


// ------------------------------------------------------ Vietnamese UTF-8 --
namespace {

// Bảng tách chữ Việt tiền tổ hợp (NFD): codepoint → chữ gốc + mặt nạ dấu.
// Sinh tự động từ cơ sở dữ liệu Unicode nên đủ 178 chữ cái Việt + đ/Đ.
// Mặt nạ: 1=huyền 2=sắc 4=ngã 8=hỏi 16=nặng(dưới) 32=^ 64=˘ 128=horn 256=gạch-đ.
struct VietLetter {
  uint32_t cp;
  char base;
  uint16_t top;
  uint8_t below;
};

constexpr VietLetter kViet[] = {
    {0x00C0, 'A', 1, 0},
    {0x00C1, 'A', 2, 0},
    {0x00C2, 'A', 32, 0},
    {0x00C3, 'A', 4, 0},
    {0x00C8, 'E', 1, 0},
    {0x00C9, 'E', 2, 0},
    {0x00CA, 'E', 32, 0},
    {0x00CC, 'I', 1, 0},
    {0x00CD, 'I', 2, 0},
    {0x00CE, 'I', 32, 0},
    {0x00D1, 'N', 4, 0},
    {0x00D2, 'O', 1, 0},
    {0x00D3, 'O', 2, 0},
    {0x00D4, 'O', 32, 0},
    {0x00D5, 'O', 4, 0},
    {0x00D9, 'U', 1, 0},
    {0x00DA, 'U', 2, 0},
    {0x00DB, 'U', 32, 0},
    {0x00DD, 'Y', 2, 0},
    {0x00E0, 'A', 1, 0},
    {0x00E1, 'A', 2, 0},
    {0x00E2, 'A', 32, 0},
    {0x00E3, 'A', 4, 0},
    {0x00E8, 'E', 1, 0},
    {0x00E9, 'E', 2, 0},
    {0x00EA, 'E', 32, 0},
    {0x00EC, 'I', 1, 0},
    {0x00ED, 'I', 2, 0},
    {0x00EE, 'I', 32, 0},
    {0x00F1, 'N', 4, 0},
    {0x00F2, 'O', 1, 0},
    {0x00F3, 'O', 2, 0},
    {0x00F4, 'O', 32, 0},
    {0x00F5, 'O', 4, 0},
    {0x00F9, 'U', 1, 0},
    {0x00FA, 'U', 2, 0},
    {0x00FB, 'U', 32, 0},
    {0x00FD, 'Y', 2, 0},
    {0x0102, 'A', 64, 0},
    {0x0103, 'A', 64, 0},
    {0x0106, 'C', 2, 0},
    {0x0107, 'C', 2, 0},
    {0x0108, 'C', 32, 0},
    {0x0109, 'C', 32, 0},
    {0x0110, 'D', 256, 0},
    {0x0111, 'D', 256, 0},
    {0x0114, 'E', 64, 0},
    {0x0115, 'E', 64, 0},
    {0x011C, 'G', 32, 0},
    {0x011D, 'G', 32, 0},
    {0x011E, 'G', 64, 0},
    {0x011F, 'G', 64, 0},
    {0x0124, 'H', 32, 0},
    {0x0125, 'H', 32, 0},
    {0x0128, 'I', 4, 0},
    {0x0129, 'I', 4, 0},
    {0x012C, 'I', 64, 0},
    {0x012D, 'I', 64, 0},
    {0x0134, 'J', 32, 0},
    {0x0135, 'J', 32, 0},
    {0x0139, 'L', 2, 0},
    {0x013A, 'L', 2, 0},
    {0x0143, 'N', 2, 0},
    {0x0144, 'N', 2, 0},
    {0x014E, 'O', 64, 0},
    {0x014F, 'O', 64, 0},
    {0x0154, 'R', 2, 0},
    {0x0155, 'R', 2, 0},
    {0x015A, 'S', 2, 0},
    {0x015B, 'S', 2, 0},
    {0x015C, 'S', 32, 0},
    {0x015D, 'S', 32, 0},
    {0x0168, 'U', 4, 0},
    {0x0169, 'U', 4, 0},
    {0x016C, 'U', 64, 0},
    {0x016D, 'U', 64, 0},
    {0x0174, 'W', 32, 0},
    {0x0175, 'W', 32, 0},
    {0x0176, 'Y', 32, 0},
    {0x0177, 'Y', 32, 0},
    {0x0179, 'Z', 2, 0},
    {0x017A, 'Z', 2, 0},
    {0x01A0, 'O', 128, 0},
    {0x01A1, 'O', 128, 0},
    {0x01AF, 'U', 128, 0},
    {0x01B0, 'U', 128, 0},
    {0x01F4, 'G', 2, 0},
    {0x01F5, 'G', 2, 0},
    {0x01F8, 'N', 1, 0},
    {0x01F9, 'N', 1, 0},
    {0x1EA0, 'A', 0, 16},
    {0x1EA1, 'A', 0, 16},
    {0x1EA2, 'A', 8, 0},
    {0x1EA3, 'A', 8, 0},
    {0x1EA4, 'A', 34, 0},
    {0x1EA5, 'A', 34, 0},
    {0x1EA6, 'A', 33, 0},
    {0x1EA7, 'A', 33, 0},
    {0x1EA8, 'A', 40, 0},
    {0x1EA9, 'A', 40, 0},
    {0x1EAA, 'A', 36, 0},
    {0x1EAB, 'A', 36, 0},
    {0x1EAC, 'A', 32, 16},
    {0x1EAD, 'A', 32, 16},
    {0x1EAE, 'A', 66, 0},
    {0x1EAF, 'A', 66, 0},
    {0x1EB0, 'A', 65, 0},
    {0x1EB1, 'A', 65, 0},
    {0x1EB2, 'A', 72, 0},
    {0x1EB3, 'A', 72, 0},
    {0x1EB4, 'A', 68, 0},
    {0x1EB5, 'A', 68, 0},
    {0x1EB6, 'A', 64, 16},
    {0x1EB7, 'A', 64, 16},
    {0x1EB8, 'E', 0, 16},
    {0x1EB9, 'E', 0, 16},
    {0x1EBA, 'E', 8, 0},
    {0x1EBB, 'E', 8, 0},
    {0x1EBC, 'E', 4, 0},
    {0x1EBD, 'E', 4, 0},
    {0x1EBE, 'E', 34, 0},
    {0x1EBF, 'E', 34, 0},
    {0x1EC0, 'E', 33, 0},
    {0x1EC1, 'E', 33, 0},
    {0x1EC2, 'E', 40, 0},
    {0x1EC3, 'E', 40, 0},
    {0x1EC4, 'E', 36, 0},
    {0x1EC5, 'E', 36, 0},
    {0x1EC6, 'E', 32, 16},
    {0x1EC7, 'E', 32, 16},
    {0x1EC8, 'I', 8, 0},
    {0x1EC9, 'I', 8, 0},
    {0x1ECA, 'I', 0, 16},
    {0x1ECB, 'I', 0, 16},
    {0x1ECC, 'O', 0, 16},
    {0x1ECD, 'O', 0, 16},
    {0x1ECE, 'O', 8, 0},
    {0x1ECF, 'O', 8, 0},
    {0x1ED0, 'O', 34, 0},
    {0x1ED1, 'O', 34, 0},
    {0x1ED2, 'O', 33, 0},
    {0x1ED3, 'O', 33, 0},
    {0x1ED4, 'O', 40, 0},
    {0x1ED5, 'O', 40, 0},
    {0x1ED6, 'O', 36, 0},
    {0x1ED7, 'O', 36, 0},
    {0x1ED8, 'O', 32, 16},
    {0x1ED9, 'O', 32, 16},
    {0x1EDA, 'O', 130, 0},
    {0x1EDB, 'O', 130, 0},
    {0x1EDC, 'O', 129, 0},
    {0x1EDD, 'O', 129, 0},
    {0x1EDE, 'O', 136, 0},
    {0x1EDF, 'O', 136, 0},
    {0x1EE0, 'O', 132, 0},
    {0x1EE1, 'O', 132, 0},
    {0x1EE2, 'O', 128, 16},
    {0x1EE3, 'O', 128, 16},
    {0x1EE4, 'U', 0, 16},
    {0x1EE5, 'U', 0, 16},
    {0x1EE6, 'U', 8, 0},
    {0x1EE7, 'U', 8, 0},
    {0x1EE8, 'U', 130, 0},
    {0x1EE9, 'U', 130, 0},
    {0x1EEA, 'U', 129, 0},
    {0x1EEB, 'U', 129, 0},
    {0x1EEC, 'U', 136, 0},
    {0x1EED, 'U', 136, 0},
    {0x1EEE, 'U', 132, 0},
    {0x1EEF, 'U', 132, 0},
    {0x1EF0, 'U', 128, 16},
    {0x1EF1, 'U', 128, 16},
    {0x1EF2, 'Y', 1, 0},
    {0x1EF3, 'Y', 1, 0},
    {0x1EF4, 'Y', 0, 16},
    {0x1EF5, 'Y', 0, 16},
    {0x1EF6, 'Y', 8, 0},
    {0x1EF7, 'Y', 8, 0},
    {0x1EF8, 'Y', 4, 0},
    {0x1EF9, 'Y', 4, 0},};

constexpr uint16_t kGrave = 1, kAcute = 2, kTilde = 4, kHook = 8, kDotBelow = 16,
                   kCirc = 32, kBreve = 64, kHorn = 128, kBar = 256;
constexpr uint16_t kVowelMask = kCirc | kBreve | kHorn;
constexpr uint16_t kToneMask = kGrave | kAcute | kTilde | kHook;

const VietLetter* viet_for(uint32_t cp) {
  for (const auto& v : kViet)
    if (v.cp == cp) return &v;
  return nullptr;
}

// Vẽ một dấu trong hộp 5×2: pattern hàng 0 ở py, hàng 1 ở py + scale.
void draw_mark(uint16_t bit, const Surface& s, int px, int py, int scale, uint32_t argb) {
  auto put = [&](int mx, int my) {
    fill_rect(s, Rect{px + mx * scale, py + my * scale, scale, scale}, argb);
  };
  switch (bit) {
    case kGrave: put(1, 0); put(0, 1); break;
    case kAcute: put(3, 0); put(2, 1); break;
    case kTilde: put(1, 0); put(2, 1); put(3, 0); break;
    case kHook: put(1, 0); put(2, 0); put(2, 1); break;
    case kCirc: put(2, 0); put(1, 1); put(3, 1); break;
    case kBreve: put(1, 0); put(3, 0); put(2, 1); break;
    case kHorn: put(3, 0); put(4, 0); put(4, 1); break;
    default: break;
  }
}

// Chỉ hàng dưới của dấu — dùng khi xếp chồng thanh điệu + dấu nguyên âm
// (âm sắc đi trên, nguyên âm sát chữ): tránh chồng pixel vào nhau.
void draw_mark_base(uint16_t bit, const Surface& s, int px, int py, int scale, uint32_t argb) {
  auto put = [&](int mx) {
    fill_rect(s, Rect{px + mx * scale, py, scale, scale}, argb);
  };
  switch (bit) {
    case kGrave: put(0); break;
    case kAcute: put(2); break;
    case kTilde: put(2); break;
    case kHook: put(2); break;
    case kCirc: put(1); put(3); break;
    case kBreve: put(2); break;
    case kHorn: put(4); break;
    default: break;
  }
}

}  // namespace

std::vector<uint32_t> decode_utf8(const std::string& utf8) {
  std::vector<uint32_t> out;
  size_t i = 0;
  const auto byte = [&](size_t k) { return uint8_t(utf8[k]); };
  while (i < utf8.size()) {
    const uint8_t b0 = byte(i);
    uint32_t cp = 0;
    size_t extra = 0;
    if (b0 < 0x80) {
      cp = b0;
    } else if ((b0 & 0xE0) == 0xC0) {
      cp = b0 & 0x1F;
      extra = 1;
    } else if ((b0 & 0xF0) == 0xE0) {
      cp = b0 & 0x0F;
      extra = 2;
    } else if ((b0 & 0xF8) == 0xF0) {
      cp = b0 & 0x07;
      extra = 3;
    } else {
      out.push_back(0xFFFD);
      ++i;
      continue;
    }
    if (extra > 0 && i + extra >= utf8.size()) {
      out.push_back(0xFFFD);
      break;
    }
    bool bad = false;
    for (size_t k = 1; k <= extra; ++k) {
      const uint8_t b = byte(i + k);
      if ((b & 0xC0) != 0x80) { bad = true; break; }
      cp = (cp << 6) | (b & 0x3F);
    }
    if (bad) {
      out.push_back(0xFFFD);
      ++i;
      continue;
    }
    // Từ chối overlong / surrogate / ngoài Unicode — không bao giờ "bịa" ký tự.
    if ((extra == 1 && cp < 0x80) || (extra == 2 && cp < 0x800) || (extra == 3 && cp < 0x10000) ||
        (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) {
      out.push_back(0xFFFD);
      i += extra + 1;
      continue;
    }
    out.push_back(cp);
    i += extra + 1;
  }
  return out;
}

int text_height_utf8(int scale) { return scale < 1 ? 11 : 11 * scale; }

int text_width_utf8(const std::string& utf8, int scale) {
  if (scale < 1) scale = 1;
  size_t n = decode_utf8(utf8).size();
  for (size_t k = 0; k < utf8.size(); ++k)
    if (utf8[k] == '\n') { n = k; break; }  // một dòng như text_width
  if (n == 0) return 0;
  return int(n) * 6 * scale - scale;
}

void draw_text_utf8(const Surface& s, int x, int y, const std::string& utf8, uint32_t argb,
                    int scale) {
  if (!valid(s) || scale < 1) return;
  int cx = x;
  const int gy = y;  // đỉnh chữ cái — cùng tọa độ với draw_text (ASCII y hệt)
  for (uint32_t cp : decode_utf8(utf8)) {
    if (cp == '\n') continue;  // như draw_text: một dòng, tự bỏ ký tự xuống dòng
    const VietLetter* v = viet_for(cp);
    char base;
    if (v) {
      base = v->base;
    } else if (cp < 128) {
      base = char(cp);
      if (base >= 'a' && base <= 'z') base = char(base - 'a' + 'A');
    } else {
      base = '?';  // không bịa glyph: chữ lạ vẽ '?'
    }
    const Glyph* g = glyph_for(base);
    if (!g) g = glyph_for('?');
    if (g) {
      for (int row = 0; row < 7; ++row) {
        const uint8_t bits = g->rows[row];
        for (int col = 0; col < 5; ++col) {
          if ((bits >> (4 - col)) & 1u) {
            fill_rect(s, Rect{cx + col * scale, gy + row * scale, scale, scale}, argb);
          }
        }
      }
    }
    if (v) {
      const uint16_t top = v->top;
      if (top & kBar) {
        // Đ: gạch ngang cắt thân chữ ở hàng 1 của glyph.
        for (int mx = 1; mx <= 3; ++mx)
          fill_rect(s, Rect{cx + mx * scale, gy + 1 * scale, scale, scale}, argb);
      } else {
        const uint16_t vowel = top & kVowelMask;
        const uint16_t tone = top & kToneMask;
        if (vowel && tone) {
          // xếp chồng: nguyên âm sát chữ (hàng dưới của dấu), thanh điệu lên trên
          draw_mark_base(vowel, s, cx, y - 1 * scale, scale, argb);
          draw_mark(tone, s, cx, y - 3 * scale, scale, argb);
        } else if (vowel) {
          draw_mark(vowel, s, cx, y - 2 * scale, scale, argb);
        } else if (tone) {
          draw_mark(tone, s, cx, y - 2 * scale, scale, argb);
        }
      }
      if (v->below & kDotBelow) {
        // dấu nặng: một chấm dưới chân chữ (ngay hàng dưới glyph)
        fill_rect(s, Rect{cx + 2 * scale, y + 7 * scale, scale, scale}, argb);
      }
    }
    cx += 6 * scale;
  }
}

}  // namespace yui::fb
