// Application trimmer + archiver (Tầng 5).
#include "yui/apppack.hpp"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <sstream>

#include "yui/codec.hpp"
#include "yui/crypto.hpp"
#include "yui/sandbox.hpp"
#include "yui/text.hpp"

namespace yui::apppack {
namespace {

constexpr uint32_t kArchiveMagic = 0x41495559;  // 'Y','U','I','A'
constexpr uint8_t kArchiveVersion = 1;

bool read_file(const std::string& path, Bytes& out, std::string* error) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) {
    if (error) *error = "không đọc được: " + path;
    return false;
  }
  std::fseek(f, 0, SEEK_END);
  const long size = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  if (size < 0) {
    std::fclose(f);
    if (error) *error = "kích thước không hợp lệ: " + path;
    return false;
  }
  out.assign(size_t(size), 0);
  const size_t got = size ? std::fread(out.data(), 1, out.size(), f) : 0;
  std::fclose(f);
  if (got != out.size()) {
    if (error) *error = "đọc thiếu dữ liệu: " + path;
    return false;
  }
  return true;
}

void put_u16(Bytes& b, uint16_t v) {
  b.push_back(uint8_t(v & 0xff));
  b.push_back(uint8_t(v >> 8));
}
void put_u32(Bytes& b, uint32_t v) {
  for (int i = 0; i < 4; ++i) b.push_back(uint8_t((v >> (8 * i)) & 0xff));
}
void put_u64(Bytes& b, uint64_t v) {
  for (int i = 0; i < 8; ++i) b.push_back(uint8_t((v >> (8 * i)) & 0xff));
}
uint16_t get_u16(const Bytes& b, size_t& p) {
  const uint16_t v = uint16_t(b[p] | (b[p + 1] << 8));
  p += 2;
  return v;
}
uint32_t get_u32(const Bytes& b, size_t& p) {
  uint32_t v = 0;
  for (int i = 3; i >= 0; --i) v = (v << 8) | b[p + size_t(i)];
  p += 4;
  return v;
}
uint64_t get_u64(const Bytes& b, size_t& p) {
  uint64_t v = 0;
  for (int i = 7; i >= 0; --i) v = (v << 8) | b[p + size_t(i)];
  p += 8;
  return v;
}

bool is_vcs_dir(const std::string& name) { return name == ".git" || name == ".hg" || name == ".svn"; }

// VCS metadata and regenerable caches are always dropped (unless verbatim).
// Build-product directories only with `strip_build_dirs`: inside a shipped app
// (Electron `resources/app/node_modules`, `app.asar.unpacked/node_modules`)
// they are runtime code and dropping them breaks the app (found by P1.6).
bool is_skipped_dir(const std::string& name, const TrimOptions& opts) {
  if (opts.verbatim) return false;
  static const char* kAlways[] = {".git", ".hg", ".svn", "__pycache__", ".cache", ".pytest_cache"};
  for (const char* s : kAlways)
    if (name == s) return true;
  if (!opts.strip_build_dirs) return false;
  static const char* kBuild[] = {"node_modules", ".venv", "target", "build"};
  for (const char* s : kBuild)
    if (name == s) return true;
  return false;
}

std::string lower_ascii(std::string s) {
  for (char& c : s)
    if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
  return s;
}

// Reads the first `n` bytes (fewer if the file is shorter).
Bytes read_head(const std::string& path, size_t n) {
  Bytes out;
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return out;
  out.resize(n);
  out.resize(std::fread(out.data(), 1, n, f));
  std::fclose(f);
  return out;
}

// Section names that identify a copy-protection wrapper.  Only names that are
// fixed and documented by the tools themselves / widely catalogued.
const char* protection_section_reason(const std::string& section) {
  if (section == ".vmp0" || section == ".vmp1" || section == ".vmp2") return "VMProtect (mã hoá/ảo hoá mã)";
  if (section == ".themida") return "Themida (bảo vệ chống sửa)";
  if (section == ".winlice") return "WinLicense (bản quyền + chống sửa)";
  if (section == ".enigma1" || section == ".enigma2") return "Enigma Protector";
  if (section == ".bind") return "SteamStub DRM (Steam)";
  return nullptr;
}

void scan_protection(const std::string& root, const std::string& rel, std::vector<ProtectionMarker>& out) {
  const std::string dir_path = rel.empty() ? root : root + "/" + rel;
  DIR* dir = ::opendir(dir_path.c_str());
  if (!dir) return;
  std::vector<std::string> names;
  while (dirent* ent = ::readdir(dir)) {
    const std::string name = ent->d_name;
    if (name != "." && name != "..") names.push_back(name);
  }
  ::closedir(dir);
  std::sort(names.begin(), names.end());
  for (const auto& name : names) {
    const std::string child_rel = rel.empty() ? name : rel + "/" + name;
    const std::string child_abs = root + "/" + child_rel;
    struct stat st{};
    if (::lstat(child_abs.c_str(), &st) != 0) continue;
    const std::string low = lower_ascii(name);
    if (S_ISDIR(st.st_mode)) {
      if (is_vcs_dir(name)) continue;
      if (low == "widevinecdm") {
        out.push_back({child_rel + "/", "Widevine CDM (DRM của Google)"});
        continue;  // one marker per CDM directory is enough
      }
      scan_protection(root, child_rel, out);
      continue;
    }
    if (!S_ISREG(st.st_mode)) continue;
    if (low == "libwidevinecdm.so" || low == "widevinecdm.dll" || low == "libwidevinecdm.dylib") {
      out.push_back({child_rel, "Widevine CDM (DRM của Google)"});
      continue;
    }
    if (yui::text::ends_with(low, ".sig") && name.size() > 4) {
      // castlabs VMP / integrity signature: `<binary>.sig` beside `<binary>`.
      const std::string stem = name.substr(0, name.size() - 4);
      const std::string lstem = lower_ascii(stem);
      struct stat sib{};
      const std::string sib_abs = dir_path + "/" + stem;
      if (::stat(sib_abs.c_str(), &sib) == 0 && S_ISREG(sib.st_mode) &&
          ((sib.st_mode & 0111) || yui::text::ends_with(lstem, ".exe") || yui::text::ends_with(lstem, ".dll") ||
           yui::text::ends_with(lstem, ".so") || lstem.find(".so.") != std::string::npos)) {
        out.push_back({child_rel, "chữ ký toàn vẹn/VMP cho " + stem + " (sửa một byte là hỏng)"});
        continue;
      }
    }
    if (st.st_size < 64) continue;
    const Bytes head = read_head(child_abs, 4);
    const bool elf = head.size() == 4 && head[0] == 0x7f && head[1] == 'E' && head[2] == 'L' && head[3] == 'F';
    const bool pe = head.size() >= 2 && head[0] == 'M' && head[1] == 'Z';
    if (!elf && !pe) continue;
    Bytes image;
    if (!read_file(child_abs, image, nullptr)) continue;
    for (const auto& sec : binary_section_names(image)) {
      if (const char* why = protection_section_reason(sec)) {
        out.push_back({child_rel, std::string(why) + ", section " + sec});
        break;
      }
    }
  }
}

// Locale filtering only applies to conventional layouts:
//   locale/<lang>/..., locales/<lang>/..., lang/<lang>/..., i18n/<lang>/...
// Anything else is left alone: guessing would delete real data.
bool locale_allowed(const std::string& relative_path, const TrimOptions& opts) {
  const std::string r = "/" + relative_path + "/";
  size_t start = std::string::npos;
  for (const char* marker : {"/locale/", "/locales/", "/lang/", "/i18n/"}) {
    const size_t p = r.find(marker);
    if (p != std::string::npos) {
      start = p + std::strlen(marker);
      break;
    }
  }
  if (start == std::string::npos || start >= r.size()) return true;
  const size_t end = r.find('/', start);
  const std::string lang = r.substr(start, (end == std::string::npos ? r.size() : end) - start);
  if (lang.empty()) return true;
  for (const auto& keep : opts.keep_locales)
    if (lang.compare(0, keep.size(), keep) == 0) return true;
  return false;
}

bool docs_skipped(const std::string& path, const TrimOptions& opts) {
  if (!opts.strip_docs) return false;
  return yui::text::starts_with(path, "share/doc") || yui::text::starts_with(path, "share/man") ||
         yui::text::ends_with(path, ".md") || yui::text::ends_with(path, ".1") ||
         yui::text::ends_with(path, ".3") || yui::text::ends_with(path, ".html");
}

void walk(const std::string& root, const std::string& rel, const TrimOptions& opts, std::vector<Entry>& out,
          TrimReport& rep, std::string* error) {
  const std::string dir_path = rel.empty() ? root : root + "/" + rel;
  DIR* dir = ::opendir(dir_path.c_str());
  if (!dir) {
    rep.warnings.push_back("bỏ qua thư mục không đọc được: " + dir_path);
    if (error && error->empty()) *error = "không mở được thư mục: " + dir_path;
    return;
  }
  std::vector<std::string> names;
  while (dirent* ent = ::readdir(dir)) {
    const std::string name = ent->d_name;
    if (name == "." || name == "..") continue;
    names.push_back(name);
  }
  ::closedir(dir);
  std::sort(names.begin(), names.end());

  for (const auto& name : names) {
    const std::string child_rel = rel.empty() ? name : rel + "/" + name;
    const std::string child_abs = root + "/" + child_rel;
    struct stat st{};
    if (::lstat(child_abs.c_str(), &st) != 0) continue;

    if (S_ISDIR(st.st_mode)) {
      if (is_skipped_dir(name, opts)) {
        ++rep.files_dropped;
        if (rep.dropped_samples.size() < 32) rep.dropped_samples.push_back(child_rel + "/ (thư mục)");
        continue;
      }
      if (!opts.verbatim && (!locale_allowed(child_rel, opts) || docs_skipped(child_rel, opts))) {
        ++rep.files_dropped;
        if (rep.dropped_samples.size() < 32) rep.dropped_samples.push_back(child_rel + "/ (ngôn ngữ/tài liệu)");
        continue;
      }
      walk(root, child_rel, opts, out, rep, error);
      continue;
    }
    if (!S_ISREG(st.st_mode)) {
      // Symlinks/FIFOs/sockets are never archived; say so instead of hiding it.
      if (rep.specials_skipped++ < 8)
        rep.warnings.push_back("bỏ qua " + std::string(S_ISLNK(st.st_mode) ? "liên kết mềm" : "tệp đặc biệt") +
                               ": " + child_rel);
      continue;
    }

    ++rep.files_in;
    rep.bytes_in += uint64_t(st.st_size);

    if (!opts.verbatim && (yui::text::ends_with(name, ".debug") || !locale_allowed(child_rel, opts) ||
                           docs_skipped(child_rel, opts))) {
      ++rep.files_dropped;
      if (rep.dropped_samples.size() < 32) rep.dropped_samples.push_back(child_rel);
      continue;
    }

    Entry e;
    e.path = child_rel;
    e.mode = uint32_t(st.st_mode & 07777);
    e.kind = (st.st_mode & 0111) ? 'x' : 'f';
    if (!read_file(child_abs, e.data, error)) continue;
    if (!opts.verbatim && opts.strip_debug_elf && e.data.size() > 4 && e.data[0] == 0x7f && e.data[1] == 'E' &&
        e.data[2] == 'L' && e.data[3] == 'F') {
      rep.bytes_debug_zeroed += strip_elf_debug(e.data);
    }
    e.size = e.data.size();
    rep.bytes_kept += e.size;
    ++rep.files_kept;
    out.push_back(std::move(e));
  }
}

}  // namespace

std::string TrimReport::summary_vi() const {
  std::ostringstream os;
  const double saved = bytes_in ? 100.0 * double(bytes_in > bytes_packed ? bytes_in - bytes_packed : 0) / double(bytes_in) : 0.0;
  os << "Đã gọt " << files_dropped << " mục, giữ " << files_kept << "/" << files_in << " tệp";
  if (duplicates) os << ", gộp " << duplicates << " bản trùng";
  if (bytes_debug_zeroed) os << ", xoá " << (bytes_debug_zeroed / 1024) << " KiB ký hiệu gỡ lỗi";
  auto human = [](uint64_t n) -> std::string {
    char b[48];
    if (n >= 1024ull * 1024) std::snprintf(b, sizeof(b), "%.1f MiB", double(n) / (1024.0 * 1024));
    else if (n >= 1024) std::snprintf(b, sizeof(b), "%.1f KiB", double(n) / 1024.0);
    else std::snprintf(b, sizeof(b), "%llu B", static_cast<unsigned long long>(n));
    return b;
  };
  char buf[96];
  std::snprintf(buf, sizeof(buf), " (giảm %.1f%%)", saved);
  os << " — " << human(bytes_in) << " → " << human(bytes_packed) << buf;
  if (!stripped) {
    if (protection.empty()) {
      os << " · giữ nguyên 100% byte (--verbatim), chỉ nén không mất dữ liệu";
    } else {
      os << " · KHÔNG GỌT: phát hiện bảo vệ bản quyền/toàn vẹn (" << protection.front().reason;
      if (protection.size() > 1) os << " và " << (protection.size() - 1) << " dấu hiệu khác";
      os << ") — giữ nguyên 100% byte, chỉ nén không mất dữ liệu";
    }
  }
  if (!warnings.empty()) os << " · " << warnings.size() << " cảnh báo";
  return os.str();
}

uint64_t strip_elf_debug(Bytes& image) {
  if (image.size() < 64 || image[0] != 0x7f || image[1] != 'E' || image[2] != 'L' || image[3] != 'F')
    return 0;
  if (image[4] != 2) return 0;  // ELFCLASS64 only (ELF32 layout differs)
  auto rd16 = [&](size_t off) -> uint16_t { return uint16_t(image[off] | (image[off + 1] << 8)); };
  auto rd32 = [&](size_t off) -> uint32_t {
    uint32_t v = 0;
    for (int i = 3; i >= 0; --i) v = (v << 8) | image[off + size_t(i)];
    return v;
  };
  auto rd64 = [&](size_t off) -> uint64_t {
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | image[off + size_t(i)];
    return v;
  };
  const uint64_t shoff = rd64(0x28);
  const uint16_t shentsize = rd16(0x3A);
  const uint16_t shnum = rd16(0x3C);
  const uint16_t shstrndx = rd16(0x3E);
  const uint64_t phoff = rd64(0x20);
  const uint16_t phentsize = rd16(0x36);
  const uint16_t phnum = rd16(0x38);
  if (shentsize < 64 || shnum == 0 || shoff == 0 || shoff + uint64_t(shentsize) * shnum > image.size())
    return 0;

  // Section-header string table.
  if (shstrndx >= shnum) return 0;
  const size_t shstr_off = size_t(shoff) + size_t(shstrndx) * shentsize;
  const uint64_t names_off = rd64(shstr_off + 0x18);
  const uint64_t names_size = rd64(shstr_off + 0x20);
  if (names_off + names_size > image.size()) return 0;

  // Ranges covered by program headers: never touch those bytes.
  std::vector<std::pair<uint64_t, uint64_t>> load_ranges;
  for (uint16_t i = 0; i < phnum; ++i) {
    const size_t ph = size_t(phoff) + size_t(i) * phentsize;
    if (ph + 56 > image.size()) break;
    const uint64_t offset = rd64(ph + 0x08);
    const uint64_t filesz = rd64(ph + 0x20);
    if (filesz) load_ranges.emplace_back(offset, offset + filesz);
  }

  uint64_t zeroed = 0;
  for (uint16_t i = 0; i < shnum; ++i) {
    const size_t sh = size_t(shoff) + size_t(i) * shentsize;
    if (sh + 64 > image.size()) break;
    const uint32_t name_off = rd32(sh + 0x00);
    const uint32_t type = rd32(sh + 0x04);
    const uint64_t offset = rd64(sh + 0x18);
    const uint64_t size = rd64(sh + 0x20);
    if (name_off >= names_size || size == 0) continue;
    std::string name;
    for (uint64_t k = names_off + name_off; k < names_off + names_size && image[size_t(k)]; ++k)
      name += char(image[size_t(k)]);
    const bool debug = yui::text::starts_with(name, ".debug_") || name == ".comment" ||
                       yui::text::starts_with(name, ".note") || name == ".gnu_debuglink";
    if (!debug || type == 0 /*SHT_NULL*/) continue;
    if (offset + size > image.size()) continue;
    // Skip anything that overlaps a loaded segment.
    bool overlaps = false;
    for (const auto& r : load_ranges) {
      if (!(offset + size <= r.first || offset >= r.second)) {
        overlaps = true;
        break;
      }
    }
    if (overlaps) continue;
    std::memset(image.data() + offset, 0, size_t(size));
    zeroed += size;
  }
  return zeroed;
}

std::vector<std::string> binary_section_names(const Bytes& image) {
  std::vector<std::string> names;
  auto rd16 = [&](size_t off) -> uint16_t { return uint16_t(image[off] | (image[off + 1] << 8)); };
  auto rd32 = [&](size_t off) -> uint32_t {
    uint32_t v = 0;
    for (int i = 3; i >= 0; --i) v = (v << 8) | image[off + size_t(i)];
    return v;
  };
  auto rd64 = [&](size_t off) -> uint64_t {
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | image[off + size_t(i)];
    return v;
  };
  if (image.size() >= 64 && image[0] == 0x7f && image[1] == 'E' && image[2] == 'L' && image[3] == 'F' &&
      image[4] == 2 && image[5] == 1) {  // ELF64 little-endian
    const uint64_t shoff = rd64(0x28);
    const uint16_t shentsize = rd16(0x3A), shnum = rd16(0x3C), shstrndx = rd16(0x3E);
    if (shentsize < 64 || shnum == 0 || shstrndx >= shnum || shoff > image.size() ||
        uint64_t(shentsize) * shnum > image.size() - shoff)
      return names;
    const size_t str_sh = size_t(shoff) + size_t(shstrndx) * shentsize;
    const uint64_t str_off = rd64(str_sh + 0x18), str_size = rd64(str_sh + 0x20);
    if (str_off > image.size() || str_size > image.size() - str_off) return names;
    for (uint16_t i = 0; i < shnum; ++i) {
      const uint32_t name_off = rd32(size_t(shoff) + size_t(i) * shentsize);
      if (name_off >= str_size) continue;
      std::string n;
      for (uint64_t k = str_off + name_off; k < str_off + str_size && image[size_t(k)]; ++k) n += char(image[size_t(k)]);
      if (!n.empty()) names.push_back(n);
    }
    return names;
  }
  if (image.size() >= 0x40 && image[0] == 'M' && image[1] == 'Z') {  // PE/COFF
    const uint32_t pe = rd32(0x3C);
    if (uint64_t(pe) + 24 > image.size() || image[pe] != 'P' || image[pe + 1] != 'E' || image[pe + 2] != 0 ||
        image[pe + 3] != 0)
      return names;
    const uint16_t nsec = rd16(pe + 6), opt = rd16(pe + 20);
    const uint64_t table = uint64_t(pe) + 24 + opt;
    for (uint16_t i = 0; i < nsec && table + uint64_t(i + 1) * 40 <= image.size(); ++i) {
      const size_t s = size_t(table) + size_t(i) * 40;
      std::string n;
      for (size_t k = 0; k < 8 && image[s + k]; ++k) n += char(image[s + k]);
      if (!n.empty()) names.push_back(n);
    }
  }
  return names;
}

std::vector<ProtectionMarker> detect_protection(const std::string& root) {
  std::vector<ProtectionMarker> out;
  scan_protection(root, "", out);
  return out;
}

bool collect(const std::string& root, const TrimOptions& opts_in, std::vector<Entry>& out, TrimReport& rep,
             std::string* error) {
  struct stat st{};
  if (::stat(root.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
    if (error) *error = "không phải thư mục: " + root;
    return false;
  }
  TrimOptions opts = opts_in;
  if (opts.detect_protection && !opts.verbatim) {
    rep.protection = detect_protection(root);
    if (!rep.protection.empty()) opts.verbatim = true;  // 0 bytes modified
  }
  rep.stripped = !opts.verbatim;
  out.clear();
  walk(root, "", opts, out, rep, error);
  std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) { return a.path < b.path; });

  if (opts.dedupe && !opts.verbatim) {
    std::map<std::string, std::string> seen;  // sha256 -> first path
    for (auto& e : out) {
      if (e.data.empty()) continue;
      const std::string digest = yui::crypto::hex(yui::crypto::sha256(e.data));
      auto it = seen.find(digest);
      if (it == seen.end()) {
        seen.emplace(digest, e.path);
        rep.bytes_after_dedupe += e.size;
        continue;
      }
      const uint64_t saved = e.size;
      e.kind = 'd';
      e.data.assign(it->second.begin(), it->second.end());
      e.size = 0;
      ++rep.duplicates;
      rep.bytes_after_dedupe += 0;
      (void)saved;
    }
  } else {
    for (const auto& e : out) rep.bytes_after_dedupe += e.size;
  }
  return true;
}

Bytes build_archive(const std::vector<Entry>& entries) {
  Bytes out;
  put_u32(out, kArchiveMagic);
  out.push_back(kArchiveVersion);
  put_u32(out, uint32_t(entries.size()));
  for (const auto& e : entries) {
    put_u16(out, uint16_t(e.path.size()));
    out.insert(out.end(), e.path.begin(), e.path.end());
    out.push_back(e.kind);
    put_u32(out, e.mode);
    put_u64(out, uint64_t(e.data.size()));
    out.insert(out.end(), e.data.begin(), e.data.end());
  }
  return out;
}

bool parse_archive(const Bytes& archive, std::vector<Entry>& out, std::string* error) {
  auto fail = [&](const char* m) {
    if (error) *error = m;
    out.clear();
    return false;
  };
  if (archive.size() < 9) return fail("kho lưu trữ quá ngắn");
  size_t p = 0;
  if (get_u32(archive, p) != kArchiveMagic) return fail("không phải kho YUIA");
  if (archive[p++] != kArchiveVersion) return fail("phiên bản kho không hỗ trợ");
  const uint32_t count = get_u32(archive, p);
  out.clear();
  out.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    if (p + 2 > archive.size()) return fail("kho bị cắt cụt (tên tệp)");
    const uint16_t name_len = get_u16(archive, p);
    if (p + name_len + 1 + 4 + 8 > archive.size()) return fail("kho bị cắt cụt (tiêu đề)");
    Entry e;
    e.path.assign(archive.begin() + long(p), archive.begin() + long(p + name_len));
    p += name_len;
    e.kind = archive[p++];
    e.mode = get_u32(archive, p);
    const uint64_t size = get_u64(archive, p);
    if (p + size > archive.size()) return fail("kho bị cắt cụt (nội dung)");
    e.data.assign(archive.begin() + long(p), archive.begin() + long(p + size));
    p += size_t(size);
    e.size = e.kind == 'd' ? 0 : size;
    if (e.path.empty() || e.path.find("..") != std::string::npos || e.path[0] == '/')
      return fail("đường dẫn không an toàn trong kho");
    out.push_back(std::move(e));
  }
  return true;
}

bool extract(const std::vector<Entry>& entries, const std::string& dest, std::string* error) {
  // Build the directory skeleton first so ordering does not matter.
  std::map<std::string, std::string> links;
  for (const auto& e : entries) {
    const std::string full = dest + "/" + e.path;
    const size_t slash = full.rfind('/');
    if (slash != std::string::npos) {
      const std::string dir = full.substr(0, slash);
      // Keep relative destinations relative (`yui-trim --unpack x -C out`):
      // before P1.6 every prefix got a leading '/', so "out/a" became "/out".
      std::string cur = dir[0] == '/' ? "" : ".";
      std::istringstream parts(dir);
      std::string part;
      while (std::getline(parts, part, '/')) {
        if (part.empty()) continue;
        cur += "/" + part;
        ::mkdir(cur.c_str(), 0755);
      }
    }
    if (e.kind == 'd') {
      links[e.path] = std::string(e.data.begin(), e.data.end());
      continue;
    }
    FILE* f = std::fopen(full.c_str(), "wb");
    if (!f) {
      if (error) *error = "không ghi được: " + full;
      return false;
    }
    if (!e.data.empty()) std::fwrite(e.data.data(), 1, e.data.size(), f);
    std::fclose(f);
    ::chmod(full.c_str(), mode_t(e.mode ? e.mode : 0644));
  }
  // Materialise dedupe links last (targets are guaranteed to exist by then).
  for (const auto& kv : links) {
    const std::string target = dest + "/" + kv.second;
    const std::string link_path = dest + "/" + kv.first;
    ::unlink(link_path.c_str());
    if (::link(target.c_str(), link_path.c_str()) != 0) {
      // Cross-device or unsupported filesystem: fall back to a copy.
      Bytes data;
      if (read_file(target, data, nullptr)) {
        FILE* f = std::fopen(link_path.c_str(), "wb");
        if (f) {
          if (!data.empty()) std::fwrite(data.data(), 1, data.size(), f);
          std::fclose(f);
        }
      }
    }
  }
  return true;
}

Bytes pack_tree(const std::string& root, const TrimOptions& opts, TrimReport& rep, std::string* error) {
  std::vector<Entry> entries;
  TrimOptions o = opts;
  if (!collect(root, o, entries, rep, error)) return Bytes{};
  const Bytes archive = build_archive(entries);
  Bytes packed = yui::codec::pack(archive, o.level);
  rep.bytes_packed = packed.size();
  return packed;
}

bool unpack_to(const Bytes& packed, const std::string& dest, std::string* error) {
  codec::Bytes archive;
  std::string e;
  if (!codec::unpack(packed, archive, nullptr, &e)) {
    if (error) *error = e;
    return false;
  }
  std::vector<Entry> entries;
  if (!parse_archive(archive, entries, &e)) {
    if (error) *error = e;
    return false;
  }
  return extract(entries, dest, error);
}

// --------------------------------- Proposed Policy Generator (P3.15) --
const char* proposed_scope_source_name(ProposedScopeSource s) {
  switch (s) {
    case ProposedScopeSource::Manifest: return "manifest";
    case ProposedScopeSource::ScannedString: return "scanned_string";
    case ProposedScopeSource::SandboxTrial: return "sandbox_trial";
  }
  return "unknown";
}

const char* proposed_scope_source_name_vi(ProposedScopeSource s) {
  switch (s) {
    case ProposedScopeSource::Manifest: return "khai báo trong manifest";
    case ProposedScopeSource::ScannedString: return "quét từ chuỗi tĩnh trong bộ cài";
    case ProposedScopeSource::SandboxTrial: return "ghi nhận khi chạy thử trong hộp cát";
  }
  return "không rõ";
}

json::Value ProposedPolicy::to_json() const {
  json::Value v(json::Object{});
  v.set("version", version);
  v.set("app", app_id);
  v.set("tier", tier);
  v.set("enabled", enabled);
  v.set("comment_vi", "Tệp gợi ý chính sách hộp cát — không bao giờ tự bật, người dùng duyệt từng đích");

  json::Value caps(json::Array{});
  for (const auto& c : capabilities) caps.push(c);
  v.set("capabilities", caps);

  json::Value tgts(json::Array{});
  for (const auto& t : targets) {
    json::Value item(json::Object{});
    item.set("target", t.target);
    item.set("source", proposed_scope_source_name(t.source));
    item.set("confidence", t.confidence);
    item.set("reason_vi", t.reason_vi);
    tgts.push(item);
  }
  v.set("proposed_targets", tgts);

  json::Value scopes_v(json::Array{});
  for (const auto& t : targets) scopes_v.push(t.target);
  v.set("scopes", scopes_v);

  json::Value d_arr(json::Array{});
  for (const auto& d : deny) d_arr.push(d);
  v.set("deny", d_arr);
  return v;
}

std::string ProposedPolicy::to_json_string(bool pretty) const {
  return to_json().dump(pretty);
}

ProposedPolicy ProposedPolicy::from_json(const json::Value& v, std::string* error) {
  ProposedPolicy p;
  if (!v.is_object()) {
    if (error) *error = "JSON không phải đối tượng";
    return p;
  }
  p.version = static_cast<int>(v["version"].as_int(1));
  p.app_id = v["app"].as_string();
  p.tier = v["tier"].as_string("microvm");
  p.enabled = v["enabled"].as_bool(false);

  for (const auto& c : v["capabilities"].arr()) {
    p.capabilities.push_back(c.as_string());
  }

  for (const auto& item : v["proposed_targets"].arr()) {
    ProposedTarget t;
    t.target = item["target"].as_string();
    std::string src = item["source"].as_string();
    if (src == "manifest") t.source = ProposedScopeSource::Manifest;
    else if (src == "sandbox_trial") t.source = ProposedScopeSource::SandboxTrial;
    else t.source = ProposedScopeSource::ScannedString;
    t.confidence = item["confidence"].as_string("low");
    t.reason_vi = item["reason_vi"].as_string();
    p.targets.push_back(t);
  }

  // Nếu không có proposed_targets nhưng có scopes
  if (p.targets.empty()) {
    for (const auto& s : v["scopes"].arr()) {
      ProposedTarget t;
      t.target = s.as_string();
      t.source = ProposedScopeSource::Manifest;
      t.confidence = "high";
      t.reason_vi = "Khai báo trong scopes";
      p.targets.push_back(t);
    }
  }

  for (const auto& d : v["deny"].arr()) {
    p.deny.push_back(d.as_string());
  }
  return p;
}

sandbox::Policy ProposedPolicy::to_sandbox_policy(bool approve_all,
                                                  const std::vector<std::string>& approved_targets) const {
  sandbox::Policy sp;
  sp.version = version;
  sp.app_id = app_id;
  sandbox::tier_from(tier, sp.tier);
  sp.granted = sandbox::kRightNone;

  for (const auto& c : capabilities) {
    if (c == "*") {
      sp.granted = ~0ull;
    } else {
      sp.granted |= sandbox::rights_from_string(c);
    }
  }

  for (const auto& t : targets) {
    bool approved = approve_all;
    if (!approved) {
      for (const auto& at : approved_targets) {
        if (at == t.target) {
          approved = true;
          break;
        }
      }
    }
    if (approved) {
      sp.scopes.push_back(t.target);
      if (t.target.find('/') == std::string::npos) {
        sp.granted |= sandbox::kRightNetConnect;
      } else {
        sp.granted |= sandbox::kRightFsRead;
      }
    }
  }

  sp.deny = deny;
  return sp;
}

std::vector<std::string> extract_domains_from_data(const Bytes& data) {
  std::vector<std::string> results;
  if (data.empty()) return results;

  std::string cur;
  auto is_domain_char = [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '.' || c == '-' || c == ':';
  };

  std::vector<std::string> candidates;
  for (size_t i = 0; i < data.size(); ++i) {
    char c = static_cast<char>(data[i]);
    if (is_domain_char(c)) {
      cur += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    } else {
      if (cur.size() >= 4 && cur.size() <= 80 && cur.find('.') != std::string::npos) {
        candidates.push_back(cur);
      }
      cur.clear();
    }
  }
  if (cur.size() >= 4 && cur.size() <= 80 && cur.find('.') != std::string::npos) {
    candidates.push_back(cur);
  }

  // Lọc các đuôi tệp phổ biến
  static const char* kIgnoredExts[] = {
      ".cpp", ".hpp", ".c", ".h", ".txt", ".json", ".xml", ".png", ".jpg", ".jpeg",
      ".gif", ".so", ".a", ".o", ".md", ".dat", ".bin", ".py", ".rs", ".sh", ".html", ".css", ".js"
  };

  for (const auto& cand : candidates) {
    // Phải có ít nhất một dấu chấm
    size_t dot = cand.find('.');
    if (dot == std::string::npos || dot == 0 || dot == cand.size() - 1) continue;

    bool skip = false;
    for (const char* ext : kIgnoredExts) {
      if (cand.size() >= std::strlen(ext) &&
          cand.compare(cand.size() - std::strlen(ext), std::strlen(ext), ext) == 0) {
        skip = true;
        break;
      }
    }
    if (skip) continue;

    // Tách port nếu có
    std::string domain_only = cand;
    size_t col = cand.rfind(':');
    if (col != std::string::npos && col + 1 < cand.size()) {
      domain_only = cand.substr(0, col);
    }

    if (sandbox::EgressFilter::validate_dns_name(domain_only)) {
      if (std::find(results.begin(), results.end(), domain_only) == results.end()) {
        results.push_back(domain_only);
      }
    }
  }
  return results;
}

ProposedPolicy generate_proposed_policy(
    const std::string& app_id,
    const std::string& manifest_json,
    const std::vector<Entry>& entries,
    const std::vector<std::string>& sandbox_trial_blocked_domains,
    const std::vector<std::string>& initial_deny) {
  ProposedPolicy prop;
  prop.version = 1;
  prop.app_id = app_id;
  prop.tier = "microvm";
  prop.enabled = false;
  prop.capabilities = {"net.connect", "fs.read"};

  // (a) Đích khai báo trong manifest
  if (!manifest_json.empty()) {
    const auto v = json::parse(manifest_json);
    if (v.ok && v.value.is_object()) {
      auto add_target = [&](const std::string& s, const std::string& reason) {
        if (s.empty()) return;
        for (const auto& ex : prop.targets) {
          if (ex.target == s) return;
        }
        ProposedTarget t;
        t.target = s;
        t.source = ProposedScopeSource::Manifest;
        t.confidence = "high";
        t.reason_vi = reason;
        prop.targets.push_back(t);
      };

      for (const auto& s : v.value["scopes"].arr()) {
        add_target(s.as_string(), "Đích khai báo trong scopes của manifest");
      }
      for (const auto& s : v.value["net_allow"].arr()) {
        add_target(s.as_string(), "Đích khai báo trong net_allow của manifest");
      }
      for (const auto& s : v.value["fs_allow"].arr()) {
        add_target(s.as_string(), "Đích khai báo trong fs_allow của manifest");
      }
      for (const auto& c : v.value["capabilities"].arr()) {
        std::string cap = c.as_string();
        if (cap.rfind("net:", 0) == 0) {
          add_target(cap.substr(4), "Khai báo trong capabilities (net:*)");
        } else if (cap.rfind("fs:", 0) == 0) {
          add_target(cap.substr(3), "Khai báo trong capabilities (fs:*)");
        }
      }
    }
  }

  // (b) Tên miền quét từ chuỗi tĩnh trong bộ cài
  for (const auto& e : entries) {
    if (e.kind == 'd') continue;  // Bỏ qua liên kết dedupe
    auto scanned = extract_domains_from_data(e.data);
    for (const auto& domain : scanned) {
      bool exists = false;
      for (const auto& existing : prop.targets) {
        if (existing.target == domain) {
          exists = true;
          break;
        }
      }
      if (!exists) {
        ProposedTarget t;
        t.target = domain;
        t.source = ProposedScopeSource::ScannedString;
        t.confidence = "low";
        t.reason_vi = "Tên miền quét từ chuỗi trong tệp " + e.path;
        prop.targets.push_back(t);
      }
    }
  }

  // (c) Đích ghi nhận khi chạy thử trong hộp cát (chặn và ghi)
  for (const auto& raw_domain : sandbox_trial_blocked_domains) {
    std::string domain = raw_domain;
    if (domain.rfind("dns:", 0) == 0) domain = domain.substr(4);
    else if (domain.rfind("net:", 0) == 0) domain = domain.substr(4);

    bool exists = false;
    for (auto& existing : prop.targets) {
      if (existing.target == domain) {
        exists = true;
        break;
      }
    }
    if (!exists) {
      ProposedTarget t;
      t.target = domain;
      t.source = ProposedScopeSource::SandboxTrial;
      t.confidence = "observed";
      t.reason_vi = "Đích ghi nhận khi chạy thử trong hộp cát (chặn và ghi)";
      prop.targets.push_back(t);
    }
  }

  // Cấu hình deny mặc định: deny luôn thắng allow
  std::vector<std::string> default_deny = {"/home/user/.ssh", "/etc/shadow", "/home/user/.gnupg"};
  for (const auto& d : default_deny) prop.deny.push_back(d);
  for (const auto& d : initial_deny) {
    if (std::find(prop.deny.begin(), prop.deny.end(), d) == prop.deny.end()) {
      prop.deny.push_back(d);
    }
  }

  return prop;
}

}  // namespace yui::apppack
