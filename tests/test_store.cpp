// Kho Ứng Dụng — lõi (P4.15): danh mục ký → tải → kiểm → gọt → hộp cát → biểu tượng.
//
// Mỗi kịch bản trong DoD có ít nhất một test riêng: băm lệch, danh mục bị sửa,
// URL ngoài danh sách, tải dở dang, gỡ sạch dấu vết, hai app cùng tên, zip-slip,
// không xác nhận thì không cài, app không chữ ký ⇒ P3.12, …
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "testutil.hpp"
#include "yui/crypto.hpp"
#include "yui/service.hpp"
#include "yui/shell.hpp"
#include "yui/store.hpp"
#include "yui/text.hpp"

using namespace yui;

namespace {

using Bytes = std::vector<uint8_t>;

std::string mkdtemp_dir(const std::string& tag) {
  std::string tmpl = "/tmp/yuitest-" + tag + "-XXXXXX";
  std::vector<char> buf(tmpl.begin(), tmpl.end());
  buf.push_back('\0');
  const char* dir = ::mkdtemp(buf.data());
  return dir ? std::string(dir) : std::string();
}

bool exists(const std::string& p) {
  struct stat st{};
  return ::lstat(p.c_str(), &st) == 0;
}

int64_t fsize(const std::string& p) {
  struct stat st{};
  return ::stat(p.c_str(), &st) == 0 ? int64_t(st.st_size) : -1;
}

std::string slurp(const std::string& path) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return "";
  std::string s;
  char buf[4096];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
  std::fclose(f);
  return s;
}

void spit(const std::string& path, const Bytes& b) {
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return;
  if (!b.empty()) std::fwrite(b.data(), 1, b.size(), f);
  std::fclose(f);
}

// Mọi đường dẫn (tương đối) dưới root, để kiểm "gỡ sạch dấu vết".
void walk(const std::string& root, const std::string& rel, std::vector<std::string>& out) {
  DIR* d = ::opendir((rel.empty() ? root : root + "/" + rel).c_str());
  if (!d) return;
  std::vector<std::string> names;
  while (dirent* e = ::readdir(d)) {
    const std::string n = e->d_name;
    if (n != "." && n != "..") names.push_back(n);
  }
  ::closedir(d);
  for (const auto& n : names) {
    const std::string r = rel.empty() ? n : rel + "/" + n;
    out.push_back(r);
    struct stat st{};
    if (::lstat((root + "/" + r).c_str(), &st) == 0 && S_ISDIR(st.st_mode)) walk(root, r, out);
  }
}

apppack::Entry file_entry(const std::string& path, const std::string& content, uint32_t mode = 0644) {
  apppack::Entry e;
  e.path = path;
  e.mode = mode;
  e.kind = (mode & 0111) ? 'x' : 'f';
  e.data.assign(content.begin(), content.end());
  e.size = e.data.size();
  return e;
}

Bytes hello_tar(const std::string& banner = "xin chao") {
  return store::build_tar({
      file_entry("bin/hello", "#!/bin/sh\necho " + banner + "\n", 0755),
      file_entry("locale/vi/messages", "Xin chào\n"),
      file_entry("locale/fr/messages", "Bonjour\n"),
      file_entry("share/hello/data.txt", "dữ liệu\n"),
  });
}

ed25519::Seed seed_of(uint8_t b) {
  ed25519::Seed s{};
  for (size_t i = 0; i < s.size(); ++i) s[i] = uint8_t(b + i * 7);
  return s;
}

const ed25519::Seed kCatalogSeed = seed_of(1);
const ed25519::Seed kPublisherSeed = seed_of(2);
const ed25519::Seed kStrangerSeed = seed_of(3);

bundle::Keyring trusted_keys() {
  bundle::Keyring k;
  k.add("kho-yui", ed25519::public_key(kCatalogSeed));
  k.add("nxb-vidu", ed25519::public_key(kPublisherSeed));
  return k;
}

store::AppEntry entry_for(const std::string& id, const Bytes& pkg, const std::string& url) {
  store::AppEntry e;
  e.id = id;
  e.name = "Xin Chào";
  e.version = "1.0.0";
  e.publisher = "Ví Dụ";
  e.kind = "native";
  e.format = "tar";
  e.source_url = url;
  e.sha256 = crypto::hex(crypto::sha256(pkg));
  e.size = pkg.size();
  e.license = "MIT";
  e.exec = "bin/hello";
  json::Value pv(json::Object{});
  pv.set("app", id);
  pv.set("tier", "isolated");
  json::Value caps(json::Array{});
  caps.push("fs.read");
  caps.push("net:api.example.org");
  pv.set("capabilities", caps);
  e.policy = sandbox::Policy::from_json(pv);
  return e;
}

store::Catalog catalog_with(std::vector<store::AppEntry> apps) {
  store::Catalog c;
  c.key_id = "kho-yui";
  c.issued = "2026-11-19";
  c.allowed_hosts = {"downloads.example.org"};
  c.apps = std::move(apps);
  return c;
}

// Tải giả: phục vụ byte theo URL, có thể dừng giữa chừng hoặc "chuyển hướng".
struct MapFetcher : store::Fetcher {
  std::map<std::string, Bytes> files;
  size_t fail_after = 0;           // >0: chỉ ghi chừng này byte rồi báo lỗi
  std::string redirect_to;         // URL cuối giả lập
  int calls = 0;
  std::vector<int64_t> offsets;    // kích thước .part khi bắt đầu mỗi lần gọi
  bool fetch(const std::string& url, const std::string& dest, std::string& effective, std::string* err) override {
    ++calls;
    effective = redirect_to.empty() ? url : redirect_to;
    const auto it = files.find(url);
    if (it == files.end()) {
      if (err) *err = "404";
      return false;
    }
    const int64_t have = std::max<int64_t>(0, fsize(dest));
    offsets.push_back(have);
    FILE* f = std::fopen(dest.c_str(), "ab");
    if (!f) return false;
    size_t n = it->second.size() - size_t(std::min<int64_t>(have, int64_t(it->second.size())));
    bool ok = true;
    if (fail_after && n > fail_after) {
      n = fail_after;
      ok = false;
    }
    std::fwrite(it->second.data() + have, 1, n, f);
    std::fclose(f);
    if (!ok && err) *err = "mất kết nối giữa chừng";
    return ok;
  }
};

struct AllowGate : store::ConfirmationGate {
  int asked = 0;
  bool confirmed(const store::InstallPlan&) override {
    ++asked;
    return true;
  }
};
struct DenyGate : store::ConfirmationGate {
  int asked = 0;
  bool confirmed(const store::InstallPlan&) override {
    ++asked;
    return false;
  }
};

const std::string kUrl = "https://downloads.example.org/hello-1.0.0.tar";

struct Fixture {
  std::string root = mkdtemp_dir("store");
  bundle::Keyring keys = trusted_keys();
  Bytes pkg = hello_tar();
  MapFetcher fetcher;
  store::Options opts() const {
    store::Options o;
    o.root = root;
    return o;
  }
};

}  // namespace

// ------------------------------------------------------------- tiện ích --
YUI_TEST(store, url_and_unaccent_helpers) {
  CHECK_EQ(store::url_host("https://Downloads.Example.org:443/a?b"), std::string("downloads.example.org"));
  CHECK_EQ(store::url_host("https://user@evil.example/x"), std::string(""));  // lừa host bằng '@'
  CHECK_EQ(store::url_host("https://bad host/x"), std::string(""));
  CHECK_EQ(store::url_scheme("HTTPS://a.b/"), std::string("https"));
  CHECK_EQ(store::fold_unaccent("Ghi Chú Đẹp"), std::string("ghi chu dep"));
  CHECK_EQ(store::fold_unaccent("TRÌNH SOẠN THẢO"), std::string("trinh soan thao"));
  CHECK(store::safe_relative_path("bin/hello"));
  CHECK(!store::safe_relative_path("../x"));
  CHECK(!store::safe_relative_path("/etc/passwd"));
  CHECK(!store::safe_relative_path("a/./b"));
  CHECK(!store::safe_relative_path("a\\..\\b"));
}

YUI_TEST(store, tar_round_trip_and_rejects_escape) {
  std::vector<apppack::Entry> out;
  std::string err;
  CHECK(store::parse_tar(hello_tar(), out, &err));
  CHECK_EQ(out.size(), size_t(4));
  CHECK_EQ(out[0].path, std::string("bin/hello"));
  CHECK(out[0].kind == 'x');
  // Đường dẫn dài > 100 ký tự đi qua trường prefix của ustar.
  const std::string long_path = std::string(60, 'a') + "/" + std::string(60, 'b') + "/tệp.txt";
  CHECK(store::parse_tar(store::build_tar({file_entry(long_path, "x")}), out, &err));
  CHECK(out.size() == 1 && out[0].path == long_path);

  CHECK(!store::parse_tar(store::build_tar({file_entry("../evil.sh", "x")}), out, &err));
  CHECK(err.find("zip-slip") != std::string::npos);
  CHECK(!store::parse_tar(store::build_tar({file_entry("/tmp/evil.sh", "x")}), out, &err));
  apppack::Entry link;
  link.path = "bin/sh";
  link.kind = 'l';
  const std::string target = "/bin/sh";
  link.data.assign(target.begin(), target.end());
  CHECK(!store::parse_tar(store::build_tar({link}), out, &err));
  CHECK(err.find("liên kết") != std::string::npos);
  Bytes cut = hello_tar();
  cut.resize(700);  // tải dở: header + một phần dữ liệu
  CHECK(!store::parse_tar(cut, out, &err));
  Bytes bad = hello_tar();
  bad[10] ^= 0x01;  // hỏng header ⇒ sai checksum
  CHECK(!store::parse_tar(bad, out, &err));
}

// --------------------------------------------------------- danh mục ký --
YUI_TEST(store, catalog_signature_is_mandatory) {
  Fixture fx;
  const auto cat = catalog_with({entry_for("org.example.hello", fx.pkg, kUrl)});
  const std::string text = store::catalog_to_json(cat);
  const std::string sig = store::sign_catalog(cat, kCatalogSeed);
  store::Store s(fx.opts(), fx.keys);

  CHECK_EQ(s.load_catalog(text, "").code, std::string("unsigned_catalog"));
  CHECK(!s.has_catalog());
  CHECK_EQ(s.load_catalog(text, std::string(128, '0')).code, std::string("bad_signature"));
  CHECK_EQ(s.load_catalog(text, "zz").code, std::string("bad_signature"));

  // Ký bằng khoá lạ (không có trong keyring).
  auto stranger = cat;
  stranger.key_id = "la-mat";
  CHECK_EQ(s.load_catalog(store::catalog_to_json(stranger), store::sign_catalog(stranger, kStrangerSeed)).code,
           std::string("untrusted_key"));
  // Khoá tin cậy nhưng ghi key_id của người khác.
  auto forged = cat;
  CHECK_EQ(s.load_catalog(text, store::sign_catalog(forged, kStrangerSeed)).code, std::string("bad_signature"));

  // Danh mục bị sửa sau khi ký: đổi URL / đổi băm / thêm host.
  std::string tampered = text;
  tampered.replace(tampered.find("hello-1.0.0.tar"), 15, "hello-1.0.1.tar");
  CHECK_EQ(s.load_catalog(tampered, sig).code, std::string("bad_signature"));
  auto more_hosts = cat;
  more_hosts.allowed_hosts.push_back("evil.example.net");
  CHECK_EQ(s.load_catalog(store::catalog_to_json(more_hosts), sig).code, std::string("bad_signature"));
  CHECK(!s.has_catalog());

  // Bản gốc: hợp lệ, kể cả khi in lại khác khoảng trắng (chữ ký trên dạng chuẩn hoá).
  const auto ok = s.load_catalog(text, sig);
  CHECK(ok.ok);
  CHECK(s.has_catalog());
  CHECK(s.load_catalog(store::catalog_to_json(cat, false), sig).ok);
  // Lưu và nạp lại từ đĩa.
  CHECK(s.save_catalog(text, sig));
  store::Store again(fx.opts(), fx.keys);
  CHECK(again.load_catalog_files().ok);
}

YUI_TEST(store, catalog_structure_rules) {
  Fixture fx;
  std::string err;
  store::Catalog out;
  auto e = entry_for("org.example.hello", fx.pkg, kUrl);
  // http:// (không mã hoá) bị từ chối ngay khi parse.
  auto http = e;
  http.source_url = "http://downloads.example.org/hello.tar";
  CHECK(!store::parse_catalog(store::catalog_to_json(catalog_with({http})), out, &err));
  // id trùng.
  CHECK(!store::parse_catalog(store::catalog_to_json(catalog_with({e, e})), out, &err));
  CHECK(err.find("cùng id") != std::string::npos);
  // id sai dạng, exec thoát thư mục, sha256 sai.
  auto bad_id = e;
  bad_id.id = "Org/Evil";
  CHECK(!store::parse_catalog(store::catalog_to_json(catalog_with({bad_id})), out, &err));
  auto bad_exec = e;
  bad_exec.exec = "../../bin/sh";
  CHECK(!store::parse_catalog(store::catalog_to_json(catalog_with({bad_exec})), out, &err));
  auto bad_sha = e;
  bad_sha.sha256 = "abc";
  CHECK(!store::parse_catalog(store::catalog_to_json(catalog_with({bad_sha})), out, &err));
  // Chính sách xin admin hoặc '*' bị từ chối.
  std::string js = store::catalog_to_json(catalog_with({e}));
  std::string with_admin = js;
  with_admin.replace(with_admin.find("\"fs.read\""), 9, "\"admin\"");
  CHECK(!store::parse_catalog(with_admin, out, &err));
  CHECK(err.find("admin") != std::string::npos);
  std::string with_star = js;
  with_star.replace(with_star.find("\"fs.read\""), 9, "\"*\"");
  CHECK(!store::parse_catalog(with_star, out, &err));
  // Tier native trong danh mục bị hạ xuống isolated.
  std::string native = js;
  native.replace(native.find("\"isolated\""), 10, "\"native\"");
  CHECK(store::parse_catalog(native, out, &err));
  CHECK(out.apps[0].policy.tier == sandbox::Tier::Isolated);
}

// ------------------------------------------------------ cài thành công --
YUI_TEST(store, install_happy_path_signed_publisher) {
  Fixture fx;
  auto e = entry_for("org.example.hello", fx.pkg, kUrl);
  e.signer_key_id = "nxb-vidu";
  e.signature_hex = store::sign_entry(e, kPublisherSeed);
  const auto cat = catalog_with({e});
  store::Store s(fx.opts(), fx.keys);
  CHECK(s.load_catalog(store::catalog_to_json(cat), store::sign_catalog(cat, kCatalogSeed)).ok);

  store::InstallPlan plan;
  CHECK(s.plan("org.example.hello", plan).ok);
  const std::string sum = plan.summary_vi();
  CHECK(sum.find(kUrl) != std::string::npos);
  CHECK(sum.find("MIT") != std::string::npos);
  CHECK(sum.find("net.connect") != std::string::npos);
  CHECK(plan.app_signature == sandbox::AppSignatureStatus::ValidTrusted);

  fx.fetcher.files[kUrl] = fx.pkg;
  AllowGate gate;
  const auto r = s.install("org.example.hello", fx.fetcher, gate);
  CHECK_MSG(r.ok, r.reason_vi);
  CHECK_EQ(gate.asked, 1);
  const std::string dir = fx.root + "/apps/org.example.hello";
  CHECK(exists(dir + "/bin/hello"));
  struct stat st{};
  CHECK(::stat((dir + "/bin/hello").c_str(), &st) == 0 && (st.st_mode & 0100));
  CHECK(exists(dir + "/locale/vi/messages"));
  CHECK(!exists(dir + "/locale/fr/messages"));  // gọt cục bộ: chỉ giữ vi, en
  CHECK(r.app.bytes_trimmed > 0);
  CHECK(!exists(s.cache_part_path(e)));         // không giữ bản sao gói gốc

  const auto pol = json::parse(slurp(fx.root + "/policies/org.example.hello.policy.json"));
  CHECK(pol.ok);
  CHECK_EQ(pol.value["app"].as_string(), std::string("org.example.hello"));
  CHECK_EQ(pol.value["tier"].as_string(), std::string("isolated"));
  CHECK(pol.value.dump().find("net.connect") != std::string::npos);  // chính sách khai báo được giữ
  const std::string desk = slurp(fx.root + "/menu/org.example.hello.desktop");
  CHECK(desk.find("Exec=" + dir + "/bin/hello") != std::string::npos);
  CHECK(desk.find("X-Yui-App-Id=org.example.hello") != std::string::npos);
  CHECK(desk.find("X-Yui-Policy=") != std::string::npos);
  CHECK(s.is_installed("org.example.hello"));
  CHECK_EQ(s.installed()[0].signature, std::string("valid"));

  // Cài lại cùng bản: không hỏi, không tải.
  const auto again = s.install("org.example.hello", fx.fetcher, gate);
  CHECK(again.ok && again.code == "already_installed");
  CHECK_EQ(fx.fetcher.calls, 1);
}

YUI_TEST(store, unsigned_app_gets_p312_sandbox) {
  Fixture fx;
  const auto cat = catalog_with({entry_for("org.example.hello", fx.pkg, kUrl)});
  store::Store s(fx.opts(), fx.keys);
  CHECK(s.load_catalog(store::catalog_to_json(cat), store::sign_catalog(cat, kCatalogSeed)).ok);
  store::InstallPlan plan;
  CHECK(s.plan("org.example.hello", plan).ok);
  CHECK(plan.app_signature == sandbox::AppSignatureStatus::Unsigned);
  CHECK((plan.enforced_policy.granted & sandbox::kRightNetConnect) == 0);
  CHECK(plan.enforced_policy.tier == sandbox::Tier::Isolated);

  fx.fetcher.files[kUrl] = fx.pkg;
  AllowGate gate;
  CHECK(s.install("org.example.hello", fx.fetcher, gate).ok);
  const std::string pol = slurp(fx.root + "/policies/org.example.hello.policy.json");
  CHECK(pol.find("net.connect") == std::string::npos);
  CHECK(pol.find("net:*") != std::string::npos);  // deny rõ ràng
  CHECK_EQ(s.installed()[0].signature, std::string("unsigned"));

  // Chữ ký nhà phát hành SAI ⇒ từ chối hẳn (không hạ xuống "không chữ ký").
  auto forged = entry_for("org.example.forged", fx.pkg, kUrl);
  forged.signer_key_id = "nxb-vidu";
  forged.signature_hex = store::sign_entry(forged, kStrangerSeed);
  const auto cat2 = catalog_with({forged});
  store::Store s2(fx.opts(), fx.keys);
  CHECK(s2.load_catalog(store::catalog_to_json(cat2), store::sign_catalog(cat2, kCatalogSeed)).ok);
  CHECK_EQ(s2.plan("org.example.forged", plan).code, std::string("bad_app_signature"));
}

// ------------------------------------------------------------ băm lệch --
YUI_TEST(store, hash_mismatch_rejected_and_cleaned) {
  Fixture fx;
  const auto cat = catalog_with({entry_for("org.example.hello", fx.pkg, kUrl)});
  store::Store s(fx.opts(), fx.keys);
  CHECK(s.load_catalog(store::catalog_to_json(cat), store::sign_catalog(cat, kCatalogSeed)).ok);
  Bytes evil = hello_tar("da bi sua");  // cùng cấu trúc, khác nội dung
  evil.resize(fx.pkg.size(), 0);
  CHECK(evil != fx.pkg);
  fx.fetcher.files[kUrl] = evil;
  AllowGate gate;
  const auto r = s.install("org.example.hello", fx.fetcher, gate);
  CHECK(!r.ok);
  CHECK_EQ(r.code, std::string("hash_mismatch"));
  CHECK(!exists(fx.root + "/apps/org.example.hello"));
  CHECK(!exists(s.cache_part_path(cat.apps[0])));
  CHECK(s.installed().empty());
  // Dài hơn khai báo ⇒ cũng từ chối.
  Bytes longer = fx.pkg;
  longer.push_back(0);
  fx.fetcher.files[kUrl] = longer;
  CHECK_EQ(s.install("org.example.hello", fx.fetcher, gate).code, std::string("size_mismatch"));
  CHECK(!exists(s.cache_part_path(cat.apps[0])));
}

// ------------------------------------------------- URL ngoài danh sách --
YUI_TEST(store, url_outside_allowlist_never_fetched) {
  Fixture fx;
  const std::string evil_url = "https://evil.example.net/hello.tar";
  const auto cat = catalog_with({entry_for("org.example.hello", fx.pkg, evil_url)});
  store::Store s(fx.opts(), fx.keys);
  CHECK(s.load_catalog(store::catalog_to_json(cat), store::sign_catalog(cat, kCatalogSeed)).ok);
  fx.fetcher.files[evil_url] = fx.pkg;
  AllowGate gate;
  const auto r = s.install("org.example.hello", fx.fetcher, gate);
  CHECK_EQ(r.code, std::string("host_not_allowed"));
  CHECK_EQ(fx.fetcher.calls, 0);
  CHECK_EQ(gate.asked, 0);

  // URL đúng nhưng bị chuyển hướng ra ngoài danh sách ⇒ xoá phần tải, từ chối.
  const auto cat2 = catalog_with({entry_for("org.example.hello", fx.pkg, kUrl)});
  store::Store s2(fx.opts(), fx.keys);
  CHECK(s2.load_catalog(store::catalog_to_json(cat2), store::sign_catalog(cat2, kCatalogSeed)).ok);
  fx.fetcher.files[kUrl] = fx.pkg;
  fx.fetcher.redirect_to = "https://mirror.evil.example.net/hello.tar";
  const auto r2 = s2.install("org.example.hello", fx.fetcher, gate);
  CHECK_EQ(r2.code, std::string("host_not_allowed"));
  CHECK(!exists(s2.cache_part_path(cat2.apps[0])));
  CHECK(!exists(fx.root + "/apps/org.example.hello"));

  // file:// không được nhận nếu không bật chế độ thử nghiệm.
  const auto cat3 = catalog_with({entry_for("org.example.hello", fx.pkg, "file:///tmp/hello.tar")});
  store::Store s3(fx.opts(), fx.keys);
  CHECK(s3.load_catalog(store::catalog_to_json(cat3), store::sign_catalog(cat3, kCatalogSeed)).ok);
  CHECK_EQ(s3.install("org.example.hello", fx.fetcher, gate).code, std::string("scheme_not_allowed"));
}

// ------------------------------------------------------- tải dở dang --
YUI_TEST(store, partial_download_resumes_never_installs_half) {
  Fixture fx;
  const auto cat = catalog_with({entry_for("org.example.hello", fx.pkg, kUrl)});
  store::Store s(fx.opts(), fx.keys);
  CHECK(s.load_catalog(store::catalog_to_json(cat), store::sign_catalog(cat, kCatalogSeed)).ok);
  fx.fetcher.files[kUrl] = fx.pkg;
  fx.fetcher.fail_after = fx.pkg.size() / 2;
  AllowGate gate;
  const auto r = s.install("org.example.hello", fx.fetcher, gate);
  CHECK_EQ(r.code, std::string("download_incomplete"));
  CHECK(!exists(fx.root + "/apps/org.example.hello"));
  CHECK(s.installed().empty());
  CHECK_EQ(fsize(s.cache_part_path(cat.apps[0])), int64_t(fx.pkg.size() / 2));  // giữ để tải tiếp

  fx.fetcher.fail_after = 0;
  const auto r2 = s.install("org.example.hello", fx.fetcher, gate);
  CHECK_MSG(r2.ok, r2.reason_vi);
  CHECK_EQ(fx.fetcher.offsets.size(), size_t(2));
  CHECK_EQ(fx.fetcher.offsets[1], int64_t(fx.pkg.size() / 2));  // nối tiếp, không tải lại từ đầu
  CHECK(exists(fx.root + "/apps/org.example.hello/bin/hello"));
}

// ---------------------------------------------------- gỡ sạch dấu vết --
YUI_TEST(store, remove_leaves_no_trace) {
  Fixture fx;
  const auto cat = catalog_with({entry_for("org.example.hello", fx.pkg, kUrl)});
  store::Store s(fx.opts(), fx.keys);
  CHECK(s.load_catalog(store::catalog_to_json(cat), store::sign_catalog(cat, kCatalogSeed)).ok);
  fx.fetcher.files[kUrl] = fx.pkg;
  AllowGate gate;
  CHECK(s.install("org.example.hello", fx.fetcher, gate).ok);
  // Một phần tải dở còn sót của cùng app (lần trước) cũng phải bị dọn.
  spit(fx.root + "/cache/org.example.hello@0.9.0.part", Bytes{1, 2, 3});

  const auto r = s.remove("org.example.hello");
  CHECK_MSG(r.ok, r.reason_vi);
  std::vector<std::string> all;
  walk(fx.root, "", all);
  bool trace = false;
  for (const auto& p : all)
    if (p.find("org.example.hello") != std::string::npos) trace = true;
  CHECK(!trace);
  CHECK(slurp(fx.root + "/installed.json").find("org.example.hello") == std::string::npos);
  CHECK_EQ(s.remove("org.example.hello").code, std::string("not_installed"));
}

// -------------------------------------------------- hai app cùng tên --
YUI_TEST(store, same_name_two_publishers_needs_id) {
  Fixture fx;
  auto a = entry_for("org.alpha.notes", fx.pkg, kUrl);
  a.name = "Ghi Chú";
  a.publisher = "Alpha";
  auto b = entry_for("vn.beta.notes", fx.pkg, kUrl);
  b.name = "Ghi Chú";
  b.publisher = "Beta";
  const auto cat = catalog_with({a, b});
  store::Store s(fx.opts(), fx.keys);
  CHECK(s.load_catalog(store::catalog_to_json(cat), store::sign_catalog(cat, kCatalogSeed)).ok);
  CHECK_EQ(s.search("ghi chu").size(), size_t(2));  // tìm không dấu
  CHECK_EQ(s.search("beta").size(), size_t(1));
  store::AppEntry got;
  const auto r = s.resolve("Ghi Chú", got);
  CHECK_EQ(r.code, std::string("ambiguous"));
  CHECK_EQ(r.candidates.size(), size_t(2));
  CHECK(r.reason_vi.find("Alpha") != std::string::npos && r.reason_vi.find("Beta") != std::string::npos);
  fx.fetcher.files[kUrl] = fx.pkg;
  AllowGate gate;
  CHECK_EQ(s.install("Ghi Chú", fx.fetcher, gate).code, std::string("ambiguous"));
  CHECK_EQ(gate.asked, 0);
  CHECK(s.install("vn.beta.notes", fx.fetcher, gate).ok);
  CHECK(s.is_installed("vn.beta.notes") && !s.is_installed("org.alpha.notes"));
}

// --------------------------------------------------------- zip-slip --
YUI_TEST(store, zip_slip_package_rejected) {
  Fixture fx;
  const Bytes evil = store::build_tar({file_entry("bin/hello", "#!/bin/sh\n", 0755),
                                       file_entry("../../yui-zipslip-proof.sh", "rm -rf ~\n", 0755)});
  const auto cat = catalog_with({entry_for("org.example.hello", evil, kUrl)});
  store::Store s(fx.opts(), fx.keys);
  CHECK(s.load_catalog(store::catalog_to_json(cat), store::sign_catalog(cat, kCatalogSeed)).ok);
  fx.fetcher.files[kUrl] = evil;
  AllowGate gate;
  const auto r = s.install("org.example.hello", fx.fetcher, gate);
  CHECK_EQ(r.code, std::string("unsafe_path"));
  CHECK(!exists(fx.root + "/../yui-zipslip-proof.sh"));
  CHECK(!exists("/tmp/yui-zipslip-proof.sh"));
  CHECK(!exists(fx.root + "/apps/org.example.hello"));
  std::vector<std::string> staging;
  walk(fx.root + "/staging", "", staging);
  CHECK(staging.empty());  // thư mục tạm luôn được dọn
}

// ----------------------------------------- không xác nhận thì không cài --
YUI_TEST(store, no_trusted_confirmation_no_install) {
  Fixture fx;
  const auto cat = catalog_with({entry_for("org.example.hello", fx.pkg, kUrl)});
  store::Store s(fx.opts(), fx.keys);
  CHECK(s.load_catalog(store::catalog_to_json(cat), store::sign_catalog(cat, kCatalogSeed)).ok);
  fx.fetcher.files[kUrl] = fx.pkg;

  DenyGate deny;
  CHECK_EQ(s.install("org.example.hello", fx.fetcher, deny).code, std::string("not_confirmed"));
  CHECK_EQ(fx.fetcher.calls, 0);  // không tải trước khi xác nhận

  sandbox::TrustedConfirmationHost host;
  const std::string dlg = host.show_dialog(store::TrustedDialogConfirmation::target_for("org.example.hello"),
                                           "Cài ứng dụng", "…", 1000);
  // Script/app giả cú bấm ⇒ bị chặn.
  sandbox::TaggedInputEvent synthetic;
  synthetic.source = sandbox::InputSourceKind::SyntheticApp;
  synthetic.x = 150;
  synthetic.y = 220;
  synthetic.is_button_down = true;
  host.handle_input(dlg, synthetic, 2000);
  store::TrustedDialogConfirmation g1(host, dlg);
  CHECK_EQ(s.install("org.example.hello", fx.fetcher, g1).code, std::string("not_confirmed"));

  // Cú bấm thật nhưng trên hộp thoại của việc KHÁC ⇒ không dùng được cho việc cài này.
  const std::string other = host.show_dialog("compat:org.other", "Khác", "…", 1000);
  sandbox::TaggedInputEvent physical = synthetic;
  physical.source = sandbox::InputSourceKind::PhysicalHardware;
  physical.hardware_device_id = 7;
  CHECK(host.handle_input(other, physical, 2000));
  store::TrustedDialogConfirmation g2(host, other);
  CHECK_EQ(s.install("org.example.hello", fx.fetcher, g2).code, std::string("not_confirmed"));
  CHECK_EQ(fx.fetcher.calls, 0);

  CHECK(host.handle_input(dlg, physical, 2000));
  store::TrustedDialogConfirmation g3(host, dlg);
  CHECK(s.install("org.example.hello", fx.fetcher, g3).ok);
}

// -------------------------------- giới hạn, đổi nhà phát hành, nâng cấp --
YUI_TEST(store, size_limit_takeover_and_upgrade) {
  Fixture fx;
  const auto cat = catalog_with({entry_for("org.example.hello", fx.pkg, kUrl)});
  store::Options small = fx.opts();
  small.max_package_bytes = 100;
  store::Store tiny(small, fx.keys);
  CHECK(tiny.load_catalog(store::catalog_to_json(cat), store::sign_catalog(cat, kCatalogSeed)).ok);
  AllowGate gate;
  CHECK_EQ(tiny.install("org.example.hello", fx.fetcher, gate).code, std::string("too_large"));

  store::Store s(fx.opts(), fx.keys);
  CHECK(s.load_catalog(store::catalog_to_json(cat), store::sign_catalog(cat, kCatalogSeed)).ok);
  fx.fetcher.files[kUrl] = fx.pkg;
  CHECK(s.install("org.example.hello", fx.fetcher, gate).ok);

  // Danh mục mới ghi cùng id nhưng nhà phát hành khác ⇒ không cho đè.
  auto hijack = entry_for("org.example.hello", fx.pkg, kUrl);
  hijack.publisher = "Kẻ Mạo Danh";
  hijack.version = "9.9.9";
  const auto cat2 = catalog_with({hijack});
  CHECK(s.load_catalog(store::catalog_to_json(cat2), store::sign_catalog(cat2, kCatalogSeed)).ok);
  CHECK_EQ(s.install("org.example.hello", fx.fetcher, gate).code, std::string("publisher_mismatch"));

  // Nâng cấp hợp lệ: cùng nhà phát hành, bản 2 thay nguyên khối bản 1.
  const std::string url2 = "https://downloads.example.org/hello-2.0.0.tar";
  const Bytes pkg2 = store::build_tar({file_entry("bin/hello", "#!/bin/sh\necho v2\n", 0755)});
  auto v2 = entry_for("org.example.hello", pkg2, url2);
  v2.version = "2.0.0";
  const auto cat3 = catalog_with({v2});
  CHECK(s.load_catalog(store::catalog_to_json(cat3), store::sign_catalog(cat3, kCatalogSeed)).ok);
  fx.fetcher.files[url2] = pkg2;
  store::InstallPlan plan;
  CHECK(s.plan("org.example.hello", plan).ok && plan.already_installed && plan.installed_version == "1.0.0");
  const auto up = s.install("org.example.hello", fx.fetcher, gate);
  CHECK_MSG(up.ok, up.reason_vi);
  const std::string dir = fx.root + "/apps/org.example.hello";
  CHECK(slurp(dir + "/bin/hello").find("v2") != std::string::npos);
  CHECK(!exists(dir + "/share/hello/data.txt"));  // tệp của bản cũ không còn
  CHECK(!exists(dir + ".old") && !exists(dir + ".new"));
  CHECK_EQ(s.installed().size(), size_t(1));
  CHECK_EQ(s.installed()[0].version, std::string("2.0.0"));
}

// --------------------------- không gọt app có DRM, tệp đơn, thiếu tệp chạy --
YUI_TEST(store, no_trim_single_file_and_missing_exec) {
  Fixture fx;
  auto drm = entry_for("com.example.drm", fx.pkg, kUrl);
  drm.trim = false;
  const std::string apk_url = "https://downloads.example.org/game.apk";
  const std::string apk_body = "PK\x03\x04 gia lap apk";
  const Bytes apk(apk_body.begin(), apk_body.end());
  auto single = entry_for("com.example.game", apk, apk_url);
  single.kind = "apk";
  single.format = "file";
  single.exec = "game.apk";
  const std::string bad_url = "https://downloads.example.org/noexec.tar";
  const Bytes no_exec = store::build_tar({file_entry("README", "x")});
  auto missing = entry_for("com.example.noexec", no_exec, bad_url);
  const auto cat = catalog_with({drm, single, missing});
  store::Store s(fx.opts(), fx.keys);
  CHECK(s.load_catalog(store::catalog_to_json(cat), store::sign_catalog(cat, kCatalogSeed)).ok);
  fx.fetcher.files[kUrl] = fx.pkg;
  fx.fetcher.files[apk_url] = apk;
  fx.fetcher.files[bad_url] = no_exec;
  AllowGate gate;

  CHECK(s.install("com.example.drm", fx.fetcher, gate).ok);
  CHECK(exists(fx.root + "/apps/com.example.drm/locale/fr/messages"));  // không gọt: 0 byte bị sửa

  CHECK(s.install("com.example.game", fx.fetcher, gate).ok);
  const std::string apk_path = fx.root + "/apps/com.example.game/game.apk";
  CHECK_EQ(crypto::hex(crypto::sha256(slurp(apk_path))), single.sha256);  // lưu nguyên vẹn
  struct stat st{};
  CHECK(::stat(apk_path.c_str(), &st) == 0 && (st.st_mode & 0111) == 0);

  CHECK_EQ(s.install("com.example.noexec", fx.fetcher, gate).code, std::string("missing_exec"));
  CHECK(!exists(fx.root + "/apps/com.example.noexec"));
}

// Gốc Kho tương đối (vd `yui-store --root kho`) vẫn bung đúng chỗ.
YUI_TEST(store, relative_root_is_resolved) {
  Fixture fx;
  char cwd[4096];
  CHECK(::getcwd(cwd, sizeof cwd) != nullptr);
  CHECK(::chdir(fx.root.c_str()) == 0);
  const auto cat = catalog_with({entry_for("org.example.hello", fx.pkg, kUrl)});
  store::Options o;
  o.root = "kho-tuong-doi";
  store::Store s(o, fx.keys);
  CHECK(s.load_catalog(store::catalog_to_json(cat), store::sign_catalog(cat, kCatalogSeed)).ok);
  fx.fetcher.files[kUrl] = fx.pkg;
  AllowGate gate;
  const auto r = s.install("org.example.hello", fx.fetcher, gate);
  CHECK_MSG(r.ok, r.reason_vi);
  CHECK(exists(fx.root + "/kho-tuong-doi/apps/org.example.hello/bin/hello"));
  CHECK(::chdir(cwd) == 0);
}

// ------------------------------------------ yuid RPC + shell song ngữ --
YUI_TEST(store, daemon_rpc_and_shell_commands) {
  Fixture fx;
  const std::string state = mkdtemp_dir("store-daemon");
  const std::string keydir = state + "/keys";
  CHECK(fx.keys.save_dir(keydir));
  const std::string pkg_path = state + "/hello-1.0.0.tar";
  spit(pkg_path, fx.pkg);
  auto e = entry_for("org.example.hello", fx.pkg, "file://" + pkg_path);
  const auto cat = catalog_with({e});

  service::Options o;
  o.state_dir = state;
  o.store_keyring_dir = keydir;
  o.store_allow_file_urls = true;
  service::Daemon d(o);
  service::RpcError err;

  // Chưa có danh mục.
  auto r = d.handle("store.search", json::Value(json::Object{}), err);
  CHECK_EQ(r["code"].as_string(), std::string("no_catalog"));

  store::Store seed_store(store::Options{state + "/store"}, fx.keys);
  CHECK(seed_store.save_catalog(store::catalog_to_json(cat), store::sign_catalog(cat, kCatalogSeed)));

  json::Value q(json::Object{});
  q.set("query", "xin chao");
  r = d.handle("store.search", q, err);
  CHECK(r["ok"].as_bool());
  CHECK_EQ(r["apps"].size(), size_t(1));

  json::Value p(json::Object{});
  p.set("app", "org.example.hello");
  r = d.handle("store.plan", p, err);
  CHECK(r["plan_vi"].as_string().find("MIT") != std::string::npos);

  // Không có hộp thoại ⇒ mở hộp thoại tin cậy, chưa cài.
  r = d.handle("store.install", p, err);
  CHECK_EQ(r["code"].as_string(), std::string("needs_confirmation"));
  const std::string dlg = r["dialog_id"].as_string();
  CHECK(!dlg.empty());
  CHECK(!exists(state + "/store/apps/org.example.hello"));

  // Gửi lại dialog_id khi CHƯA bấm ⇒ not_confirmed, vẫn chưa cài.
  p.set("dialog_id", dlg);
  r = d.handle("store.install", p, err);
  CHECK_EQ(r["code"].as_string(), std::string("not_confirmed"));

  sandbox::TaggedInputEvent click;
  click.source = sandbox::InputSourceKind::PhysicalHardware;
  click.hardware_device_id = 1;
  click.x = 150;
  click.y = 220;
  click.is_button_down = true;
  CHECK(d.trusted_confirmation().handle_input(dlg, click, 0));
  r = d.handle("store.install", p, err);
  CHECK_MSG(r["ok"].as_bool(), r["reason_vi"].as_string());
  CHECK(exists(state + "/store/apps/org.example.hello/bin/hello"));

  // Có dòng audit cho cả lần chờ và lần cài.
  int audit_hits = 0;
  for (const auto& a : d.enforcer().audit())
    if (a.action.kind == "store.install") ++audit_hits;
  CHECK(audit_hits >= 2);

  // Gỡ rồi phát lại đúng dialog cũ ⇒ không được dùng lại.
  json::Value rm(json::Object{});
  rm.set("app", "org.example.hello");
  CHECK(d.handle("store.remove", rm, err)["ok"].as_bool());
  r = d.handle("store.install", p, err);
  CHECK_EQ(r["code"].as_string(), std::string("needs_confirmation"));
  CHECK(!exists(state + "/store/apps/org.example.hello"));

  // https khi yuid ngoại tuyến ⇒ không tải.
  const auto cat2 = catalog_with({entry_for("org.example.net", fx.pkg, kUrl)});
  CHECK(seed_store.save_catalog(store::catalog_to_json(cat2), store::sign_catalog(cat2, kCatalogSeed)));
  json::Value pn(json::Object{});
  pn.set("app", "org.example.net");
  r = d.handle("store.install", pn, err);
  const std::string dlg2 = r["dialog_id"].as_string();
  CHECK(d.trusted_confirmation().handle_input(dlg2, click, 0));
  pn.set("dialog_id", dlg2);
  r = d.handle("store.install", pn, err);
  CHECK_EQ(r["code"].as_string(), std::string("offline"));

  // Shell: tìmapp / cài / gỡ (song ngữ, không dấu).
  CHECK(seed_store.save_catalog(store::catalog_to_json(cat), store::sign_catalog(cat, kCatalogSeed)));
  shell::Shell sh(d, ui::Layout{});
  const std::string found = sh.run("tìmapp xin chao");
  CHECK(found.find("org.example.hello") != std::string::npos);
  CHECK(sh.run("searchapp chao").find("Xin Chào") != std::string::npos);
  const std::string ask = sh.run("cài org.example.hello");
  CHECK(ask.find("Nguồn tải") != std::string::npos);
  CHECK(ask.find("xác nhận") != std::string::npos);
  CHECK(!exists(state + "/store/apps/org.example.hello"));  // shell không tự cài
  CHECK(sh.run("install org.example.hello").find("dlg-") != std::string::npos);
  CHECK(sh.run("gỡ org.example.hello").find("chưa được cài") != std::string::npos);
  CHECK(sh.help_text().find("tìmapp") != std::string::npos);
}

YUI_TEST_MAIN("test_store")
