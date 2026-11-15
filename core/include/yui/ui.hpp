// Giao diện Thích ứng (Tầng 5): decides how to draw before drawing anything.
//
// Two render paths, chosen from measured hardware rather than a build flag:
//   * FramebufferDirect — weak machines / no GPU: direct framebuffer writes,
//     no compositor, no Vulkan driver in RAM.
//   * GpuZeroCopy    — strong machines: Vulkan with zero-copy DRM dumb buffers.
#ifndef YUI_UI_HPP
#define YUI_UI_HPP

#include <string>

namespace yui::ui {

enum class RenderPath { FramebufferDirect, GpuZeroCopy };
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
};

struct Layout {
  RenderPath path = RenderPath::FramebufferDirect;
  int scale_px = 1;      // integer pixel scale for text
  int columns = 80;      // characters per row
  int rows = 24;
  int cell_width_px = 8;
  int cell_height_px = 16;
  std::string reason;    // Vietnamese explanation, shown in the status bar
};

Layout plan(const DisplayInfo& display, const HardwareForUi& hw);

}  // namespace yui::ui

#endif
