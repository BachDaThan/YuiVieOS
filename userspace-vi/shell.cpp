// Bilingual shell implementation.  Everything routes through service::Daemon so
// the terminal, the GUI and the MCP bridge obey exactly the same rules.
#include "yui/shell.hpp"

#include <algorithm>
#include <cstdlib>
#include <sstream>

#include "yui/apppack.hpp"
#include "yui/b64.hpp"
#include "yui/codec.hpp"
#include "yui/ime.hpp"
#include "yui/legal.hpp"
#include "yui/telemetry.hpp"
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

size_t levenshtein_distance(const std::string& s1, const std::string& s2) {
  const size_t m = s1.size();
  const size_t n = s2.size();
  std::vector<size_t> dp(n + 1, 0);
  for (size_t j = 0; j <= n; ++j) dp[j] = j;

  for (size_t i = 1; i <= m; ++i) {
    size_t prev = dp[0];
    dp[0] = i;
    for (size_t j = 1; j <= n; ++j) {
      size_t temp = dp[j];
      if (s1[i - 1] == s2[j - 1]) {
        dp[j] = prev;
      } else {
        dp[j] = 1 + std::min({prev, dp[j], dp[j - 1]});
      }
      prev = temp;
    }
  }
  return dp[n];
}

// Aliases: Vietnamese and English are interchangeable, along with unaccented forms and shortcuts.
const std::map<std::string, std::string> kAliases = {
    // 1. giúp / help
    {"help", "giúp"},        {"giup", "giúp"},      {"trợgiúp", "giúp"},   {"trogiup", "giúp"},
    // 2. thôngtin / info
    {"info", "thôngtin"},    {"thongtin", "thôngtin"},
    // 3. cấuhình / profile
    {"profile", "cấuhình"},  {"cauhinh", "cấuhình"},
    // 4. tinhchỉnh / tune
    {"tune", "tinhchỉnh"},   {"tinhchinh", "tinhchỉnh"},
    // 5. gõ / ime
    {"ime", "gõ"},           {"go", "gõ"},
    // 6. ghinhớ / remember
    {"remember", "ghinhớ"},  {"ghinho", "ghinhớ"},
    // 7. nối / link
    {"link", "nối"},         {"noi", "nối"},
    // 8. tìm / search
    {"search", "tìm"},       {"tim", "tìm"},
    // 9. đường / path
    {"path", "đường"},       {"duong", "đường"},
    // 10. thốngkê / stats
    {"stats", "thốngkê"},    {"thongke", "thốngkê"},
    // 11. gọttỉa / trim
    {"trim", "gọttỉa"},      {"gottia", "gọttỉa"},
    // 12. mởtệp / openpack
    {"openpack", "mởtệp"},   {"motep", "mởtệp"},
    // 13. hộpcát / sandbox
    {"sandbox", "hộpcát"},   {"hopcat", "hộpcát"},
    // 14. niêmphong / seal
    {"seal", "niêmphong"},   {"niemphong", "niêmphong"},
    // 15. thoát / exit
    {"exit", "thoát"},       {"thoat", "thoát"},    {"quit", "thoát"},
    // 16. đotải / telemetry
    {"telemetry", "đotải"},  {"dotai", "đotải"},
    // 17. mởkhoá / unseal
    {"unseal", "mởkhoá"},    {"mokhoa", "mởkhoá"},
    // 18. xoánút / remove
    {"remove", "xoánút"},    {"xoanut", "xoánút"},  {"rm", "xoánút"},
    // 19. nhậtký / audit
    {"audit", "nhậtký"},     {"nhatky", "nhậtký"},  {"xoánhậtký", "nhậtký"}, {"xoanhatky", "nhậtký"}, {"clearaudit", "nhậtký"},
    // 20. cảnhbáo / alerts
    {"alerts", "cảnhbáo"},   {"canhbao", "cảnhbáo"}, {"alert", "cảnhbáo"},
    // 21. phiênbản / version
    {"version", "phiênbản"}, {"phienban", "phiênbản"}, {"ver", "phiênbản"},
    // 22. trakho / vaultinfo
    {"vaultinfo", "trakho"}, {"vault", "trakho"},
    // 23. tìmweb / websearch
    {"websearch", "tìmweb"}, {"timweb", "tìmweb"},  {"web", "tìmweb"},
    // 24. xoámàn / clear
    {"clear", "xoámàn"},     {"xoaman", "xoámàn"},  {"cls", "xoámàn"},
    // 25. thờigian / uptime
    {"uptime", "thờigian"},  {"thoigian", "thờigian"}, {"time", "thờigian"},
    // 26. tươngthích / compat (P3.16)
    {"compat", "tươngthích"}, {"tuongthich", "tươngthích"}, {"compatibility", "tươngthích"},
    // 27–29. Kho Ứng Dụng (P4.15): tìmapp / cài / gỡ
    {"searchapp", "tìmapp"}, {"timapp", "tìmapp"}, {"findapp", "tìmapp"},
    {"install", "cài"},      {"cai", "cài"},
    {"uninstall", "gỡ"},     {"gocai", "gỡ"},       {"gỡcài", "gỡ"},
    {"delete", "xoá"},       {"del", "xoá"},        {"trash", "xoá"},      {"xoa", "xoá"},   {"xóa", "xoá"},
    {"trashlist", "thùngrác"}, {"thungrac", "thùngrác"}, {"lsrac", "thùngrác"},
    {"restore", "khôiphục"}, {"khoiphuc", "khôiphục"}, {"undelete", "khôiphục"},
    {"purge", "xoávĩnhviễn"}, {"xoavinhvien", "xoávĩnhviễn"}, {"xóavĩnhviễn", "xoávĩnhviễn"},
    {"emptytrash", "đổrác"}, {"dorac", "đổrác"},
    {"cleanup", "dọnrác"},   {"donrac", "dọnrác"},  {"clean", "dọnrác"},
    {"run", "chạy"},         {"chay", "chạy"},      {"posix", "chạy"},
};

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
      {"đotải", "telemetry", "đotải", "telemetry", "Đọc cảm biến nhiệt độ, pin, RAM của máy.",
       "Read hardware sensors (temp, battery, RAM)."},
      {"mởkhoá", "unseal", "mởkhoá <dữ_liệu_base64>", "unseal <base64_blob>",
       "Mở dữ liệu niêm phong từ kho Vault.", "Open sealed data from the vault."},
      {"xoánút", "remove", "xoánút <id>", "remove <id>", "Xoá một mục khỏi đồ thị tri thức.",
       "Remove an item from the knowledge graph."},
      {"nhậtký", "audit", "nhậtký", "audit", "Xem lịch sử kiểm toán của hộp cát.",
       "View sandbox enforcer audit log."},
      {"cảnhbáo", "alerts", "cảnhbáo", "alerts", "Xem các cảnh báo an ninh từ Guard.",
       "View active security alerts from Guard."},
      {"phiênbản", "version", "phiênbản", "version", "Xem thông tin phiên bản OS và ABI.",
       "Show OS and ABI version."},
      {"trakho", "vaultinfo", "trakho", "vaultinfo", "Kiểm tra trạng thái kho bí mật Vault.",
       "Check vault configuration status."},
      {"tìmweb", "websearch", "tìmweb <truy_vấn>", "websearch <query>",
       "Tìm kiếm thông tin web qua yuid.", "Search the web via yuid."},
      {"xoámàn", "clear", "xoámàn", "clear", "Xoá sạch màn hình terminal.", "Clear terminal screen."},
      {"thờigian", "uptime", "thờigian", "uptime", "Xem số lệnh đã thực thi trong phiên.",
       "Show session command count and status."},
      {"tươngthích", "compat", "tươngthích [app] [bật|tắt]", "compat [app] [on|off]",
       "Bật/tắt chế độ tương thích hộp cát cho ứng dụng (mặc định tắt).",
       "Enable or disable sandbox compatibility mode for an app (default off)."},
      {"tìmapp", "searchapp", "tìmapp [từ khoá]", "searchapp [query]",
       "Tìm ứng dụng trong Kho (không dấu, danh mục ký Ed25519).",
       "Search the signed app catalog (accent-insensitive)."},
      {"cài", "install", "cài <tên|mã>", "install <name|id>",
       "Xem nguồn tải, giấy phép, quyền rồi mở hộp thoại xác nhận tin cậy để cài.",
       "Show source, license and rights, then open the trusted confirmation dialog."},
      {"gỡ", "uninstall", "gỡ <mã>", "uninstall <id>",
       "Gỡ sạch ứng dụng: tệp, chính sách hộp cát, biểu tượng, bộ nhớ đệm.",
       "Remove an app completely: files, sandbox policy, menu entry, cache."},
      {"xoá", "delete", "xoá <đường_dẫn>", "delete <path>",
       "Chuyển tệp/thư mục vào thùng rác (khôi phục được, kiểm băm).",
       "Move a file/folder to the trash (restorable, hash-checked)."},
      {"thùngrác", "trashlist", "thùngrác", "trashlist", "Xem các món trong thùng rác và dung lượng.",
       "List trash items and usage."},
      {"khôiphục", "restore", "khôiphục <mã> [đường_dẫn_mới]", "restore <id> [new_path]",
       "Trả món về chỗ cũ (không ghi đè tệp đang có).", "Restore an item (never overwrites)."},
      {"xoávĩnhviễn", "purge", "xoávĩnhviễn <mã>", "purge <id>",
       "Xoá hẳn một món trong thùng rác (cần xác nhận tin cậy).",
       "Permanently delete one trash item (trusted confirmation)."},
      {"đổrác", "emptytrash", "đổrác", "emptytrash", "Đổ cả thùng rác (cần xác nhận tin cậy).",
       "Empty the whole trash (trusted confirmation)."},
      {"dọnrác", "cleanup", "dọnrác", "cleanup",
       "Dọn bộ nhớ đệm/tệp tạm trong danh sách có tên; hiện dung lượng rồi xin xác nhận.",
       "Clean named caches/temp files only; shows the size, then asks for confirmation."},
      {"chạy", "run", "chạy <dòng lệnh POSIX>  (hoặc !<dòng lệnh>)", "run <POSIX command line>  (or !<line>)",
       "Chạy lệnh hệ thống trong hộp cát (Landlock): | > >> < 2>&1 & jobs kill wait cd export.",
       "Run a system command in the sandbox (Landlock): | > >> < 2>&1 & jobs kill wait cd export."},
  };
}

std::string Shell::banner() const {
  std::ostringstream os;
  os << "YuiVieOS · shell song ngữ Việt–Anh\n";
  os << "Giao diện: " << ui::path_name(layout_.path) << " · " << layout_.columns << "×" << layout_.rows
     << " ký tự · tỉ lệ " << layout_.scale_px << "x\n";
  os << "Lý do: " << layout_.reason << "\n";
  os << legal::kAsIsVi << "\n";
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

std::string Shell::suggest(const std::string& input) const {
  const std::string q = text::fold(text::trim(input));
  if (q.empty()) return "";

  struct Cand {
    std::string display;
    std::string key;
  };
  std::vector<Cand> cands;
  cands.reserve(commands_.size() * 2 + kAliases.size());
  for (const auto& c : commands_) {
    cands.push_back({c.name_vi, text::fold(c.name_vi)});
    cands.push_back({c.name_en, text::fold(c.name_en)});
  }
  for (const auto& kv : kAliases) {
    cands.push_back({kv.first, text::fold(kv.first)});
  }

  // 1. Exact match
  for (const auto& c : cands) {
    if (c.key == q) return c.display;
  }

  // 2. Prefix match (prefer commands if input is >= 2 characters)
  std::string best_prefix;
  size_t min_pfx_diff = 999;
  for (const auto& c : cands) {
    if (c.key.rfind(q, 0) == 0 && c.key.size() > q.size()) {
      size_t diff = c.key.size() - q.size();
      if (diff < min_pfx_diff) {
        min_pfx_diff = diff;
        best_prefix = c.display;
      }
    }
  }

  // 3. Levenshtein edit distance
  std::string best_edit;
  size_t min_dist = 999;
  for (const auto& c : cands) {
    size_t d = levenshtein_distance(q, c.key);
    if (d < min_dist) {
      min_dist = d;
      best_edit = c.display;
    }
  }

  if (!best_prefix.empty() && (min_dist > 2 || min_pfx_diff <= 3)) {
    return best_prefix;
  }

  const size_t max_dist = std::max<size_t>(2, q.size() / 2);
  if (min_dist <= max_dist && min_dist <= 3) {
    return best_edit;
  }

  return "";
}

posix::Runner& Shell::posix_runner() {
  if (!posix_) posix_ = std::make_unique<posix::Runner>(daemon_, posix_opts_);
  return *posix_;
}

std::string Shell::run(const std::string& line) {
  const std::string trimmed = text::trim(line);
  if (trimmed.empty()) return "";
  // Chỉ chạy lệnh hệ thống khi bạn nói rõ: "!…" hoặc "chạy …" (P4.12).
  if (trimmed[0] == '!') {
    ++history_;
    const std::string rest = text::trim(trimmed.substr(1));
    if (rest.empty()) return "Cách dùng: !<dòng lệnh POSIX>, ví dụ: !ls -la | grep txt > ds.txt";
    return posix_runner().run(rest);
  }
  const auto tokens = tokenize(trimmed);
  if (tokens.empty()) return "";
  const std::string cmd = text::fold(tokens[0]);

  std::string canon = cmd;
  auto it = kAliases.find(cmd);
  if (it != kAliases.end()) canon = it->second;

  if (canon == "chạy") {
    ++history_;
    const size_t sp = trimmed.find_first_of(" \t");
    const std::string rest = sp == std::string::npos ? "" : text::trim(trimmed.substr(sp));
    if (rest.empty()) return "Cách dùng: chạy <dòng lệnh POSIX>  (hoặc !<dòng lệnh>)";
    return posix_runner().run(rest);
  }

  return dispatch(canon, tokens);
}

std::string Shell::dispatch(const std::string& cmd, const std::vector<std::string>& args) {
  ++history_;

  if (cmd == "thoát") {
    exit_ = true;
    return "Tạm biệt — hẹn gặp lại ở YuiVieOS. / Goodbye.";
  }
  if (cmd == "giúp") {
    if (args.size() > 1) {
      const std::string target = text::fold(args[1]);
      for (const auto& c : commands_) {
        if (text::fold(c.name_vi) == target || text::fold(c.name_en) == target) {
          std::ostringstream os;
          os << c.name_vi << " / " << c.name_en << "\n  "
             << c.help_vi << "\n  " << c.help_en << "\n  "
             << "Cú pháp VI: " << c.usage_vi << "\n  "
             << "Syntax EN: " << c.usage_en;
          return os.str();
        }
      }
      return "Không tìm thấy trợ giúp cho \"" + args[1] + "\".";
    }
    return help_text();
  }

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
      if (args.back() == k) {
        kind = k;
        label = join_args(args, 2);
        const auto pos = label.rfind(' ');
        if (pos != std::string::npos) label = label.substr(0, pos);
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
    return "Đã ghi nhớ " + v["id"].as_string() + " [" + kind + "]";
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
    return "Đã nối " + args[1] + " —(" + args[2] + ")→ " + args[3];
  }

  if (cmd == "tìm") {
    if (args.size() < 2) return "Dùng: tìm <từ khoá>";
    json::Value params(json::Object{});
    params.set("q", join_args(args, 1));
    service::RpcError err;
    const auto v = daemon_.handle("graph.query", params, err);
    if (err.code) return "Lỗi: " + err.message;
    const auto& hits = v["hits"].arr();
    if (hits.empty()) return "Không tìm thấy kết quả nào.";
    std::ostringstream os;
    for (size_t i = 0; i < hits.size(); ++i) {
      if (i) os << "\n";
      os << "• " << hits[i]["id"].as_string() << ": " << hits[i]["label"].as_string()
         << " [" << hits[i]["kind"].as_string() << "]";
    }
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
    const auto& p = v["path"].arr();
    if (p.empty()) return "Không có đường đi giữa " + args[1] + " và " + args[2];
    std::string out;
    for (size_t i = 0; i < p.size(); ++i) {
      if (i) out += " → ";
      out += p[i].as_string();
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
    return "Đã niêm phong (" + std::to_string(v["bytes"].as_int()) + " byte): " + v["blob"].as_string();
  }

  if (cmd == "đotải") {
    const auto r = telemetry::read();
    const auto a = telemetry::advise(r);
    return "Đo tải phần cứng: " + telemetry::summary_vi(r, a);
  }

  if (cmd == "mởkhoá") {
    if (args.size() < 2) return "Dùng: mởkhoá <dữ_liệu_base64>";
    if (daemon_.options().vault_password.empty())
      return "Kho mã hoá chưa mở: chạy yuid với --vault-key hoặc đặt YUI_VAULT_KEY.";
    json::Value params(json::Object{});
    params.set("blob", args[1]);
    service::RpcError err;
    const auto v = daemon_.handle("vault.open", params, err);
    if (err.code) return "Lỗi: " + err.message;
    std::vector<uint8_t> plain_bytes;
    if (b64::decode(v["plain"].as_string(), plain_bytes)) {
      return "Đã mở khoá: " + std::string(plain_bytes.begin(), plain_bytes.end());
    }
    return "Đã mở khoá (" + std::to_string(v["bytes"].as_int()) + " byte).";
  }

  if (cmd == "xoánút") {
    if (args.size() < 2) return "Dùng: xoánút <id>";
    json::Value params(json::Object{});
    params.set("id", args[1]);
    service::RpcError err;
    const auto v = daemon_.handle("graph.remove", params, err);
    if (err.code) return "Lỗi: " + err.message;
    return v["removed"].as_bool() ? ("Đã xoá nút \"" + args[1] + "\".")
                                  : ("Không tìm thấy nút \"" + args[1] + "\" để xoá.");
  }

  if (cmd == "nhậtký") {
    if (args.size() > 1 && (args[1] == "xoá" || args[1] == "xoa" || args[1] == "clear" || args[1] == "--clear")) {
      daemon_.audit_clear();
      return "Đã xoá sạch nhật ký kiểm toán (0 sự kiện).";
    }
    const auto& audit = daemon_.enforcer().audit();
    if (audit.empty()) return "Nhật ký kiểm toán hộp cát: 0 sự kiện ghi nhận.";
    std::ostringstream os;
    os << "Nhật ký kiểm toán (" << audit.size() << " sự kiện):\n";
    size_t start = audit.size() > 10 ? audit.size() - 10 : 0;
    for (size_t i = start; i < audit.size(); ++i) {
      const auto& e = audit[i];
      os << "  [" << (e.decision.allowed ? "CHO PHÉP" : "TỪ CHỐI") << "] "
         << e.subject << " " << e.action.kind << " " << e.action.target
         << " (" << e.decision.reason << ")\n";
    }
    return os.str();
  }

  if (cmd == "cảnhbáo") {
    service::RpcError err;
    const auto v = daemon_.handle("guard.alerts", json::Value(json::Object{}), err);
    if (err.code) return "Lỗi: " + err.message;
    const auto& alerts = v["alerts"].arr();
    if (alerts.empty()) return "Hệ thống an toàn: 0 cảnh báo Guard.";
    std::ostringstream os;
    os << "Cảnh báo an ninh (" << alerts.size() << " cảnh báo):\n";
    for (const auto& a : alerts) {
      os << "  PID " << a["pid"].as_int() << " [" << a["name"].as_string() << "]: "
         << a["reason_vi"].as_string() << "\n";
    }
    return os.str();
  }

  if (cmd == "phiênbản") {
    service::RpcError err;
    const auto v = daemon_.handle("yuid.info", json::Value(nullptr), err);
    std::ostringstream os;
    os << "YuiVieOS Beta · Phiên bản: "
       << (v.is_object() && v.has("version") ? v["version"].as_string() : "0.4.0")
       << " · Giao thức: JSON-RPC 2.0 / MCP 2024-11-05 · C-ABI v1\n"
       << legal::kAsIsVi << "\n"
       << legal::kAsIsEn;
    return os.str();
  }

  if (cmd == "trakho") {
    const bool opened = !daemon_.options().vault_password.empty();
    std::ostringstream os;
    os << "Kho bí mật Vault: "
       << (opened ? "ĐÃ MỞ (sẵn sàng niêm phong/mở khoá)"
                  : "CHƯA KÍCH HOẠT (chạy yuid với --vault-key hoặc đặt YUI_VAULT_KEY)");
    return os.str();
  }

  if (cmd == "tìmweb") {
    if (args.size() < 2) return "Dùng: tìmweb <truy_vấn>";
    json::Value params(json::Object{});
    params.set("q", join_args(args, 1));
    service::RpcError err;
    const auto v = daemon_.handle("web.search", params, err);
    if (err.code) return "Lỗi: " + err.message;
    return "Tìm kiếm web: \"" + v["query"].as_string() + "\" [" + v["backend"].as_string() +
           "] — " + v["note"].as_string();
  }

  if (cmd == "xoámàn") {
    return "\033[2J\033[H";
  }

  if (cmd == "thờigian") {
    std::ostringstream os;
    os << "Phiên làm việc YuiVieOS · Lệnh đã chạy: " << history_
       << " · Giao diện: " << ui::path_name(layout_.path)
       << " (" << layout_.columns << "x" << layout_.rows << ")";
    return os.str();
  }

  if (cmd == "tươngthích") {
    if (args.size() == 1) {
      service::RpcError err;
      const auto r = daemon_.handle("compat.list", json::Value(json::Object{}), err);
      if (err.code != 0) return "Lỗi: " + err.message;
      std::ostringstream os;
      os << "Chế độ tương thích ứng dụng (Compatibility Mode):\n";
      const auto& arr = r["enabled_apps"].arr();
      if (arr.empty()) {
        os << "  (không có ứng dụng nào bật — mặc định TẮT cho toàn bộ ứng dụng)\n";
      } else {
        os << "  Ứng dụng đang bật:\n";
        for (const auto& a : arr) {
          os << "   • " << a.as_string() << "\n";
        }
      }
      os << "  Lượt hỏi xác nhận: " << r["prompt_count"].as_int()
         << " · Thẻ tạm thời đã cấp: " << r["grant_count"].as_int();
      return os.str();
    }
    const std::string target_app = args[1];
    if (args.size() == 2) {
      service::RpcError err;
      json::Value p(json::Object{});
      p.set("app", target_app);
      const auto r = daemon_.handle("compat.get", p, err);
      if (err.code != 0) return "Lỗi: " + err.message;
      return "Ứng dụng " + target_app + ": " + (r["enabled"].as_bool() ? "ĐANG BẬT" : "TẮT (mặc định)");
    }
    const std::string action = text::fold(args[2]);
    bool enable = (action == "bật" || action == "bat" || action == "on" || action == "1" || action == "true");
    service::RpcError err;
    json::Value p(json::Object{});
    p.set("app", target_app);
    p.set("enabled", enable);
    const auto r = daemon_.handle("compat.set", p, err);
    if (err.code != 0) return "Lỗi: " + err.message;
    return r["reason_vi"].as_string();
  }

  if (cmd == "tìmapp") {
    service::RpcError err;
    json::Value p(json::Object{});
    p.set("query", join_args(args, 1));
    const auto r = daemon_.handle("store.search", p, err);
    if (err.code != 0) return "Lỗi: " + err.message;
    if (!r["ok"].as_bool()) return r["reason_vi"].as_string();
    const auto& apps = r["apps"].arr();
    if (apps.empty()) return "Không tìm thấy ứng dụng nào trong Kho.";
    std::ostringstream os;
    os << "Kho Ứng Dụng — " << apps.size() << " kết quả:";
    for (const auto& a : apps) {
      os << "\n  · " << a["name"].as_string() << " " << a["version"].as_string() << " [" << a["id"].as_string()
         << "] — " << a["publisher"].as_string() << " · " << a["license"].as_string()
         << (a["installed"].as_bool() ? " · ĐÃ CÀI" : "");
    }
    return os.str();
  }

  if (cmd == "cài") {
    if (args.size() < 2) return "Cách dùng: cài <tên|mã>";
    service::RpcError err;
    json::Value p(json::Object{});
    p.set("app", join_args(args, 1));
    const auto r = daemon_.handle("store.install", p, err);
    if (err.code != 0) return "Lỗi: " + err.message;
    std::string out;
    if (r.has("plan_vi")) out = r["plan_vi"].as_string() + "\n";
    out += r["reason_vi"].as_string();
    return out;
  }

  if (cmd == "gỡ") {
    if (args.size() < 2) return "Cách dùng: gỡ <mã>";
    service::RpcError err;
    json::Value p(json::Object{});
    p.set("app", args[1]);
    const auto r = daemon_.handle("store.remove", p, err);
    if (err.code != 0) return "Lỗi: " + err.message;
    return r["reason_vi"].as_string();
  }

  if (cmd == "xoá") {
    if (args.size() < 2) return "Cách dùng: xoá <đường_dẫn>  (vào thùng rác, khôi phục được)";
    service::RpcError err;
    json::Value p(json::Object{});
    p.set("path", join_args(args, 1));
    const auto r = daemon_.handle("trash.put", p, err);
    if (err.code != 0) return "Lỗi: " + err.message;
    std::string out = r["reason_vi"].as_string();
    if (r.has("warning_vi")) out += "\n⚠ " + r["warning_vi"].as_string();
    return out;
  }

  if (cmd == "thùngrác") {
    service::RpcError err;
    const auto r = daemon_.handle("trash.list", json::Value(json::Object{}), err);
    if (err.code != 0) return "Lỗi: " + err.message;
    const auto& items = r["items"].arr();
    std::ostringstream os;
    os << "Thùng rác — " << items.size() << " món · " << r["usage_vi"].as_string();
    for (const auto& it : items) {
      os << "\n  · [" << it["id"].as_string() << "] " << it["original_path"].as_string()
         << (it["is_dir"].as_bool() ? "/" : "") << " — " << it["deleted_at"].as_string()
         << (it["verified"].as_bool() ? "" : " · không có băm");
    }
    if (items.empty()) os << "\n  (trống)";
    return os.str();
  }

  // Mã món có thể chứa khoảng trắng ("báo cáo.txt"): lấy tiền tố dài nhất khớp
  // một mã đang có trong thùng; phần còn lại (nếu có) là đường dẫn đích.
  auto trash_id = [&](std::string& rest) -> std::string {
    service::RpcError lerr;
    const auto lst = daemon_.handle("trash.list", json::Value(json::Object{}), lerr);
    for (size_t k = args.size(); k > 1; --k) {
      std::vector<std::string> head(args.begin(), args.begin() + long(k));
      const std::string cand = join_args(head, 1);
      for (const auto& it : lst["items"].arr()) {
        if (it["id"].as_string() == cand) {
          rest = join_args(args, k);
          return cand;
        }
      }
    }
    rest = join_args(args, 2);
    return args[1];
  };

  if (cmd == "khôiphục") {
    if (args.size() < 2) return "Cách dùng: khôiphục <mã> [đường_dẫn_mới]";
    service::RpcError err;
    json::Value p(json::Object{});
    std::string dest;
    p.set("id", trash_id(dest));
    if (!dest.empty()) p.set("to", dest);
    const auto r = daemon_.handle("trash.restore", p, err);
    if (err.code != 0) return "Lỗi: " + err.message;
    return r["reason_vi"].as_string();
  }

  if (cmd == "xoávĩnhviễn" || cmd == "đổrác" || cmd == "dọnrác") {
    if (cmd == "xoávĩnhviễn" && args.size() < 2) return "Cách dùng: xoávĩnhviễn <mã>";
    service::RpcError err;
    json::Value p(json::Object{});
    if (cmd == "xoávĩnhviễn") {
      std::string ignored;
      p.set("id", trash_id(ignored));
    }
    const char* method = cmd == "xoávĩnhviễn" ? "trash.purge" : cmd == "đổrác" ? "trash.empty" : "trash.clean";
    // Shell chỉ mở hộp thoại tin cậy; không tự xoá vĩnh viễn gì.
    const auto r = daemon_.handle(method, p, err);
    if (err.code != 0) return "Lỗi: " + err.message;
    std::string out;
    if (r.has("plan_vi")) out = r["plan_vi"].as_string() + "\n";
    out += r["reason_vi"].as_string();
    return out;
  }

  const std::string sug = suggest(cmd);
  std::string msg = "Không hiểu lệnh \"" + cmd + "\". Gõ \"giúp\" để xem danh sách lệnh.";
  if (!sug.empty()) {
    msg += " Có phải bạn muốn dùng \"" + sug + "\"?";
  }
  // Dòng không nhận ra KHÔNG tự chạy như lệnh hệ thống — chỉ gợi ý cách chạy rõ ràng.
  const char* path_env = std::getenv("PATH");
  const std::string prog = posix::find_program(args.empty() ? cmd : args[0], path_env ? path_env : "", "/");
  if (!prog.empty() && (args.empty() || args[0].find('/') == std::string::npos))
    msg += "\n\"" + (args.empty() ? cmd : args[0]) + "\" là chương trình hệ thống (" + prog +
           "). Shell không tự chạy; muốn chạy trong hộp cát: !" + (args.empty() ? cmd : args[0]) + " …";
  return msg;
}

}  // namespace yui::shell
