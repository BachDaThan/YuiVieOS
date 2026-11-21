// yui-profiler — Hardware Auto-Profiler CLI (Tầng 1).
//
//   yui-profiler            tóm tắt tiếng Việt
//   yui-profiler --json     JSON đầy đủ (dùng cho /var/lib/yui/profile.json)
//   yui-profiler --write P  ghi JSON ra P
//   yui-profiler --hw       kèm bảng thiết bị + hạng tương thích của yui-hw (P6.31)
#include <cstdio>
#include <cstring>
#include <string>

#include "yui/hw.hpp"
#include "yui/profile.hpp"

int main(int argc, char** argv) {
  bool json_out = false, with_hw = false;
  std::string write_path;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--json") json_out = true;
    else if (a == "--hw") with_hw = true;
    else if (a == "--write" && i + 1 < argc) write_path = argv[++i];
    else if (a == "--help" || a == "-h") {
      std::printf("yui-profiler [--json] [--write PATH] [--hw]\n");
      return 0;
    } else {
      std::fprintf(stderr, "yui-profiler: tham số không hợp lệ: %s\n", a.c_str());
      return 2;
    }
  }
  const auto p = yui::profile::probe();
  auto j = yui::profile::to_json(p);
  yui::hw::Report hw;
  if (with_hw) {
    hw = yui::hw::scan("/", yui::hw::load_default_hwdb());
    j.set("hw", yui::hw::to_json(hw));
  }
  const std::string doc = j.dump(true);
  if (!write_path.empty()) {
    FILE* f = std::fopen(write_path.c_str(), "wb");
    if (!f) {
      std::fprintf(stderr, "yui-profiler: không ghi được %s\n", write_path.c_str());
      return 1;
    }
    std::fwrite(doc.data(), 1, doc.size(), f);
    std::fclose(f);
  }
  if (json_out) {
    std::printf("%s\n", doc.c_str());
    return 0;
  }
  std::printf("YuiVieOS · Hồ sơ phần cứng\n");
  std::printf("  Kiến trúc : %s (%s, %u bit)\n", p.arch.c_str(), p.simd.best.c_str(), p.simd.vector_bits);
  std::printf("  CPU       : %s — %d luồng / %d nhân\n", p.cpu_model.c_str(), p.threads, p.cores);
  std::printf("  RAM       : %.1f GB (trống %.1f GB)\n", p.ram_gb, p.ram_available_gb);
  if (!p.gpu.empty()) std::printf("  GPU       : %s\n", p.gpu.c_str());
  if (p.has_battery)
    std::printf("  Pin       : %d%%%s%s\n", p.battery_percent, p.on_battery ? " (đang xả)" : "",
                p.charging ? " (đang sạc)" : "");
  if (p.temperature_c > 0) std::printf("  Nhiệt độ  : %.1f°C\n", p.temperature_c);
  std::printf("  Phân loại : %s\n", p.tier.c_str());
  std::printf("  Tự chỉnh  : k=%d, cửa sổ=%d, luồng=%d (%s)\n", p.tune.k, p.tune.window, p.tune.threads,
              p.tune.profile.c_str());
  std::printf("  Ghi chú   : %s\n", p.tune.reason.c_str());
  if (with_hw) std::printf("\n%s", yui::hw::summary_vi(hw).c_str());
  return 0;
}
