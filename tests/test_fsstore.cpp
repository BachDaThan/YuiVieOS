// Hệ tệp bất biến + Ghi gộp (P2.6) + CoW Snapshot (P2.14).
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "testutil.hpp"
#include "yui/fsstore.hpp"

using namespace yui;

namespace {

std::string make_tmpdir() {
  char tmpl[] = "/tmp/yui-fsstore-XXXXXX";
  char* d = mkdtemp(tmpl);
  return d ? std::string(d) : std::string();
}

}  // namespace

YUI_TEST(fsstore, safe_relpath_accepts_plain_relative_paths) {
  std::string err;
  CHECK(fsstore::safe_relpath("a.txt", &err));
  CHECK(fsstore::safe_relpath("core/module.ko", &err));
  CHECK(fsstore::safe_relpath("slots/A/bin/app", &err));
}

YUI_TEST(fsstore, safe_relpath_rejects_traversal_and_absolute) {
  std::string err;
  CHECK(!fsstore::safe_relpath("/etc/passwd", &err));
  CHECK(err.find("tuyệt đối") != std::string::npos);
  CHECK(!fsstore::safe_relpath("a/../b", &err));
  CHECK(err.find("..") != std::string::npos);
  CHECK(!fsstore::safe_relpath("x//y", &err));
  CHECK(!fsstore::safe_relpath("", &err));
}

YUI_TEST(fsstore, coalescer_merges_writes_to_the_same_page) {
  fsstore::WriteCoalescer co(16);
  fsstore::WriteOp op;
  op.path = "state/config.json";
  for (int i = 0; i < 3; ++i) {
    op.data = {uint8_t('A' + i)};
    CHECK(co.submit(op));
  }
  CHECK_EQ(co.submitted_total(), size_t(3));
  CHECK_EQ(co.pending(), size_t(1));  // 3 lần ghi → 1 trang chờ chốt
  size_t commits = 0;
  const auto r = co.flush([&](const std::string&, const fsstore::WriteOp& o) {
    ++commits;
    return o.data == std::vector<uint8_t>{uint8_t('C')};  // bản cuối thắng
  });
  CHECK(r.ok);
  CHECK_EQ(r.logical_writes, size_t(3));
  CHECK_EQ(r.physical_writes, size_t(1));  // chỉ chốt 1 lần vật lý
  CHECK_EQ(commits, size_t(1));
  CHECK(co.coalesce_ratio() < 0.5);
}

YUI_TEST(fsstore, coalescer_flush_failure_keeps_pages_for_retry) {
  fsstore::WriteCoalescer co(16);
  fsstore::WriteOp a;
  a.path = "a";
  a.data = {1};
  fsstore::WriteOp b;
  b.path = "b";
  b.data = {2};
  CHECK(co.submit(a));
  CHECK(co.submit(b));
  size_t calls = 0;
  const auto r = co.flush([&](const std::string& path, const fsstore::WriteOp&) {
    ++calls;
    return path == "b";  // "a" chốt lỗi
  });
  CHECK(!r.ok);
  CHECK(r.reason_vi.find("không chốt được") != std::string::npos);
  CHECK_EQ(co.pending(), size_t(1));  // giữ trang lỗi để thử lại
}

YUI_TEST(fsstore, coalescer_rejects_unsafe_and_overflow) {
  fsstore::WriteCoalescer co(2);
  fsstore::WriteOp op;
  std::string err;
  op.path = "../escape";
  CHECK(!co.submit(op, &err));
  CHECK(err.find("từ chối") != std::string::npos);
  op.path = "p1";
  CHECK(co.submit(op));
  op.path = "p2";
  CHECK(co.submit(op));
  op.path = "p3";
  CHECK(!co.submit(op, &err));  // đầy cửa sổ gộp
  CHECK(err.find("flush") != std::string::npos);
}

YUI_TEST(fsstore, immutable_partition_always_refuses_writes) {
  const fsstore::ImmutablePartition core("/core");
  CHECK(!core.write("kernel.elf", {1, 2, 3}));
  const std::string why = core.write_denied_reason();
  CHECK(why.find("chỉ-đọc") != std::string::npos);
  CHECK(why.find("/state") != std::string::npos);
}

YUI_TEST(fsstore, immutable_partition_reads_from_its_root) {
  const std::string dir = make_tmpdir();
  CHECK(!dir.empty());
  const std::string nested_dir = dir + "/modules";
  CHECK_EQ(::mkdir(nested_dir.c_str(), 0755), 0);
  {
    std::ofstream f(dir + "/module.bin", std::ios::binary);
    f << "\x7f""ELF";
  }
  {
    std::ofstream f(nested_dir + "/nested.bin", std::ios::binary);
    f << "nested";
  }
  const fsstore::ImmutablePartition core(dir);
  std::vector<uint8_t> out;
  CHECK(core.read("module.bin", &out));
  CHECK_EQ(out.size(), size_t(4));
  CHECK(core.read("modules/nested.bin", &out));
  CHECK_EQ(out.size(), size_t(6));
  CHECK(!core.read("../etc/passwd", &out));  // không thoát khỏi gốc
  std::remove((nested_dir + "/nested.bin").c_str());
  std::remove((dir + "/module.bin").c_str());
  rmdir(nested_dir.c_str());
  rmdir(dir.c_str());
}

YUI_TEST(fsstore, immutable_partition_does_not_follow_symlinks) {
  const std::string root = make_tmpdir();
  const std::string outside = make_tmpdir();
  CHECK(!root.empty());
  CHECK(!outside.empty());
  if (root.empty() || outside.empty()) return;

  const std::string secret = outside + "/secret.bin";
  const std::string file_link = root + "/file-link";
  const std::string dir_link = root + "/dir-link";
  {
    std::ofstream f(secret, std::ios::binary);
    f << "outside-root";
  }
  CHECK_EQ(::symlink(secret.c_str(), file_link.c_str()), 0);
  CHECK_EQ(::symlink(outside.c_str(), dir_link.c_str()), 0);

  const fsstore::ImmutablePartition core(root);
  std::vector<uint8_t> out{0xAA};
  CHECK(!core.read("file-link", &out));
  CHECK(out.size() == 1 && out[0] == 0xAA);  // failed read does not expose partial data
  CHECK(!core.read("dir-link/secret.bin", &out));
  CHECK(!core.read("module.bin", nullptr));

  std::remove(file_link.c_str());
  std::remove(dir_link.c_str());
  std::remove(secret.c_str());
  rmdir(root.c_str());
  rmdir(outside.c_str());
}

YUI_TEST(fsstore, cow_store_basic_operations) {
  fsstore::CowStore store;
  std::string err;

  // 1. Ghi tệp an toàn
  CHECK(store.write_file("user/config.json", {'{', '}'}, &err));
  CHECK(store.has_file("user/config.json"));
  CHECK_EQ(store.file_count(), size_t(1));

  // 2. Từ chối đường dẫn nguy hiểm
  CHECK(!store.write_file("/etc/passwd", {'x'}, &err));
  CHECK(!store.write_file("user/../secret", {'x'}, &err));

  // 3. Đọc tệp
  std::vector<uint8_t> out;
  CHECK(store.read_file("user/config.json", &out));
  CHECK_EQ(out.size(), size_t(2));
  CHECK_EQ(out[0], uint8_t('{'));

  // 4. Danh sách tệp
  CHECK(store.write_file("user/notes.txt", {'h', 'i'}));
  auto files = store.list_files();
  CHECK_EQ(files.size(), size_t(2));
  CHECK_EQ(files[0], std::string("user/config.json"));
  CHECK_EQ(files[1], std::string("user/notes.txt"));

  // 5. Xoá tệp
  CHECK(store.delete_file("user/notes.txt", &err));
  CHECK(!store.has_file("user/notes.txt"));
  CHECK_EQ(store.file_count(), size_t(1));
  CHECK(!store.delete_file("user/notes.txt", &err));
}

YUI_TEST(fsstore, cow_store_snapshot_and_rollback) {
  fsstore::CowStore store;
  store.write_file("docs/report.txt", {'O', 'K'});
  store.write_file("docs/todo.txt", {'T', 'o', 'd', 'o'});

  // Tạo snapshot ban đầu kèm đo thời gian thực tế
  double elapsed_ms = -1.0;
  const std::string snap1 = store.create_snapshot("clean-state", 1000, &elapsed_ms);
  CHECK(store.has_snapshot(snap1));
  CHECK(elapsed_ms >= 0.0);
  CHECK_EQ(store.snapshot_count(), size_t(1));

  fsstore::SnapshotMeta meta;
  CHECK(store.get_snapshot_meta(snap1, &meta));
  CHECK_EQ(meta.file_count, size_t(2));
  CHECK_EQ(meta.bytes, size_t(6));

  // Sửa đổi tệp và thêm tệp mới (mô phỏng bị thay đổi/ghi đè)
  store.write_file("docs/report.txt", {'B', 'A', 'D', '!'});
  store.write_file("docs/malware.exe", {'X'});
  store.delete_file("docs/todo.txt");

  CHECK_EQ(store.file_count(), size_t(2));
  CHECK(!store.has_file("docs/todo.txt"));
  std::vector<uint8_t> rep;
  CHECK(store.read_file("docs/report.txt", &rep));
  CHECK_EQ(rep.size(), size_t(4));

  // Thực hiện rollback về snapshot snap1
  std::string err;
  CHECK(store.rollback_to(snap1, &err));

  // Xác minh toàn bộ trạng thái ban đầu được phục hồi 100%
  CHECK_EQ(store.file_count(), size_t(2));
  CHECK(store.has_file("docs/todo.txt"));
  CHECK(!store.has_file("docs/malware.exe"));  // tệp rác mới thêm bị biến mất

  rep.clear();
  CHECK(store.read_file("docs/report.txt", &rep));
  CHECK_EQ(rep.size(), size_t(2));
  CHECK_EQ(rep[0], uint8_t('O'));
  CHECK_EQ(rep[1], uint8_t('K'));

  // Rollback snapshot không tồn tại -> lỗi
  CHECK(!store.rollback_to("snap-9999", &err));
  CHECK(err.find("không tìm thấy") != std::string::npos);

  // Xoá snapshot
  CHECK(store.drop_snapshot(snap1));
  CHECK_EQ(store.snapshot_count(), size_t(0));
}

YUI_TEST(fsstore, cow_store_storage_accounting_and_deduplication) {
  fsstore::CowStore store;
  // Ghi tệp 100 byte
  std::vector<uint8_t> data1(100, 0xAA);
  store.write_file("large.bin", data1);

  // Snapshot 1
  const std::string snap1 = store.create_snapshot("snap1");

  // Snapshot 2 (chưa sửa gì, CoW chia sẻ hoàn toàn 100 byte)
  const std::string snap2 = store.create_snapshot("snap2");

  auto acc = store.compute_accounting();
  // 1 file x 3 views (active + snap1 + snap2) = 300 byte logical
  CHECK_EQ(acc.logical_bytes, size_t(300));
  // Vật lý chỉ lưu đúng 100 byte (chia sẻ qua 3 con trỏ)
  CHECK_EQ(acc.physical_bytes, size_t(100));
  CHECK_EQ(acc.shared_bytes, size_t(100));
  CHECK_EQ(acc.exclusive_bytes, size_t(0));
  CHECK(acc.savings_ratio > 0.60);  // tiết kiệm ~66.7%

  // Ghi đè tệp mới (exclusive)
  std::vector<uint8_t> data2(50, 0xBB);
  store.write_file("large.bin", data2);

  acc = store.compute_accounting();
  // logical: 50 (active) + 100 (snap1) + 100 (snap2) = 250 byte
  CHECK_EQ(acc.logical_bytes, size_t(250));
  // physical: 100 (shared giữa snap1 và snap2) + 50 (exclusive cho active) = 150 byte
  CHECK_EQ(acc.physical_bytes, size_t(150));
  CHECK_EQ(acc.shared_bytes, size_t(100));
  CHECK_EQ(acc.exclusive_bytes, size_t(50));
  CHECK(acc.savings_ratio > 0.35);
}

YUI_TEST_MAIN("test_fsstore")
