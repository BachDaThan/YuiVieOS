// Yui Store — lõi Kho Ứng Dụng (P4.15). Xem include/yui/store.hpp cho luồng
// đầy đủ và giới hạn đã biết. Clean-room, C++17, POSIX, không thư viện ngoài.
#include "yui/store.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <memory>
#include <sstream>

#include "yui/crypto.hpp"
#include "yui/json.hpp"
#include "yui/text.hpp"

namespace yui::store {
namespace {

using Bytes = std::vector<uint8_t>;

// ------------------------------------------------------------ tệp & thư mục --
bool read_file(const std::string& path, Bytes& out) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  out.clear();
  uint8_t buf[65536];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.insert(out.end(), buf, buf + n);
  std::fclose(f);
  return true;
}

bool read_text(const std::string& path, std::string& out) {
  Bytes b;
  if (!read_file(path, b)) return false;
  out.assign(b.begin(), b.end());
  return true;
}

bool is_dir(const std::string& p) {
  struct stat st{};
  return ::lstat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool is_regular(const std::string& p) {
  struct stat st{};
  return ::lstat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

bool path_exists(const std::string& p) {
  struct stat st{};
  return ::lstat(p.c_str(), &st) == 0;
}

int64_t file_size(const std::string& p) {
  struct stat st{};
  if (::stat(p.c_str(), &st) != 0) return -1;
  return int64_t(st.st_size);
}

bool mkdir_p(const std::string& path) {
  if (path.empty()) return false;
  std::string cur;
  for (size_t i = 0; i < path.size(); ++i) {
    cur += path[i];
    if (path[i] == '/' && cur.size() > 1) ::mkdir(cur.c_str(), 0755);
  }
  ::mkdir(path.c_str(), 0755);
  return is_dir(path);
}

void remove_tree(const std::string& path) {
  struct stat st{};
  if (::lstat(path.c_str(), &st) != 0) return;
  if (S_ISDIR(st.st_mode)) {
    if (DIR* d = ::opendir(path.c_str())) {
      std::vector<std::string> names;
      while (dirent* e = ::readdir(d)) {
        const std::string n = e->d_name;
        if (n != "." && n != "..") names.push_back(n);
      }
      ::closedir(d);
      for (const auto& n : names) remove_tree(path + "/" + n);
    }
    ::rmdir(path.c_str());
  } else {
    ::unlink(path.c_str());
  }
}

// Ghi nguyên tử: tmp → fsync → rename. Không bao giờ để lại tệp ghi dở.
bool write_atomic(const std::string& path, const std::string& data, std::string* err) {
  const size_t slash = path.rfind('/');
  if (slash != std::string::npos) mkdir_p(path.substr(0, slash));
  const std::string tmp = path + ".tmp." + std::to_string(::getpid());
  const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0) {
    if (err) *err = "không ghi được " + tmp + ": " + std::strerror(errno);
    return false;
  }
  size_t off = 0;
  while (off < data.size()) {
    const ssize_t w = ::write(fd, data.data() + off, data.size() - off);
    if (w <= 0) {
      ::close(fd);
      ::unlink(tmp.c_str());
      if (err) *err = "ghi lỗi " + tmp;
      return false;
    }
    off += size_t(w);
  }
  ::fsync(fd);
  ::close(fd);
  if (::rename(tmp.c_str(), path.c_str()) != 0) {
    ::unlink(tmp.c_str());
    if (err) *err = "không đổi tên được " + tmp + " → " + path;
    return false;
  }
  return true;
}

bool copy_file(const std::string& from, const std::string& to, mode_t mode) {
  Bytes b;
  if (!read_file(from, b)) return false;
  FILE* f = std::fopen(to.c_str(), "wb");
  if (!f) return false;
  const bool ok = b.empty() || std::fwrite(b.data(), 1, b.size(), f) == b.size();
  std::fclose(f);
  ::chmod(to.c_str(), mode);
  return ok;
}

std::string sha256_file(const std::string& path) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return "";
  crypto::Sha256 h;
  uint8_t buf[65536];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) h.update(buf, n);
  std::fclose(f);
  return crypto::hex(h.final());
}

// Đếm tệp + byte trong một cây (cho báo cáo).
void tree_stats(const std::string& root, uint64_t& files, uint64_t& bytes) {
  DIR* d = ::opendir(root.c_str());
  if (!d) return;
  std::vector<std::string> names;
  while (dirent* e = ::readdir(d)) {
    const std::string n = e->d_name;
    if (n != "." && n != "..") names.push_back(n);
  }
  ::closedir(d);
  for (const auto& n : names) {
    const std::string p = root + "/" + n;
    struct stat st{};
    if (::lstat(p.c_str(), &st) != 0) continue;
    if (S_ISDIR(st.st_mode)) {
      tree_stats(p, files, bytes);
    } else if (S_ISREG(st.st_mode)) {
      ++files;
      bytes += uint64_t(st.st_size);
    }
  }
}

// ------------------------------------------------------------- kiểm trường --
bool valid_id(const std::string& id) {
  if (id.size() < 3 || id.size() > 64) return false;
  if (id.front() == '.' || id.back() == '.' || id.front() == '-') return false;
  if (id.find("..") != std::string::npos) return false;
  for (char c : id) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-';
    if (!ok) return false;
  }
  return true;
}

bool is_lower_hex(const std::string& s, size_t len) {
  if (s.size() != len) return false;
  for (char c : s)
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  return true;
}

bool has_control_chars(const std::string& s) {
  for (unsigned char c : s)
    if (c < 0x20 || c == 0x7f) return true;
  return false;
}

bool one_of(const std::string& s, std::initializer_list<const char*> opts) {
  for (const char* o : opts)
    if (s == o) return true;
  return false;
}

json::Value str_array(const std::vector<std::string>& v) {
  json::Value a(json::Array{});
  for (const auto& s : v) a.push(s);
  return a;
}

// Chính sách đã chuẩn hoá: app_id = id, không bao giờ Native, không admin.
sandbox::Policy normalized_policy(const sandbox::Policy& in, const std::string& id) {
  sandbox::Policy p = in;
  p.app_id = id;
  if (p.tier == sandbox::Tier::Native) p.tier = sandbox::Tier::Isolated;
  p.granted &= ~uint64_t(sandbox::kRightAdmin);
  return p;
}

json::Value entry_fields(const AppEntry& e) {
  json::Value v(json::Object{});
  v.set("id", e.id);
  v.set("name", e.name);
  v.set("version", e.version);
  v.set("publisher", e.publisher);
  v.set("kind", e.kind);
  v.set("format", e.format);
  v.set("source_url", e.source_url);
  v.set("sha256", e.sha256);
  v.set("size", static_cast<long long>(e.size));
  v.set("license", e.license);
  v.set("exec", e.exec);
  v.set("trim", e.trim);
  v.set("low_ram_args", e.low_ram_args);
  v.set("policy", normalized_policy(e.policy, e.id).to_json());
  return v;
}

std::string installed_to_json(const std::vector<InstalledApp>& apps) {
  json::Value root(json::Object{});
  root.set("format", "yui-store-installed/1");
  json::Value arr(json::Array{});
  for (const auto& a : apps) {
    json::Value v(json::Object{});
    v.set("id", a.id);
    v.set("name", a.name);
    v.set("version", a.version);
    v.set("publisher", a.publisher);
    v.set("kind", a.kind);
    v.set("sha256", a.sha256);
    v.set("signature", a.signature);
    v.set("app_dir", a.app_dir);
    v.set("policy_path", a.policy_path);
    v.set("menu_path", a.menu_path);
    v.set("installed_at", static_cast<long long>(a.installed_at));
    v.set("files", static_cast<long long>(a.files));
    v.set("bytes", static_cast<long long>(a.bytes));
    v.set("bytes_trimmed", static_cast<long long>(a.bytes_trimmed));
    arr.push(v);
  }
  root.set("apps", arr);
  return root.dump(true) + "\n";
}

Result make(bool ok, const std::string& code, const std::string& reason) {
  Result r;
  r.ok = ok;
  r.code = code;
  r.reason_vi = reason;
  return r;
}

std::string human_bytes(uint64_t n) {
  char buf[64];
  if (n >= 1024ull * 1024 * 1024)
    std::snprintf(buf, sizeof buf, "%.1f GiB", double(n) / (1024.0 * 1024 * 1024));
  else if (n >= 1024ull * 1024)
    std::snprintf(buf, sizeof buf, "%.1f MiB", double(n) / (1024.0 * 1024));
  else if (n >= 1024)
    std::snprintf(buf, sizeof buf, "%.1f KiB", double(n) / 1024.0);
  else
    std::snprintf(buf, sizeof buf, "%llu B", static_cast<unsigned long long>(n));
  return buf;
}

const char* signature_name_vi(sandbox::AppSignatureStatus s) {
  switch (s) {
    case sandbox::AppSignatureStatus::ValidTrusted: return "có chữ ký nhà phát hành, khoá tin cậy";
    case sandbox::AppSignatureStatus::InvalidSignature: return "CHỮ KÝ SAI";
    case sandbox::AppSignatureStatus::UntrustedKey: return "ký bằng khoá chưa tin cậy (xử lý như không chữ ký)";
    case sandbox::AppSignatureStatus::Unsigned: return "không có chữ ký nhà phát hành";
  }
  return "?";
}

// Thư mục tạm tự xoá khi ra khỏi phạm vi.
struct TempDir {
  std::string path;
  ~TempDir() {
    if (!path.empty()) remove_tree(path);
  }
};

}  // namespace

// ================================================================== URL ======
std::string url_scheme(const std::string& url) {
  const size_t p = url.find("://");
  if (p == std::string::npos || p == 0) return "";
  std::string s = url.substr(0, p);
  for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string url_host(const std::string& url) {
  const size_t p = url.find("://");
  if (p == std::string::npos) return "";
  size_t start = p + 3;
  size_t end = url.find_first_of("/?#", start);
  std::string auth = url.substr(start, end == std::string::npos ? std::string::npos : end - start);
  // Thông tin đăng nhập trong URL ("user@host") không được phép: dễ dùng để lừa host.
  if (auth.find('@') != std::string::npos) return "";
  if (!auth.empty() && auth[0] == '[') return "";  // IPv6 literal: không nhận trong danh mục
  const size_t colon = auth.find(':');
  if (colon != std::string::npos) {
    const std::string port = auth.substr(colon + 1);
    if (port.empty() || port.find_first_not_of("0123456789") != std::string::npos) return "";
    auth = auth.substr(0, colon);
  }
  for (auto& c : auth) c = char(std::tolower(static_cast<unsigned char>(c)));
  for (char c : auth)
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-')) return "";
  return auth;
}

// ============================================================ bỏ dấu ========
std::string fold_unaccent(const std::string& s) {
  static const std::map<uint32_t, char> table = [] {
    std::map<uint32_t, char> m;
    const std::pair<const char*, char> groups[] = {
        {"àáảãạăằắẳẵặâầấẩẫậ", 'a'}, {"èéẻẽẹêềếểễệ", 'e'}, {"ìíỉĩị", 'i'},
        {"òóỏõọôồốổỗộơờớởỡợ", 'o'}, {"ùúủũụưừứửữự", 'u'}, {"ỳýỷỹỵ", 'y'},
        {"đ", 'd'}};
    for (const auto& g : groups) {
      const std::string str = g.first;
      size_t pos = 0;
      while (pos < str.size()) m[text::next_codepoint(str, pos)] = g.second;
    }
    return m;
  }();
  const std::string f = text::fold(s);
  std::string out;
  size_t pos = 0;
  while (pos < f.size()) {
    const uint32_t cp = text::next_codepoint(f, pos);
    if (cp >= 0x300 && cp <= 0x36f) continue;  // dấu kết hợp (NFD)
    const auto it = table.find(cp);
    if (it != table.end()) out += it->second;
    else text::append_codepoint(out, cp);
  }
  return out;
}

bool safe_relative_path(const std::string& p) {
  if (p.empty() || p.size() > 1024 || p[0] == '/') return false;
  if (p.find('\\') != std::string::npos || p.find('\0') != std::string::npos) return false;
  for (const auto& part : text::split(p, '/')) {
    if (part.empty() || part == "." || part == "..") return false;
  }
  return true;
}

// ======================================================= chuẩn hoá & ký =====
std::string entry_canonical(const AppEntry& e) {
  json::Value v(json::Object{});
  v.set("format", "yui-store-entry/1");
  const json::Value fields = entry_fields(e);  // giữ sống: không lặp trên đối tượng tạm
  for (const auto& m : fields.obj()) v.set(m.first, m.second);
  return v.dump(false);
}

std::string canonical(const Catalog& c) {
  std::vector<const AppEntry*> sorted;
  for (const auto& a : c.apps) sorted.push_back(&a);
  std::sort(sorted.begin(), sorted.end(), [](const AppEntry* a, const AppEntry* b) { return a->id < b->id; });
  std::vector<std::string> hosts = c.allowed_hosts;
  std::sort(hosts.begin(), hosts.end());
  json::Value v(json::Object{});
  v.set("format", c.format);
  v.set("key_id", c.key_id);
  v.set("issued", c.issued);
  v.set("allowed_hosts", str_array(hosts));
  json::Value apps(json::Array{});
  for (const AppEntry* a : sorted) {
    json::Value e = entry_fields(*a);
    e.set("signer_key_id", a->signer_key_id);
    e.set("signature", a->signature_hex);
    apps.push(e);
  }
  v.set("apps", apps);
  return v.dump(false);
}

std::string catalog_to_json(const Catalog& c, bool pretty) {
  json::Value v(json::Object{});
  v.set("format", c.format);
  v.set("key_id", c.key_id);
  v.set("issued", c.issued);
  v.set("allowed_hosts", str_array(c.allowed_hosts));
  json::Value apps(json::Array{});
  for (const auto& a : c.apps) {
    json::Value e = entry_fields(a);
    if (!a.signature_hex.empty()) {
      e.set("signer_key_id", a.signer_key_id);
      e.set("signature", a.signature_hex);
    }
    apps.push(e);
  }
  v.set("apps", apps);
  return v.dump(pretty) + (pretty ? "\n" : "");
}

std::string sign_catalog(const Catalog& c, const ed25519::Seed& seed) {
  const std::string msg = canonical(c);
  const auto sig = ed25519::sign(seed, msg.data(), msg.size());
  return ed25519::to_hex(sig.data(), sig.size());
}

std::string sign_entry(const AppEntry& e, const ed25519::Seed& seed) {
  const std::string msg = entry_canonical(e);
  const auto sig = ed25519::sign(seed, msg.data(), msg.size());
  return ed25519::to_hex(sig.data(), sig.size());
}

bool parse_catalog(const std::string& json_text, Catalog& out, std::string* err) {
  auto fail = [&](const std::string& m) {
    if (err) *err = m;
    return false;
  };
  const auto r = json::parse(json_text);
  if (!r.ok || !r.value.is_object()) return fail("danh mục không phải JSON object: " + r.error);
  const json::Value& v = r.value;
  Catalog c;
  c.format = v["format"].as_string();
  if (c.format != "yui-store-catalog/1") return fail("định dạng danh mục không hỗ trợ: '" + c.format + "'");
  c.key_id = v["key_id"].as_string();
  if (c.key_id.empty()) return fail("danh mục thiếu key_id");
  c.issued = v["issued"].as_string();
  for (const auto& h : v["allowed_hosts"].arr()) {
    const std::string host = h.as_string();
    if (host.empty() || url_host("https://" + host + "/") != host)
      return fail("allowed_hosts có host không hợp lệ: '" + host + "'");
    c.allowed_hosts.push_back(host);
  }
  if (!v["apps"].is_array()) return fail("danh mục thiếu mảng apps");
  std::map<std::string, bool> seen;
  for (const auto& a : v["apps"].arr()) {
    AppEntry e;
    e.id = a["id"].as_string();
    if (!valid_id(e.id)) return fail("id ứng dụng không hợp lệ: '" + e.id + "'");
    if (seen.count(e.id)) return fail("hai mục cùng id trong danh mục: " + e.id);
    seen[e.id] = true;
    const std::string where = " (" + e.id + ")";
    e.name = a["name"].as_string();
    e.version = a["version"].as_string();
    e.publisher = a["publisher"].as_string();
    e.kind = a["kind"].as_string("native");
    e.format = a["format"].as_string("tar");
    e.source_url = a["source_url"].as_string();
    e.sha256 = a["sha256"].as_string();
    if (!a["size"].is_number() || a["size"].as_int() <= 0) return fail("size phải là số > 0" + where);
    e.size = uint64_t(a["size"].as_int());
    e.license = a["license"].as_string();
    e.exec = a["exec"].as_string();
    e.trim = a["trim"].as_bool(true);
    e.low_ram_args = a["low_ram_args"].as_string();
    e.signer_key_id = a["signer_key_id"].as_string();
    e.signature_hex = a["signature"].as_string();

    if (e.name.empty() || e.version.empty() || e.publisher.empty() || e.license.empty())
      return fail("thiếu name/version/publisher/license" + where);
    if (has_control_chars(e.name) || has_control_chars(e.version) || has_control_chars(e.publisher) ||
        has_control_chars(e.license) || has_control_chars(e.low_ram_args) || has_control_chars(e.exec))
      return fail("trường chứa ký tự điều khiển" + where);
    if (!one_of(e.kind, {"native", "wine", "apk"})) return fail("kind không hỗ trợ: " + e.kind + where);
    if (!one_of(e.format, {"tar", "yuip", "file"})) return fail("format không hỗ trợ: " + e.format + where);
    if (!is_lower_hex(e.sha256, 64)) return fail("sha256 phải là 64 hex thường" + where);
    const std::string scheme = url_scheme(e.source_url);
    if (scheme == "https") {
      if (url_host(e.source_url).empty()) return fail("source_url có host không hợp lệ" + where);
    } else if (scheme != "file") {
      return fail("source_url chỉ nhận https:// (hoặc file:// khi thử nghiệm)" + where);
    }
    if (!safe_relative_path(e.exec)) return fail("exec phải là đường dẫn tương đối an toàn" + where);
    if (e.format == "file" && e.exec.find('/') != std::string::npos)
      return fail("format=file: exec phải là tên tệp, không có thư mục" + where);
    if (!e.signature_hex.empty() && (!is_lower_hex(e.signature_hex, 128) || e.signer_key_id.empty()))
      return fail("chữ ký nhà phát hành phải là 128 hex kèm signer_key_id" + where);

    if (a.has("policy")) {
      json::Value pv = a["policy"];
      if (!pv.is_object()) return fail("policy phải là object" + where);
      pv.set("app", e.id);
      for (const auto& cap : pv["capabilities"].arr())
        if (cap.as_string() == "*") return fail("policy '*' (mọi quyền) không được phép trong Kho" + where);
      for (const auto& sc : pv["scopes"].arr())
        if (sc.as_string() == "*") return fail("phạm vi '*' không được phép trong Kho" + where);
      std::string perr;
      e.policy = sandbox::Policy::from_json(pv, &perr);
      if (!perr.empty()) return fail("policy lỗi: " + perr + where);
      if (e.policy.granted & sandbox::kRightAdmin) return fail("ứng dụng trong Kho không được xin quyền admin" + where);
    } else {
      e.policy = sandbox::Policy{};
    }
    e.policy = normalized_policy(e.policy, e.id);
    c.apps.push_back(std::move(e));
  }
  out = std::move(c);
  return true;
}

// ================================================================ tar =======
namespace {

uint64_t octal(const uint8_t* p, size_t n, bool& ok) {
  uint64_t v = 0;
  size_t i = 0;
  while (i < n && (p[i] == ' ' || p[i] == 0)) ++i;  // khoảng trắng đầu
  bool any = false;
  for (; i < n; ++i) {
    if (p[i] == 0 || p[i] == ' ') break;
    if (p[i] < '0' || p[i] > '7') {
      ok = false;
      return 0;
    }
    v = (v << 3) | uint64_t(p[i] - '0');
    any = true;
  }
  if (!any) v = 0;
  return v;
}

std::string cstr(const uint8_t* p, size_t n) {
  size_t len = 0;
  while (len < n && p[len]) ++len;
  return std::string(reinterpret_cast<const char*>(p), len);
}

void put_octal(uint8_t* p, size_t n, uint64_t v) {
  // n-1 chữ số bát phân + NUL.
  std::string s(n - 1, '0');
  for (size_t i = n - 1; i-- > 0;) {
    s[i] = char('0' + (v & 7));
    v >>= 3;
  }
  std::memcpy(p, s.data(), n - 1);
  p[n - 1] = 0;
}

}  // namespace

bool parse_tar(const std::vector<uint8_t>& data, std::vector<apppack::Entry>& out, std::string* err) {
  auto fail = [&](const std::string& m) {
    if (err) *err = m;
    return false;
  };
  out.clear();
  size_t off = 0;
  std::string next_name;  // từ pax 'x' path= hoặc GNU 'L'
  bool ended = false;
  while (off + 512 <= data.size()) {
    const uint8_t* h = data.data() + off;
    bool zero = true;
    for (size_t i = 0; i < 512; ++i)
      if (h[i]) {
        zero = false;
        break;
      }
    if (zero) {
      ended = true;
      break;
    }
    bool ok = true;
    const uint64_t want_sum = octal(h + 148, 8, ok);
    uint64_t sum = 0;
    for (size_t i = 0; i < 512; ++i) sum += (i >= 148 && i < 156) ? uint8_t(' ') : h[i];
    if (!ok || sum != want_sum) return fail("tar hỏng: sai checksum header tại byte " + std::to_string(off));
    if (h[124] & 0x80) return fail("tar dùng kích thước base-256 (tệp quá lớn) — chưa hỗ trợ");
    const uint64_t size = octal(h + 124, 12, ok);
    const uint64_t mode = octal(h + 100, 8, ok);
    if (!ok) return fail("tar hỏng: trường số không phải bát phân");
    const char type = char(h[156]);
    const size_t data_off = off + 512;
    const uint64_t padded = (size + 511) / 512 * 512;
    if (data_off + size > data.size())
      return fail("tar bị cắt cụt (tải dở?)");

    std::string name = cstr(h, 100);
    if (std::memcmp(h + 257, "ustar", 5) == 0) {
      const std::string prefix = cstr(h + 345, 155);
      if (!prefix.empty()) name = prefix + "/" + name;
    }
    if (!next_name.empty()) {
      name = next_name;
      next_name.clear();
    }

    const uint8_t* body = data.data() + data_off;
    if (type == 'x') {
      // pax: các bản ghi "<len> key=value\n"; chỉ cần path.
      size_t p = 0;
      while (p < size) {
        size_t sp = p;
        while (sp < size && body[sp] != ' ') ++sp;
        if (sp >= size) break;
        const size_t rec_len = size_t(std::strtoul(std::string(reinterpret_cast<const char*>(body + p), sp - p).c_str(), nullptr, 10));
        if (rec_len == 0 || p + rec_len > size) return fail("tar hỏng: bản ghi pax sai độ dài");
        const std::string rec(reinterpret_cast<const char*>(body + sp + 1), rec_len - (sp + 1 - p) - 1);
        if (text::starts_with(rec, "path=")) next_name = rec.substr(5);
        if (text::starts_with(rec, "linkpath=")) return fail("tar chứa liên kết (pax linkpath) — bị từ chối");
        p += rec_len;
      }
    } else if (type == 'g') {
      // header pax toàn cục: bỏ qua
    } else if (type == 'L') {
      next_name = cstr(body, size_t(size));
    } else if (type == '0' || type == '\0' || type == '7' || type == '5') {
      while (text::starts_with(name, "./")) name = name.substr(2);
      const bool is_dir_entry = type == '5' || (!name.empty() && name.back() == '/');
      while (!name.empty() && name.back() == '/') name.pop_back();
      if (name.empty() && is_dir_entry) {
        // "./" gốc
      } else if (!safe_relative_path(name)) {
        return fail("đường dẫn không an toàn trong gói (chống zip-slip): '" + name + "'");
      } else if (!is_dir_entry) {
        apppack::Entry e;
        e.path = name;
        e.mode = uint32_t(mode & 0755);
        if (e.mode == 0) e.mode = 0644;
        e.kind = (e.mode & 0111) ? 'x' : 'f';
        e.data.assign(body, body + size);
        e.size = size;
        out.push_back(std::move(e));
      }
    } else if (type == '1' || type == '2') {
      return fail("gói chứa liên kết '" + name + "' — bị từ chối (chống thoát thư mục)");
    } else {
      return fail(std::string("gói chứa loại mục không hỗ trợ ('") + type + "') tại '" + name + "'");
    }
    off = data_off + size_t(padded);
  }
  if (!ended) return fail("tar bị cắt cụt (thiếu khối kết thúc) — tải dở?");
  return true;
}

std::vector<uint8_t> build_tar(const std::vector<apppack::Entry>& entries) {
  std::vector<uint8_t> out;
  for (const auto& e : entries) {
    uint8_t h[512] = {};
    std::string name = e.path;
    std::string prefix;
    if (name.size() > 99) {
      const size_t cut = name.rfind('/', 154);
      if (cut != std::string::npos) {
        prefix = name.substr(0, cut);
        name = name.substr(cut + 1);
      }
    }
    std::memcpy(h, name.data(), std::min<size_t>(name.size(), 100));
    std::memcpy(h + 345, prefix.data(), std::min<size_t>(prefix.size(), 155));
    put_octal(h + 100, 8, e.mode ? e.mode : 0644);
    put_octal(h + 108, 8, 0);
    put_octal(h + 116, 8, 0);
    const bool link = e.kind == 'l';
    const bool dir = e.kind == 'D';
    put_octal(h + 124, 12, (link || dir) ? 0 : e.data.size());
    put_octal(h + 136, 12, 0);
    h[156] = link ? '2' : dir ? '5' : '0';
    if (link) std::memcpy(h + 157, e.data.data(), std::min<size_t>(e.data.size(), 100));
    std::memcpy(h + 257, "ustar", 6);
    h[263] = '0';
    h[264] = '0';
    std::memset(h + 148, ' ', 8);
    uint64_t sum = 0;
    for (uint8_t b : h) sum += b;
    put_octal(h + 148, 7, sum);
    h[155] = ' ';
    out.insert(out.end(), h, h + 512);
    if (!link && !dir) {
      out.insert(out.end(), e.data.begin(), e.data.end());
      out.resize((out.size() + 511) / 512 * 512, 0);
    }
  }
  out.resize(out.size() + 1024, 0);
  return out;
}

// ============================================================== tải =========
bool FileFetcher::fetch(const std::string& url, const std::string& dest, std::string& effective_url,
                        std::string* err) {
  effective_url = url;
  if (url_scheme(url) != "file") {
    if (err) *err = "FileFetcher chỉ nhận file://";
    return false;
  }
  const std::string src = url.substr(7);
  FILE* in = std::fopen(src.c_str(), "rb");
  if (!in) {
    if (err) *err = "không mở được " + src;
    return false;
  }
  const int64_t have = std::max<int64_t>(0, file_size(dest));
  if (std::fseek(in, long(have), SEEK_SET) != 0) {
    std::fclose(in);
    if (err) *err = "không nối tiếp được tại byte " + std::to_string(have);
    return false;
  }
  FILE* out = std::fopen(dest.c_str(), "ab");
  if (!out) {
    std::fclose(in);
    if (err) *err = "không ghi được " + dest;
    return false;
  }
  uint8_t buf[65536];
  size_t n;
  bool ok = true;
  while ((n = std::fread(buf, 1, sizeof buf, in)) > 0) {
    if (std::fwrite(buf, 1, n, out) != n) {
      ok = false;
      break;
    }
  }
  std::fclose(in);
  std::fclose(out);
  if (!ok && err) *err = "ghi lỗi " + dest;
  return ok;
}

bool CurlFetcher::fetch(const std::string& url, const std::string& dest, std::string& effective_url,
                        std::string* err) {
  effective_url.clear();
  if (url_scheme(url) != "https") {
    if (err) *err = "CurlFetcher chỉ tải https://";
    return false;
  }
  int pipefd[2];
  if (::pipe(pipefd) != 0) {
    if (err) *err = "pipe() lỗi";
    return false;
  }
  const pid_t pid = ::fork();
  if (pid < 0) {
    ::close(pipefd[0]);
    ::close(pipefd[1]);
    if (err) *err = "fork() lỗi";
    return false;
  }
  if (pid == 0) {
    ::dup2(pipefd[1], 1);
    ::close(pipefd[0]);
    ::close(pipefd[1]);
    std::vector<std::string> args = {curl_, "--fail", "--silent", "--show-error", "--location",
                                     "--proto", "=https", "--proto-redir", "=https",
                                     "--max-redirs", "5", "--continue-at", "-",
                                     "--output", dest, "--write-out", "%{url_effective}", "--", url};
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(a.data());
    argv.push_back(nullptr);
    ::execvp(argv[0], argv.data());
    ::_exit(127);
  }
  ::close(pipefd[1]);
  std::string captured;
  char buf[1024];
  ssize_t n;
  while ((n = ::read(pipefd[0], buf, sizeof buf)) > 0) captured.append(buf, size_t(n));
  ::close(pipefd[0]);
  int status = 0;
  ::waitpid(pid, &status, 0);
  effective_url = text::trim(captured);
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    const int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    if (err)
      *err = code == 127 ? "không chạy được '" + curl_ + "' (chưa cài curl?)"
                         : "curl thoát mã " + std::to_string(code);
    return false;
  }
  return true;
}

// ========================================================= kế hoạch ========
std::string InstallPlan::summary_vi() const {
  std::ostringstream os;
  os << "Cài \"" << entry.name << "\" " << entry.version << " (" << entry.id << ")\n";
  os << "  Nhà phát hành : " << entry.publisher << "\n";
  os << "  Giấy phép     : " << entry.license << "\n";
  os << "  Nguồn tải     : " << entry.source_url << "\n";
  os << "  Dung lượng    : " << human_bytes(entry.size) << " · sha256 " << entry.sha256.substr(0, 16) << "…\n";
  os << "  Loại          : " << entry.kind << " (" << entry.format << ")"
     << (entry.trim ? " · gọt cục bộ sau khi tải" : " · không gọt (nhà phát hành không cho phép)") << "\n";
  os << "  Chữ ký        : " << signature_name_vi(app_signature) << "\n";
  const std::string rights = sandbox::rights_to_string(enforced_policy.granted);
  os << "  Hộp cát       : " << sandbox::tier_name(enforced_policy.tier) << " · quyền sẽ cấp: "
     << (rights.empty() ? "(không)" : rights) << "\n";
  if (!enforced_policy.scopes.empty()) os << "  Phạm vi       : " << text::join(enforced_policy.scopes, ", ") << "\n";
  if (already_installed) os << "  Đang cài      : bản " << installed_version << " (sẽ được thay)\n";
  os << "  Lưu ý         : bản gọt chỉ nằm trên máy bạn; Kho không phân phối lại.";
  return os.str();
}

bool TrustedDialogConfirmation::confirmed(const InstallPlan& plan) {
  return host_.is_confirmed(dialog_id_) && host_.dialog_target(dialog_id_) == target_for(plan.entry.id);
}

bool TtyConfirmation::confirmed(const InstallPlan& plan) {
  const int fd = ::open("/dev/tty", O_RDWR | O_CLOEXEC);
  if (fd < 0) return false;
  const std::string prompt = plan.summary_vi() + "\n\nGõ lại đúng mã ứng dụng \"" + plan.entry.id +
                             "\" rồi Enter để xác nhận (Enter trống = huỷ): ";
  if (::write(fd, prompt.data(), prompt.size()) < 0) {
    ::close(fd);
    return false;
  }
  std::string line;
  char c;
  while (::read(fd, &c, 1) == 1 && c != '\n') {
    line += c;
    if (line.size() > 256) break;
  }
  ::close(fd);
  return text::trim(line) == plan.entry.id;
}

// ============================================================== Store ======
Store::Store(Options opts, const bundle::Keyring& keyring) : opts_(std::move(opts)), keyring_(keyring) {
  // Gốc tương đối ⇒ tuyệt đối (apppack::extract dựng thư mục theo đường dẫn tuyệt đối).
  if (!opts_.root.empty() && opts_.root[0] != '/') {
    char cwd[4096];
    if (::getcwd(cwd, sizeof cwd)) opts_.root = std::string(cwd) + "/" + opts_.root;
  }
  while (opts_.root.size() > 1 && opts_.root.back() == '/') opts_.root.pop_back();
}

Result Store::load_catalog(const std::string& catalog_json, const std::string& sig_hex_in) {
  loaded_ = false;
  Catalog c;
  std::string err;
  if (!parse_catalog(catalog_json, c, &err)) return make(false, "bad_catalog", "Danh mục không hợp lệ: " + err);
  const std::string sig_hex = text::trim(sig_hex_in);
  if (sig_hex.empty()) return make(false, "unsigned_catalog", "Danh mục chưa ký: từ chối (Kho chỉ nhận danh mục ký Ed25519)");
  if (!keyring_.contains(c.key_id))
    return make(false, "untrusted_key", "Danh mục ký bằng khoá '" + c.key_id + "' không có trong keyring: từ chối");
  ed25519::PublicKey pub{};
  keyring_.get(c.key_id, pub);
  ed25519::Signature sig{};
  if (!is_lower_hex(sig_hex, 128) || !ed25519::from_hex(sig_hex, sig.data(), sig.size()))
    return make(false, "bad_signature", "Chữ ký danh mục không đúng định dạng (cần 128 hex): từ chối");
  const std::string msg = canonical(c);
  if (!ed25519::verify(pub, msg.data(), msg.size(), sig))
    return make(false, "bad_signature", "Chữ ký danh mục không khớp — danh mục đã bị sửa hoặc ký sai: từ chối");
  catalog_ = std::move(c);
  loaded_ = true;
  Result r = make(true, "ok", "Đã nạp danh mục " + std::to_string(catalog_.apps.size()) + " ứng dụng, ký bởi '" +
                                  catalog_.key_id + "'");
  return r;
}

Result Store::load_catalog_files() {
  std::string json_text, sig;
  if (!read_text(opts_.root + "/catalog.json", json_text))
    return make(false, "no_catalog", "Chưa có danh mục tại " + opts_.root + "/catalog.json");
  read_text(opts_.root + "/catalog.sig", sig);
  return load_catalog(json_text, sig);
}

bool Store::save_catalog(const std::string& catalog_json, const std::string& sig_hex, std::string* err) {
  return write_atomic(opts_.root + "/catalog.json", catalog_json, err) &&
         write_atomic(opts_.root + "/catalog.sig", text::trim(sig_hex) + "\n", err);
}

std::vector<AppEntry> Store::search(const std::string& query) const {
  std::vector<AppEntry> out;
  const std::string q = fold_unaccent(text::trim(query));
  for (const auto& a : catalog_.apps) {
    if (q.empty() || fold_unaccent(a.name).find(q) != std::string::npos ||
        fold_unaccent(a.id).find(q) != std::string::npos ||
        fold_unaccent(a.publisher).find(q) != std::string::npos)
      out.push_back(a);
  }
  std::sort(out.begin(), out.end(), [](const AppEntry& a, const AppEntry& b) {
    return a.name != b.name ? a.name < b.name : a.id < b.id;
  });
  return out;
}

Result Store::resolve(const std::string& name_or_id, AppEntry& out) const {
  if (!loaded_) return make(false, "no_catalog", "Chưa nạp danh mục đã ký");
  for (const auto& a : catalog_.apps)
    if (a.id == name_or_id) {
      out = a;
      return make(true, "ok", "");
    }
  const std::string q = fold_unaccent(text::trim(name_or_id));
  std::vector<const AppEntry*> hits;
  for (const auto& a : catalog_.apps)
    if (fold_unaccent(a.name) == q) hits.push_back(&a);
  if (hits.size() == 1) {
    out = *hits[0];
    return make(true, "ok", "");
  }
  if (hits.size() > 1) {
    Result r = make(false, "ambiguous",
                    "Có " + std::to_string(hits.size()) + " ứng dụng tên \"" + name_or_id +
                        "\" — hãy chọn theo mã (id):");
    for (const auto* h : hits) {
      r.candidates.push_back(h->id);
      r.reason_vi += "\n  · " + h->id + " — " + h->publisher;
    }
    return r;
  }
  return make(false, "not_found", "Không có ứng dụng \"" + name_or_id + "\" trong danh mục");
}

Result Store::plan(const std::string& name_or_id, InstallPlan& out) const {
  AppEntry e;
  Result r = resolve(name_or_id, e);
  if (!r.ok) return r;
  InstallPlan p;
  p.entry = e;
  // Chữ ký nhà phát hành → P3.12 quyết chính sách thật sự được áp.
  sandbox::AppInstallPackage pkg;
  pkg.app_id = e.id;
  pkg.manifest_canonical = entry_canonical(e);
  pkg.sig_hex = e.signature_hex;
  pkg.signer_key_id = e.signer_key_id;
  pkg.declared_policy = e.policy;
  pkg.user_confirmed = true;  // kế hoạch giả định đã xác nhận; install() mới thật sự đòi xác nhận
  const sandbox::AppSignatureGate gate(keyring_);
  const auto dec = gate.evaluate(pkg);
  p.app_signature = dec.sig_status;
  if (dec.sig_status == sandbox::AppSignatureStatus::InvalidSignature)
    return make(false, "bad_app_signature", "Chữ ký nhà phát hành của " + e.id + " không khớp: " + dec.reason_vi);
  p.enforced_policy = normalized_policy(dec.enforced_policy, e.id);

  const std::string scheme = url_scheme(e.source_url);
  p.source_host = url_host(e.source_url);
  if (scheme == "https") {
    if (std::find(catalog_.allowed_hosts.begin(), catalog_.allowed_hosts.end(), p.source_host) ==
        catalog_.allowed_hosts.end())
      return make(false, "host_not_allowed",
                  "Nguồn tải '" + p.source_host + "' không nằm trong danh sách host chính thức của danh mục: từ chối");
  } else if (scheme == "file") {
    if (!opts_.allow_file_urls)
      return make(false, "scheme_not_allowed", "file:// chỉ được dùng ở chế độ thử nghiệm (allow_file_urls)");
  } else {
    return make(false, "scheme_not_allowed", "Chỉ tải qua https://");
  }
  if (e.size > opts_.max_package_bytes)
    return make(false, "too_large", "Gói " + human_bytes(e.size) + " vượt giới hạn " +
                                        human_bytes(opts_.max_package_bytes));
  InstalledApp cur;
  if (is_installed(e.id, &cur)) {
    p.already_installed = true;
    p.installed_version = cur.version;
    if (cur.publisher != e.publisher)
      return make(false, "publisher_mismatch",
                  "Mã " + e.id + " đang được cài từ nhà phát hành \"" + cur.publisher +
                      "\", danh mục giờ ghi \"" + e.publisher + "\": từ chối thay thế (gỡ bản cũ trước nếu bạn chắc chắn)");
  }
  out = p;
  return make(true, "ok", "");
}

std::string Store::cache_part_path(const AppEntry& e) const {
  return opts_.root + "/cache/" + e.id + "@" + e.version + ".part";
}

Result Store::install(const std::string& name_or_id, Fetcher& fetcher, ConfirmationGate& gate) {
  InstallPlan p;
  Result r = plan(name_or_id, p);
  if (!r.ok) return r;
  const AppEntry& e = p.entry;
  InstalledApp cur;
  if (p.already_installed && is_installed(e.id, &cur) && cur.version == e.version && cur.sha256 == e.sha256) {
    Result done = make(true, "already_installed", e.name + " " + e.version + " đã được cài, không cần làm gì");
    done.app = cur;
    return done;
  }

  // 1. Xác nhận tin cậy TRƯỚC mọi tác dụng phụ (kể cả tải).
  if (!gate.confirmed(p))
    return make(false, "not_confirmed", "Chưa có xác nhận tin cậy của người dùng: không tải, không cài " + e.id);

  // 2. Tải vào .part (nối tiếp nếu lần trước tải dở).
  if (!mkdir_p(opts_.root + "/cache")) return make(false, "io_error", "Không tạo được " + opts_.root + "/cache");
  const std::string part = cache_part_path(e);
  if (file_size(part) > int64_t(e.size)) ::unlink(part.c_str());
  if (file_size(part) < int64_t(e.size)) {
    std::string effective, ferr;
    const bool fetched = fetcher.fetch(e.source_url, part, effective, &ferr);
    if (url_scheme(e.source_url) == "https") {
      const std::string eff_host = url_host(effective.empty() ? e.source_url : effective);
      if (url_scheme(effective.empty() ? e.source_url : effective) != "https" ||
          std::find(catalog_.allowed_hosts.begin(), catalog_.allowed_hosts.end(), eff_host) ==
              catalog_.allowed_hosts.end()) {
        ::unlink(part.c_str());
        return make(false, "host_not_allowed",
                    "Chuyển hướng tới '" + eff_host + "' — ngoài danh sách host chính thức: đã xoá phần tải, từ chối");
      }
    }
    if (!fetched) {
      const int64_t have = file_size(part);
      if (have > int64_t(e.size)) ::unlink(part.c_str());
      return make(false, "download_incomplete",
                  "Tải chưa xong (" + human_bytes(uint64_t(std::max<int64_t>(have, 0))) + "/" + human_bytes(e.size) +
                      "): " + ferr + ". Phần đã tải được giữ để lần sau tải tiếp; chưa cài gì.");
    }
  }
  const int64_t got = file_size(part);
  if (got < int64_t(e.size))
    return make(false, "download_incomplete",
                "Tải chưa đủ (" + human_bytes(uint64_t(std::max<int64_t>(got, 0))) + "/" + human_bytes(e.size) +
                    "); phần đã tải được giữ để tải tiếp, chưa cài gì");
  if (got != int64_t(e.size)) {
    ::unlink(part.c_str());
    return make(false, "size_mismatch", "Kích thước tải về lệch danh mục: đã xoá, từ chối");
  }

  // 3. Băm.
  const std::string digest = sha256_file(part);
  if (digest != e.sha256) {
    ::unlink(part.c_str());
    return make(false, "hash_mismatch",
                "SHA-256 lệch danh mục (" + digest.substr(0, 16) + "… ≠ " + e.sha256.substr(0, 16) +
                    "…): gói có thể bị sửa trên đường truyền — đã xoá, từ chối");
  }

  // 4. Bung vào staging.
  TempDir staging;
  staging.path = opts_.root + "/staging/" + e.id + "." + std::to_string(::getpid()) + "." +
                 crypto::hex(crypto::random_bytes(4));
  if (!mkdir_p(staging.path)) return make(false, "io_error", "Không tạo được thư mục tạm");
  if (e.format == "tar") {
    Bytes data;
    std::vector<apppack::Entry> entries;
    std::string terr;
    if (!read_file(part, data) || !parse_tar(data, entries, &terr)) return make(false, "unsafe_path", "Gói bị từ chối: " + terr);
    if (!apppack::extract(entries, staging.path, &terr)) return make(false, "io_error", "Bung gói lỗi: " + terr);
  } else if (e.format == "yuip") {
    Bytes data;
    std::string uerr;
    if (!read_file(part, data) || !apppack::unpack_to(data, staging.path, &uerr))
      return make(false, "unsafe_path", "Gói .yuip bị từ chối: " + uerr);
  } else {
    if (!copy_file(part, staging.path + "/" + e.exec, e.kind == "native" ? 0755 : 0644))
      return make(false, "io_error", "Không chép được tệp");
  }
  if (!is_regular(staging.path + "/" + e.exec))
    return make(false, "missing_exec", "Gói không có tệp chạy '" + e.exec + "' như danh mục khai báo: từ chối");

  // 5. Gọt cục bộ (nếu được phép) rồi dựng thư mục mới.
  const std::string apps_dir = opts_.root + "/apps";
  mkdir_p(apps_dir);
  const std::string final_dir = apps_dir + "/" + e.id;
  const std::string new_dir = final_dir + ".new";
  remove_tree(new_dir);
  uint64_t before_files = 0, before_bytes = 0;
  tree_stats(staging.path, before_files, before_bytes);
  if (e.trim && e.format != "file") {
    std::vector<apppack::Entry> kept;
    apppack::TrimReport rep;
    std::string terr;
    if (!apppack::collect(staging.path, opts_.trim, kept, rep, &terr))
      return make(false, "trim_failed", "Gọt lỗi: " + terr);
    mkdir_p(new_dir);
    if (!apppack::extract(kept, new_dir, &terr)) {
      remove_tree(new_dir);
      return make(false, "io_error", "Ghi bản gọt lỗi: " + terr);
    }
    if (!is_regular(new_dir + "/" + e.exec)) {
      remove_tree(new_dir);
      return make(false, "trim_removed_exec", "Gọt đã bỏ mất tệp chạy '" + e.exec + "': huỷ cài (báo lỗi danh mục)");
    }
  } else {
    if (::rename(staging.path.c_str(), new_dir.c_str()) != 0)
      return make(false, "io_error", "Không chuyển được thư mục tạm");
    staging.path.clear();
  }

  // 6. Thay nguyên khối: cũ → .old, mới → chỗ chính, xoá .old.
  const std::string old_dir = final_dir + ".old";
  remove_tree(old_dir);
  if (path_exists(final_dir) && ::rename(final_dir.c_str(), old_dir.c_str()) != 0) {
    remove_tree(new_dir);
    return make(false, "io_error", "Không thay được bản cũ");
  }
  if (::rename(new_dir.c_str(), final_dir.c_str()) != 0) {
    if (path_exists(old_dir)) ::rename(old_dir.c_str(), final_dir.c_str());
    remove_tree(new_dir);
    return make(false, "io_error", "Không đặt được bản mới");
  }
  remove_tree(old_dir);

  // 7. Hộp cát + biểu tượng menu + sổ đã cài.
  std::string werr;
  const std::string policy_path = opts_.root + "/policies/" + e.id + ".policy.json";
  if (!write_atomic(policy_path, p.enforced_policy.to_json().dump(true) + "\n", &werr))
    return make(false, "io_error", werr);
  const std::string menu_path = opts_.root + "/menu/" + e.id + ".desktop";
  std::ostringstream desk;
  desk << "[Desktop Entry]\nType=Application\nName=" << e.name << "\n";
  desk << "Exec=" << final_dir << "/" << e.exec << (e.low_ram_args.empty() ? "" : " " + e.low_ram_args) << "\n";
  desk << "Comment=" << e.publisher << " · " << e.license << " · nguồn: " << e.source_url << "\n";
  desk << "X-Yui-App-Id=" << e.id << "\nX-Yui-Version=" << e.version << "\n";
  desk << "X-Yui-Policy=" << policy_path << "\nX-Yui-Sandbox-Tier=" << sandbox::tier_name(p.enforced_policy.tier)
       << "\n";
  if (!write_atomic(menu_path, desk.str(), &werr)) return make(false, "io_error", werr);

  InstalledApp rec;
  rec.id = e.id;
  rec.name = e.name;
  rec.version = e.version;
  rec.publisher = e.publisher;
  rec.kind = e.kind;
  rec.sha256 = e.sha256;
  rec.signature = p.app_signature == sandbox::AppSignatureStatus::ValidTrusted ? "valid" : "unsigned";
  rec.app_dir = final_dir;
  rec.policy_path = policy_path;
  rec.menu_path = menu_path;
  rec.installed_at = int64_t(std::time(nullptr));
  tree_stats(final_dir, rec.files, rec.bytes);
  rec.bytes_trimmed = before_bytes > rec.bytes ? before_bytes - rec.bytes : 0;
  std::vector<InstalledApp> all = installed();
  all.erase(std::remove_if(all.begin(), all.end(), [&](const InstalledApp& a) { return a.id == e.id; }), all.end());
  all.push_back(rec);
  if (!write_installed(all, &werr)) return make(false, "io_error", werr);
  ::unlink(part.c_str());  // gói gốc đã dùng xong; không giữ bản sao

  Result ok = make(true, "ok",
                   "Đã cài " + e.name + " " + e.version + " vào hộp cát " + sandbox::tier_name(p.enforced_policy.tier) +
                       " (" + std::to_string(rec.files) + " tệp, " + human_bytes(rec.bytes) +
                       (rec.bytes_trimmed ? ", gọt bớt " + human_bytes(rec.bytes_trimmed) : std::string()) + ")");
  ok.app = rec;
  return ok;
}

Result Store::remove(const std::string& id) {
  InstalledApp cur;
  if (!is_installed(id, &cur)) return make(false, "not_installed", "Ứng dụng " + id + " chưa được cài");
  if (!valid_id(id)) return make(false, "bad_id", "Mã ứng dụng không hợp lệ");
  remove_tree(opts_.root + "/apps/" + id);
  remove_tree(opts_.root + "/apps/" + id + ".new");
  remove_tree(opts_.root + "/apps/" + id + ".old");
  ::unlink((opts_.root + "/policies/" + id + ".policy.json").c_str());
  ::unlink((opts_.root + "/menu/" + id + ".desktop").c_str());
  // Bộ nhớ đệm + thư mục tạm còn sót của đúng app này (so tiền tố "<id>@" / "<id>.").
  for (const std::string sub : {"/cache", "/staging"}) {
    const std::string dir = opts_.root + sub;
    if (DIR* d = ::opendir(dir.c_str())) {
      std::vector<std::string> victims;
      while (dirent* ent = ::readdir(d)) {
        const std::string n = ent->d_name;
        if (text::starts_with(n, id + "@") || text::starts_with(n, id + ".")) victims.push_back(dir + "/" + n);
      }
      ::closedir(d);
      for (const auto& v : victims) remove_tree(v);
    }
  }
  std::vector<InstalledApp> all = installed();
  all.erase(std::remove_if(all.begin(), all.end(), [&](const InstalledApp& a) { return a.id == id; }), all.end());
  std::string werr;
  if (!write_installed(all, &werr)) return make(false, "io_error", werr);
  Result r = make(true, "ok", "Đã gỡ " + cur.name + " (" + id + "): xoá tệp, chính sách hộp cát, biểu tượng, bộ nhớ đệm");
  r.app = cur;
  return r;
}

std::vector<InstalledApp> Store::installed() const {
  std::vector<InstalledApp> out;
  std::string text_json;
  if (!read_text(opts_.root + "/installed.json", text_json)) return out;
  const auto r = json::parse(text_json);
  if (!r.ok) return out;
  for (const auto& a : r.value["apps"].arr()) {
    InstalledApp x;
    x.id = a["id"].as_string();
    x.name = a["name"].as_string();
    x.version = a["version"].as_string();
    x.publisher = a["publisher"].as_string();
    x.kind = a["kind"].as_string();
    x.sha256 = a["sha256"].as_string();
    x.signature = a["signature"].as_string();
    x.app_dir = a["app_dir"].as_string();
    x.policy_path = a["policy_path"].as_string();
    x.menu_path = a["menu_path"].as_string();
    x.installed_at = a["installed_at"].as_int();
    x.files = uint64_t(a["files"].as_int());
    x.bytes = uint64_t(a["bytes"].as_int());
    x.bytes_trimmed = uint64_t(a["bytes_trimmed"].as_int());
    if (valid_id(x.id)) out.push_back(x);
  }
  return out;
}

bool Store::is_installed(const std::string& id, InstalledApp* out) const {
  for (const auto& a : installed())
    if (a.id == id) {
      if (out) *out = a;
      return true;
    }
  return false;
}

bool Store::write_installed(const std::vector<InstalledApp>& apps, std::string* err) const {
  return write_atomic(opts_.root + "/installed.json", installed_to_json(apps), err);
}

}  // namespace yui::store
