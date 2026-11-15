// Sovereign Keyring & .bundle — xem include/yui/bundle.hpp.
#include "yui/bundle.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "yui/crypto.hpp"
#include "yui/json.hpp"

namespace yui::bundle {
namespace {

bool read_file(const std::string& path, std::vector<uint8_t>* out) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  out->clear();
  uint8_t buf[4096];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out->insert(out->end(), buf, buf + n);
  std::fclose(f);
  return true;
}

bool write_file(const std::string& path, const void* data, size_t len, bool sync) {
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return false;
  const bool ok = std::fwrite(data, 1, len, f) == len;
  if (sync) std::fflush(f), (void)::fsync(fileno(f));
  std::fclose(f);
  return ok;
}

bool write_file(const std::string& path, const std::string& data, bool sync) {
  return write_file(path, data.data(), data.size(), sync);
}

bool is_dir(const std::string& path) {
  struct stat st{};
  return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool mkdir_p(const std::string& path) {
  std::string cur;
  for (size_t i = 0; i <= path.size(); ++i) {
    if (i == path.size() || path[i] == '/') {
      if (!cur.empty() && !is_dir(cur)) {
        if (::mkdir(cur.c_str(), 0755) != 0 && !is_dir(cur)) return false;
      }
      if (i < path.size()) cur += '/';
    } else {
      cur += path[i];
    }
  }
  return true;
}

// Gỡ đệ quy một thư mục (dùng cho slot A/B trước khi cài).
void remove_tree(const std::string& path) {
  DIR* d = ::opendir(path.c_str());
  if (!d) {
    ::unlink(path.c_str());
    return;
  }
  while (struct dirent* e = ::readdir(d)) {
    if (std::strcmp(e->d_name, ".") == 0 || std::strcmp(e->d_name, "..") == 0) continue;
    remove_tree(path + "/" + e->d_name);
  }
  ::closedir(d);
  ::rmdir(path.c_str());
}

bool safe_rel_path(const std::string& p) {
  if (p.empty() || p[0] == '/') return false;
  if (p.find("..") != std::string::npos) return false;
  if (p.find("//") != std::string::npos) return false;
  return true;
}

std::string escape_json(const std::string& s) {
  std::string out;
  for (char c : s) {
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if (static_cast<unsigned char>(c) < 0x20) {
      char buf[8];
      std::snprintf(buf, sizeof buf, "\\u%04x", c);
      out += buf;
    } else {
      out += c;
    }
  }
  return out;
}

std::string lower_hex(const crypto::Digest32& d) { return ed25519::to_hex(d.data(), d.size()); }

}  // namespace

// ------------------------------------------------------------- Keyring -----
bool Keyring::add(const std::string& key_id, const ed25519::PublicKey& pub) {
  if (key_id.empty() || keys_.count(key_id)) return false;
  keys_.emplace(key_id, pub);
  return true;
}

bool Keyring::remove(const std::string& key_id) { return keys_.erase(key_id) > 0; }

bool Keyring::contains(const std::string& key_id) const { return keys_.count(key_id) != 0; }

bool Keyring::get(const std::string& key_id, ed25519::PublicKey& out) const {
  auto it = keys_.find(key_id);
  if (it == keys_.end()) return false;
  out = it->second;
  return true;
}

std::vector<std::string> Keyring::ids() const {
  std::vector<std::string> out;
  for (const auto& kv : keys_) out.push_back(kv.first);
  return out;
}

bool Keyring::load_dir(const std::string& dir, std::string* err) {
  DIR* d = ::opendir(dir.c_str());
  if (!d) {
    if (err) *err = "không mở được thư mục kho khoá: " + dir;
    return false;
  }
  size_t loaded = 0;
  while (struct dirent* e = ::readdir(d)) {
    const std::string name = e->d_name;
    if (name.size() <= 4 || name.compare(name.size() - 4, 4, ".pub") != 0) continue;
    std::vector<uint8_t> raw;
    if (!read_file(dir + "/" + name, &raw)) continue;
    // Nội dung tệp: 64 hex + xuống dòng (xem save_dir) — cắt khoảng trắng
    // thừa trước khi giải mã.
    std::string text(raw.begin(), raw.end());
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' '))
      text.pop_back();
    ed25519::PublicKey pub{};
    if (!ed25519::from_hex(text, pub.data(), pub.size())) continue;
    const std::string key_id = name.substr(0, name.size() - 4);
    if (add(key_id, pub)) ++loaded;
  }
  ::closedir(d);
  if (loaded == 0 && keys_.empty()) {
    if (err) *err = "không nạp được khoá phát hành nào từ " + dir;
    return false;
  }
  return true;
}

bool Keyring::save_dir(const std::string& dir, std::string* err) const {
  if (!mkdir_p(dir)) {
    if (err) *err = "không tạo được thư mục kho khoá: " + dir;
    return false;
  }
  for (const auto& kv : keys_) {
    const std::string hex = ed25519::to_hex(kv.second.data(), kv.second.size()) + "\n";
    if (!write_file(dir + "/" + kv.first + ".pub", hex, true)) {
      if (err) *err = "không ghi được khoá " + kv.first;
      return false;
    }
  }
  return true;
}

// ------------------------------------------------------------- Manifest ----
std::string canonical(const Manifest& m) {
  std::vector<ManifestFile> files = m.files;
  std::sort(files.begin(), files.end(),
            [](const ManifestFile& a, const ManifestFile& b) { return a.path < b.path; });
  std::string out = "{\"files\":[";
  for (size_t i = 0; i < files.size(); ++i) {
    if (i) out += ',';
    out += "{\"mode\":\"" + escape_json(files[i].mode) + "\",\"path\":\"" +
           escape_json(files[i].path) + "\",\"sha256\":\"" + escape_json(files[i].sha256_hex) +
           "\"}";
  }
  out += "],\"key_id\":\"" + escape_json(m.key_id) + "\",\"version\":\"" + escape_json(m.version) +
         "\"}";
  return out;
}

bool parse_manifest(const std::string& json_text, Manifest& out, std::string* err) {
  auto parsed = json::parse(json_text);
  if (!parsed.ok) {
    if (err) *err = "manifest.json không phải JSON hợp lệ";
    return false;
  }
  const json::Value& v = parsed.value;
  if (!v.is_object()) {
    if (err) *err = "manifest.json phải là một đối tượng JSON";
    return false;
  }
  Manifest m;
  m.version = v["version"].as_string();
  m.key_id = v["key_id"].as_string();
  if (m.version.empty() || m.key_id.empty()) {
    if (err) *err = "manifest thiếu version hoặc key_id";
    return false;
  }
  for (const auto& f : v["files"].arr()) {
    ManifestFile mf;
    mf.path = f["path"].as_string();
    mf.sha256_hex = f["sha256"].as_string();
    mf.mode = f["mode"].as_string("0644");
    if (!safe_rel_path(mf.path)) {
      if (err) *err = "đường dẫn tệp không an toàn trong manifest: " + mf.path;
      return false;
    }
    if (mf.sha256_hex.size() != 64) {
      if (err) *err = "sha256 không hợp lệ cho tệp: " + mf.path;
      return false;
    }
    m.files.push_back(mf);
  }
  out = std::move(m);
  return true;
}

// ------------------------------------------------------------- Verify ------
VerifyResult verify(const std::string& bundle_dir, const Keyring& keys) {
  VerifyResult r;
  std::vector<uint8_t> manifest_raw;
  if (!read_file(bundle_dir + "/manifest.json", &manifest_raw)) {
    r.reason_vi = "bundle thiếu manifest.json";
    return r;
  }
  Manifest m;
  std::string err;
  if (!parse_manifest(std::string(manifest_raw.begin(), manifest_raw.end()), m, &err)) {
    r.reason_vi = err;
    return r;
  }

  ed25519::PublicKey pub{};
  if (!keys.get(m.key_id, pub)) {
    r.reason_vi = "khoá chủ quyền không chứa khoá '" + m.key_id + "' — từ chối";
    return r;
  }

  std::vector<uint8_t> sig_raw;
  if (!read_file(bundle_dir + "/manifest.sig", &sig_raw) || sig_raw.size() != 64) {
    r.reason_vi = "bundle thiếu manifest.sig hoặc chữ ký không đủ 64 byte";
    return r;
  }
  ed25519::Signature sig{};
  std::memcpy(sig.data(), sig_raw.data(), 64);

  // DỰNG LẠI chuỗi chuẩn từ trường đã parse, rồi mới so chữ ký.
  const std::string canon = canonical(m);
  if (!ed25519::verify(pub, canon.data(), canon.size(), sig)) {
    r.reason_vi = "chữ ký Ed25519 của manifest không hợp lệ — bundle bị sửa hoặc sai khoá";
    return r;
  }
  r.signature_ok = true;

  for (const auto& f : m.files) {
    std::vector<uint8_t> data;
    if (!read_file(bundle_dir + "/payload/" + f.path, &data)) {
      r.reason_vi = "thiếu tệp payload: " + f.path;
      return r;
    }
    const std::string got = lower_hex(crypto::sha256(data));
    if (got != f.sha256_hex) {
      r.reason_vi = "SHA-256 không khớp cho tệp: " + f.path + " — nội dung đã bị sửa";
      return r;
    }
  }
  r.payload_ok = true;
  r.ok = true;
  r.key_id = m.key_id;
  r.reason_vi = "bundle hợp lệ: chữ ký đúng khoá '" + m.key_id + "', " +
                std::to_string(m.files.size()) + " tệp đúng SHA-256";
  return r;
}

// ------------------------------------------------------------- Install -----
std::string active_slot(const std::string& state_dir) {
  std::vector<uint8_t> raw;
  if (!read_file(state_dir + "/boot_pointer", &raw)) return "A";
  std::string s(raw.begin(), raw.end());
  while (!s.empty() && (s.back() == '\n' || s.back() == ' ')) s.pop_back();
  return s == "B" ? "B" : "A";
}

InstallResult install(const std::string& bundle_dir, const Keyring& keys,
                      const std::string& state_dir) {
  InstallResult r;
  const VerifyResult v = verify(bundle_dir, keys);
  if (!v.ok) {
    r.reason_vi = "không cài được: " + v.reason_vi;
    return r;
  }

  Manifest m;
  std::vector<uint8_t> manifest_raw;
  read_file(bundle_dir + "/manifest.json", &manifest_raw);
  std::string err;
  if (!parse_manifest(std::string(manifest_raw.begin(), manifest_raw.end()), m, &err)) {
    r.reason_vi = err;
    return r;
  }

  const std::string current = active_slot(state_dir);
  const std::string target = current == "A" ? "B" : "A";
  const std::string slot_dir = state_dir + "/slots/" + target;
  remove_tree(slot_dir);
  if (!mkdir_p(slot_dir)) {
    r.reason_vi = "không tạo được thư mục slot " + target;
    return r;
  }

  for (const auto& f : m.files) {
    std::vector<uint8_t> data;
    if (!read_file(bundle_dir + "/payload/" + f.path, &data)) {
      r.reason_vi = "thiếu tệp payload khi cài: " + f.path;
      return r;
    }
    const std::string dst = slot_dir + "/" + f.path;
    const size_t slash = dst.rfind('/');
    if (slash != std::string::npos) mkdir_p(dst.substr(0, slash));
    if (!write_file(dst, data.data(), data.size(), true)) {
      r.reason_vi = "không ghi được tệp khi cài: " + f.path;
      return r;
    }
    if (f.mode.size() == 4) {
      const mode_t mode = static_cast<mode_t>(std::strtoul(f.mode.c_str(), nullptr, 8));
      ::chmod(dst.c_str(), mode);
    }
    // Kiểm lại hash SAU khi chép (chống hỏng ghi/TOCTOU giữa verify và copy).
    const std::string got = lower_hex(crypto::sha256(data));
    if (got != f.sha256_hex) {
      r.reason_vi = "SHA-256 lệch sau khi chép: " + f.path;
      return r;
    }
  }

  write_file(slot_dir + "/manifest.json", std::string(manifest_raw.begin(), manifest_raw.end()), true);

  // Đổi con trỏ boot bằng ghi nguyên tử: tmp → rename (mất điện giữa chừng
  // vẫn giữ con trỏ cũ hoặc mới trọn vẹn).
  const std::string ptr_path = state_dir + "/boot_pointer";
  mkdir_p(state_dir);
  const std::string tmp = ptr_path + ".tmp";
  if (!write_file(tmp, target + "\n", true) || ::rename(tmp.c_str(), ptr_path.c_str()) != 0) {
    r.reason_vi = "không đổi được con trỏ boot";
    return r;
  }

  r.ok = true;
  r.slot = target;
  r.boot_pointer_path = ptr_path;
  r.reason_vi = "đã cài bundle vào slot " + target + " và đổi con trỏ boot (slot " + current +
                " giữ nguyên làm bản rollback)";
  return r;
}

}  // namespace yui::bundle
