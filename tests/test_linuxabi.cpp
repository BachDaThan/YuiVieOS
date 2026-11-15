// Bảng phân phối syscall Linux (P2.7) — docs/ARCHITECTURE.md §4.6.
#include <cstring>
#include <string>

#include "testutil.hpp"
#include "yui/linuxabi.hpp"

using namespace yui;

namespace {

long call_write(linuxabi::SyscallCtx& c, int fd, const std::string& s) {
  return linuxabi::dispatch(c, linuxabi::SYS_write, fd, long(uintptr_t(s.data())),
                            long(s.size()))
      .ret;
}

}  // namespace

YUI_TEST(linuxabi, table_covers_the_promised_calls) {
  CHECK(linuxabi::syscall_table_size() >= size_t(13));
  bool has_utimensat = false, has_openat = false;
  for (size_t i = 0; i < linuxabi::syscall_table_size(); ++i) {
    if (linuxabi::syscall_table()[i].nr == linuxabi::SYS_utimensat) has_utimensat = true;
    if (linuxabi::syscall_table()[i].nr == linuxabi::SYS_openat) has_openat = true;
  }
  CHECK(has_utimensat);
  CHECK(has_openat);
  CHECK(std::string(linuxabi::abi_mapping(linuxabi::SYS_utimensat)).find("EPERM") !=
        std::string::npos);
  CHECK(std::string(linuxabi::abi_mapping(9999)).find("ENOSYS") != std::string::npos);
}

YUI_TEST(linuxabi, nr_from32_maps_i386_space) {
  CHECK_EQ(linuxabi::nr_from32(1), linuxabi::SYS_exit);
  CHECK_EQ(linuxabi::nr_from32(3), linuxabi::SYS_read);
  CHECK_EQ(linuxabi::nr_from32(4), linuxabi::SYS_write);
  CHECK_EQ(linuxabi::nr_from32(5), linuxabi::SYS_open);
  CHECK_EQ(linuxabi::nr_from32(252), linuxabi::SYS_exit_group);
  CHECK_EQ(linuxabi::nr_from32(295), linuxabi::SYS_openat);
}

YUI_TEST(linuxabi, write_goes_to_console_on_fd1_fd2) {
  linuxabi::SyscallCtx c;
  CHECK_EQ(call_write(c, 1, "chao "), 5L);
  CHECK_EQ(call_write(c, 2, "the gioi"), 8L);
  CHECK_EQ(c.console, std::string("chao the gioi"));
  CHECK_EQ(linuxabi::dispatch(c, linuxabi::SYS_write, 7, 0, 0).ret, linuxabi::L_EBADF);
}

YUI_TEST(linuxabi, open_write_on_core_partition_is_denied) {
  linuxabi::SyscallCtx c;
  // O_WRONLY = 1: ghi vào /core bị chặn — "bảo vệ chuỗi cung ứng"
  const std::string path = "/core/kernel.elf";
  const auto r = linuxabi::dispatch(c, linuxabi::SYS_open, long(uintptr_t(path.c_str())), 1);
  CHECK_EQ(r.ret, linuxabi::L_EPERM);
}

YUI_TEST(linuxabi, open_read_write_close_roundtrip_on_state) {
  linuxabi::SyscallCtx c;
  c.files["/state/config.txt"] = {'h', 'i'};
  const std::string ro = "/state/config.txt";
  auto r = linuxabi::dispatch(c, linuxabi::SYS_open, long(uintptr_t(ro.c_str())), 0);
  CHECK(r.ret >= 3);
  const int fd = int(r.ret);
  char buf[8] = {0};
  r = linuxabi::dispatch(c, linuxabi::SYS_read, fd, long(uintptr_t(buf)), 8);
  CHECK_EQ(r.ret, 2L);
  CHECK_EQ(std::string(buf, 2), std::string("hi"));
  CHECK_EQ(linuxabi::dispatch(c, linuxabi::SYS_close, fd).ret, 0L);
  CHECK_EQ(linuxabi::dispatch(c, linuxabi::SYS_close, fd).ret, linuxabi::L_EBADF);

  const std::string rw = "/state/new.txt";
  r = linuxabi::dispatch(c, linuxabi::SYS_open, long(uintptr_t(rw.c_str())), 1);
  CHECK(r.ret >= 3);
  CHECK_EQ(call_write(c, int(r.ret), "moi"), 3L);
  CHECK_EQ(c.files["/state/new.txt"].size(), size_t(3));
}

YUI_TEST(linuxabi, openat_resolves_relative_paths_to_state) {
  linuxabi::SyscallCtx c;
  c.files["/state/notes.md"] = {'x'};
  const std::string rel = "notes.md";
  const auto r = linuxabi::dispatch(c, linuxabi::SYS_openat, 0, long(uintptr_t(rel.c_str())), 0);
  CHECK(r.ret >= 3);
  CHECK_EQ(c.fd_paths[size_t(r.ret) - 3], std::string("/state/notes.md"));
}

YUI_TEST(linuxabi, timestomp_and_fork_are_refused) {
  linuxabi::SyscallCtx c;
  CHECK_EQ(linuxabi::dispatch(c, linuxabi::SYS_utimensat).ret, linuxabi::L_EPERM);
  CHECK_EQ(linuxabi::dispatch(c, linuxabi::SYS_fork).ret, linuxabi::L_EPERM);
  CHECK_EQ(linuxabi::dispatch(c, linuxabi::SYS_execve).ret, linuxabi::L_EPERM);
  CHECK_EQ(linuxabi::dispatch(c, linuxabi::SYS_wait4).ret, linuxabi::L_EPERM);
  CHECK_EQ(linuxabi::dispatch(c, linuxabi::SYS_connect).ret, linuxabi::L_EPERM);
  CHECK_EQ(linuxabi::dispatch(c, linuxabi::SYS_socket).ret, linuxabi::L_EPERM);
}

YUI_TEST(linuxabi, exit_marks_context_and_blocks_further_calls) {
  linuxabi::SyscallCtx c;
  const auto r = linuxabi::dispatch(c, linuxabi::SYS_exit_group, 7);
  CHECK(r.exited);
  CHECK_EQ(r.exit_code, 7);
  CHECK(c.exited);
  const auto after = linuxabi::dispatch(c, linuxabi::SYS_write, 1, 0, 0);
  CHECK_EQ(after.ret, linuxabi::L_EPERM);
  CHECK(after.exited);
}

YUI_TEST(linuxabi, unknown_syscall_returns_enosys) {
  linuxabi::SyscallCtx c;
  CHECK_EQ(linuxabi::dispatch(c, 9999).ret, linuxabi::L_ENOSYS);
}

YUI_TEST_MAIN("test_linuxabi")
