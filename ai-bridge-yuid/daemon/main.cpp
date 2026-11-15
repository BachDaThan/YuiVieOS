// yuid — YuiVieOS core daemon (Tầng 4).
//
//   yuid                        serve /run/yuid.sock (line-delimited JSON-RPC)
//   yuid --mcp                  serve the Model Context Protocol on stdio
//   yuid --info                 print the daemon capability manifest and exit
//   yuid --profile              print the hardware profile + tuned k/W
//   yuid --call METHOD [JSON]   run one request in-process (scripting/debug)
//   yuid --socket PATH | --state-dir DIR | --vault-key K | --enable-net | --verbose
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "yui/profile.hpp"
#include "yui/server.hpp"
#include "yui/service.hpp"
#include "yui/text.hpp"

namespace {

void usage() {
  std::printf(
      "yuid %s — YuiVieOS core daemon\n"
      "\n"
      "Cách dùng:\n"
      "  yuid                        Chạy dịch vụ trên /run/yuid.sock\n"
      "  yuid --mcp                  Chạy máy chủ MCP trên stdio\n"
      "  yuid --info                 In thông tin năng lực của daemon\n"
      "  yuid --profile              In cấu hình phần cứng và k/W tự chỉnh\n"
      "  yuid --call METHOD [JSON]   Gọi một phương thức rồi thoát\n"
      "\n"
      "Tuỳ chọn:\n"
      "  --socket PATH               Đường dẫn socket (mặc định /run/yuid.sock)\n"
      "  --state-dir DIR             Thư mục trạng thái (mặc định /var/lib/yui)\n"
      "  --vault-key KEY             Khoá mở kho mã hoá (hoặc YUI_VAULT_KEY)\n"
      "  --enable-net                Cho phép tầng tìm kiếm web gọi mạng\n"
      "  --serve-once                Nhận đúng một kết nối rồi thoát (kiểm thử)\n"
      "  --verbose                   Ghi log ra stderr\n",
      yui::service::version());
}

}  // namespace

int main(int argc, char** argv) {
  yui::service::Options opts;
  std::string socket_path = "/run/yuid.sock";
  bool mcp = false, info = false, show_profile = false, serve_once = false, verbose = false;
  std::string call_method, call_params = "{}";

  if (const char* env = std::getenv("YUI_VAULT_KEY")) opts.vault_password = env;
  if (const char* env = std::getenv("YUI_STATE_DIR")) opts.state_dir = env;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto next = [&](const char* what) -> std::string {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "yuid: %s cần một giá trị\n", what);
        std::exit(2);
      }
      return argv[++i];
    };
    if (arg == "--help" || arg == "-h") {
      usage();
      return 0;
    } else if (arg == "--mcp") {
      mcp = true;
    } else if (arg == "--info") {
      info = true;
    } else if (arg == "--profile") {
      show_profile = true;
    } else if (arg == "--serve-once") {
      serve_once = true;
    } else if (arg == "--verbose") {
      verbose = true;
    } else if (arg == "--enable-net") {
      opts.offline = false;
    } else if (arg == "--socket") {
      socket_path = next("--socket");
    } else if (arg == "--state-dir") {
      opts.state_dir = next("--state-dir");
    } else if (arg == "--vault-key") {
      opts.vault_password = next("--vault-key");
    } else if (arg == "--call") {
      call_method = next("--call");
      if (i + 1 < argc && argv[i + 1][0] == '{') call_params = argv[++i];
    } else {
      std::fprintf(stderr, "yuid: tham số không hợp lệ: %s\n", arg.c_str());
      usage();
      return 2;
    }
  }
  opts.verbose = verbose;
  if (opts.offline) {
    // Offline is the default: yuid must not reach the network unless asked.
    opts.offline = true;
  }

  yui::service::Daemon daemon(opts);
  daemon.set_host_snapshot(yui::profile::to_json(yui::profile::probe()));

  if (info || show_profile || !call_method.empty()) {
    if (show_profile) {
      std::printf("%s\n", daemon.dispatch([] {
                     yui::json::Value req(yui::json::Object{});
                     req.set("jsonrpc", "2.0");
                     req.set("id", 1);
                     req.set("method", "yuid.profile");
                     return req;
                   }())["result"].dump(true).c_str());
      return 0;
    }
    auto parsed = yui::json::parse(call_params);
    if (!parsed.ok) {
      std::fprintf(stderr, "yuid: JSON không hợp lệ: %s\n", parsed.error.c_str());
      return 2;
    }
    yui::service::RpcError err;
    const yui::json::Value result = info
                                        ? daemon.handle("yuid.info", yui::json::Value(nullptr), err)
                                        : daemon.handle(call_method, parsed.value, err);
    if (err.code != 0) {
      std::fprintf(stderr, "yuid: %s (mã %d)\n", err.message.c_str(), err.code);
      return 1;
    }
    std::printf("%s\n", result.dump(true).c_str());
    return 0;
  }

  if (verbose) std::fprintf(stderr, "yuid: khởi động (%s, mcp=%d)\n", socket_path.c_str(), int(mcp));
  if (mcp) return yui::server::run_mcp_stdio(daemon);

  std::atomic<bool> stop{false};
  const int rc = yui::server::run_unix_server(socket_path, daemon, serve_once, &stop);
  if (rc != 0) {
    std::fprintf(stderr, "yuid: không mở được socket %s (mã %d)\n", socket_path.c_str(), rc);
    return 1;
  }
  return 0;
}
