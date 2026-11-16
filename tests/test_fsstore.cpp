// Hệ tệp bất biến + Ghi gộp (P2.6).
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
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
  {
    std::ofstream f(dir + "/module.bin", std::ios::binary);
    f << "\x7f""ELF";
  }
  const fsstore::ImmutablePartition core(dir);
  std::vector<uint8_t> out;
  CHECK(core.read("module.bin", &out));
  CHECK_EQ(out.size(), size_t(4));
  CHECK(!core.read("../etc/passwd", &out));  // không thoát khỏi gốc
  std::remove((dir + "/module.bin").c_str());
  rmdir(dir.c_str());
}

YUI_TEST_MAIN("test_fsstore")
