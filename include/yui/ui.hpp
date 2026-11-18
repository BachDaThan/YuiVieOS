// Giao diện Thích ứng (Tầng 5): decides how to draw before drawing anything.
//
// Two render paths, chosen from measured hardware rather than a build flag:
//   * FramebufferDirect — weak machines / no GPU: direct framebuffer writes,
//     no compositor, no Vulkan driver in RAM.
//   * GpuZeroCopy    — strong machines: Vulkan with zero-copy DRM dumb buffers.
#ifndef YUI_UI_HPP
#define YUI_UI_HPP

#include <string>
#include <vector>

#include "yui/fb.hpp"

namespace yui::ui {

enum class RenderPath { FramebufferDirect = 0, GpuZeroCopy = 1, HeadlessTui = 2 };
const char* path_name(RenderPath p);

struct DisplayInfo {
  int width_px = 1920;
  int height_px = 1080;
  int dpi = 96;
};

struct HardwareForUi {
  double ram_gb = 8.0;
  bool has_gpu = false;
  bool low_power = false;
  std::string simd = "avx2";
  bool headless = false;
};

struct Layout {
  RenderPath path = RenderPath::FramebufferDirect;
  bool compositor_enabled = true;
  int scale_px = 1;      // integer pixel scale for text
  int columns = 80;      // characters per row
  int rows = 24;
  int cell_width_px = 8;
  int cell_height_px = 16;
  std::string reason;    // Vietnamese explanation, shown in the status bar
};

Layout plan(const DisplayInfo& display, const HardwareForUi& hw);

// ------------------------------------------------------- cửa sổ / văn bản --
// Khung cửa sổ vẽ qua framebuffer với tiêu đề + nội dung tiếng Việt ĐẦY ĐỦ
// DẤU (fb::draw_text_utf8) — "ngôn ngữ hiển thị" của P2.10. Thang máy tính
// yếu chỉ cần Surface + Rect, không phụ thuộc compositor.
struct WindowChrome {
  uint32_t border = 0xFF203040;
  uint32_t title_bg = 0xFF305070;
  uint32_t title_fg = 0xFFFFFFFF;
  uint32_t body_bg = 0xFF101820;
  uint32_t text_fg = 0xFFD0D8E0;
};

/// Vẽ khung cửa sổ: viền 1 px (× scale), thanh tiêu đề, thân chứa các dòng
/// văn bản tiếng Việt. Trả về số pixel đã vẽ (0 nếu surface không hợp lệ).
long render_window(const fb::Surface& s, const fb::Rect& r, const std::string& title_utf8,
                   const std::vector<std::string>& lines, const WindowChrome& c = WindowChrome{},
                   int scale = 1);

/// Vẽ một dòng nhãn tiếng Việt có dấu tại (x, y) — tiện cho status bar.
void render_label(const fb::Surface& s, int x, int y, const std::string& text_utf8,
                  uint32_t argb, int scale = 1);

}  // namespace yui::ui

#endif
