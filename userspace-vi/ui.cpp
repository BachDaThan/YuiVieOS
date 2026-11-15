// Adaptive UI layout planner.
#include "yui/ui.hpp"

#include <algorithm>

namespace yui::ui {

const char* path_name(RenderPath p) {
  switch (p) {
    case RenderPath::FramebufferDirect: return "framebuffer-direct";
    case RenderPath::GpuZeroCopy: return "vulkan-drm-zero-copy";
  }
  return "framebuffer-direct";
}

Layout plan(const DisplayInfo& display, const HardwareForUi& hw) {
  Layout l;
  const int dpi = std::max(72, display.dpi);
  // Integer scaling only: fractional scaling is what makes weak GPUs sweat.
  l.scale_px = dpi >= 192 ? 3 : (dpi >= 144 ? 2 : 1);

  const bool gpu_ok = hw.has_gpu && hw.ram_gb >= 4.0 && !hw.low_power &&
                      (hw.simd == "avx2" || hw.simd == "avx512f" || hw.simd == "neon" ||
                       hw.simd == "sve" || hw.simd == "rvv");
  if (gpu_ok) {
    l.path = RenderPath::GpuZeroCopy;
    l.reason = "Máy mạnh: dùng Vulkan + DRM zero-copy để vẽ mượt.";
  } else {
    l.path = RenderPath::FramebufferDirect;
    if (!hw.has_gpu)
      l.reason = "Không có GPU rời: ghi trực tiếp framebuffer, không cần Vulkan.";
    else if (hw.ram_gb < 4.0)
      l.reason = "RAM dưới 4 GB: ưu tiên framebuffer trực tiếp để tiết kiệm bộ nhớ.";
    else
      l.reason = "Đang ở chế độ tiết kiệm điện: framebuffer trực tiếp.";
  }

  l.cell_width_px = 8 * l.scale_px;
  l.cell_height_px = 16 * l.scale_px;
  l.columns = std::max(40, display.width_px / l.cell_width_px);
  l.rows = std::max(10, (display.height_px - 32) / l.cell_height_px);
  return l;
}

long render_window(const fb::Surface& s, const fb::Rect& r, const std::string& title_utf8,
                   const std::vector<std::string>& lines, const WindowChrome& c, int scale) {
  if (!fb::valid(s) || scale < 1 || r.w <= 2 || r.h <= 2) return 0;
  long drawn = 0;
  // Thân + viền 1 px (× scale).
  fb::fill_rect(s, r, c.border);
  fb::fill_rect(s, fb::Rect{r.x + scale, r.y + scale, r.w - 2 * scale, r.h - 2 * scale},
                c.body_bg);
  drawn += long(r.w) * r.h;

  // Thanh tiêu đề: cao đủ cho dòng chữ Việt có dấu (11 px × scale) + 2 px đệm.
  // Chữ vẽ ở đỉnh glyph — dải dấu nằm phía trên nên lùi vào 3 px trong thanh.
  const int title_h = fb::text_height_utf8(scale) + 2 * scale;
  fb::fill_rect(s, fb::Rect{r.x + scale, r.y + scale, r.w - 2 * scale, title_h}, c.title_bg);
  fb::draw_text_utf8(s, r.x + 2 * scale, r.y + scale + 3 * scale, title_utf8, c.title_fg, scale);

  // Thân: các dòng văn bản tiếng Việt, mỗi dòng một ô 12 px × scale
  // (3 dấu + 7 chữ + 1 nặng + 1 giãn), cắt đúng mép cửa sổ.
  const int cell_h = fb::text_height_utf8(scale) + scale;
  int cell_top = r.y + scale + title_h + scale;
  for (const auto& line : lines) {
    if (cell_top + cell_h > r.y + r.h - scale) break;
    fb::draw_text_utf8(s, r.x + 2 * scale, cell_top + 3 * scale, line, c.text_fg, scale);
    cell_top += cell_h;
  }
  return drawn;
}

void render_label(const fb::Surface& s, int x, int y, const std::string& text_utf8, uint32_t argb,
                  int scale) {
  fb::draw_text_utf8(s, x, y, text_utf8, argb, scale < 1 ? 1 : scale);
}

}  // namespace yui::ui
