// yui-shell — shell lệnh song ngữ Việt–Anh (Tầng 5).
//
//   yui-shell                     REPL tương tác
//   yui-shell --script FILE       chạy một loạt lệnh rồi thoát
//   echo "giúp" | yui-shell       đọc lệnh từ stdin
//   --width/--height/--dpi/--ram/--gpu/--low-power  mô tả màn hình & máy
//   YUI_STATE_DIR=...             thư mục trạng thái (mặc định /var/lib/yui)
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>

#include "yui/profile.hpp"
#include "yui/shell.hpp"
#include "yui/ui.hpp"

int main(int argc, char** argv) {
  std::string script;
  bool plain = false;
  yui::ui::DisplayInfo display;
  yui::ui::HardwareForUi hw;
  bool hw_given = false;

  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&](const char* what) -> std::string {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "yui-shell: %s cần giá trị\n", what);
        std::exit(2);
      }
      return argv[++i];
    };
    if (a == "--help" || a == "-h") {
      std::printf(
          "yui-shell [--script FILE] [--plain] [--width N] [--height N] [--dpi N]\n"
          "          [--ram GB] [--gpu] [--low-power]\n");
      return 0;
    } else if (a == "--script") script = next("--script");
    else if (a == "--plain") plain = true;
    else if (a == "--width") display.width_px = std::atoi(next("--width").c_str());
    else if (a == "--height") display.height_px = std::atoi(next("--height").c_str());
    else if (a == "--dpi") display.dpi = std::atoi(next("--dpi").c_str());
    else if (a == "--ram") { hw.ram_gb = std::atof(next("--ram").c_str()); hw_given = true; }
    else if (a == "--gpu") { hw.has_gpu = true; hw_given = true; }
    else if (a == "--low-power") { hw.low_power = true; hw_given = true; }
    else {
      std::fprintf(stderr, "yui-shell: tham số không hợp lệ: %s\n", a.c_str());
      return 2;
    }
  }
  (void)plain;

  // Default to what this machine actually is; flags override for demos/tests.
  if (!hw_given) {
    const auto host = yui::profile::probe();
    hw.ram_gb = host.ram_gb > 0 ? host.ram_gb : 8.0;
    hw.has_gpu = host.gpu.find("NVIDIA") != std::string::npos || host.gpu.find("AMD") != std::string::npos;
    hw.simd = host.simd.best;
  }

  const yui::ui::Layout layout = yui::ui::plan(display, hw);

  yui::service::Options opts;
  opts.state_dir = "/var/lib/yui";
  // Thư mục trạng thái (Kho, thùng rác…) — đổi được để chạy thử không cần quyền root.
  if (const char* env = std::getenv("YUI_STATE_DIR")) opts.state_dir = env;
  if (const char* env = std::getenv("YUI_VAULT_KEY")) opts.vault_password = env;
  yui::service::Daemon daemon(opts);
  daemon.set_host_snapshot(yui::profile::to_json(yui::profile::probe()));

  yui::shell::Shell shell(daemon, layout);
  std::printf("%s\n\n", shell.banner().c_str());

  auto handle = [&](const std::string& line) {
    if (line.empty()) return true;
    std::printf("› %s\n", line.c_str());
    const std::string out = shell.run(line);
    if (!out.empty()) std::printf("%s\n", out.c_str());
    std::printf("\n");
    return !shell.wants_exit();
  };

  if (!script.empty()) {
    FILE* f = std::fopen(script.c_str(), "rb");
    if (!f) {
      std::fprintf(stderr, "yui-shell: không đọc được %s\n", script.c_str());
      return 1;
    }
    char buf[4096];
    while (std::fgets(buf, sizeof(buf), f)) {
      std::string line = buf;
      while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
      if (!line.empty() && line[0] == '#') continue;
      if (!handle(line)) break;
    }
    std::fclose(f);
    return 0;
  }

  std::string line;
  while (true) {
    std::printf("yui› ");
    std::fflush(stdout);
    if (!std::getline(std::cin, line)) break;
    if (!handle(line)) break;
  }
  return 0;
}
