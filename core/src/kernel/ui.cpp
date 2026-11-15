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

}  // namespace yui::ui
