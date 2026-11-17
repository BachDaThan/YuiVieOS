// tools/update/main.cpp — CLI quản trị cập nhật A/B và chữ ký số YuiVieOS (P4.7 + P4.9).
//
// Clean-room, C++17, chuẩn POSIX, không phụ thuộc thư viện ngoài.
// Hỗ trợ song ngữ (tiếng Việt / English).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "yui/bundle.hpp"
#include "yui/crypto.hpp"
#include "yui/ed25519.hpp"

namespace {

void show_help() {
  std::cout
      << "yui-update — Quản lý cập nhật hệ thống A/B và chữ ký số YuiVieOS (P4.7 / P4.9)\n\n"
      << "Cú pháp / Usage:\n"
      << "  yui-update status [--state-dir <thư_mục>] [--stream <os|models|apps>]\n"
      << "  yui-update check <đường_dẫn_bundle> [--keyring <thư_mục>]\n"
      << "  yui-update apply <đường_dẫn_bundle> [--keyring <thư_mục>] [--state-dir <thư_mục>]\n"
      << "                   [--stream <os|models|apps>] [--now]\n"
      << "  yui-update rollback [--state-dir <thư_mục>] [--stream <os|models|apps>]\n"
      << "  yui-update commit [--state-dir <thư_mục>] [--stream <os|models|apps>]\n"
      << "  yui-update sign <thư_mục_payload> --out <thư_mục_bundle> --key <khoá_bí_mật>\n"
      << "                  --key-id <id> [--version <ver>] [--stream <os|models|apps>]\n"
      << "  yui-update verify <đường_dẫn_bundle> [--keyring <thư_mục>]\n"
      << "  yui-update keygen --id <id> --out-keyring <thư_mục> --out-priv <tệp_lưu>\n\n"
      << "Tuỳ chọn / Options:\n"
      << "  --now             Áp dụng và kích hoạt boot ngay lập tức (mặc định: hoãn kích hoạt\n"
      << "                    để phiên làm việc của người dùng không bị gián đoạn)\n"
      << "  --stream <tên>    Luồng cập nhật độc lập: 'os' (mặc định), 'models', hoặc 'apps'\n"
      << "  --keyring <đường_dẫn> Thư mục chứa các khoá công khai chủ quyền (.pub)\n"
      << "  --state-dir <đường_dẫn> Thư mục trạng thái slot A/B (mặc định: /var/lib/yui/update)\n"
      << "  --trogiup, --help Hiển thị trợ giúp này\n";
}

std::string get_opt(int argc, char** argv, const std::string& opt, const std::string& def = "") {
  for (int i = 1; i < argc; ++i) {
    if (std::string(argv[i]) == opt && i + 1 < argc) {
      return argv[i + 1];
    }
  }
  return def;
}

bool has_flag(int argc, char** argv, const std::string& flag) {
  for (int i = 1; i < argc; ++i) {
    if (std::string(argv[i]) == flag) return true;
  }
  return false;
}

std::string resolve_state_dir(int argc, char** argv) {
  std::string s = get_opt(argc, argv, "--state-dir");
  if (!s.empty()) return s;
  const char* env = std::getenv("YUI_UPDATE_STATE");
  if (env && env[0]) return env;
  return "/var/lib/yui/update";
}

std::string resolve_keyring_dir(int argc, char** argv) {
  std::string s = get_opt(argc, argv, "--keyring");
  if (!s.empty()) return s;
  const char* env = std::getenv("YUI_KEYRING_DIR");
  if (env && env[0]) return env;
  return "/etc/yui/trusted-keys";
}

bool load_keyring(const std::string& dir, yui::bundle::Keyring& keys) {
  std::string err;
  if (!keys.load_dir(dir, &err)) {
    // Thử thư mục dự phòng
    if (dir == "/etc/yui/trusted-keys") {
      keys.load_dir("/var/lib/yui/trusted-keys", nullptr);
    }
  }
  return keys.size() > 0;
}

bool read_secret_seed(const std::string& key_str_or_path, yui::ed25519::Seed& seed) {
  std::string hex = key_str_or_path;
  std::ifstream f(key_str_or_path);
  if (f.is_open()) {
    std::string line;
    if (std::getline(f, line)) hex = line;
  }
  while (!hex.empty() && (hex.back() == '\n' || hex.back() == '\r' || hex.back() == ' '))
    hex.pop_back();
  return yui::ed25519::from_hex(hex, seed.data(), seed.size());
}

int cmd_status(int argc, char** argv) {
  const std::string state_dir = resolve_state_dir(argc, argv);
  const std::string stream = get_opt(argc, argv, "--stream");

  std::cout << "YuiVieOS · Cập nhật hệ thống & mô hình (A/B Sovereign Update)\n";
  std::cout << "  Thư mục trạng thái: " << state_dir << "\n";

  std::vector<std::string> streams;
  if (!stream.empty()) {
    streams.push_back(stream);
  } else {
    streams = {"os", "models", "apps"};
  }

  for (const auto& s : streams) {
    const auto st = yui::bundle::update_status(state_dir, s);
    std::cout << "\n[Luồng: " << s << "]\n";
    std::cout << "  Khe hoạt động : " << st.active_slot;
    if (st.active_version != "chưa cài đặt" && st.active_version != "trống") {
      std::cout << " (phiên bản " << st.active_version;
      if (!st.active_key_id.empty()) std::cout << ", khoá " << st.active_key_id;
      if (st.file_count > 0) std::cout << ", " << st.file_count << " tệp";
      std::cout << ")";
    } else {
      std::cout << " (chưa có bản cài)";
    }
    std::cout << "\n";

    std::cout << "  Khe dự phòng  : " << st.backup_slot;
    if (st.backup_version != "chưa cài đặt" && st.backup_version != "trống") {
      std::cout << " (phiên bản " << st.backup_version << ")";
    } else {
      std::cout << " (trống)";
    }
    std::cout << "\n";

    std::cout << "  Chờ kích hoạt : ";
    if (st.has_pending) {
      std::cout << "khe " << st.pending_slot << " (sẽ kích hoạt ở lần khởi động tới)\n";
    } else {
      std::cout << "không (hệ thống đang chạy ổn định)\n";
    }
  }
  return 0;
}

int cmd_check(int argc, char** argv) {
  if (argc < 3) {
    std::cerr << "Lỗi: thiếu đường dẫn tới thư mục bundle\n";
    return 1;
  }
  const std::string bundle_dir = argv[2];
  const std::string keyring_dir = resolve_keyring_dir(argc, argv);

  yui::bundle::Keyring keys;
  load_keyring(keyring_dir, keys);

  const auto r = yui::bundle::verify(bundle_dir, keys);
  if (!r.ok) {
    std::cerr << "✗ Bundle không hợp lệ: " << r.reason_vi << "\n";
    return 1;
  }

  // Đọc thông tin manifest
  std::ifstream f(bundle_dir + "/manifest.json");
  std::string json_content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  yui::bundle::Manifest m;
  yui::bundle::parse_manifest(json_content, m);

  std::cout << "✓ Bundle hợp lệ: " << r.reason_vi << "\n";
  std::cout << "  Khoá ký  : " << r.key_id << "\n";
  std::cout << "  Luồng    : " << m.stream << "\n";
  std::cout << "  Phiên bản: " << m.version << "\n";
  std::cout << "  Số tệp   : " << m.files.size() << "\n";
  return 0;
}

int cmd_apply(int argc, char** argv) {
  if (argc < 3) {
    std::cerr << "Lỗi: thiếu đường dẫn tới thư mục bundle\n";
    return 1;
  }
  const std::string bundle_dir = argv[2];
  const std::string state_dir = resolve_state_dir(argc, argv);
  const std::string keyring_dir = resolve_keyring_dir(argc, argv);
  const std::string stream = get_opt(argc, argv, "--stream");
  const bool now = has_flag(argc, argv, "--now");

  yui::bundle::Keyring keys;
  load_keyring(keyring_dir, keys);

  yui::bundle::UpdateOptions opts;
  opts.stream = stream;
  opts.defer_reboot = !now;

  const auto r = yui::bundle::update_apply(bundle_dir, keys, state_dir, opts);
  if (!r.ok) {
    std::cerr << "✗ Áp dụng thất bại: " << r.reason_vi << "\n";
    return 1;
  }

  std::cout << "✓ " << r.reason_vi << "\n";
  if (r.pending) {
    std::cout << "  Ghi chú: Bản cập nhật đã sẵn sàng ở khe " << r.slot
              << ". Lần khởi động tiếp theo sẽ tự động chuyển sang khe mới mà không làm mất dữ liệu.\n";
  } else {
    std::cout << "  Con trỏ khởi động đã trỏ tới khe " << r.slot << " ngay lập tức.\n";
  }
  return 0;
}

int cmd_rollback(int argc, char** argv) {
  const std::string state_dir = resolve_state_dir(argc, argv);
  const std::string stream = get_opt(argc, argv, "--stream", "os");

  const auto r = yui::bundle::update_rollback(state_dir, stream);
  if (!r.ok) {
    std::cerr << "✗ Quay lui thất bại: " << r.reason_vi << "\n";
    return 1;
  }

  std::cout << "✓ " << r.reason_vi << "\n";
  return 0;
}

int cmd_commit(int argc, char** argv) {
  const std::string state_dir = resolve_state_dir(argc, argv);
  const std::string stream = get_opt(argc, argv, "--stream", "os");

  std::string err;
  if (!yui::bundle::update_commit_pending(state_dir, stream, &err)) {
    std::cerr << "✗ Kích hoạt thất bại: " << err << "\n";
    return 1;
  }

  std::cout << "✓ Đã kích hoạt bản cập nhật đang chờ cho luồng '" << stream
            << "' sang khe hoạt động.\n";
  return 0;
}

int cmd_sign(int argc, char** argv) {
  if (argc < 3) {
    std::cerr << "Lỗi: thiếu thư mục payload\n";
    return 1;
  }
  const std::string payload_dir = argv[2];
  const std::string bundle_dir = get_opt(argc, argv, "--out");
  const std::string key_id = get_opt(argc, argv, "--key-id");
  const std::string key_str = get_opt(argc, argv, "--key");
  const std::string version = get_opt(argc, argv, "--version", "1.0.0");
  const std::string stream = get_opt(argc, argv, "--stream", "os");

  if (bundle_dir.empty() || key_id.empty() || key_str.empty()) {
    std::cerr << "Lỗi: yui-update sign cần --out <dir>, --key-id <id>, --key <priv_key>\n";
    return 1;
  }

  yui::ed25519::Seed seed{};
  if (!read_secret_seed(key_str, seed)) {
    std::cerr << "Lỗi: khoá bí mật không hợp lệ (cần 64 ký tự hex hoặc tệp chứa seed)\n";
    return 1;
  }

  yui::bundle::SignOptions opts;
  opts.version = version;
  opts.key_id = key_id;
  opts.stream = stream;

  std::string err;
  if (!yui::bundle::sign_bundle(payload_dir, bundle_dir, opts, seed, &err)) {
    std::cerr << "✗ Ký bundle thất bại: " << err << "\n";
    return 1;
  }

  std::cout << "✓ Đã đóng gói và ký số thành công gói bundle vào: " << bundle_dir << "\n";
  std::cout << "  Khoá ký  : " << key_id << "\n";
  std::cout << "  Luồng    : " << stream << "\n";
  std::cout << "  Phiên bản: " << version << "\n";
  return 0;
}

int cmd_keygen(int argc, char** argv) {
  const std::string key_id = get_opt(argc, argv, "--id");
  const std::string out_keyring = get_opt(argc, argv, "--out-keyring");
  const std::string out_priv = get_opt(argc, argv, "--out-priv");

  if (key_id.empty() || out_keyring.empty() || out_priv.empty()) {
    std::cerr << "Lỗi: keygen cần --id <id>, --out-keyring <thư_mục>, --out-priv <tệp>\n";
    return 1;
  }

  const auto rand = yui::crypto::random_bytes(32);
  yui::ed25519::Seed seed{};
  std::memcpy(seed.data(), rand.data(), 32);

  const auto pub = yui::ed25519::public_key(seed);

  yui::bundle::Keyring ring;
  ring.add(key_id, pub);
  std::string err;
  if (!ring.save_dir(out_keyring, &err)) {
    std::cerr << "✗ Không lưu được khoá công khai: " << err << "\n";
    return 1;
  }

  const std::string seed_hex = yui::ed25519::to_hex(seed.data(), seed.size()) + "\n";
  std::ofstream priv_file(out_priv);
  if (!priv_file.is_open()) {
    std::cerr << "✗ Không mở được tệp khoá riêng để ghi: " << out_priv << "\n";
    return 1;
  }
  priv_file << seed_hex;
  priv_file.close();

  std::cout << "✓ Đã sinh cặp khoá chủ quyền Ed25519 thành công:\n";
  std::cout << "  Key ID        : " << key_id << "\n";
  std::cout << "  Khoá công khai: " << out_keyring << "/" << key_id << ".pub\n";
  std::cout << "  Khoá riêng    : " << out_priv << " (hãy cất giữ ngoại tuyến an toàn!)\n";
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2 || has_flag(argc, argv, "--help") || has_flag(argc, argv, "--trogiup") ||
      has_flag(argc, argv, "-h")) {
    show_help();
    return 0;
  }

  const std::string cmd = argv[1];
  if (cmd == "status" || cmd == "trangthai") {
    return cmd_status(argc, argv);
  } else if (cmd == "check" || cmd == "kiemtra") {
    return cmd_check(argc, argv);
  } else if (cmd == "apply" || cmd == "apdung" || cmd == "cai") {
    return cmd_apply(argc, argv);
  } else if (cmd == "rollback" || cmd == "quaylui") {
    return cmd_rollback(argc, argv);
  } else if (cmd == "commit" || cmd == "kichhoat") {
    return cmd_commit(argc, argv);
  } else if (cmd == "sign" || cmd == "ky") {
    return cmd_sign(argc, argv);
  } else if (cmd == "verify" || cmd == "xacminh") {
    return cmd_check(argc, argv);
  } else if (cmd == "keygen" || cmd == "taokhoa") {
    return cmd_keygen(argc, argv);
  } else {
    std::cerr << "Lệnh không hợp lệ: " << cmd << ". Chạy 'yui-update --trogiup' để xem hướng dẫn.\n";
    return 1;
  }
}
