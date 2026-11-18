// yui-trim — Trình Gọt Tỉa & Nén App (Tầng 5).
//
//   yui-trim <thưmục> -o app.yuip [--strip-docs] [--no-dedupe] [--level N] [--dry-run]
//   yui-trim --unpack app.yuip -C <thưmục>
//   yui-trim --info app.yuip
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "yui/apppack.hpp"
#include "yui/codec.hpp"
#include "yui/text.hpp"

namespace {

void usage() {
  std::printf(
      "yui-trim — gọt tỉa và nén ứng dụng YuiVieOS\n"
      "\n"
      "  yui-trim <thưmục> -o <tệp.yuip>    Gọt tỉa rồi đóng gói\n"
      "  yui-trim --unpack <tệp.yuip> -C <thưmục>\n"
      "  yui-trim --info <tệp.yuip>\n"
      "\n"
      "Tuỳ chọn:\n"
      "  --strip-docs          Bỏ tài liệu (share/doc, share/man, *.md)\n"
      "  --no-dedupe           Không gộp tệp trùng nội dung\n"
      "  --no-elf-strip        Không xoá ký hiệu gỡ lỗi trong ELF\n"
      "  --keep-locales vi,en  Danh sách ngôn ngữ giữ lại\n"
      "  --level N             Mức nén 0..9 (mặc định 7)\n"
      "  --dry-run             Chỉ báo cáo, không ghi tệp\n"
      "  --propose-policy <tệp> Sinh chính sách hộp cát đề xuất (.proposed.json)\n");
}

bool read_all(const std::string& path, yui::apppack::Bytes& out) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  std::fseek(f, 0, SEEK_END);
  const long size = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  out.assign(size_t(size > 0 ? size : 0), 0);
  const size_t got = out.empty() ? 0 : std::fread(out.data(), 1, out.size(), f);
  std::fclose(f);
  return got == out.size();
}

std::string human(uint64_t n) {
  char buf[64];
  if (n >= 1024ull * 1024) std::snprintf(buf, sizeof(buf), "%.1f MiB", double(n) / (1024.0 * 1024));
  else if (n >= 1024) std::snprintf(buf, sizeof(buf), "%.1f KiB", double(n) / 1024.0);
  else std::snprintf(buf, sizeof(buf), "%llu B", static_cast<unsigned long long>(n));
  return buf;
}

}  // namespace

int main(int argc, char** argv) {
  yui::apppack::TrimOptions opts;
  std::string source, out_path, unpack_path, info_path, dest, propose_path;
  bool dry_run = false;
  std::vector<std::string> positional;

  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&](const char* what) -> std::string {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "yui-trim: %s cần giá trị\n", what);
        std::exit(2);
      }
      return argv[++i];
    };
    if (a == "-h" || a == "--help") {
      usage();
      return 0;
    } else if (a == "-o") out_path = next("-o");
    else if (a == "--unpack") unpack_path = next("--unpack");
    else if (a == "--info") info_path = next("--info");
    else if (a == "-C") dest = next("-C");
    else if (a == "--strip-docs") opts.strip_docs = true;
    else if (a == "--no-dedupe") opts.dedupe = false;
    else if (a == "--no-elf-strip") opts.strip_debug_elf = false;
    else if (a == "--dry-run") dry_run = true;
    else if (a == "--propose-policy") propose_path = next("--propose-policy");
    else if (a == "--level") opts.level = std::atoi(next("--level").c_str());
    else if (a == "--keep-locales") {
      opts.keep_locales.clear();
      for (const auto& part : yui::text::split(next("--keep-locales"), ','))
        if (!part.empty()) opts.keep_locales.push_back(part);
    } else if (!a.empty() && a[0] == '-') {
      std::fprintf(stderr, "yui-trim: tham số không hợp lệ: %s\n", a.c_str());
      return 2;
    } else {
      positional.push_back(a);
    }
  }

  if (!info_path.empty()) {
    yui::apppack::Bytes packed;
    if (!read_all(info_path, packed)) {
      std::fprintf(stderr, "yui-trim: không đọc được %s\n", info_path.c_str());
      return 1;
    }
    yui::codec::Bytes archive;
    yui::codec::PackInfo info;
    std::string err;
    if (!yui::codec::unpack(packed, archive, &info, &err)) {
      std::fprintf(stderr, "yui-trim: %s\n", err.c_str());
      return 1;
    }
    std::vector<yui::apppack::Entry> entries;
    if (!yui::apppack::parse_archive(archive, entries, &err)) {
      std::fprintf(stderr, "yui-trim: %s\n", err.c_str());
      return 1;
    }
    uint64_t raw = 0, links = 0;
    for (const auto& e : entries) {
      raw += e.data.size();
      if (e.kind == 'd') ++links;
    }
    std::printf("Gói: %s\n  Tệp: %zu (%llu liên kết trùng)\n  Dữ liệu: %s\n  Gói: %s\n  SHA-256 gốc: %s\n",
                info_path.c_str(), entries.size(), static_cast<unsigned long long>(links),
                human(raw).c_str(), human(packed.size()).c_str(), info.digest_hex.c_str());
    return 0;
  }

  if (!unpack_path.empty()) {
    if (dest.empty()) {
      std::fprintf(stderr, "yui-trim: cần -C <thưmục> để bung\n");
      return 2;
    }
    yui::apppack::Bytes packed;
    if (!read_all(unpack_path, packed)) {
      std::fprintf(stderr, "yui-trim: không đọc được %s\n", unpack_path.c_str());
      return 1;
    }
    std::string err;
    if (!yui::apppack::unpack_to(packed, dest, &err)) {
      std::fprintf(stderr, "yui-trim: %s\n", err.c_str());
      return 1;
    }
    std::printf("Đã bung %s vào %s\n", unpack_path.c_str(), dest.c_str());
    return 0;
  }

  if (positional.empty()) {
    usage();
    return 2;
  }
  source = positional[0];

  yui::apppack::TrimReport rep;
  std::string err;
  const auto packed = yui::apppack::pack_tree(source, opts, rep, &err);
  if (packed.empty()) {
    std::fprintf(stderr, "yui-trim: %s\n", err.empty() ? "không có dữ liệu" : err.c_str());
    return 1;
  }
  std::printf("%s\n", rep.summary_vi().c_str());
  for (const auto& d : rep.dropped_samples) std::printf("  − %s\n", d.c_str());
  for (const auto& w : rep.warnings) std::fprintf(stderr, "  ! %s\n", w.c_str());

  if (dry_run) {
    std::printf("(chạy thử — không ghi tệp)\n");
    return 0;
  }
  if (out_path.empty()) out_path = source + ".yuip";
  FILE* f = std::fopen(out_path.c_str(), "wb");
  if (!f) {
    std::fprintf(stderr, "yui-trim: không ghi được %s\n", out_path.c_str());
    return 1;
  }
  std::fwrite(packed.data(), 1, packed.size(), f);
  std::fclose(f);
  std::printf("Đã ghi: %s (%s)\n", out_path.c_str(), human(packed.size()).c_str());

  if (!propose_path.empty()) {
    std::vector<yui::apppack::Entry> entries;
    yui::apppack::TrimReport tr;
    std::string coll_err;
    if (yui::apppack::collect(source, opts, entries, tr, &coll_err)) {
      std::string manifest_json;
      yui::apppack::Bytes m_data;
      if (read_all(source + "/vm-manifest.json", m_data) || read_all(source + "/manifest.json", m_data)) {
        manifest_json.assign(reinterpret_cast<const char*>(m_data.data()), m_data.size());
      }
      std::string app_name = source;
      size_t slash = app_name.rfind('/');
      if (slash != std::string::npos) app_name = app_name.substr(slash + 1);
      const auto prop = yui::apppack::generate_proposed_policy("app:" + app_name, manifest_json, entries, {});
      FILE* pf = std::fopen(propose_path.c_str(), "w");
      if (pf) {
        const std::string s = prop.to_json_string(true);
        std::fwrite(s.data(), 1, s.size(), pf);
        std::fclose(pf);
        std::printf("Đã sinh chính sách đề xuất: %s (%zu đích, mặc định tắt)\n",
                    propose_path.c_str(), prop.targets.size());
      }
    }
  }

  return 0;
}
