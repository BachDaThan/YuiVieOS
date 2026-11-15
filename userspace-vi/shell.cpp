// Bilingual shell implementation.  Everything routes through service::Daemon so
// the terminal, the GUI and the MCP bridge obey exactly the same rules.
#include "yui/shell.hpp"

#include <sstream>

#include "yui/apppack.hpp"
#include "yui/b64.hpp"
#include "yui/codec.hpp"
#include "yui/ime.hpp"
#include "yui/text.hpp"

namespace yui::shell {
namespace {

std::string join_args(const std::vector<std::string>& args, size_t from) {
  std::string out;
  for (size_t i = from; i < args.size(); ++i) {
    if (i > from) out += ' ';
    out += args[i];
  }
  return out;
}

std::vector<std::string> tokenize(const std::string& line) {
  std::vector<std::string> out;
  std::string cur;
  bool in_quote = false;
  for (char c : line) {
    if (c == '"') {
      in_quote = !in_quote;
      continue;
    }
    if (!in_quote && (c == ' ' || c == '\t')) {
      if (!cur.empty()) {
        out.push_back(cur);
        cur.clear();
      }
      continue;
    }
    cur += c;
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

std::string human_bytes(uint64_t n) {
  char buf[64];
  if (n >= 1024ull * 1024 * 1024)
    std::snprintf(buf, sizeof(buf), "%.1f GiB", double(n) / (1024.0 * 1024 * 1024));
  else if (n >= 1024ull * 1024)
    std::snprintf(buf, sizeof(buf), "%.1f MiB", double(n) / (1024.0 * 1024));
  else if (n >= 1024)
    std::snprintf(buf, sizeof(buf), "%.1f KiB", double(n) / 1024.0);
  else
    std::snprintf(buf, sizeof(buf), "%llu B", static_cast<unsigned long long>(n));
  return buf;
}

}  // namespace

Shell::Shell(service::Daemon& daemon, const ui::Layout& layout) : daemon_(daemon), layout_(layout) {
  commands_ = {
      {"giúp", "help", "giúp [lệnh]", "help [command]", "Liệt kê mọi lệnh của YuiVieOS.",
       "List every YuiVieOS command."},
      {"thôngtin", "info", "thôngtin", "info", "Xem năng lực của dịch vụ yuid.",
       "Show the yuid capability manifest."},
      {"cấuhình", "profile", "cấuhình", "profile", "Xem phần cứng và cấu hình k/W tự chỉnh.",
       "Show hardware and the auto-tuned k/W."},
      {"tinhchỉnh", "tune", "tinhchỉnh [ram_gb] [nhiệt_độ] [pin]", "tune [ram_gb] [temp_c] [battery]",
       "Tính lại k/W theo RAM, nhiệt độ, pin.", "Recompute k/W for RAM, heat and battery."},
      {"gõ", "ime", "gõ telex|vni <phím>", "ime telex|vni <keys>",
       "Chuyển phím gõ thành chữ Việt có dấu.", "Convert keystrokes into composed Vietnamese."},
      {"ghinhớ", "remember", "ghinhớ <id> <nhãn> [loại]", "remember <id> <label> [kind]",
       "Ghi một mục vào đồ thị tri thức.", "Store an item in the knowledge graph."},
      {"nối", "link", "nối <từ> <quan_hệ> <đến>", "link <from> <rel> <to>", "Nối hai mục.",
       "Link two items."},
      {"tìm", "search", "tìm <từ khoá>", "search <query>", "Tìm trong đồ thị tri thức.",
       "Search the knowledge graph."},
      {"đường", "path", "đường <từ> <đến>", "path <from> <to>", "Tìm đường ngắn nhất giữa hai mục.",
       "Shortest path between two items."},
      {"thốngkê", "stats", "thốngkê", "stats", "Số nút và cạnh của đồ thị.", "Graph size."},
      {"gọttỉa", "trim", "gọttỉa <thưmục> <tệp.yuip>", "trim <dir> <file.yuip>",
       "Gọt tỉa và nén một cây ứng dụng.", "Trim and compress an application tree."},
      {"mởtệp", "openpack", "mởtệp <tệp.yuip> <thưmục>", "openpack <file.yuip> <dir>",
       "Bung gói .yuip ra thư mục.", "Extract a .yuip package."},
      {"hộpcát", "sandbox", "hộpcát <loại> <đối_tượng>", "sandbox <kind> <target>",
       "Kiểm tra quyền của một hành động trong hộp cát.", "Check a sandboxed action."},
      {"niêmphong", "seal", "niêmphong <vănbản>", "seal <text>", "Niêm phong dữ liệu vào kho.",
       "Seal data into the vault."},
      {"thoát", "exit", "thoát", "exit", "Rời shell.", "Leave the shell."},
  };
}

std::string Shell::banner() const {
  std::ostringstream os;
  os << "YuiVieOS · shell song ngữ Việt–Anh\n";
  os << "Giao diện: " << ui::path_name(layout_.path) << " · " << layout_.columns << "×" << layout_.rows
     << " ký tự · tỉ lệ " << layout_.scale_px << "x\n";
  os << "Lý do: " << layout_.reason << "\n";
  os << "Gõ \"giúp\" hoặc \"help\" để xem lệnh. Thoát bằng \"thoát\".";
  return os.str();
}

std::string Shell::help_text() const {
  std::ostringstream os;
  os << "Lệnh / Commands:\n";
  for (const auto& c : commands_)
    os << "  " << c.name_vi << " / " << c.name_en << "\n      " << c.help_vi << "\n      "
       << c.help_en << "\n      " << c.usage_vi << "\n";
  return os.str();
}

std::string Shell::run(const std::string& line) {
  const std::string trimmed = text::trim(line);
  if (trimmed.empty()) return "";
  const auto tokens = tokenize(trimmed);
  if (tokens.empty()) return "";
  const std::string cmd = text::fold(tokens[0]);

  // Aliases: Vietnamese and English are interchangeable.
  static const std::map<std::string, std::string> kAliases = {
      {"help", "giúp"},        {"trợgiúp", "giúp"},  {"info", "thôngtin"},
      {"profile", "cấuhình"},  {"tune", "tinhchỉnh"}, {"ime", "gõ"},
      {"remember", "ghinhớ"},  {"link", "nối"},       {"search", "tìm"},
      {"path", "đường"},       {"stats", "thốngkê"},  {"trim", "gọttỉa"},
      {"openpack", "mởtệp"},   {"sandbox", "hộpcát"}, {"seal", "niêmphong"},
      {"exit", "thoát"},       {"quit", "thoát"},
  };
  std::string canon = cmd;
  auto it = kAliases.find(cmd);
  if (it != kAliases.end()) canon = it->second;

  return dispatch(canon, tokens);
}

std::string Shell::dispatch(const std::string& cmd, const std::vector<std::string>& args) {
  ++history_;

  if (cmd == "thoát") {
    exit_ = true;
    return "Tạm biệt — hẹn gặp lại ở YuiVieOS. / Goodbye.";
  }
  if (cmd == "giúp") return help_text();

  if (cmd == "thôngtin") {
    service::RpcError err;
    const auto v = daemon_.handle("yuid.info", json::Value(nullptr), err);
    std::ostringstream os;
    os << "yuid " << v["version"].as_string() << " · tầng " << v["tier"].as_int() << " · "
       << v["simd"]["arch"].as_string() << "/" << v["simd"]["best"].as_string() << "\n"
       << "Đồ thị: " << v["graph_nodes"].as_int() << " nút, " << v["graph_edges"].as_int() << " cạnh\n"
       << "Ngoại tuyến: " << (v["offline"].as_bool() ? "có" : "không") << " · kho mã hoá: "
       << (v["vault_enabled"].as_bool() ? "đã mở khoá" : "chưa cấu hình");
    return os.str();
  }

  if (cmd == "cấuhình") {
    service::RpcError err;
    const auto v = daemon_.handle("yuid.profile", json::Value(nullptr), err);
    return v["summary"].as_string() + "\n" + v["tune"]["reason"].as_string();
  }

  if (cmd == "tinhchỉnh") {
    json::Value params(json::Object{});
    if (args.size() >= 2) params.set("ram_gb", std::atof(args[1].c_str()));
    if (args.size() >= 3) params.set("temperature_c", std::atof(args[2].c_str()));
    if (args.size() >= 4) params.set("on_battery", args[3] == "pin" || args[3] == "battery");
    service::RpcError err;
    const auto v = daemon_.handle("yuid.tune", params, err);
    if (err.code) return "Lỗi: " + err.message;
    std::ostringstream os;
    os << "k = " << v["k"].as_int() << ", cửa sổ = " << v["window"].as_int()
       << ", luồng = " << v["threads"].as_int() << " (" << v["profile"].as_string() << ")\n"
       << v["reason"].as_string();
    return os.str();
  }

  if (cmd == "gõ") {
    if (args.size() < 3) return "Dùng: gõ telex|vni <phím>";
    const std::string method = args[1] == "vni" ? "vni" : "telex";
    json::Value params(json::Object{});
    params.set("text", join_args(args, 2));
    params.set("method", method);
    service::RpcError err;
    const auto v = daemon_.handle("ime.convert", params, err);
    if (err.code) return "Lỗi: " + err.message;
    return v["result"].as_string();
  }

  if (cmd == "ghinhớ") {
    if (args.size() < 3) return "Dùng: ghinhớ <id> <nhãn> [loại]";
    // The optional trailing word is a node kind when it names one.
    static const char* kKinds[] = {"file", "app",    "entity", "concept", "device",
                                   "capability", "person", "place", "note"};
    std::string label = join_args(args, 2);
    std::string kind = "concept";
    for (const char* k : kKinds) {
      if (args.back() == k && args.size() >= 4) {
        kind = k;
        label = join_args(args, 2);
        const size_t cut = label.rfind(' ');  // drop the trailing kind word
        label = cut == std::string::npos ? label : label.substr(0, cut);
        break;
      }
    }
    json::Value params(json::Object{});
    params.set("id", args[1]);
    params.set("label", label);
    params.set("kind", kind);
    service::RpcError err;
    const auto v = daemon_.handle("graph.upsert", params, err);
    if (err.code) return "Lỗi: " + err.message;
    return v["created"].as_bool() ? "Đã ghi nhớ: " + args[1] : "Đã cập nhật: " + args[1];
  }

  if (cmd == "nối") {
    if (args.size() < 4) return "Dùng: nối <từ> <quan_hệ> <đến>";
    json::Value params(json::Object{});
    params.set("from", args[1]);
    params.set("rel", args[2]);
    params.set("to", args[3]);
    service::RpcError err;
    daemon_.handle("graph.link", params, err);
    if (err.code) return "Lỗi: " + err.message;
    return "Đã nối " + args[1] + " —" + args[2] + "→ " + args[3];
  }

  if (cmd == "tìm") {
    if (args.size() < 2) return "Dùng: tìm <từ khoá>";
    json::Value params(json::Object{});
    params.set("q", join_args(args, 1));
    service::RpcError err;
    const auto v = daemon_.handle("graph.query", params, err);
    if (err.code) return "Lỗi: " + err.message;
    std::ostringstream os;
    const auto& hits = v["hits"].arr();
    if (hits.empty()) return "Không tìm thấy mục nào khớp \"" + join_args(args, 1) + "\".";
    for (const auto& h : hits)
      os << "· " << h["id"].as_string() << " — " << h["label"].as_string() << " ["
         << h["kind"].as_string() << "]\n";
    return os.str();
  }

  if (cmd == "đường") {
    if (args.size() < 3) return "Dùng: đường <từ> <đến>";
    json::Value params(json::Object{});
    params.set("from", args[1]);
    params.set("to", args[2]);
    service::RpcError err;
    const auto v = daemon_.handle("graph.path", params, err);
    if (err.code) return "Lỗi: " + err.message;
    const auto& path = v["path"].arr();
    if (path.empty()) return "Không có đường đi.";
    std::string out;
    for (size_t i = 0; i < path.size(); ++i) {
      if (i) out += " → ";
      out += path[i].as_string();
    }
    return out;
  }

  if (cmd == "thốngkê") {
    service::RpcError err;
    const auto v = daemon_.handle("graph.stats", json::Value(nullptr), err);
    std::ostringstream os;
    os << "Nút: " << v["nodes"].as_int() << " · Cạnh: " << v["edges"].as_int();
    return os.str();
  }

  if (cmd == "gọttỉa") {
    if (args.size() < 3) return "Dùng: gọttỉa <thưmục> <tệp.yuip>";
    apppack::TrimOptions opts;
    apppack::TrimReport rep;
    std::string err;
    const auto packed = apppack::pack_tree(args[1], opts, rep, &err);
    if (!err.empty() || packed.empty()) return "Lỗi: " + (err.empty() ? "không có dữ liệu" : err);
    FILE* f = std::fopen(args[2].c_str(), "wb");
    if (!f) return "Không ghi được: " + args[2];
    std::fwrite(packed.data(), 1, packed.size(), f);
    std::fclose(f);
    return rep.summary_vi() + "\nĐã lưu: " + args[2] + " (" + human_bytes(packed.size()) + ")";
  }

  if (cmd == "mởtệp") {
    if (args.size() < 3) return "Dùng: mởtệp <tệp.yuip> <thưmục>";
    FILE* f = std::fopen(args[1].c_str(), "rb");
    if (!f) return "Không đọc được: " + args[1];
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    apppack::Bytes data(size_t(size > 0 ? size : 0));
    if (!data.empty()) std::fread(data.data(), 1, data.size(), f);
    std::fclose(f);
    std::string err;
    if (!apppack::unpack_to(data, args[2], &err)) return "Lỗi: " + err;
    return "Đã bung vào " + args[2];
  }

  if (cmd == "hộpcát") {
    if (args.size() < 3) return "Dùng: hộpcát <loại> <đối_tượng>";
    json::Value policy(json::Object{});
    policy.set("app", "shell:interactive");
    policy.set("tier", "native");
    policy.set("capabilities", json::Value(json::Array{}));
    json::Value caps(json::Array{});
    caps.push("*");
    policy.set("capabilities", caps);
    json::Value action(json::Object{});
    action.set("kind", args[1]);
    action.set("target", join_args(args, 2));
    json::Value params(json::Object{});
    params.set("policy", policy);
    params.set("action", action);
    service::RpcError err;
    const auto v = daemon_.handle("sandbox.check", params, err);
    if (err.code) return "Lỗi: " + err.message;
    return (v["allowed"].as_bool() ? "ĐƯỢC PHÉP — " : "BỊ TỪ CHỐI — ") + v["reason"].as_string();
  }

  if (cmd == "niêmphong") {
    if (args.size() < 2) return "Dùng: niêmphong <vănbản>";
    service::RpcError err;
    if (daemon_.options().vault_password.empty())
      return "Kho mã hoá chưa mở: chạy yuid với --vault-key hoặc đặt YUI_VAULT_KEY.";
    const std::string plain = join_args(args, 1);
    json::Value params(json::Object{});
    params.set("plain", b64::encode(reinterpret_cast<const uint8_t*>(plain.data()), plain.size()));
    const auto v = daemon_.handle("vault.seal", params, err);
    if (err.code) return "Lỗi: " + err.message;
    return "Đã niêm phong (" + std::to_string(v["bytes"].as_int()) + " byte).";
  }

  return "Không hiểu lệnh \"" + cmd + "\". Gõ \"giúp\" để xem danh sách lệnh.";
}

}  // namespace yui::shell
