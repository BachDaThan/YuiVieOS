// yui-splash — render the boot splash into a linear framebuffer and save it as
// PPM. On a real machine the same `fb::render_splash` writes straight into the
// framebuffer the kernel handed over; this CLI exists so the result is
// inspectable (and testable in CI) without a display.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "yui/fb.hpp"
#include "yui/profile.hpp"

namespace {

void usage() {
  std::fprintf(stderr,
               "yui-splash — vẽ màn hình chào YuiVieOS ra bộ đệm khung hình tuyến tính\n"
               "\n"
               "Dùng: yui-splash [--width N] [--height N] [--out FILE.ppm]\n"
               "  --width, --height  kích thước khung hình (mặc định 640x400)\n"
               "  --out              ghi ảnh PPM nhị phân (mặc định splash.ppm, '-' = stdout)\n"
               "\n"
               "Trong hệ điều hành thật, cùng hàm này ghi trực tiếp vào framebuffer.\n");
}

}  // namespace

int main(int argc, char** argv) {
  int width = 640;
  int height = 400;
  std::string out_path = "splash.ppm";

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto next = [&](const char* flag) -> std::string {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "%s cần một giá trị\n", flag);
        std::exit(2);
      }
      return argv[++i];
    };
    if (arg == "--help" || arg == "-h") {
      usage();
      return 0;
    } else if (arg == "--width") {
      width = std::atoi(next("--width").c_str());
    } else if (arg == "--height") {
      height = std::atoi(next("--height").c_str());
    } else if (arg == "--out") {
      out_path = next("--out");
    } else {
      std::fprintf(stderr, "tham số lạ: %s\n", arg.c_str());
      usage();
      return 2;
    }
  }

  if (width < 64 || height < 48 || width > 7680 || height > 4320) {
    std::fprintf(stderr, "kích thước không hợp lệ: %dx%d\n", width, height);
    return 2;
  }

  std::vector<uint32_t> pixels(size_t(width) * size_t(height), 0);
  yui::fb::Surface surface;
  surface.width = width;
  surface.height = height;
  surface.pixels = pixels.data();

  const yui::profile::HostProfile host = yui::profile::probe();
  const long drawn = yui::fb::render_splash(surface, host);
  if (drawn <= 0) {
    std::fprintf(stderr, "không vẽ được: khung hình quá nhỏ (%dx%d)\n", width, height);
    return 1;
  }

  const std::string ppm = yui::fb::to_ppm(surface);
  if (out_path == "-") {
    std::fwrite(ppm.data(), 1, ppm.size(), stdout);
  } else {
    FILE* f = std::fopen(out_path.c_str(), "wb");
    if (!f) {
      std::fprintf(stderr, "không mở được %s\n", out_path.c_str());
      return 1;
    }
    std::fwrite(ppm.data(), 1, ppm.size(), f);
    std::fclose(f);
  }

  std::printf("YuiVieOS · màn hình chào %dx%d → %s\n", width, height, out_path.c_str());
  std::printf("  Máy      : %s (%d luồng, %.1f GB)\n", host.tier.c_str(), host.threads, host.ram_gb);
  std::printf("  Tự chỉnh : k=%d, W=%d (%s)\n", host.tune.k, host.tune.window, host.tune.reason.c_str());
  return 0;
}
