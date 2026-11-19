// Thùng rác + dọn rác an toàn (P4.17).
//
// DoD: xoá → khôi phục đúng băm; dọn rác không chạm dữ liệu người dùng; thùng
// đầy ⇒ cảnh báo, không xoá lặng lẽ; xoá vĩnh viễn/đổ thùng/dọn rác phải có
// xác nhận tin cậy (P4.13).
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
#include <vector>

#include "testutil.hpp"
#include "yui/sandbox.hpp"
#include "yui/service.hpp"
#include "yui/shell.hpp"
#include "yui/trash.hpp"

using namespace yui;

namespace {

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

bool is_link(const std::string& p) {
  struct stat st{};
  return ::lstat(p.c_str(), &st) == 0 && S_ISLNK(st.st_mode);
}

void spit(const std::string& path, const std::string& s) {
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return;
  std::fwrite(s.data(), 1, s.size(), f);
  std::fclose(f);
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

void make_old(const std::string& p, int64_t seconds_ago) {
  struct timespec ts[2];
  ts[0].tv_sec = ts[1].tv_sec = std::time(nullptr) - seconds_ago;
  ts[0].tv_nsec = ts[1].tv_nsec = 0;
  ::utimensat(AT_FDCWD, p.c_str(), ts, AT_SYMLINK_NOFOLLOW);
}

std::string tree_hash(const std::string& p) {
  std::string h;
  uint64_t b = 0;
  return trash::hash_tree(p, h, b, nullptr) ? h : std::string("ERR");
}

// Cây mẫu: tên tiếng Việt có dấu + khoảng trắng, thư mục lồng, liên kết, quyền chạy.
void build_tree(const std::string& d) {
  ::mkdir(d.c_str(), 0755);
  ::mkdir((d + "/ảnh chụp").c_str(), 0755);
  spit(d + "/ghi chú.txt", "Xin chào thế giới\n");
  spit(d + "/ảnh chụp/màn hình 1.ppm", std::string(5000, 'x'));
  spit(d + "/chạy.sh", "#!/bin/sh\necho hi\n");
  ::chmod((d + "/chạy.sh").c_str(), 0755);
  ::symlink("ghi chú.txt", (d + "/liên kết").c_str());
}

trash::Options opts_in(const std::string& base) {
  trash::Options o;
  o.root = base + "/thung";
  return o;
}

struct Gate : trash::ConfirmGate {
  bool allow = false;
  std::string only_target;  // rỗng = mọi đích
  std::string seen_target, seen_summary;
  int calls = 0;
  bool confirmed(const std::string& target, const std::string& summary) override {
    ++calls;
    seen_target = target;
    seen_summary = summary;
    return allow && (only_target.empty() || only_target == target);
  }
};

sandbox::TaggedInputEvent physical_click() {
  sandbox::TaggedInputEvent click;
  click.source = sandbox::InputSourceKind::PhysicalHardware;
  click.hardware_device_id = 1;
  click.x = 150;
  click.y = 220;
  click.is_button_down = true;
  return click;
}

}  // namespace

YUI_TEST(trash, path_encoding_round_trip) {
  const std::string p = "/home/an/Tài liệu/báo cáo 100%#1.txt";
  const std::string e = trash::encode_path(p);
  CHECK(e.find(' ') == std::string::npos);
  CHECK(e.find("%25") != std::string::npos);  // '%' được mã hoá
  CHECK(e.find("/home/an/") == 0);            // '/' giữ nguyên
  CHECK_EQ(trash::decode_path(e), p);
  CHECK_EQ(trash::decode_path("a%2"), std::string("a%2"));  // cụt thì giữ nguyên
}

YUI_TEST(trash, put_then_restore_same_hash_file_and_tree) {
  const std::string base = mkdtemp_dir("trash-basic");
  trash::Trash t(opts_in(base));

  // Một tệp.
  spit(base + "/thư.txt", "nội dung quan trọng");
  const std::string h_file = tree_hash(base + "/thư.txt");
  auto r = t.put(base + "/thư.txt");
  CHECK_MSG(r.ok, r.reason_vi);
  CHECK(!exists(base + "/thư.txt"));
  CHECK_EQ(r.item.sha256, h_file);
  CHECK(exists(base + "/thung/files/thư.txt"));
  const std::string info = slurp(base + "/thung/info/thư.txt.trashinfo");
  CHECK(info.find("[Trash Info]\nPath=") == 0);
  CHECK(info.find("DeletionDate=") != std::string::npos);
  CHECK(info.find("X-Yui-Sha256=" + h_file) != std::string::npos);
  CHECK_EQ(t.list().size(), size_t(1));
  CHECK_EQ(std::string(t.list()[0].original_path), base + "/thư.txt");  // bản sao, không giữ tham chiếu vào vector tạm

  auto back = t.restore(r.item.id);
  CHECK_MSG(back.ok, back.reason_vi);
  CHECK_EQ(tree_hash(base + "/thư.txt"), h_file);
  CHECK_EQ(slurp(base + "/thư.txt"), std::string("nội dung quan trọng"));
  CHECK(t.list().empty());
  CHECK(!exists(base + "/thung/info/thư.txt.trashinfo"));

  // Cả cây thư mục (liên kết bên trong giữ là liên kết, quyền chạy giữ nguyên).
  build_tree(base + "/dự án");
  const std::string h_tree = tree_hash(base + "/dự án");
  r = t.put(base + "/dự án");
  CHECK_MSG(r.ok, r.reason_vi);
  CHECK(r.item.is_dir);
  CHECK(!exists(base + "/dự án"));
  back = t.restore(r.item.id);
  CHECK_MSG(back.ok, back.reason_vi);
  CHECK_EQ(tree_hash(base + "/dự án"), h_tree);
  CHECK(is_link(base + "/dự án/liên kết"));
  CHECK(::access((base + "/dự án/chạy.sh").c_str(), X_OK) == 0);
  CHECK(back.reason_vi.find("băm khớp") != std::string::npos);

  // Đường dẫn tương đối cũng được.
  CHECK(::chdir(base.c_str()) == 0);
  spit(base + "/tương đối.txt", "x");
  r = t.put("tương đối.txt");
  CHECK_MSG(r.ok, r.reason_vi);
  CHECK_EQ(r.item.original_path, base + "/tương đối.txt");
  CHECK(::chdir("/") == 0);
}

YUI_TEST(trash, cross_device_copy_path_verifies_hash) {
  const std::string base = mkdtemp_dir("trash-xdev");
  auto o = opts_in(base);
  o.force_copy = true;  // giả như thùng rác nằm ở ổ khác (EXDEV)
  trash::Trash t(o);
  build_tree(base + "/khác ổ");
  const std::string h = tree_hash(base + "/khác ổ");
  auto r = t.put(base + "/khác ổ");
  CHECK_MSG(r.ok, r.reason_vi);
  CHECK(!exists(base + "/khác ổ"));
  CHECK_EQ(tree_hash(base + "/thung/files/khác ổ"), h);
  auto back = t.restore(r.item.id);
  CHECK_MSG(back.ok, back.reason_vi);
  CHECK_EQ(tree_hash(base + "/khác ổ"), h);
  CHECK(!exists(base + "/thung/files/khác ổ"));
}

YUI_TEST(trash, restore_never_overwrites_and_can_go_elsewhere) {
  const std::string base = mkdtemp_dir("trash-exists");
  trash::Trash t(opts_in(base));
  spit(base + "/a.txt", "bản cũ");
  auto r = t.put(base + "/a.txt");
  CHECK(r.ok);
  spit(base + "/a.txt", "bản mới của bạn");
  auto back = t.restore(r.item.id);
  CHECK(!back.ok);
  CHECK_EQ(back.code, std::string("exists"));
  CHECK_EQ(slurp(base + "/a.txt"), std::string("bản mới của bạn"));  // không bị ghi đè
  CHECK_EQ(t.list().size(), size_t(1));                               // vẫn trong thùng

  back = t.restore(r.item.id, base + "/khôi phục/a (cũ).txt");
  CHECK_MSG(back.ok, back.reason_vi);
  CHECK_EQ(slurp(base + "/khôi phục/a (cũ).txt"), std::string("bản cũ"));
  CHECK(t.list().empty());
  CHECK_EQ(t.restore("không-có").code, std::string("not_found"));
  CHECK_EQ(t.restore("../thoát").code, std::string("not_found"));
}

YUI_TEST(trash, tampered_trash_content_is_not_restored) {
  const std::string base = mkdtemp_dir("trash-tamper");
  trash::Trash t(opts_in(base));
  spit(base + "/hợp đồng.txt", "giá: 100");
  auto r = t.put(base + "/hợp đồng.txt");
  CHECK(r.ok);
  spit(base + "/thung/files/hợp đồng.txt", "giá: 999");  // ai đó sửa trong thùng
  auto back = t.restore(r.item.id);
  CHECK(!back.ok);
  CHECK_EQ(back.code, std::string("hash_mismatch"));
  CHECK(!exists(base + "/hợp đồng.txt"));
  CHECK_EQ(t.list().size(), size_t(1));
}

YUI_TEST(trash, full_trash_warns_and_never_deletes_silently) {
  const std::string base = mkdtemp_dir("trash-full");
  auto o = opts_in(base);
  o.capacity_bytes = 1000;
  trash::Trash t(o);
  spit(base + "/một.bin", std::string(600, 'a'));
  spit(base + "/hai.bin", std::string(600, 'b'));
  spit(base + "/to.bin", std::string(5000, 'c'));

  auto r1 = t.put(base + "/một.bin");
  CHECK(r1.ok);
  CHECK(r1.warning_vi.empty());  // 60 % < 90 %

  auto r2 = t.put(base + "/hai.bin");
  CHECK(!r2.ok);
  CHECK_EQ(r2.code, std::string("trash_full"));
  CHECK(!r2.warning_vi.empty());
  CHECK(exists(base + "/hai.bin"));             // tệp gốc giữ nguyên
  CHECK_EQ(t.list().size(), size_t(1));         // món cũ KHÔNG bị đẩy ra
  CHECK(exists(base + "/thung/files/một.bin"));

  auto r3 = t.put(base + "/to.bin");
  CHECK_EQ(r3.code, std::string("trash_full"));
  CHECK(r3.warning_vi.find("lớn hơn cả thùng") != std::string::npos);
  CHECK(exists(base + "/to.bin"));

  // Gần đầy ⇒ có cảnh báo nhưng vẫn nhận.
  spit(base + "/nhỏ.bin", std::string(350, 'd'));
  auto r4 = t.put(base + "/nhỏ.bin");
  CHECK(r4.ok);
  CHECK(r4.warning_vi.find("sắp đầy") != std::string::npos);
  CHECK_EQ(t.used_bytes(), uint64_t(950));
}

YUI_TEST(trash, protected_paths_are_refused) {
  const std::string base = mkdtemp_dir("trash-protect");
  auto o = opts_in(base);
  ::mkdir((base + "/nhà").c_str(), 0755);
  o.protected_exact.push_back(base + "/nhà");
  trash::Trash t(o);
  spit(base + "/nhà/tệp.txt", "x");
  CHECK(t.put(base + "/x").code == "not_found");
  CHECK_EQ(t.put("/").code, std::string("protected"));
  CHECK_EQ(t.put("/etc/passwd").code, std::string("protected"));
  CHECK_EQ(t.put("/usr").code, std::string("protected"));
  CHECK_EQ(t.put(base + "/nhà").code, std::string("protected"));   // chính thư mục nhà
  CHECK_MSG(t.put(base + "/nhà/tệp.txt").ok, "bên trong thư mục nhà thì được");
  CHECK(t.put(base + "/thung").code == "protected");               // chính thùng rác
  CHECK(t.put(base + "/thung/files").code == "protected");         // bên trong thùng
  CHECK(t.put(base).code == "protected");                          // thư mục chứa thùng
  CHECK(t.put(base + "/.").code != "ok");
  CHECK(t.put(base + "/..").code != "ok");
  CHECK(exists("/etc/passwd"));
  CHECK(exists(base + "/nhà"));
}

YUI_TEST(trash, symlink_is_moved_as_link_not_target) {
  const std::string base = mkdtemp_dir("trash-link");
  trash::Trash t(opts_in(base));
  ::mkdir((base + "/dữ liệu").c_str(), 0755);
  spit(base + "/dữ liệu/quý.txt", "đừng xoá tôi");
  ::symlink((base + "/dữ liệu").c_str(), (base + "/lối tắt").c_str());
  auto r = t.put(base + "/lối tắt");
  CHECK_MSG(r.ok, r.reason_vi);
  CHECK(!exists(base + "/lối tắt"));
  CHECK_EQ(slurp(base + "/dữ liệu/quý.txt"), std::string("đừng xoá tôi"));  // đích không bị đụng
  CHECK(is_link(base + "/thung/files/lối tắt"));
  CHECK(t.restore(r.item.id).ok);
  CHECK(is_link(base + "/lối tắt"));
}

YUI_TEST(trash, same_name_from_two_places_gets_unique_ids) {
  const std::string base = mkdtemp_dir("trash-dup");
  trash::Trash t(opts_in(base));
  ::mkdir((base + "/A").c_str(), 0755);
  ::mkdir((base + "/B").c_str(), 0755);
  spit(base + "/A/báo cáo.txt", "từ A");
  spit(base + "/B/báo cáo.txt", "từ B");
  auto ra = t.put(base + "/A/báo cáo.txt");
  auto rb = t.put(base + "/B/báo cáo.txt");
  CHECK(ra.ok && rb.ok);
  CHECK(ra.item.id != rb.item.id);
  CHECK_EQ(rb.item.id, std::string("báo cáo.txt.2"));
  CHECK(t.restore(rb.item.id).ok);
  CHECK(t.restore(ra.item.id).ok);
  CHECK_EQ(slurp(base + "/A/báo cáo.txt"), std::string("từ A"));
  CHECK_EQ(slurp(base + "/B/báo cáo.txt"), std::string("từ B"));
}

YUI_TEST(trash, special_files_are_refused_and_left_alone) {
  const std::string base = mkdtemp_dir("trash-fifo");
  trash::Trash t(opts_in(base));
  CHECK(::mkfifo((base + "/ống").c_str(), 0600) == 0);
  auto r = t.put(base + "/ống");
  CHECK_EQ(r.code, std::string("unsupported"));
  CHECK(exists(base + "/ống"));
  CHECK(t.list().empty());
}

YUI_TEST(trash, purge_and_empty_need_confirmation_bound_to_target) {
  const std::string base = mkdtemp_dir("trash-purge");
  trash::Trash t(opts_in(base));
  spit(base + "/a", "1");
  spit(base + "/b", "22");
  const auto ra = t.put(base + "/a");
  const auto rb = t.put(base + "/b");

  Gate deny;
  auto r = t.purge(ra.item.id, deny);
  CHECK_EQ(r.code, std::string("not_confirmed"));
  CHECK_EQ(deny.seen_target, trash::Trash::purge_target("a"));
  CHECK(deny.seen_summary.find("VĨNH VIỄN") != std::string::npos);
  CHECK_EQ(t.list().size(), size_t(2));

  // Xác nhận cho món khác ⇒ không có hiệu lực với món này.
  Gate wrong;
  wrong.allow = true;
  wrong.only_target = trash::Trash::purge_target("b");
  CHECK_EQ(t.purge("a", wrong).code, std::string("not_confirmed"));

  Gate ok;
  ok.allow = true;
  ok.only_target = trash::Trash::purge_target("a");
  r = t.purge("a", ok);
  CHECK_MSG(r.ok, r.reason_vi);
  CHECK_EQ(r.freed_bytes, uint64_t(1));
  CHECK_EQ(t.list().size(), size_t(1));

  // Đổ thùng: đích gắn với danh sách hiện có; thêm món sau khi hỏi ⇒ lần bấm cũ hết hiệu lực.
  const std::string asked = t.empty_target();
  spit(base + "/c", "333");
  CHECK(t.put(base + "/c").ok);
  CHECK(t.empty_target() != asked);
  Gate stale;
  stale.allow = true;
  stale.only_target = asked;
  CHECK_EQ(t.empty(stale).code, std::string("not_confirmed"));
  CHECK_EQ(t.list().size(), size_t(2));

  Gate yes;
  yes.allow = true;
  yes.only_target = t.empty_target();
  r = t.empty(yes);
  CHECK_MSG(r.ok, r.reason_vi);
  CHECK_EQ(r.removed, size_t(2));
  CHECK(t.list().empty());
  CHECK_EQ(t.used_bytes(), uint64_t(0));
  (void)rb;
}

YUI_TEST(trash, reads_items_trashed_by_other_programs) {
  // Món do trình quản lý tệp khác bỏ vào (đặc tả freedesktop, không có X-Yui-*).
  const std::string base = mkdtemp_dir("trash-foreign");
  trash::Trash t(opts_in(base));
  ::mkdir((base + "/thung").c_str(), 0700);
  ::mkdir((base + "/thung/files").c_str(), 0700);
  ::mkdir((base + "/thung/info").c_str(), 0700);
  spit(base + "/thung/files/Tài liệu.odt", "odt");
  spit(base + "/thung/info/Tài liệu.odt.trashinfo",
       "[Trash Info]\nPath=" + trash::encode_path(base + "/Tài liệu.odt") + "\nDeletionDate=2026-01-02T03:04:05\n");
  const auto items = t.list();
  CHECK_EQ(items.size(), size_t(1));
  CHECK_EQ(items[0].original_path, base + "/Tài liệu.odt");
  CHECK(items[0].sha256.empty());
  CHECK_EQ(items[0].size, uint64_t(3));
  const auto r = t.restore("Tài liệu.odt");
  CHECK_MSG(r.ok, r.reason_vi);
  CHECK(r.reason_vi.find("không có băm") != std::string::npos);
  CHECK_EQ(slurp(base + "/Tài liệu.odt"), std::string("odt"));
}

YUI_TEST(trash, cleanup_touches_only_named_targets) {
  const std::string base = mkdtemp_dir("trash-clean");
  const std::string store = base + "/store", state = base + "/state", home = base + "/nhà";
  for (const auto& d : {store, store + "/cache", store + "/staging", state, state + "/tmp", home})
    ::mkdir(d.c_str(), 0755);
  // Dữ liệu người dùng — tuyệt đối không được đụng.
  spit(home + "/luận văn.part", "bản nháp luận văn");  // cùng đuôi .part nhưng ngoài danh sách
  spit(home + "/ảnh.jpg", std::string(2000, 'j'));
  // Rác hợp lệ (cũ).
  spit(store + "/cache/app@1.part", std::string(3000, 'p'));
  make_old(store + "/cache/app@1.part", 7200);
  ::mkdir((store + "/staging/app.123.abc").c_str(), 0755);
  spit(store + "/staging/app.123.abc/bin", std::string(1000, 'b'));
  make_old(store + "/staging/app.123.abc", 7200);
  spit(state + "/tmp/yuid-1.tmp", std::string(10, 't'));
  make_old(state + "/tmp/yuid-1.tmp", 86400);
  // Không được xoá: mới tải dở (có thể đang chạy), sai đuôi, liên kết trỏ ra nhà.
  spit(store + "/cache/app@2.part", "đang tải");
  spit(store + "/cache/ghi-chú.txt", "không phải .part");
  make_old(store + "/cache/ghi-chú.txt", 7200);
  ::symlink((home + "/luận văn.part").c_str(), (store + "/cache/bẫy.part").c_str());
  ::symlink(home.c_str(), (store + "/staging/bẫy-thư-mục").c_str());
  make_old(store + "/cache/bẫy.part", 7200);

  const auto targets = trash::default_targets(store, state);
  const auto plan = trash::plan_cleanup(targets, std::time(nullptr));
  CHECK_EQ(plan.entries.size(), size_t(3));
  CHECK_EQ(plan.total_bytes, uint64_t(3000 + 1000 + 10));
  for (const auto& e : plan.entries) CHECK(e.path.find(home) != 0);
  CHECK(plan.summary_vi().find("giải phóng 3.9 KiB") != std::string::npos);
  bool saw_link_skip = false, saw_new_skip = false;
  for (const auto& s : plan.skipped_vi) {
    if (s.find("bẫy.part") != std::string::npos && s.find("liên kết") != std::string::npos) saw_link_skip = true;
    if (s.find("app@2.part") != std::string::npos && s.find("gần đây") != std::string::npos) saw_new_skip = true;
  }
  CHECK(saw_link_skip);
  CHECK(saw_new_skip);

  // Không xác nhận ⇒ không xoá gì.
  Gate deny;
  CHECK_EQ(trash::apply_cleanup(plan, targets, deny).code, std::string("not_confirmed"));
  CHECK_EQ(deny.seen_target, plan.target());
  CHECK(exists(store + "/cache/app@1.part"));

  Gate yes;
  yes.allow = true;
  yes.only_target = plan.target();
  const auto r = trash::apply_cleanup(plan, targets, yes);
  CHECK_MSG(r.ok, r.reason_vi);
  CHECK_EQ(r.removed, size_t(3));
  CHECK_EQ(r.freed_bytes, uint64_t(4010));
  CHECK(!exists(store + "/cache/app@1.part"));
  CHECK(!exists(store + "/staging/app.123.abc"));
  CHECK(!exists(state + "/tmp/yuid-1.tmp"));
  // Dữ liệu người dùng + mục không thuộc kế hoạch còn nguyên.
  CHECK_EQ(slurp(home + "/luận văn.part"), std::string("bản nháp luận văn"));
  CHECK(exists(home + "/ảnh.jpg"));
  CHECK(exists(store + "/cache/app@2.part"));
  CHECK(exists(store + "/cache/ghi-chú.txt"));
  CHECK(is_link(store + "/cache/bẫy.part"));
  CHECK(is_link(store + "/staging/bẫy-thư-mục"));
}

YUI_TEST(trash, cleanup_rechecks_every_entry_before_deleting) {
  const std::string base = mkdtemp_dir("trash-stale");
  const std::string store = base + "/store", state = base + "/state", home = base + "/nhà";
  for (const auto& d : {store, store + "/cache", state, home}) ::mkdir(d.c_str(), 0755);
  spit(store + "/cache/a.part", std::string(100, 'a'));
  spit(store + "/cache/b.part", std::string(100, 'b'));
  make_old(store + "/cache/a.part", 7200);
  make_old(store + "/cache/b.part", 7200);
  spit(home + "/quý.part", "của bạn");
  const auto targets = trash::default_targets(store, state);
  auto plan = trash::plan_cleanup(targets, std::time(nullptr));
  CHECK_EQ(plan.entries.size(), size_t(2));

  // Sau khi lập kế hoạch: b.part bị thay bằng tệp khác.
  ::unlink((store + "/cache/b.part").c_str());
  spit(store + "/cache/b.part", std::string(500, 'B'));
  make_old(store + "/cache/b.part", 7200);
  // Và ai đó chèn một mục giả trỏ vào thư mục nhà.
  trash::CleanEntry forged;
  forged.path = home + "/quý.part";
  forged.label_vi = "giả";
  plan.entries.push_back(forged);

  Gate yes;
  yes.allow = true;  // xác nhận cho mọi đích — luật trong apply_cleanup vẫn phải chặn
  const auto r = trash::apply_cleanup(plan, targets, yes);
  CHECK_EQ(r.removed, size_t(1));
  CHECK_EQ(r.code, std::string("stale_plan"));
  CHECK(!exists(store + "/cache/a.part"));
  CHECK(exists(store + "/cache/b.part"));
  CHECK_EQ(slurp(home + "/quý.part"), std::string("của bạn"));
  CHECK(r.warning_vi.find("không thuộc danh sách có tên") != std::string::npos);
  CHECK(r.warning_vi.find("đã thay đổi") != std::string::npos);

  // Thư mục đích bị thay bằng liên kết ⇒ bỏ qua cả đích.
  const std::string state2 = base + "/state2";
  ::mkdir(state2.c_str(), 0755);
  ::symlink(home.c_str(), (state2 + "/tmp").c_str());
  make_old(home + "/quý.part", 86400);
  const auto plan2 = trash::plan_cleanup(trash::default_targets(base + "/không-có", state2), std::time(nullptr));
  CHECK(plan2.entries.empty());
  CHECK(!plan2.skipped_vi.empty());
}

YUI_TEST(trash, daemon_rpc_and_shell_commands) {
  const std::string base = mkdtemp_dir("trash-daemon");
  const std::string home = base + "/nhà";
  ::mkdir(home.c_str(), 0755);
  ::setenv("HOME", home.c_str(), 1);
  service::Options o;
  o.state_dir = base + "/state";
  ::mkdir(o.state_dir.c_str(), 0755);
  service::Daemon d(o);
  service::RpcError err;

  spit(home + "/việc cần làm.txt", "mua rau");
  json::Value p(json::Object{});
  p.set("path", home + "/việc cần làm.txt");
  auto r = d.handle("trash.put", p, err);
  CHECK_MSG(r["ok"].as_bool(), r["reason_vi"].as_string());
  const std::string id = r["id"].as_string();
  CHECK(!exists(home + "/việc cần làm.txt"));

  r = d.handle("trash.list", json::Value(json::Object{}), err);
  CHECK_EQ(r["items"].size(), size_t(1));
  CHECK(r["items"].arr()[0]["verified"].as_bool());

  json::Value rp(json::Object{});
  rp.set("id", id);
  r = d.handle("trash.restore", rp, err);
  CHECK_MSG(r["ok"].as_bool(), r["reason_vi"].as_string());
  CHECK_EQ(slurp(home + "/việc cần làm.txt"), std::string("mua rau"));

  // Thư mục nhà và thư mục trạng thái của yuid được bảo vệ.
  json::Value ph(json::Object{});
  ph.set("path", home);
  CHECK_EQ(d.handle("trash.put", ph, err)["code"].as_string(), std::string("protected"));
  ph.set("path", o.state_dir);
  CHECK_EQ(d.handle("trash.put", ph, err)["code"].as_string(), std::string("protected"));
  CHECK(exists(home));

  // Đổ thùng: lần đầu chỉ mở hộp thoại tin cậy.
  CHECK(d.handle("trash.put", p, err)["ok"].as_bool());
  r = d.handle("trash.empty", json::Value(json::Object{}), err);
  CHECK_EQ(r["code"].as_string(), std::string("needs_confirmation"));
  const std::string dlg = r["dialog_id"].as_string();
  CHECK(r["plan_vi"].as_string().find("VĨNH VIỄN") != std::string::npos);
  json::Value pe(json::Object{});
  pe.set("dialog_id", dlg);
  r = d.handle("trash.empty", pe, err);  // chưa bấm
  CHECK_EQ(r["code"].as_string(), std::string("not_confirmed"));
  CHECK_EQ(d.handle("trash.list", json::Value(json::Object{}), err)["items"].size(), size_t(1));

  // Đầu vào giả lập không bấm được hộp thoại.
  auto fake = physical_click();
  fake.source = sandbox::InputSourceKind::SyntheticApp;
  CHECK(!d.trusted_confirmation().handle_input(dlg, fake, 0));
  CHECK(d.trusted_confirmation().handle_input(dlg, physical_click(), 1000));
  r = d.handle("trash.empty", pe, err);
  CHECK_MSG(r["ok"].as_bool(), r["reason_vi"].as_string());
  CHECK_EQ(d.handle("trash.list", json::Value(json::Object{}), err)["items"].size(), size_t(0));

  // Hộp thoại đã dùng thì không dùng lại được.
  spit(home + "/khác.txt", "x");
  json::Value p2(json::Object{});
  p2.set("path", home + "/khác.txt");
  CHECK(d.handle("trash.put", p2, err)["ok"].as_bool());
  CHECK_EQ(d.handle("trash.empty", pe, err)["code"].as_string(), std::string("needs_confirmation"));

  // Hộp thoại của "đổ thùng" không dùng được để xoá vĩnh viễn một món (khác đích).
  json::Value pp(json::Object{});
  pp.set("id", "khác.txt");
  r = d.handle("trash.purge", pp, err);
  const std::string dlg_purge = r["dialog_id"].as_string();
  CHECK_EQ(r["code"].as_string(), std::string("needs_confirmation"));
  r = d.handle("trash.empty", json::Value(json::Object{}), err);
  const std::string dlg_empty = r["dialog_id"].as_string();
  CHECK(d.trusted_confirmation().handle_input(dlg_empty, physical_click(), 2000));
  pp.set("dialog_id", dlg_empty);
  CHECK_EQ(d.handle("trash.purge", pp, err)["code"].as_string(), std::string("needs_confirmation"));
  CHECK(d.trusted_confirmation().handle_input(dlg_purge, physical_click(), 3000));
  pp.set("dialog_id", dlg_purge);
  r = d.handle("trash.purge", pp, err);
  CHECK_MSG(r["ok"].as_bool(), r["reason_vi"].as_string());

  // Dọn rác qua RPC: kế hoạch trước, xoá sau khi bấm.
  const std::string cache = o.state_dir + "/store/cache";
  ::mkdir((o.state_dir + "/store").c_str(), 0755);
  ::mkdir(cache.c_str(), 0755);
  spit(cache + "/x@1.part", std::string(2048, 'x'));
  make_old(cache + "/x@1.part", 7200);
  r = d.handle("trash.clean_plan", json::Value(json::Object{}), err);
  CHECK_EQ(r["entries"].size(), size_t(1));
  CHECK_EQ(r["total_bytes"].as_int(), 2048);
  r = d.handle("trash.clean", json::Value(json::Object{}), err);
  CHECK_EQ(r["code"].as_string(), std::string("needs_confirmation"));
  CHECK(exists(cache + "/x@1.part"));
  const std::string dlg_clean = r["dialog_id"].as_string();
  CHECK(d.trusted_confirmation().handle_input(dlg_clean, physical_click(), 4000));
  json::Value pc(json::Object{});
  pc.set("dialog_id", dlg_clean);
  r = d.handle("trash.clean", pc, err);
  CHECK_MSG(r["ok"].as_bool(), r["reason_vi"].as_string());
  CHECK(!exists(cache + "/x@1.part"));

  int audit_put = 0, audit_clean = 0;
  for (const auto& a : d.enforcer().audit()) {
    if (a.action.kind == "trash.put") ++audit_put;
    if (a.action.kind == "trash.clean") ++audit_clean;
  }
  CHECK(audit_put >= 3);
  CHECK(audit_clean >= 2);

  // Shell song ngữ.
  shell::Shell sh(d, ui::Layout{});
  spit(home + "/nháp.txt", "nháp");
  CHECK(sh.run("xoá " + home + "/nháp.txt").find("thùng rác") != std::string::npos);
  CHECK(!exists(home + "/nháp.txt"));
  CHECK(sh.run("thùngrác").find("nháp.txt") != std::string::npos);
  CHECK(sh.run("trashlist").find("1 món") != std::string::npos);
  CHECK(sh.run("khôiphục nháp.txt").find("Đã khôi phục") != std::string::npos);
  CHECK(exists(home + "/nháp.txt"));
  CHECK(sh.run("delete " + home + "/nháp.txt").find("khôiphục") != std::string::npos);
  const std::string ask = sh.run("đổrác");
  CHECK(ask.find("VĨNH VIỄN") != std::string::npos);
  CHECK(ask.find("dlg-") != std::string::npos);
  CHECK_EQ(d.handle("trash.list", json::Value(json::Object{}), err)["items"].size(), size_t(1));  // shell không tự xoá
  CHECK(sh.run("dọnrác").find("Không có gì để dọn") != std::string::npos);
  // Mã có khoảng trắng + khôi phục sang chỗ mới (thấy khi thử tay bằng yui-shell).
  spit(home + "/báo cáo quý.txt", "q3");
  CHECK(sh.run("xoá \"" + home + "/báo cáo quý.txt\"").find("báo cáo quý.txt") != std::string::npos);
  CHECK(sh.run("khôiphục báo cáo quý.txt " + home + "/bản cũ/q3.txt").find("Đã khôi phục") != std::string::npos);
  CHECK_EQ(slurp(home + "/bản cũ/q3.txt"), std::string("q3"));
  CHECK(sh.run("xoávĩnhviễn nháp.txt").find("VĨNH VIỄN") != std::string::npos);
  CHECK(sh.run("xoá " + home).find("bảo vệ") != std::string::npos);
  CHECK(sh.help_text().find("dọnrác") != std::string::npos);
}

YUI_TEST_MAIN("test_trash")
