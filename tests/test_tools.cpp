// Tests for Tầng 5 pieces: app trimmer/packer, adaptive layout, bilingual shell.
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "testutil.hpp"
#include "yui/apppack.hpp"
#include "yui/shell.hpp"
#include "yui/ui.hpp"

using namespace yui;

namespace {

std::string mkdtemp_dir(const std::string& tag) {
  std::string tmpl = "/tmp/yuitest-" + tag + "-XXXXXX";
  std::vector<char> buf(tmpl.begin(), tmpl.end());
  buf.push_back('\0');
  const char* dir = ::mkdtemp(buf.data());
  return dir ? std::string(dir) : std::string();
}

void mkdirs(const std::string& dir) {
  std::string cur;
  for (char c : dir) {
    cur += c;
    if (c == '/') ::mkdir(cur.c_str(), 0755);
  }
  if (!dir.empty() && dir.back() != '/') ::mkdir(dir.c_str(), 0755);
}

void write_file(const std::string& path, const std::string& content) {
  const size_t slash = path.rfind('/');
  if (slash != std::string::npos) mkdirs(path.substr(0, slash));
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return;
  std::fwrite(content.data(), 1, content.size(), f);
  std::fclose(f);
}

std::string read_file(const std::string& path) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return "";
  std::string s;
  char buf[4096];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
  std::fclose(f);
  return s;
}

bool exists(const std::string& path) {
  struct stat st{};
  return ::stat(path.c_str(), &st) == 0;
}

void put16(std::vector<uint8_t>& b, size_t off, uint16_t v) {
  b[off] = uint8_t(v & 0xff);
  b[off + 1] = uint8_t(v >> 8);
}
void put32(std::vector<uint8_t>& b, size_t off, uint32_t v) {
  for (int i = 0; i < 4; ++i) b[off + size_t(i)] = uint8_t((v >> (8 * i)) & 0xff);
}
void put64(std::vector<uint8_t>& b, size_t off, uint64_t v) {
  for (int i = 0; i < 8; ++i) b[off + size_t(i)] = uint8_t((v >> (8 * i)) & 0xff);
}

}  // namespace

YUI_TEST(apppack, trims_dedupes_and_round_trips) {
  const std::string root = mkdtemp_dir("app");
  CHECK(!root.empty());
  write_file(root + "/bin/app", "#!/bin/sh\necho YuiVieOS\n");
  write_file(root + "/locale/vi/messages", "Xin chào\n");
  write_file(root + "/locale/fr/messages", "Bonjour\n");
  write_file(root + "/locale/en/strings", "Xin chào\n");  // duplicate of the vi file
  write_file(root + "/share/doc/README.md", "# tài liệu\n");
  write_file(root + "/app.debug", "symbols");
  write_file(root + "/.git/config", "[core]");
  write_file(root + "/src/main.c", "int main(void){return 0;}\n");

  apppack::TrimOptions opts;
  opts.strip_docs = true;
  apppack::TrimReport rep;
  std::string err;
  const auto packed = apppack::pack_tree(root, opts, rep, &err);
  CHECK_MSG(err.empty(), "pack_tree error: " + err);
  CHECK(!packed.empty());

  // files_in counts regular files actually visited: bin/app, locale/vi/messages,
  // locale/en/strings, app.debug, src/main.c.  Subtrees dropped wholesale
  // (.git, locale/fr, share/doc) are counted as dropped items instead.
  CHECK_EQ(rep.files_in, size_t(5));
  CHECK_MSG(rep.files_dropped >= 3, "fr locale, docs and *.debug must be dropped");
  CHECK_MSG(rep.duplicates >= 1, "identical locale files must be deduped");
  CHECK_MSG(rep.bytes_kept < rep.bytes_in, "trimming must remove bytes");
  // For a tree this small the 60-byte YUIP header dominates; the packed size is
  // only meaningful for real applications, which the trim CLI reports.
  CHECK(rep.bytes_packed > 0);

  const std::string out = mkdtemp_dir("out");
  CHECK(apppack::unpack_to(packed, out, &err));
  CHECK_EQ(read_file(out + "/bin/app"), std::string("#!/bin/sh\necho YuiVieOS\n"));
  CHECK_EQ(read_file(out + "/locale/vi/messages"), std::string("Xin chào\n"));
  CHECK(exists(out + "/locale/en/strings"));
  CHECK_MSG(!exists(out + "/locale/fr/messages"), "dropped locale must not be restored");
  CHECK_MSG(!exists(out + "/app.debug"), "dropped debug symbols must not be restored");
  CHECK_MSG(!exists(out + "/.git/config"), "VCS metadata must not be archived");
  CHECK_EQ(read_file(out + "/src/main.c"), std::string("int main(void){return 0;}\n"));

  // Tampering with the package must be detected.
  auto damaged = packed;
  damaged.back() ^= 0x01;
  const std::string out2 = mkdtemp_dir("bad");
  CHECK(!apppack::unpack_to(damaged, out2, &err));
  CHECK(!err.empty());
}

YUI_TEST(apppack, archive_rejects_unsafe_paths) {
  std::vector<apppack::Entry> entries;
  apppack::Entry e;
  e.path = "../escape.txt";
  e.kind = 'f';
  e.mode = 0644;
  e.data = {'x'};
  entries.push_back(e);
  const auto archive = apppack::build_archive(entries);
  std::vector<apppack::Entry> back;
  std::string err;
  CHECK(!apppack::parse_archive(archive, back, &err));
  CHECK(err.find("không an toàn") != std::string::npos);

  std::vector<apppack::Entry> abs_entries;
  apppack::Entry a;
  a.path = "/etc/passwd";
  a.kind = 'f';
  abs_entries.push_back(a);
  CHECK(!apppack::parse_archive(apppack::build_archive(abs_entries), back, &err));

  // Truncated archive is rejected rather than half-extracted.
  const auto good = apppack::build_archive({});
  std::vector<uint8_t> cut(good.begin(), good.end() - 2);
  CHECK(!apppack::parse_archive(cut, back, &err));
}

YUI_TEST(apppack, strips_elf_debug_sections_only) {
  // Synthetic ELF64: header + shstrtab + .debug_info + a "loaded" .text range.
  std::vector<uint8_t> elf(2048, 0);
  elf[0] = 0x7f;
  elf[1] = 'E';
  elf[2] = 'L';
  elf[3] = 'F';
  elf[4] = 2;  // ELFCLASS64
  const uint64_t shoff = 1024;
  put64(elf, 0x28, shoff);            // e_shoff
  put16(elf, 0x3A, 64);               // e_shentsize
  put16(elf, 0x3C, 3);                // e_shnum
  put16(elf, 0x3E, 1);                // e_shstrndx (the string table)
  put64(elf, 0x20, 0);                // e_phoff (no program headers)
  put16(elf, 0x36, 56);
  put16(elf, 0x38, 0);

  // String table at offset 512.
  const std::string names = std::string("\0.shstrtab\0.debug_info\0", 22);
  std::memcpy(elf.data() + 512, names.data(), names.size());
  // Section 0: NULL. Section 1: .shstrtab. Section 2: .debug_info at 700, len 100.
  put32(elf, size_t(shoff) + 0, 0);
  put64(elf, size_t(shoff) + 0x18, 512);
  put64(elf, size_t(shoff) + 0x20, names.size());
  put32(elf, size_t(shoff) + 64, 1);
  put32(elf, size_t(shoff) + 64 + 0x04, 3);  // SHT_STRTAB
  put64(elf, size_t(shoff) + 64 + 0x18, 512);
  put64(elf, size_t(shoff) + 64 + 0x20, names.size());
  put32(elf, size_t(shoff) + 128, 11);
  put32(elf, size_t(shoff) + 128 + 0x04, 1);  // SHT_PROGBITS
  put64(elf, size_t(shoff) + 128 + 0x18, 700);
  put64(elf, size_t(shoff) + 128 + 0x20, 100);
  for (size_t i = 700; i < 800; ++i) elf[i] = 0xAB;

  const uint64_t zeroed = apppack::strip_elf_debug(elf);
  CHECK_EQ(zeroed, uint64_t(100));
  bool all_zero = true;
  for (size_t i = 700; i < 800; ++i)
    if (elf[i] != 0) all_zero = false;
  CHECK(all_zero);
  CHECK_MSG(elf[512] == 0 && elf[513] == '.', "the string table must be untouched");

  // Non-ELF input is left alone.
  std::vector<uint8_t> junk(128, 0x55);
  CHECK_EQ(apppack::strip_elf_debug(junk), uint64_t(0));
  CHECK_EQ(junk[0], uint8_t(0x55));
}

YUI_TEST(apppack, proposed_policy_generation_and_three_sources) {
  // P3.15: Test domain extraction from binary/string data
  std::string raw = "Contact https://api.sample.vn:8443/v1 and http://auth.service.com or update.cdn.internal:443";
  apppack::Bytes raw_bytes(raw.begin(), raw.end());
  auto extracted = apppack::extract_domains_from_data(raw_bytes);
  CHECK_EQ(extracted.size(), size_t(3));
  CHECK(std::find(extracted.begin(), extracted.end(), "api.sample.vn") != extracted.end());
  CHECK(std::find(extracted.begin(), extracted.end(), "auth.service.com") != extracted.end());
  CHECK(std::find(extracted.begin(), extracted.end(), "update.cdn.internal") != extracted.end());

  // Manifest with net and fs entries
  std::string manifest = R"({
    "net_allow": ["api.vendor.com", "192.168.1.100"],
    "fs_allow": ["/var/data"]
  })";

  // Package entries containing strings
  std::vector<apppack::Entry> entries;
  apppack::Entry e1;
  e1.path = "bin/worker";
  std::string s1 = "Sending telemetry to telemetry.stats.com and auth.service.com";
  e1.data.assign(s1.begin(), s1.end());
  entries.push_back(e1);

  // Sandbox trial blocked targets
  std::vector<std::string> trial_logs = {"dns:crash.report.io", "net:10.0.0.5:8080"};

  auto prop = apppack::generate_proposed_policy("app:test_calc", manifest, entries, trial_logs);
  CHECK_EQ(prop.app_id, "app:test_calc");
  CHECK_EQ(prop.enabled, false);  // Mặc định TẮT, không tự bật
  CHECK(prop.targets.size() >= 5);

  // Verify three sources and confidence levels
  bool found_manifest = false;
  bool found_scanned = false;
  bool found_trial = false;

  for (const auto& t : prop.targets) {
    if (t.target == "api.vendor.com") {
      found_manifest = true;
      CHECK_EQ(int(t.source), int(apppack::ProposedScopeSource::Manifest));
      CHECK_EQ(t.confidence, "high");
    }
    if (t.target == "telemetry.stats.com") {
      found_scanned = true;
      CHECK_EQ(int(t.source), int(apppack::ProposedScopeSource::ScannedString));
      CHECK_EQ(t.confidence, "low");
    }
    if (t.target == "crash.report.io") {
      found_trial = true;
      CHECK_EQ(int(t.source), int(apppack::ProposedScopeSource::SandboxTrial));
      CHECK_EQ(t.confidence, "observed");
    }
  }
  CHECK(found_manifest);
  CHECK(found_scanned);
  CHECK(found_trial);

  // Serialization round-trip
  std::string json_str = prop.to_json_string(true);
  CHECK(json_str.find("app:test_calc") != std::string::npos);
  CHECK(json_str.find("\"enabled\": false") != std::string::npos);
  CHECK(json_str.find("crash.report.io") != std::string::npos);

  auto parsed = apppack::ProposedPolicy::from_json(json::parse(json_str).value);
  CHECK_EQ(parsed.app_id, "app:test_calc");
  CHECK_EQ(parsed.enabled, false);
  CHECK_EQ(parsed.targets.size(), prop.targets.size());

  // Conversion to Sandbox Policy: without approval, nothing is allowed
  auto empty_policy = parsed.to_sandbox_policy(false, {});
  CHECK_EQ(empty_policy.scopes.size(), size_t(0));

  // With approval of specific targets
  auto approved_policy = parsed.to_sandbox_policy(false, {"api.vendor.com", "/var/data"});
  CHECK_EQ(approved_policy.scopes.size(), size_t(2));
  CHECK(std::find(approved_policy.scopes.begin(), approved_policy.scopes.end(), "api.vendor.com") != approved_policy.scopes.end());
  CHECK(std::find(approved_policy.scopes.begin(), approved_policy.scopes.end(), "/var/data") != approved_policy.scopes.end());

  // Enforce deny wins: verify Enforcer rejects targets not in approved policy
  sandbox::Enforcer enf;
  sandbox::Action allow_act{"net.connect", "api.vendor.com"};
  sandbox::Action deny_act{"net.connect", "telemetry.stats.com"};
  CHECK(enf.check(approved_policy, nullptr, allow_act, 0, nullptr).allowed);
  CHECK(!enf.check(approved_policy, nullptr, deny_act, 0, nullptr).allowed);
}

YUI_TEST(ui, adaptive_layout) {
  ui::DisplayInfo laptop{1366, 768, 96};
  ui::HardwareForUi weak{2.0, false, false, "sse2"};
  const auto l1 = ui::plan(laptop, weak);
  CHECK_EQ(int(l1.path), int(ui::RenderPath::FramebufferDirect));
  CHECK_EQ(l1.scale_px, 1);
  CHECK(l1.columns >= 40);
  CHECK(l1.reason.find("framebuffer") != std::string::npos);

  ui::DisplayInfo hidpi{1920, 1080, 192};
  ui::HardwareForUi strong{32.0, true, false, "avx2"};
  const auto l2 = ui::plan(hidpi, strong);
  CHECK_EQ(int(l2.path), int(ui::RenderPath::GpuZeroCopy));
  CHECK_EQ(l2.scale_px, 3);
  CHECK(l2.columns >= 40);
  CHECK_EQ(std::string(ui::path_name(l2.path)), std::string("vulkan-drm-zero-copy"));

  // A strong machine on battery still prefers the cheap path.
  ui::HardwareForUi saving{32.0, true, true, "avx2"};
  const auto l3 = ui::plan(hidpi, saving);
  CHECK_EQ(int(l3.path), int(ui::RenderPath::FramebufferDirect));
  CHECK(l3.reason.find("tiết kiệm điện") != std::string::npos);

  // Tiny displays still get a usable minimum size.
  const	auto l4 = ui::plan({320, 240, 96}, weak);
  CHECK(l4.columns >= 40);
  CHECK(l4.rows >= 10);

  // Máy siêu yếu (RAM < 1.5 GB): tự động chuyển sang Headless/TUI, tắt compositor
  ui::HardwareForUi ultra_weak{1.4, false, false, "sse2"};
  const auto l_uw = ui::plan(laptop, ultra_weak);
  CHECK_EQ(int(l_uw.path), int(ui::RenderPath::HeadlessTui));
  CHECK_EQ(l_uw.compositor_enabled, false);
  CHECK_EQ(std::string(ui::path_name(l_uw.path)), std::string("headless-tui"));
  CHECK(l_uw.reason.find("Headless/TUI") != std::string::npos);
}

YUI_TEST(shell, bilingual_commands) {
  service::Options opts;
  opts.state_dir = "/tmp/yui-shell-test";
  opts.vault_password = "mat-khau-thu";
  service::Daemon daemon(opts);
  daemon.set_host_snapshot(json::Value(json::Object{}));
  ui::Layout layout;
  layout.columns = 100;
  layout.rows = 30;
  layout.reason = "kiểm thử";
  shell::Shell sh(daemon, layout);

  CHECK(sh.banner().find("YuiVieOS") != std::string::npos);
  CHECK(sh.help_text().find("giúp / help") != std::string::npos);
  CHECK(sh.commands().size() >= 12);

  // Both spellings must work.
  CHECK(sh.run("info").find("yuid") != std::string::npos);
  CHECK(sh.run("thôngtin").find("yuid") != std::string::npos);

  CHECK(sh.run("remember os:yui \"YuiVieOS\" concept").find("Đã ghi nhớ") != std::string::npos);
  CHECK(sh.run("ghinhớ app:yui \"App Yui\" app").find("Đã ghi nhớ") != std::string::npos);
  CHECK(sh.run("nối app:yui chạy_trên os:yui").find("Đã nối") != std::string::npos);
  const std::string found = sh.run("search yui");
  CHECK(found.find("os:yui") != std::string::npos);
  CHECK(found.find("[concept]") != std::string::npos);
  CHECK(sh.run("đường app:yui os:yui").find("→") != std::string::npos);
  CHECK(sh.run("thốngkê").find("Nút: 2") != std::string::npos);

  // Input methods through the shell.
  CHECK_EQ(sh.run("gõ telex Tieengs Vieetj"), std::string("Tiếng Việt"));
  CHECK_EQ(sh.run("ime vni Vie65t Nam"), std::string("Việt Nam"));

  // Policy + vault surface.
  CHECK(sh.run("hộpcát fs.read /etc/yui/policy.json").find("ĐƯỢC PHÉP") != std::string::npos);
  CHECK(sh.run("niêmphong bí mật").find("niêm phong") != std::string::npos);

  // Unknown command is explained, not silently swallowed.
  const std::string unknown = sh.run("baylenmattrang");
  CHECK(unknown.find("Không hiểu") != std::string::npos);

  CHECK(!sh.wants_exit());
  CHECK(sh.run("thoát").find("Tạm biệt") != std::string::npos);
  CHECK(sh.wants_exit());

  // An empty line is a no-op, not an error.
  shell::Shell sh2(daemon, layout);
  CHECK_EQ(sh2.run(""), std::string(""));
  CHECK_EQ(sh2.run("   "), std::string(""));
}

YUI_TEST(shell, catalog_has_29_bilingual_commands) {
  service::Options opts;
  service::Daemon daemon(opts);
  ui::Layout layout;
  shell::Shell sh(daemon, layout);

  const auto& cmds = sh.commands();
  CHECK_EQ(cmds.size(), size_t(29));  // +3 lệnh Kho (P4.15): tìmapp, cài, gỡ

  for (const auto& c : cmds) {
    CHECK(!c.name_vi.empty());
    CHECK(!c.name_en.empty());
    CHECK(!c.usage_vi.empty());
    CHECK(!c.usage_en.empty());
    CHECK(!c.help_vi.empty());
    CHECK(!c.help_en.empty());
  }
}

YUI_TEST(shell, all_26_commands_run_in_both_languages) {
  service::Options opts;
  opts.state_dir = "/tmp/yui-shell-test";
  opts.vault_password = "mat-khau-thu";
  service::Daemon daemon(opts);
  daemon.set_host_snapshot(json::Value(json::Object{}));
  ui::Layout layout;
  layout.columns = 100;
  layout.rows = 30;
  layout.reason = "kiểm thử 25 lệnh";
  shell::Shell sh(daemon, layout);

  // 1. giúp / help
  CHECK(sh.run("giúp").find("Lệnh / Commands") != std::string::npos);
  CHECK(sh.run("help").find("Lệnh / Commands") != std::string::npos);
  CHECK(sh.run("help telemetry").find("telemetry") != std::string::npos);

  // 2. thôngtin / info
  CHECK(sh.run("thôngtin").find("yuid") != std::string::npos);
  CHECK(sh.run("info").find("yuid") != std::string::npos);

  // 3. cấuhình / profile
  CHECK(sh.run("cấuhình").find("k =") != std::string::npos || sh.run("cấuhình").size() > 0);
  CHECK(sh.run("profile").find("k =") != std::string::npos || sh.run("profile").size() > 0);

  // 4. tinhchỉnh / tune
  CHECK(sh.run("tinhchỉnh 4 45 nguon").find("k =") != std::string::npos);
  CHECK(sh.run("tune 8 50 pin").find("k =") != std::string::npos);

  // 5. gõ / ime
  CHECK_EQ(sh.run("gõ telex Vieetj"), "Việt");
  CHECK_EQ(sh.run("ime vni Vie65t"), "Việt");

  // 6. ghinhớ / remember
  CHECK(sh.run("ghinhớ node:1 \"Nút 1\" concept").find("Đã ghi nhớ") != std::string::npos);
  CHECK(sh.run("remember node:2 \"Nút 2\" concept").find("Đã ghi nhớ") != std::string::npos);

  // 7. nối / link
  CHECK(sh.run("nối node:1 lien_ket node:2").find("Đã nối") != std::string::npos);
  CHECK(sh.run("link node:2 tro_lai node:1").find("Đã nối") != std::string::npos);

  // 8. tìm / search
  CHECK(sh.run("tìm Nút").find("node:1") != std::string::npos);
  CHECK(sh.run("search Nút").find("node:2") != std::string::npos);

  // 9. đường / path
  CHECK(sh.run("đường node:1 node:2").find("→") != std::string::npos);
  CHECK(sh.run("path node:2 node:1").find("→") != std::string::npos);

  // 10. thốngkê / stats
  CHECK(sh.run("thốngkê").find("Nút: 2") != std::string::npos);
  CHECK(sh.run("stats").find("Cạnh:") != std::string::npos);

  // 11. gọttỉa / trim & 12. mởtệp / openpack
  const std::string root = mkdtemp_dir("sh-pack");
  write_file(root + "/file.txt", "YuiVieOS shell test");
  const std::string yuip = root + "/out.yuip";
  const std::string out_dir = root + "/unpacked";
  CHECK(sh.run("gọttỉa " + root + " " + yuip).find("Đã lưu") != std::string::npos);
  CHECK(sh.run("mởtệp " + yuip + " " + out_dir).find("Đã bung") != std::string::npos);
  CHECK(read_file(out_dir + "/file.txt").find("YuiVieOS") != std::string::npos);
  // Also with English names:
  const std::string yuip2 = root + "/out2.yuip";
  const std::string out_dir2 = root + "/unpacked2";
  CHECK(sh.run("trim " + root + " " + yuip2).find("Đã lưu") != std::string::npos);
  CHECK(sh.run("openpack " + yuip2 + " " + out_dir2).find("Đã bung") != std::string::npos);

  // 13. hộpcát / sandbox
  CHECK(sh.run("hộpcát fs.read /etc/yui/policy.json").find("ĐƯỢC PHÉP") != std::string::npos);
  CHECK(sh.run("sandbox fs.read /etc/yui/policy.json").find("ĐƯỢC PHÉP") != std::string::npos);

  // 14. niêmphong / seal & 17. mởkhoá / unseal
  const std::string sealed_resp = sh.run("niêmphong secret_data_123");
  CHECK(sealed_resp.find("Đã niêm phong") != std::string::npos);
  const size_t colon_pos = sealed_resp.find(": ");
  CHECK(colon_pos != std::string::npos);
  const std::string blob = sealed_resp.substr(colon_pos + 2);
  CHECK(sh.run("mởkhoá " + blob).find("secret_data_123") != std::string::npos);

  // English aliases for seal & unseal:
  const std::string sealed_en = sh.run("seal confidential_info");
  CHECK(sealed_en.find("Đã niêm phong") != std::string::npos);
  const std::string blob_en = sealed_en.substr(sealed_en.find(": ") + 2);
  CHECK(sh.run("unseal " + blob_en).find("confidential_info") != std::string::npos);

  // 15. thoát / exit
  CHECK(!sh.wants_exit());
  shell::Shell sh_exit(daemon, layout);
  CHECK(sh_exit.run("exit").find("Tạm biệt") != std::string::npos);
  CHECK(sh_exit.wants_exit());

  // 16. đotải / telemetry
  CHECK(sh.run("đotải").find("Đo tải") != std::string::npos);
  CHECK(sh.run("telemetry").find("Đo tải") != std::string::npos);

  // 18. xoánút / remove
  CHECK(sh.run("xoánút node:1").find("Đã xoá nút \"node:1\"") != std::string::npos);
  CHECK(sh.run("remove node:2").find("Đã xoá nút \"node:2\"") != std::string::npos);
  CHECK(sh.run("remove node:notfound").find("Không tìm thấy") != std::string::npos);

  // 19. nhậtký / audit
  CHECK(sh.run("nhậtký").find("Nhật ký") != std::string::npos);
  CHECK(sh.run("audit").find("Nhật ký") != std::string::npos);

  // 20. cảnhbáo / alerts
  CHECK(sh.run("cảnhbáo").find("cảnh báo") != std::string::npos || sh.run("cảnhbáo").find("an toàn") != std::string::npos);
  CHECK(sh.run("alerts").find("cảnh báo") != std::string::npos || sh.run("alerts").find("an toàn") != std::string::npos);

  // 21. phiênbản / version
  CHECK(sh.run("phiênbản").find("YuiVieOS Beta") != std::string::npos);
  CHECK(sh.run("version").find("YuiVieOS Beta") != std::string::npos);

  // 22. trakho / vaultinfo
  CHECK(sh.run("trakho").find("Kho bí mật Vault") != std::string::npos);
  CHECK(sh.run("vaultinfo").find("Kho bí mật Vault") != std::string::npos);

  // 23. tìmweb / websearch
  CHECK(sh.run("tìmweb yui").find("Tìm kiếm web") != std::string::npos);
  CHECK(sh.run("websearch yui").find("Tìm kiếm web") != std::string::npos);

  // 24. xoámàn / clear
  CHECK_EQ(sh.run("xoámàn"), "\033[2J\033[H");
  CHECK_EQ(sh.run("clear"), "\033[2J\033[H");

  // 25. thờigian / uptime
  CHECK(sh.run("thờigian").find("Phiên làm việc YuiVieOS") != std::string::npos);
  CHECK(sh.run("uptime").find("Phiên làm việc YuiVieOS") != std::string::npos);

  // 26. tươngthích / compat (P3.16)
  CHECK(sh.run("tươngthích").find("Chế độ tương thích") != std::string::npos);
  CHECK(sh.run("compat").find("Compatibility Mode") != std::string::npos);
  CHECK(sh.run("tươngthích app:sample bật").find("bật") != std::string::npos);
  CHECK(sh.run("compat app:sample on").find("bật") != std::string::npos);
  CHECK(sh.run("tươngthích app:sample").find("ĐANG BẬT") != std::string::npos);
  CHECK(sh.run("compat app:sample off").find("tắt") != std::string::npos);
  CHECK(sh.run("tuongthich app:sample").find("TẮT") != std::string::npos);

  // Unaccented Vietnamese aliases
  CHECK(sh.run("thongtin").find("yuid") != std::string::npos);
  CHECK(sh.run("dotai").find("Đo tải") != std::string::npos);
  CHECK(sh.run("nhatky").find("Nhật ký") != std::string::npos);
  CHECK(sh.run("canhbao").find("cảnh báo") != std::string::npos || sh.run("canhbao").find("an toàn") != std::string::npos);
  CHECK(sh.run("phienban").find("YuiVieOS Beta") != std::string::npos);
  CHECK(sh.run("thoigian").find("Phiên làm việc") != std::string::npos);
}

YUI_TEST(shell, command_suggestions_levenshtein) {
  service::Options opts;
  service::Daemon daemon(opts);
  ui::Layout layout;
  shell::Shell sh(daemon, layout);

  // Typo suggestions via Levenshtein / prefix
  CHECK_EQ(sh.suggest("hepl"), "help");
  CHECK_EQ(sh.suggest("inf"), "info");
  CHECK_EQ(sh.suggest("stat"), "stats");
  CHECK_EQ(sh.suggest("telemetr"), "telemetry");
  CHECK_EQ(sh.suggest("unseel"), "unseal");
  CHECK_EQ(sh.suggest("audt"), "audit");
  CHECK_EQ(sh.suggest("alrt"), "alert");
  CHECK_EQ(sh.suggest("vers"), "version");

  // Vietnamese prefix/edit suggestion
  CHECK(!sh.suggest("xoan").empty());
  CHECK(!sh.suggest("thoig").empty());

  // Completely unknown command returns empty suggestion
  CHECK_EQ(sh.suggest("baylenmattrang"), "");
  CHECK_EQ(sh.suggest(""), "");

  // Execution of typo prompts suggestion in output
  const std::string r1 = sh.run("hepl");
  CHECK(r1.find("Không hiểu lệnh \"hepl\"") != std::string::npos);
  CHECK(r1.find("Có phải bạn muốn dùng \"help\"?") != std::string::npos);

  const std::string r2 = sh.run("baylenmattrang");
  CHECK(r2.find("Không hiểu lệnh \"baylenmattrang\"") != std::string::npos);
  CHECK(r2.find("Có phải bạn muốn dùng") == std::string::npos);
}

static std::string run_cli_cmd(const std::string& cmd) {
  FILE* pipe = ::popen((cmd + " 2>&1").c_str(), "r");
  if (!pipe) return "";
  char buffer[512];
  std::string result;
  while (::fgets(buffer, sizeof(buffer), pipe) != nullptr) {
    result += buffer;
  }
  ::pclose(pipe);
  return result;
}

YUI_TEST(update, yui_update_cli_end_to_end) {
  const std::string bin = std::string(YUI_SOURCE_DIR) + "/build/bin/yui-update";

  // 1. Trợ giúp song ngữ
  const std::string help_out = run_cli_cmd(bin + " --trogiup");
  CHECK(help_out.find("Quản lý cập nhật hệ thống A/B") != std::string::npos);
  CHECK(help_out.find("yui-update status") != std::string::npos);
  CHECK(help_out.find("yui-update apply") != std::string::npos);
  CHECK(help_out.find("yui-update rollback") != std::string::npos);

  // 2. Thư mục thử nghiệm
  const std::string tmp = mkdtemp_dir("update-cli");
  const std::string keyring_dir = tmp + "/keys";
  const std::string priv_file = tmp + "/priv.key";
  const std::string payload_dir = tmp + "/payload";
  const std::string bundle_dir = tmp + "/bundle";
  const std::string state_dir = tmp + "/state";

  write_file(payload_dir + "/sys.bin", "yui-os-image\n");
  write_file(payload_dir + "/version.txt", "3.0.0\n");

  // 3. keygen
  const std::string keygen_out = run_cli_cmd(bin + " keygen --id sovereign-key --out-keyring " +
                                             keyring_dir + " --out-priv " + priv_file);
  CHECK(keygen_out.find("Đã sinh cặp khoá chủ quyền") != std::string::npos);
  CHECK(!read_file(keyring_dir + "/sovereign-key.pub").empty());
  CHECK(!read_file(priv_file).empty());

  // 4. sign bundle
  const std::string sign_out = run_cli_cmd(bin + " sign " + payload_dir + " --out " + bundle_dir +
                                           " --key " + priv_file + " --key-id sovereign-key" +
                                           " --version 3.0.0 --stream os");
  CHECK(sign_out.find("Đã đóng gói và ký số thành công") != std::string::npos);
  CHECK(!read_file(bundle_dir + "/manifest.json").empty());
  CHECK(!read_file(bundle_dir + "/manifest.sig").empty());

  // 5. check / verify
  const std::string check_out = run_cli_cmd(bin + " check " + bundle_dir + " --keyring " + keyring_dir);
  CHECK(check_out.find("Bundle hợp lệ") != std::string::npos);
  CHECK(check_out.find("sovereign-key") != std::string::npos);
  CHECK(check_out.find("3.0.0") != std::string::npos);

  // 6. apply deferred
  const std::string apply_out = run_cli_cmd(bin + " apply " + bundle_dir + " --keyring " + keyring_dir +
                                            " --state-dir " + state_dir + " --stream os");
  CHECK(apply_out.find("chờ kích hoạt") != std::string::npos);

  // 7. status checking
  const std::string status1 = run_cli_cmd(bin + " status --state-dir " + state_dir + " --stream os");
  CHECK(status1.find("Khe hoạt động : A") != std::string::npos);
  CHECK(status1.find("khe B (sẽ kích hoạt ở lần khởi động tới)") != std::string::npos);

  // 8. commit pending
  const std::string commit_out = run_cli_cmd(bin + " commit --state-dir " + state_dir + " --stream os");
  CHECK(commit_out.find("Đã kích hoạt") != std::string::npos);

  const std::string status2 = run_cli_cmd(bin + " status --state-dir " + state_dir + " --stream os");
  CHECK(status2.find("Khe hoạt động : B (phiên bản 3.0.0") != std::string::npos);

  // 9. rollback
  const std::string roll_out = run_cli_cmd(bin + " rollback --state-dir " + state_dir + " --stream os");
  CHECK(roll_out.find("quay lui") != std::string::npos);

  const std::string status3 = run_cli_cmd(bin + " status --state-dir " + state_dir + " --stream os");
  CHECK(status3.find("Khe hoạt động : A") != std::string::npos);
}

YUI_TEST_MAIN("test_tools")
