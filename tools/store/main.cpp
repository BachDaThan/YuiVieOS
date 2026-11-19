// tools/store/main.cpp — yui-store: CLI của Kho Ứng Dụng (P4.15).
//
// Clean-room, C++17, POSIX. Song ngữ Việt/Anh. Không có cờ "--yes" cho việc
// cài: xác nhận luôn đọc từ /dev/tty (không từ stdin), xem TtyConfirmation.
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "yui/crypto.hpp"
#include "yui/store.hpp"
#include "yui/text.hpp"

namespace {

using namespace yui;

void show_help() {
  std::cout
      << "yui-store — Kho Ứng Dụng YuiVieOS (P4.15): danh mục ký Ed25519 → tải → kiểm → gọt → hộp cát\n\n"
      << "Người dùng / Users:\n"
      << "  yui-store tìm|search [từ khoá]            tìm không dấu theo tên, mã, nhà phát hành\n"
      << "  yui-store thôngtin|info <tên|mã>          xem nguồn tải, giấy phép, quyền sẽ cấp\n"
      << "  yui-store cài|install <tên|mã>            cài (luôn hỏi xác nhận qua terminal /dev/tty)\n"
      << "  yui-store gỡ|remove <mã> [--yes]          gỡ sạch: tệp, chính sách, biểu tượng, bộ nhớ đệm\n"
      << "  yui-store danhsách|list                   ứng dụng đã cài\n"
      << "  yui-store nhập|import <catalog.json> <catalog.sig>   kiểm chữ ký rồi lưu danh mục\n\n"
      << "Người soạn danh mục / Catalog authors:\n"
      << "  yui-store băm|hash <tệp>                   in size + sha256 để điền vào danh mục\n"
      << "  yui-store kýapp|sign-app <catalog.json> --app <mã> --key <seed> --key-id <id>\n"
      << "  yui-store kýdanhmục|sign-catalog <catalog.json> --key <seed> [--out <catalog.sig>]\n\n"
      << "Tuỳ chọn / Options:\n"
      << "  --root <thư_mục>     gốc Kho (mặc định $YUI_STORE_ROOT hoặc /var/lib/yui/store)\n"
      << "  --keyring <thư_mục>  khoá công khai tin cậy (mặc định $YUI_KEYRING_DIR hoặc /etc/yui/trusted-keys)\n"
      << "  --allow-file-urls    nhận file:// (bản bạn đã tự tải từ nguồn chính thức; thử nghiệm)\n"
      << "  --trogiup, --help    trợ giúp này\n\n"
      << "Kho không mirror, không phân phối lại bản đã gọt: mọi gói tải từ URL chính thức trong danh mục.\n";
}

std::string get_opt(int argc, char** argv, const std::string& opt, const std::string& def = "") {
  for (int i = 1; i + 1 < argc; ++i)
    if (argv[i] == opt) return argv[i + 1];
  return def;
}

bool has_flag(int argc, char** argv, const std::string& flag) {
  for (int i = 1; i < argc; ++i)
    if (argv[i] == flag) return true;
  return false;
}

// Đối số vị trí thứ n (bỏ qua các cặp --opt giá trị và cờ đơn).
std::string positional(int argc, char** argv, int n) {
  static const std::vector<std::string> with_value = {"--root", "--keyring", "--key", "--key-id", "--out", "--app"};
  int seen = 0;
  for (int i = 2; i < argc; ++i) {
    const std::string a = argv[i];
    if (a.rfind("--", 0) == 0) {
      for (const auto& w : with_value)
        if (a == w) {
          ++i;
          break;
        }
      continue;
    }
    if (seen++ == n) return a;
  }
  return "";
}

std::string env_or(const char* name, const std::string& def) {
  const char* v = std::getenv(name);
  return (v && v[0]) ? std::string(v) : def;
}

bool read_text(const std::string& path, std::string& out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  std::ostringstream ss;
  ss << f.rdbuf();
  out = ss.str();
  return true;
}

bool read_seed(const std::string& key_or_path, ed25519::Seed& seed) {
  std::string hex = key_or_path;
  std::string file;
  if (read_text(key_or_path, file)) hex = file;
  hex = text::trim(hex);
  return ed25519::from_hex(hex, seed.data(), seed.size());
}

struct Ctx {
  bundle::Keyring keys;
  store::Options opts;
};

Ctx make_ctx(int argc, char** argv) {
  Ctx c;
  c.opts.root = get_opt(argc, argv, "--root", env_or("YUI_STORE_ROOT", "/var/lib/yui/store"));
  c.opts.allow_file_urls = has_flag(argc, argv, "--allow-file-urls");
  const std::string kr = get_opt(argc, argv, "--keyring", env_or("YUI_KEYRING_DIR", "/etc/yui/trusted-keys"));
  c.keys.load_dir(kr, nullptr);
  return c;
}

int need_catalog(store::Store& s) {
  const auto r = s.load_catalog_files();
  if (!r.ok) {
    std::cerr << "✗ " << r.reason_vi << "\n";
    return 1;
  }
  return 0;
}

int cmd_search(int argc, char** argv) {
  Ctx c = make_ctx(argc, argv);
  store::Store s(c.opts, c.keys);
  if (need_catalog(s)) return 1;
  const auto hits = s.search(positional(argc, argv, 0));
  if (hits.empty()) {
    std::cout << "Không tìm thấy ứng dụng nào.\n";
    return 0;
  }
  for (const auto& a : hits) {
    std::cout << "· " << a.name << " " << a.version << "  [" << a.id << "]  — " << a.publisher << " · "
              << a.license << " · " << a.kind << (s.is_installed(a.id) ? " · ĐÃ CÀI" : "") << "\n";
  }
  return 0;
}

int cmd_info(int argc, char** argv) {
  Ctx c = make_ctx(argc, argv);
  store::Store s(c.opts, c.keys);
  if (need_catalog(s)) return 1;
  store::InstallPlan p;
  const auto r = s.plan(positional(argc, argv, 0), p);
  if (!r.ok) {
    std::cerr << "✗ " << r.reason_vi << "\n";
    return 1;
  }
  std::cout << p.summary_vi() << "\n";
  return 0;
}

int cmd_install(int argc, char** argv) {
  Ctx c = make_ctx(argc, argv);
  store::Store s(c.opts, c.keys);
  if (need_catalog(s)) return 1;
  const std::string app = positional(argc, argv, 0);
  if (app.empty()) {
    std::cerr << "Lỗi: thiếu tên hoặc mã ứng dụng\n";
    return 1;
  }
  store::TtyConfirmation gate;
  store::CurlFetcher curl;
  store::FileFetcher file;
  store::AppEntry e;
  const auto rr = s.resolve(app, e);
  if (!rr.ok) {
    std::cerr << "✗ " << rr.reason_vi << "\n";
    return 1;
  }
  store::Fetcher& f = store::url_scheme(e.source_url) == "file" ? static_cast<store::Fetcher&>(file)
                                                                 : static_cast<store::Fetcher&>(curl);
  const auto r = s.install(app, f, gate);
  if (!r.ok) {
    std::cerr << "✗ " << r.reason_vi << "\n";
    return 1;
  }
  std::cout << "✓ " << r.reason_vi << "\n";
  if (!r.app.menu_path.empty()) std::cout << "  Biểu tượng: " << r.app.menu_path << "\n  Hộp cát  : " << r.app.policy_path << "\n";
  return 0;
}

int cmd_remove(int argc, char** argv) {
  Ctx c = make_ctx(argc, argv);
  store::Store s(c.opts, c.keys);
  const std::string id = positional(argc, argv, 0);
  store::InstalledApp cur;
  if (!s.is_installed(id, &cur)) {
    std::cerr << "✗ Ứng dụng " << id << " chưa được cài\n";
    return 1;
  }
  if (!has_flag(argc, argv, "--yes")) {
    FILE* tty = std::fopen("/dev/tty", "r+");
    if (!tty) {
      std::cerr << "✗ Cần terminal tương tác để xác nhận (hoặc thêm --yes)\n";
      return 1;
    }
    std::fprintf(tty, "Gỡ %s (%s)? Gõ lại mã ứng dụng để xác nhận: ", cur.name.c_str(), id.c_str());
    std::fflush(tty);
    char buf[256] = {};
    const bool got = std::fgets(buf, sizeof buf, tty) != nullptr;
    std::fclose(tty);
    if (!got || text::trim(buf) != id) {
      std::cerr << "Đã huỷ.\n";
      return 1;
    }
  }
  const auto r = s.remove(id);
  if (!r.ok) {
    std::cerr << "✗ " << r.reason_vi << "\n";
    return 1;
  }
  std::cout << "✓ " << r.reason_vi << "\n";
  return 0;
}

int cmd_list(int argc, char** argv) {
  Ctx c = make_ctx(argc, argv);
  store::Store s(c.opts, c.keys);
  const auto apps = s.installed();
  std::cout << "YuiVieOS · Kho Ứng Dụng · " << c.opts.root << "\n";
  if (apps.empty()) {
    std::cout << "  (chưa cài ứng dụng nào)\n";
    return 0;
  }
  for (const auto& a : apps)
    std::cout << "  · " << a.name << " " << a.version << " [" << a.id << "] — " << a.publisher << " · " << a.files
              << " tệp · chữ ký: " << a.signature << "\n";
  return 0;
}

int cmd_import(int argc, char** argv) {
  Ctx c = make_ctx(argc, argv);
  store::Store s(c.opts, c.keys);
  std::string cat, sig;
  if (!read_text(positional(argc, argv, 0), cat) || !read_text(positional(argc, argv, 1), sig)) {
    std::cerr << "Lỗi: cần <catalog.json> <catalog.sig>\n";
    return 1;
  }
  const auto r = s.load_catalog(cat, sig);
  if (!r.ok) {
    std::cerr << "✗ " << r.reason_vi << "\n";
    return 1;
  }
  std::string err;
  if (!s.save_catalog(cat, sig, &err)) {
    std::cerr << "✗ " << err << "\n";
    return 1;
  }
  std::cout << "✓ " << r.reason_vi << " → " << c.opts.root << "/catalog.json\n";
  return 0;
}

int cmd_hash(int argc, char** argv) {
  std::string data;
  const std::string path = positional(argc, argv, 0);
  if (!read_text(path, data)) {
    std::cerr << "Lỗi: không đọc được " << path << "\n";
    return 1;
  }
  std::cout << "size   " << data.size() << "\nsha256 " << crypto::hex(crypto::sha256(data)) << "\n";
  return 0;
}

int cmd_sign_app(int argc, char** argv) {
  const std::string path = positional(argc, argv, 0);
  const std::string app = get_opt(argc, argv, "--app");
  const std::string key_id = get_opt(argc, argv, "--key-id");
  std::string text_json;
  ed25519::Seed seed{};
  if (!read_text(path, text_json) || app.empty() || key_id.empty() ||
      !read_seed(get_opt(argc, argv, "--key"), seed)) {
    std::cerr << "Lỗi: cần <catalog.json> --app <mã> --key <seed> --key-id <id>\n";
    return 1;
  }
  store::Catalog c;
  std::string err;
  if (!store::parse_catalog(text_json, c, &err)) {
    std::cerr << "✗ " << err << "\n";
    return 1;
  }
  bool found = false;
  for (auto& e : c.apps)
    if (e.id == app) {
      e.signer_key_id = key_id;
      e.signature_hex = store::sign_entry(e, seed);
      found = true;
    }
  if (!found) {
    std::cerr << "✗ Không có mục " << app << "\n";
    return 1;
  }
  std::ofstream(path, std::ios::binary) << store::catalog_to_json(c);
  std::cout << "✓ Đã ký mục " << app << " bằng khoá " << key_id << ". Nhớ ký lại cả danh mục (sign-catalog).\n";
  return 0;
}

int cmd_sign_catalog(int argc, char** argv) {
  const std::string path = positional(argc, argv, 0);
  std::string text_json;
  ed25519::Seed seed{};
  if (!read_text(path, text_json) || !read_seed(get_opt(argc, argv, "--key"), seed)) {
    std::cerr << "Lỗi: cần <catalog.json> --key <seed>\n";
    return 1;
  }
  store::Catalog c;
  std::string err;
  if (!store::parse_catalog(text_json, c, &err)) {
    std::cerr << "✗ " << err << "\n";
    return 1;
  }
  const std::string sig = store::sign_catalog(c, seed);
  const std::string out = get_opt(argc, argv, "--out", path.substr(0, path.rfind('.')) + ".sig");
  std::ofstream(out, std::ios::binary) << sig << "\n";
  std::cout << "✓ Đã ký danh mục (" << c.apps.size() << " ứng dụng, key_id '" << c.key_id << "') → " << out << "\n";
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2 || has_flag(argc, argv, "--help") || has_flag(argc, argv, "--trogiup") || has_flag(argc, argv, "-h")) {
    show_help();
    return 0;
  }
  const std::string cmd = text::fold(argv[1]);
  if (cmd == "tìm" || cmd == "tim" || cmd == "search") return cmd_search(argc, argv);
  if (cmd == "thôngtin" || cmd == "thongtin" || cmd == "info") return cmd_info(argc, argv);
  if (cmd == "cài" || cmd == "cai" || cmd == "install") return cmd_install(argc, argv);
  if (cmd == "gỡ" || cmd == "go" || cmd == "remove") return cmd_remove(argc, argv);
  if (cmd == "danhsách" || cmd == "danhsach" || cmd == "list") return cmd_list(argc, argv);
  if (cmd == "nhập" || cmd == "nhap" || cmd == "import") return cmd_import(argc, argv);
  if (cmd == "băm" || cmd == "bam" || cmd == "hash") return cmd_hash(argc, argv);
  if (cmd == "kýapp" || cmd == "kyapp" || cmd == "sign-app") return cmd_sign_app(argc, argv);
  if (cmd == "kýdanhmục" || cmd == "kydanhmuc" || cmd == "sign-catalog") return cmd_sign_catalog(argc, argv);
  std::cerr << "Lệnh không hợp lệ: " << argv[1] << ". Chạy 'yui-store --trogiup'.\n";
  return 1;
}
