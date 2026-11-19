// Thùng rác + dọn rác an toàn (P4.17). Xem include/yui/trash.hpp.
#include "yui/trash.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <sstream>

#include "yui/crypto.hpp"
#include "yui/sandbox.hpp"

namespace yui::trash {
namespace {

// ------------------------------------------------------------ tiện ích --
bool lstat_ok(const std::string& p, struct stat& st) { return ::lstat(p.c_str(), &st) == 0; }

bool exists_nofollow(const std::string& p) {
  struct stat st;
  return lstat_ok(p, st);
}

bool is_real_dir(const std::string& p) {
  struct stat st;
  return lstat_ok(p, st) && S_ISDIR(st.st_mode);
}

std::vector<std::string> list_dir(const std::string& dir) {
  std::vector<std::string> out;
  DIR* d = ::opendir(dir.c_str());
  if (!d) return out;
  while (const dirent* e = ::readdir(d)) {
    const std::string n = e->d_name;
    if (n == "." || n == "..") continue;
    out.push_back(n);
  }
  ::closedir(d);
  std::sort(out.begin(), out.end());
  return out;
}

bool mkdir_p(const std::string& path, mode_t mode = 0700) {
  if (path.empty()) return false;
  std::string cur;
  size_t i = 0;
  if (path[0] == '/') {
    cur = "/";
    i = 1;
  }
  while (i <= path.size()) {
    const size_t j = path.find('/', i);
    const std::string part = path.substr(i, j == std::string::npos ? std::string::npos : j - i);
    if (!part.empty()) {
      if (cur.size() > 1 || (cur.size() == 1 && cur[0] != '/')) cur += "/";
      cur += part;
      if (::mkdir(cur.c_str(), mode) != 0 && errno != EEXIST) return false;
      if (!is_real_dir(cur)) return false;
    }
    if (j == std::string::npos) break;
    i = j + 1;
  }
  return true;
}

// Xoá cả cây, KHÔNG đi theo liên kết (liên kết bị gỡ như một tệp).
bool remove_tree(const std::string& p) {
  struct stat st;
  if (!lstat_ok(p, st)) return errno == ENOENT;
  if (S_ISDIR(st.st_mode)) {
    bool ok = true;
    for (const auto& n : list_dir(p)) ok = remove_tree(p + "/" + n) && ok;
    return ::rmdir(p.c_str()) == 0 && ok;
  }
  return ::unlink(p.c_str()) == 0;
}

uint64_t tree_bytes(const std::string& p) {
  struct stat st;
  if (!lstat_ok(p, st)) return 0;
  if (S_ISDIR(st.st_mode)) {
    uint64_t sum = 0;
    for (const auto& n : list_dir(p)) sum += tree_bytes(p + "/" + n);
    return sum;
  }
  if (S_ISREG(st.st_mode) || S_ISLNK(st.st_mode)) return uint64_t(st.st_size);
  return 0;
}

bool copy_file(const std::string& from, const std::string& to, mode_t mode) {
  const int in = ::open(from.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (in < 0) return false;
  const int out = ::open(to.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (out < 0) {
    ::close(in);
    return false;
  }
  char buf[65536];
  bool ok = true;
  for (;;) {
    const ssize_t n = ::read(in, buf, sizeof buf);
    if (n < 0 && errno == EINTR) continue;
    if (n < 0) {
      ok = false;
      break;
    }
    if (n == 0) break;
    ssize_t off = 0;
    while (off < n) {
      const ssize_t w = ::write(out, buf + off, size_t(n - off));
      if (w < 0 && errno == EINTR) continue;
      if (w <= 0) {
        ok = false;
        break;
      }
      off += w;
    }
    if (!ok) break;
  }
  if (ok && ::fsync(out) != 0) ok = false;
  ::close(in);
  if (::fchmod(out, mode & 07777) != 0) ok = false;
  if (::close(out) != 0) ok = false;
  return ok;
}

bool copy_tree(const std::string& from, const std::string& to) {
  struct stat st;
  if (!lstat_ok(from, st)) return false;
  if (S_ISLNK(st.st_mode)) {
    std::string target(size_t(st.st_size) + 1, '\0');
    const ssize_t n = ::readlink(from.c_str(), &target[0], target.size());
    if (n < 0) return false;
    target.resize(size_t(n));
    return ::symlink(target.c_str(), to.c_str()) == 0;
  }
  if (S_ISREG(st.st_mode)) return copy_file(from, to, st.st_mode);
  if (S_ISDIR(st.st_mode)) {
    if (::mkdir(to.c_str(), 0700) != 0) return false;
    for (const auto& n : list_dir(from))
      if (!copy_tree(from + "/" + n, to + "/" + n)) return false;
    return ::chmod(to.c_str(), st.st_mode & 07777) == 0;
  }
  return false;
}

std::string base_name(const std::string& p) {
  const size_t k = p.find_last_of('/');
  return k == std::string::npos ? p : p.substr(k + 1);
}

std::string dir_name(const std::string& p) {
  const size_t k = p.find_last_of('/');
  if (k == std::string::npos) return ".";
  if (k == 0) return "/";
  return p.substr(0, k);
}

std::string strip_trailing_slashes(std::string p) {
  while (p.size() > 1 && p.back() == '/') p.pop_back();
  return p;
}

// Đường dẫn tuyệt đối với thư mục cha đã chuẩn hoá (realpath), thành phần cuối
// giữ nguyên ⇒ liên kết được xử lý như chính nó, không theo đích.
bool absolutize(const std::string& in, std::string& out, std::string* err) {
  std::string p = strip_trailing_slashes(in);
  if (p.empty()) {
    if (err) *err = "đường dẫn rỗng";
    return false;
  }
  if (p == "/") {
    out = "/";
    return true;
  }
  if (p[0] != '/') {
    char cwd[PATH_MAX];
    if (!::getcwd(cwd, sizeof cwd)) {
      if (err) *err = "không đọc được thư mục hiện tại";
      return false;
    }
    p = std::string(cwd) + "/" + p;
  }
  const std::string base = base_name(p);
  if (base.empty() || base == "." || base == "..") {
    if (err) *err = "tên cuối của đường dẫn không hợp lệ ('.', '..')";
    return false;
  }
  char buf[PATH_MAX];
  if (!::realpath(dir_name(p).c_str(), buf)) {
    if (err) *err = "thư mục cha không tồn tại: " + dir_name(p);
    return false;
  }
  const std::string parent = buf;
  out = parent == "/" ? "/" + base : parent + "/" + base;
  return true;
}

std::string canonical_or_self(const std::string& p) {
  char buf[PATH_MAX];
  if (::realpath(p.c_str(), buf)) return buf;
  return strip_trailing_slashes(p);
}

bool inside_or_equal(const std::string& p, const std::string& prefix) {
  if (prefix.empty()) return false;
  if (prefix == "/") return true;
  return p == prefix || (p.size() > prefix.size() && p.compare(0, prefix.size(), prefix) == 0 &&
                         p[prefix.size()] == '/');
}

std::string now_iso() {
  const std::time_t t = std::time(nullptr);
  std::tm tm{};
  ::localtime_r(&t, &tm);
  char buf[32];
  std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%S", &tm);
  return buf;
}

bool write_new_file(const std::string& path, const std::string& content) {
  // O_EXCL: giữ chỗ tên một cách nguyên tử (đúng gợi ý của đặc tả Trash).
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (fd < 0) return false;
  bool ok = ::write(fd, content.data(), content.size()) == ssize_t(content.size());
  ok = ::fsync(fd) == 0 && ok;
  ok = ::close(fd) == 0 && ok;
  if (!ok) ::unlink(path.c_str());
  return ok;
}

bool read_text(const std::string& path, std::string& out) {
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) return false;
  out.clear();
  char buf[4096];
  ssize_t n;
  while ((n = ::read(fd, buf, sizeof buf)) > 0) {
    out.append(buf, size_t(n));
    if (out.size() > 65536) break;
  }
  ::close(fd);
  return n >= 0;
}

bool valid_id(const std::string& id) {
  return !id.empty() && id != "." && id != ".." && id.find('/') == std::string::npos &&
         id.find('\0') == std::string::npos;
}

Result make(bool ok, const std::string& code, const std::string& reason) {
  Result r;
  r.ok = ok;
  r.code = code;
  r.reason_vi = reason;
  return r;
}

// Băm một nút (đệ quy).
bool hash_node(const std::string& p, crypto::Digest32& out, uint64_t& bytes, std::string* err) {
  struct stat st;
  if (!lstat_ok(p, st)) {
    if (err) *err = "không đọc được " + p;
    return false;
  }
  crypto::Sha256 h;
  char mode[16];
  std::snprintf(mode, sizeof mode, "%04o", unsigned(st.st_mode & 07777));
  if (S_ISREG(st.st_mode)) {
    const int fd = ::open(p.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
      if (err) *err = "không mở được " + p;
      return false;
    }
    crypto::Sha256 content;
    char buf[65536];
    ssize_t n;
    uint64_t total = 0;
    while ((n = ::read(fd, buf, sizeof buf)) > 0) {
      content.update(buf, size_t(n));
      total += uint64_t(n);
    }
    ::close(fd);
    if (n < 0) {
      if (err) *err = "đọc lỗi " + p;
      return false;
    }
    const auto c = content.final();
    h.update("F", 1);
    h.update(mode, 4);
    h.update(c.data(), c.size());
    bytes += total;
  } else if (S_ISLNK(st.st_mode)) {
    std::string target(size_t(st.st_size) + 1, '\0');
    const ssize_t n = ::readlink(p.c_str(), &target[0], target.size());
    if (n < 0) {
      if (err) *err = "không đọc được liên kết " + p;
      return false;
    }
    target.resize(size_t(n));
    h.update("L", 1);
    h.update(target);
    bytes += target.size();
  } else if (S_ISDIR(st.st_mode)) {
    h.update("D", 1);
    h.update(mode, 4);
    for (const auto& n : list_dir(p)) {
      crypto::Digest32 child;
      if (!hash_node(p + "/" + n, child, bytes, err)) return false;
      h.update(n);
      h.update("\0", 1);
      h.update(child.data(), child.size());
    }
  } else {
    if (err) *err = "tệp đặc biệt (fifo/socket/thiết bị) không đưa vào thùng rác được: " + p;
    return false;
  }
  out = h.final();
  return true;
}

std::string ini_value(const std::string& body, const std::string& key) {
  std::istringstream in(body);
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.compare(0, key.size() + 1, key + "=") == 0) return line.substr(key.size() + 1);
  }
  return "";
}

}  // namespace

// ============================================================ công khai ====
bool TrustedDialogGate::confirmed(const std::string& target, const std::string&) {
  return host_.is_confirmed(dialog_id_) && host_.dialog_target(dialog_id_) == target;
}

std::string human_bytes(uint64_t n) {
  char buf[48];
  if (n < 1024) std::snprintf(buf, sizeof buf, "%llu B", static_cast<unsigned long long>(n));
  else if (n < 1024ull * 1024) std::snprintf(buf, sizeof buf, "%.1f KiB", double(n) / 1024.0);
  else if (n < 1024ull * 1024 * 1024) std::snprintf(buf, sizeof buf, "%.1f MiB", double(n) / 1048576.0);
  else std::snprintf(buf, sizeof buf, "%.2f GiB", double(n) / 1073741824.0);
  return buf;
}

bool hash_tree(const std::string& path, std::string& sha256_hex, uint64_t& bytes, std::string* err) {
  crypto::Digest32 d;
  bytes = 0;
  if (!hash_node(path, d, bytes, err)) return false;
  sha256_hex = crypto::hex(d);
  return true;
}

std::string encode_path(const std::string& p) {
  static const char* kHex = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : p) {
    const bool keep = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                      c == '/' || c == '-' || c == '_' || c == '.' || c == '~';
    if (keep) {
      out += char(c);
    } else {
      out += '%';
      out += kHex[c >> 4];
      out += kHex[c & 15];
    }
  }
  return out;
}

std::string decode_path(const std::string& p) {
  auto val = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  std::string out;
  for (size_t i = 0; i < p.size(); ++i) {
    if (p[i] == '%' && i + 2 < p.size()) {
      const int a = val(p[i + 1]), b = val(p[i + 2]);
      if (a >= 0 && b >= 0) {
        out += char(a * 16 + b);
        i += 2;
        continue;
      }
    }
    out += p[i];
  }
  return out;
}

// ============================================================== Trash ======
Trash::Trash(Options opts) : opts_(std::move(opts)) {
  if (!opts_.root.empty() && opts_.root[0] != '/') {
    char cwd[PATH_MAX];
    if (::getcwd(cwd, sizeof cwd)) opts_.root = std::string(cwd) + "/" + opts_.root;
  }
  opts_.root = strip_trailing_slashes(opts_.root);
}

bool Trash::ensure_layout(std::string* err) const {
  if (opts_.root.empty() || !mkdir_p(opts_.root + "/files") || !mkdir_p(opts_.root + "/info")) {
    if (err) *err = "không tạo được thư mục thùng rác " + opts_.root;
    return false;
  }
  return true;
}

bool Trash::is_protected(const std::string& abs, std::string& why) const {
  if (abs == "/") {
    why = "không bao giờ đưa thư mục gốc '/' vào thùng rác";
    return true;
  }
  for (const auto& p : opts_.protected_prefixes) {
    if (inside_or_equal(abs, strip_trailing_slashes(p))) {
      why = "vùng hệ thống được bảo vệ (" + p + "): không xoá qua thùng rác";
      return true;
    }
  }
  for (const auto& p : opts_.protected_exact) {
    if (abs == canonical_or_self(p)) {
      why = "thư mục được bảo vệ (" + p + "): chỉ xoá được nội dung bên trong";
      return true;
    }
  }
  const std::string root = canonical_or_self(opts_.root);
  if (inside_or_equal(abs, root) || inside_or_equal(root, abs)) {
    why = "không đưa chính thùng rác (hoặc thư mục chứa nó) vào thùng rác";
    return true;
  }
  return false;
}

std::string Trash::unique_name(const std::string& base) const {
  std::string name = base;
  for (int i = 2; exists_nofollow(opts_.root + "/files/" + name) ||
                  exists_nofollow(opts_.root + "/info/" + name + ".trashinfo");
       ++i)
    name = base + "." + std::to_string(i);
  return name;
}

std::string Trash::warning_for(uint64_t used) const {
  if (opts_.capacity_bytes == 0) return "";
  if (double(used) >= double(opts_.capacity_bytes) * opts_.warn_ratio)
    return "Thùng rác sắp đầy: " + human_bytes(used) + " / " + human_bytes(opts_.capacity_bytes) +
           ". Thùng không tự xoá món nào; hãy xem `thùngrác` rồi đổ thùng khi chắc chắn.";
  return "";
}

uint64_t Trash::used_bytes() const { return tree_bytes(opts_.root + "/files"); }

std::string Trash::usage_vi() const {
  return human_bytes(used_bytes()) + " / " + human_bytes(opts_.capacity_bytes);
}

Result Trash::put(const std::string& path) {
  std::string abs, err;
  if (!absolutize(path, abs, &err)) return make(false, "not_found", "Không xoá: " + err);
  struct stat st;
  if (!lstat_ok(abs, st)) return make(false, "not_found", "Không có tệp/thư mục: " + abs);
  std::string why;
  if (is_protected(abs, why)) return make(false, "protected", "Từ chối: " + why);

  std::string sha;
  uint64_t bytes = 0;
  if (!hash_tree(abs, sha, bytes, &err)) return make(false, "unsupported", "Không đưa vào thùng rác: " + err);

  const uint64_t used = used_bytes();
  if (opts_.capacity_bytes && used + bytes > opts_.capacity_bytes) {
    Result r = make(false, "trash_full",
                    "Thùng rác không đủ chỗ (" + human_bytes(used) + " đã dùng / " +
                        human_bytes(opts_.capacity_bytes) + ", món này " + human_bytes(bytes) +
                        "). KHÔNG xoá gì: tệp vẫn ở chỗ cũ.");
    r.warning_vi = bytes > opts_.capacity_bytes
                       ? "Món này lớn hơn cả thùng rác. Muốn bỏ hẳn thì phải xoá vĩnh viễn, có xác nhận."
                       : "Đổ bớt thùng rác (`đổrác`) hoặc xoá vĩnh viễn từng món rồi thử lại.";
    return r;
  }
  if (!ensure_layout(&err)) return make(false, "io_error", err);

  const std::string name = unique_name(base_name(abs));
  const std::string info_path = opts_.root + "/info/" + name + ".trashinfo";
  const std::string dest = opts_.root + "/files/" + name;
  const std::string info = "[Trash Info]\nPath=" + encode_path(abs) + "\nDeletionDate=" + now_iso() +
                           "\nX-Yui-Sha256=" + sha + "\nX-Yui-Size=" + std::to_string(bytes) + "\n";
  if (!write_new_file(info_path, info)) return make(false, "io_error", "Không ghi được " + info_path);

  bool moved = false;
  if (!opts_.force_copy && ::rename(abs.c_str(), dest.c_str()) == 0) moved = true;
  if (!moved) {
    if (!opts_.force_copy && errno != EXDEV) {
      const std::string e = std::strerror(errno);
      ::unlink(info_path.c_str());
      return make(false, "io_error", "Không chuyển được vào thùng rác: " + e);
    }
    // Khác ổ đĩa: chép → kiểm băm bản chép → rồi mới xoá bản gốc.
    std::string sha2;
    uint64_t b2 = 0;
    if (!copy_tree(abs, dest) || !hash_tree(dest, sha2, b2, nullptr) || sha2 != sha) {
      remove_tree(dest);
      ::unlink(info_path.c_str());
      return make(false, "io_error", "Chép sang thùng rác không khớp băm; KHÔNG xoá bản gốc");
    }
    if (!remove_tree(abs))
      return make(false, "io_error",
                  "Đã chép vào thùng rác nhưng không xoá hết bản gốc " + abs + " (kiểm quyền thư mục)");
  }
  Result r = make(true, "ok", "Đã chuyển vào thùng rác: " + abs + " → mã '" + name + "' (" + human_bytes(bytes) +
                                  "). Khôi phục: khôiphục " + name);
  r.item.id = name;
  r.item.original_path = abs;
  r.item.sha256 = sha;
  r.item.size = bytes;
  r.item.is_dir = S_ISDIR(st.st_mode);
  r.item.deleted_at = ini_value(info, "DeletionDate");
  r.warning_vi = warning_for(used + bytes);
  return r;
}

bool Trash::read_item(const std::string& id, Item& out) const {
  if (!valid_id(id)) return false;
  std::string body;
  if (!read_text(opts_.root + "/info/" + id + ".trashinfo", body)) return false;
  if (body.find("[Trash Info]") == std::string::npos) return false;
  out = Item{};
  out.id = id;
  out.original_path = decode_path(ini_value(body, "Path"));
  out.deleted_at = ini_value(body, "DeletionDate");
  out.sha256 = ini_value(body, "X-Yui-Sha256");
  const std::string sz = ini_value(body, "X-Yui-Size");
  const std::string f = opts_.root + "/files/" + id;
  out.size = sz.empty() ? tree_bytes(f) : std::strtoull(sz.c_str(), nullptr, 10);
  out.is_dir = is_real_dir(f);
  return !out.original_path.empty();
}

std::vector<Item> Trash::list() const {
  std::vector<Item> out;
  const std::string suffix = ".trashinfo";
  for (const auto& n : list_dir(opts_.root + "/info")) {
    if (n.size() <= suffix.size() || n.compare(n.size() - suffix.size(), suffix.size(), suffix) != 0) continue;
    Item it;
    const std::string id = n.substr(0, n.size() - suffix.size());
    if (read_item(id, it) && exists_nofollow(opts_.root + "/files/" + id)) out.push_back(it);
  }
  std::sort(out.begin(), out.end(), [](const Item& a, const Item& b) {
    return a.deleted_at != b.deleted_at ? a.deleted_at > b.deleted_at : a.id < b.id;
  });
  return out;
}

Result Trash::restore(const std::string& id, const std::string& to) {
  Item it;
  if (!read_item(id, it)) return make(false, "not_found", "Không có món '" + id + "' trong thùng rác");
  const std::string src = opts_.root + "/files/" + id;
  if (!exists_nofollow(src)) return make(false, "not_found", "Thùng rác thiếu dữ liệu của món '" + id + "'");

  std::string target = it.original_path, err;
  if (!to.empty()) {
    const std::string parent = dir_name(to[0] == '/' ? to : "./" + to);
    if (!mkdir_p(canonical_or_self(parent))) return make(false, "io_error", "Không tạo được thư mục " + parent);
    if (!absolutize(to, target, &err)) return make(false, "io_error", "Không khôi phục: " + err);
  } else if (target.empty() || target[0] != '/') {
    return make(false, "io_error", "Đường dẫn gốc ghi trong thùng rác không hợp lệ");
  }
  if (exists_nofollow(target))
    return make(false, "exists", "Ở " + target + " đã có tệp khác: KHÔNG ghi đè. Chọn chỗ khác: khôiphục " + id +
                                     " <đường_dẫn_mới>");

  if (!it.sha256.empty()) {
    std::string sha;
    uint64_t b = 0;
    if (!hash_tree(src, sha, b, &err) || sha != it.sha256)
      return make(false, "hash_mismatch",
                  "Dữ liệu trong thùng rác đã bị thay đổi (băm lệch): không khôi phục, để nguyên trong thùng");
  }
  if (!mkdir_p(dir_name(target))) return make(false, "io_error", "Không tạo được thư mục " + dir_name(target));

  bool moved = !opts_.force_copy && ::rename(src.c_str(), target.c_str()) == 0;
  if (!moved) {
    if (!opts_.force_copy && errno != EXDEV)
      return make(false, "io_error", std::string("Không khôi phục được: ") + std::strerror(errno));
    std::string sha2;
    uint64_t b2 = 0;
    if (!copy_tree(src, target) || !hash_tree(target, sha2, b2, nullptr) ||
        (!it.sha256.empty() && sha2 != it.sha256)) {
      remove_tree(target);
      return make(false, "io_error", "Chép trả về không khớp băm; món vẫn nằm trong thùng rác");
    }
    remove_tree(src);
  }
  ::unlink((opts_.root + "/info/" + id + ".trashinfo").c_str());
  Result r = make(true, "ok",
                  "Đã khôi phục '" + id + "' về " + target +
                      (it.sha256.empty() ? " (món do chương trình khác bỏ vào: không có băm để đối chiếu)"
                                         : " (băm khớp)"));
  r.item = it;
  r.item.original_path = target;
  return r;
}

Result Trash::purge(const std::string& id, ConfirmGate& gate) {
  Item it;
  if (!read_item(id, it)) return make(false, "not_found", "Không có món '" + id + "' trong thùng rác");
  const std::string summary = "Xoá VĨNH VIỄN '" + id + "' (" + it.original_path + ", " + human_bytes(it.size) +
                              "). Không khôi phục được nữa.";
  if (!gate.confirmed(purge_target(id), summary))
    return make(false, "not_confirmed", "Chưa có xác nhận tin cậy: không xoá vĩnh viễn gì");
  if (!remove_tree(opts_.root + "/files/" + id))
    return make(false, "io_error", "Không xoá hết dữ liệu của '" + id + "'");
  ::unlink((opts_.root + "/info/" + id + ".trashinfo").c_str());
  Result r = make(true, "ok", "Đã xoá vĩnh viễn '" + id + "', giải phóng " + human_bytes(it.size));
  r.item = it;
  r.freed_bytes = it.size;
  r.removed = 1;
  return r;
}

std::string Trash::empty_target() const {
  crypto::Sha256 h;
  for (const auto& it : list()) {
    h.update(it.id);
    h.update("\0", 1);
    h.update(std::to_string(it.size));
    h.update("\n", 1);
  }
  return "trash:empty:" + crypto::hex(h.final()).substr(0, 16);
}

std::string Trash::empty_summary_vi() const {
  const auto items = list();
  uint64_t total = 0;
  for (const auto& it : items) total += it.size;
  return "Đổ thùng rác: xoá VĨNH VIỄN " + std::to_string(items.size()) + " món, giải phóng " + human_bytes(total) +
         ". Không khôi phục được nữa.";
}

Result Trash::empty(ConfirmGate& gate) {
  const auto items = list();
  if (items.empty()) return make(true, "ok", "Thùng rác đang trống");
  if (!gate.confirmed(empty_target(), empty_summary_vi()))
    return make(false, "not_confirmed", "Chưa có xác nhận tin cậy: không đổ thùng rác");
  Result r = make(true, "ok", "");
  for (const auto& it : items) {
    if (remove_tree(opts_.root + "/files/" + it.id)) {
      ::unlink((opts_.root + "/info/" + it.id + ".trashinfo").c_str());
      r.freed_bytes += it.size;
      ++r.removed;
    } else {
      r.ok = false;
      r.code = "io_error";
    }
  }
  r.reason_vi = "Đã đổ thùng rác: " + std::to_string(r.removed) + " món, giải phóng " + human_bytes(r.freed_bytes);
  return r;
}

// ============================================================ dọn rác =====
std::vector<CleanTarget> default_targets(const std::string& store_root, const std::string& state_dir) {
  std::vector<CleanTarget> t;
  // 10 phút: tránh đụng lượt tải/cài đang chạy.
  t.push_back({"Gói tải dở của Kho Ứng Dụng (mất khả năng tải tiếp)", store_root + "/cache", ".part", false, 600});
  t.push_back({"Thư mục tạm còn sót của Kho Ứng Dụng", store_root + "/staging", "", true, 600});
  t.push_back({"Thư mục tạm của yuid", state_dir + "/tmp", "", true, 3600});
  return t;
}

namespace {

bool name_matches(const CleanTarget& t, const std::string& name) {
  if (t.suffix.empty()) return true;
  return name.size() > t.suffix.size() && name.compare(name.size() - t.suffix.size(), t.suffix.size(), t.suffix) == 0;
}

// Mục có được phép xoá theo đúng luật của đích không (dùng cả lúc lập lẫn lúc áp kế hoạch).
bool allowed_by(const CleanTarget& t, const std::string& name, const struct stat& st, std::string& why) {
  if (S_ISLNK(st.st_mode)) {
    why = "liên kết tượng trưng — không đụng";
    return false;
  }
  if (S_ISDIR(st.st_mode) && !t.include_dirs) {
    why = "thư mục — đích này chỉ dọn tệp";
    return false;
  }
  if (!S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode)) {
    why = "tệp đặc biệt — không đụng";
    return false;
  }
  if (!name_matches(t, name)) {
    why = "không đúng đuôi " + t.suffix;
    return false;
  }
  return true;
}

}  // namespace

CleanPlan plan_cleanup(const std::vector<CleanTarget>& targets, int64_t now_s) {
  CleanPlan plan;
  for (const auto& t : targets) {
    const std::string dir = strip_trailing_slashes(t.dir);
    struct stat dst;
    if (!lstat_ok(dir, dst)) continue;  // chưa có thì thôi
    if (!S_ISDIR(dst.st_mode)) {
      plan.skipped_vi.push_back(dir + ": không phải thư mục thật (liên kết?) — bỏ qua cả đích");
      continue;
    }
    for (const auto& n : list_dir(dir)) {
      const std::string p = dir + "/" + n;
      struct stat st;
      if (!lstat_ok(p, st)) continue;
      std::string why;
      if (!allowed_by(t, n, st, why)) {
        plan.skipped_vi.push_back(p + ": " + why);
        continue;
      }
      if (t.min_age_s > 0 && now_s - int64_t(st.st_mtime) < t.min_age_s) {
        plan.skipped_vi.push_back(p + ": mới sửa gần đây — có thể đang dùng");
        continue;
      }
      CleanEntry e;
      e.path = p;
      e.label_vi = t.label_vi;
      e.is_dir = S_ISDIR(st.st_mode);
      e.bytes = e.is_dir ? tree_bytes(p) : uint64_t(st.st_size);
      e.dev = uint64_t(st.st_dev);
      e.ino = uint64_t(st.st_ino);
      e.mtime = int64_t(st.st_mtime);
      plan.total_bytes += e.bytes;
      plan.entries.push_back(e);
    }
  }
  return plan;
}

std::string CleanPlan::digest() const {
  crypto::Sha256 h;
  for (const auto& e : entries) {
    h.update(e.path);
    h.update("\0", 1);
    h.update(std::to_string(e.bytes) + ":" + std::to_string(e.dev) + ":" + std::to_string(e.ino) + ":" +
             std::to_string(e.mtime) + "\n");
  }
  return crypto::hex(h.final()).substr(0, 16);
}

std::string CleanPlan::summary_vi() const {
  if (entries.empty()) return "Không có gì để dọn trong danh sách có tên.";
  std::string s = "Dọn rác sẽ xoá " + std::to_string(entries.size()) + " mục, giải phóng " + human_bytes(total_bytes) +
                  " (chỉ trong danh sách có tên, không đụng tệp của bạn):";
  size_t shown = 0;
  for (const auto& e : entries) {
    if (shown++ == 20) {
      s += "\n  … và " + std::to_string(entries.size() - 20) + " mục nữa";
      break;
    }
    s += "\n  • " + e.label_vi + ": " + e.path + (e.is_dir ? "/" : "") + " (" + human_bytes(e.bytes) + ")";
  }
  return s;
}

Result apply_cleanup(const CleanPlan& plan, const std::vector<CleanTarget>& targets, ConfirmGate& gate) {
  if (plan.entries.empty()) return make(true, "ok", "Không có gì để dọn");
  if (!gate.confirmed(plan.target(), plan.summary_vi()))
    return make(false, "not_confirmed", "Chưa có xác nhận tin cậy: không xoá gì");
  Result r = make(true, "ok", "");
  std::vector<std::string> stale;
  for (const auto& e : plan.entries) {
    // Mục phải nằm NGAY trong một đích có tên và vẫn đúng luật của đích đó.
    const CleanTarget* owner = nullptr;
    for (const auto& t : targets)
      if (strip_trailing_slashes(t.dir) == dir_name(e.path)) owner = &t;
    struct stat dst, st;
    std::string why;
    if (!owner || !lstat_ok(dir_name(e.path), dst) || !S_ISDIR(dst.st_mode)) {
      stale.push_back(e.path + ": không thuộc danh sách có tên");
      continue;
    }
    if (!lstat_ok(e.path, st) || !allowed_by(*owner, base_name(e.path), st, why)) {
      stale.push_back(e.path + (why.empty() ? ": đã biến mất" : ": " + why));
      continue;
    }
    const bool same = uint64_t(st.st_dev) == e.dev && uint64_t(st.st_ino) == e.ino &&
                      int64_t(st.st_mtime) == e.mtime && S_ISDIR(st.st_mode) == e.is_dir &&
                      (e.is_dir ? tree_bytes(e.path) : uint64_t(st.st_size)) == e.bytes;
    if (!same) {
      stale.push_back(e.path + ": đã thay đổi sau khi lập kế hoạch — bỏ qua");
      continue;
    }
    if (remove_tree(e.path)) {
      r.freed_bytes += e.bytes;
      ++r.removed;
    } else {
      stale.push_back(e.path + ": xoá lỗi");
    }
  }
  r.reason_vi = "Đã dọn " + std::to_string(r.removed) + " mục, giải phóng " + human_bytes(r.freed_bytes);
  if (!stale.empty()) {
    r.code = "stale_plan";
    r.warning_vi = "Bỏ qua " + std::to_string(stale.size()) + " mục:";
    for (const auto& s : stale) r.warning_vi += "\n  • " + s;
  }
  return r;
}

}  // namespace yui::trash
