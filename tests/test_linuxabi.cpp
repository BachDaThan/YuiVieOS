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
  bool has_mmap = false, has_futex = false, has_epoll = false;
  for (size_t i = 0; i < linuxabi::syscall_table_size(); ++i) {
    if (linuxabi::syscall_table()[i].nr == linuxabi::SYS_utimensat) has_utimensat = true;
    if (linuxabi::syscall_table()[i].nr == linuxabi::SYS_openat) has_openat = true;
    if (linuxabi::syscall_table()[i].nr == linuxabi::SYS_mmap) has_mmap = true;
    if (linuxabi::syscall_table()[i].nr == linuxabi::SYS_futex) has_futex = true;
    if (linuxabi::syscall_table()[i].nr == linuxabi::SYS_epoll_wait) has_epoll = true;
  }
  CHECK(has_utimensat);
  CHECK(has_openat);
  CHECK(has_mmap);
  CHECK(has_futex);
  CHECK(has_epoll);
  CHECK(std::string(linuxabi::abi_mapping(linuxabi::SYS_utimensat)).find("EPERM") !=
        std::string::npos);
  CHECK(std::string(linuxabi::abi_mapping(linuxabi::SYS_mmap)).find("W^X") !=
        std::string::npos);
  CHECK(std::string(linuxabi::abi_mapping(linuxabi::SYS_futex)).find("futex") !=
        std::string::npos);
  CHECK(std::string(linuxabi::abi_mapping(linuxabi::SYS_epoll_ctl)).find("epoll") !=
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

YUI_TEST(linuxabi, mmap_and_munmap_anonymous_and_file) {
  linuxabi::SyscallCtx c;
  // 1. Anonymous mmap (MAP_PRIVATE | MAP_ANONYMOUS, PROT_READ | PROT_WRITE)
  auto r = linuxabi::dispatch(c, linuxabi::SYS_mmap, 0, 4096,
                              linuxabi::L_PROT_READ | linuxabi::L_PROT_WRITE,
                              linuxabi::L_MAP_PRIVATE | linuxabi::L_MAP_ANONYMOUS, -1, 0);
  CHECK(r.ret > 0);
  const uint64_t anon_addr = uint64_t(r.ret);
  CHECK(c.mappings.count(anon_addr));
  CHECK_EQ(c.mappings[anon_addr].length, size_t(4096));

  // 2. File-backed mmap (read from /state/config.txt)
  c.files["/state/config.txt"] = {'Y', 'u', 'i', 'O', 'S'};
  auto open_r = linuxabi::dispatch(c, linuxabi::SYS_open, long(uintptr_t("/state/config.txt")), 0);
  CHECK(open_r.ret >= 3);
  const int fd = int(open_r.ret);

  r = linuxabi::dispatch(c, linuxabi::SYS_mmap, 0, 5,
                         linuxabi::L_PROT_READ,
                         linuxabi::L_MAP_PRIVATE, fd, 0);
  CHECK(r.ret > 0);
  const uint64_t file_addr = uint64_t(r.ret);
  CHECK(c.mappings.count(file_addr));
  CHECK_EQ(std::string(reinterpret_cast<char*>(c.mappings[file_addr].data.data()), 5),
           std::string("YuiOS"));

  // 3. munmap
  auto unmap_r = linuxabi::dispatch(c, linuxabi::SYS_munmap, long(anon_addr), 4096);
  CHECK_EQ(unmap_r.ret, 0L);
  CHECK_EQ(c.mappings.count(anon_addr), size_t(0));

  // 4. munmap non-existent addr returns EINVAL
  unmap_r = linuxabi::dispatch(c, linuxabi::SYS_munmap, 0x12345000, 4096);
  CHECK_EQ(unmap_r.ret, linuxabi::L_EINVAL);
  // munmap len 0 returns EINVAL
  unmap_r = linuxabi::dispatch(c, linuxabi::SYS_munmap, long(file_addr), 0);
  CHECK_EQ(unmap_r.ret, linuxabi::L_EINVAL);
}

YUI_TEST(linuxabi, mmap_security_wx_and_core_protection) {
  linuxabi::SyscallCtx c;
  // 1. W^X protection: mapping both PROT_WRITE and PROT_EXEC must be denied with EPERM
  auto r = linuxabi::dispatch(c, linuxabi::SYS_mmap, 0, 4096,
                              linuxabi::L_PROT_WRITE | linuxabi::L_PROT_EXEC,
                              linuxabi::L_MAP_PRIVATE | linuxabi::L_MAP_ANONYMOUS, -1, 0);
  CHECK_EQ(r.ret, linuxabi::L_EPERM);

  // 2. /core protection: file-backed mmap on /core with PROT_WRITE must be denied with EPERM
  c.files["/core/kernel.elf"] = {'E', 'L', 'F'};
  auto open_r = linuxabi::dispatch(c, linuxabi::SYS_open, long(uintptr_t("/core/kernel.elf")), 0);
  CHECK(open_r.ret >= 3);
  const int fd = int(open_r.ret);

  r = linuxabi::dispatch(c, linuxabi::SYS_mmap, 0, 3,
                         linuxabi::L_PROT_READ | linuxabi::L_PROT_WRITE,
                         linuxabi::L_MAP_SHARED, fd, 0);
  CHECK_EQ(r.ret, linuxabi::L_EPERM);

  // Read-only mmap on /core is allowed
  r = linuxabi::dispatch(c, linuxabi::SYS_mmap, 0, 3,
                         linuxabi::L_PROT_READ,
                         linuxabi::L_MAP_PRIVATE, fd, 0);
  CHECK(r.ret > 0);
}

YUI_TEST(linuxabi, futex_wait_and_wake) {
  linuxabi::SyscallCtx c;
  int futex_word = 42;
  const long uaddr = long(uintptr_t(&futex_word));

  // 1. FUTEX_WAIT with value mismatch -> returns EAGAIN immediately
  auto r = linuxabi::dispatch(c, linuxabi::SYS_futex, uaddr, linuxabi::L_FUTEX_WAIT, 99);
  CHECK_EQ(r.ret, linuxabi::L_EAGAIN);

  // 2. FUTEX_WAIT with matching value -> queues waiter and returns 0
  r = linuxabi::dispatch(c, linuxabi::SYS_futex, uaddr, linuxabi::L_FUTEX_WAIT, 42);
  CHECK_EQ(r.ret, 0L);
  CHECK_EQ(c.futex_waiters[uint64_t(uaddr)], 1);

  // Add a second waiter
  r = linuxabi::dispatch(c, linuxabi::SYS_futex, uaddr, linuxabi::L_FUTEX_WAIT, 42);
  CHECK_EQ(r.ret, 0L);
  CHECK_EQ(c.futex_waiters[uint64_t(uaddr)], 2);

  // 3. FUTEX_WAKE wakes 1
  r = linuxabi::dispatch(c, linuxabi::SYS_futex, uaddr, linuxabi::L_FUTEX_WAKE, 1);
  CHECK_EQ(r.ret, 1L);
  CHECK_EQ(c.futex_waiters[uint64_t(uaddr)], 1);

  // FUTEX_WAKE remaining (request 10, only 1 available)
  r = linuxabi::dispatch(c, linuxabi::SYS_futex, uaddr, linuxabi::L_FUTEX_WAKE, 10);
  CHECK_EQ(r.ret, 1L);
  CHECK_EQ(c.futex_waiters.count(uint64_t(uaddr)), size_t(0));

  // FUTEX_WAKE on empty returns 0
  r = linuxabi::dispatch(c, linuxabi::SYS_futex, uaddr, linuxabi::L_FUTEX_WAKE, 1);
  CHECK_EQ(r.ret, 0L);
}

YUI_TEST(linuxabi, epoll_lifecycle_create_ctl_wait) {
  linuxabi::SyscallCtx c;
  // 1. epoll_create1
  auto r = linuxabi::dispatch(c, linuxabi::SYS_epoll_create1, 0);
  CHECK(r.ret >= 1000);
  const int epfd = int(r.ret);
  CHECK(c.epoll_instances.count(epfd));

  // Open a file for testing readiness
  c.files["/state/stream.log"] = {'a', 'b', 'c'};
  auto open_r = linuxabi::dispatch(c, linuxabi::SYS_open, long(uintptr_t("/state/stream.log")), 0);
  CHECK(open_r.ret >= 3);
  const int file_fd = int(open_r.ret);

  // 2. epoll_ctl ADD fd 1 (console, write) and file_fd (read)
  linuxabi::EpollEvent ev_out;
  ev_out.events = linuxabi::L_EPOLLOUT;
  ev_out.data = 101;
  r = linuxabi::dispatch(c, linuxabi::SYS_epoll_ctl, epfd, linuxabi::L_EPOLL_CTL_ADD, 1,
                         long(uintptr_t(&ev_out)));
  CHECK_EQ(r.ret, 0L);

  linuxabi::EpollEvent ev_in;
  ev_in.events = linuxabi::L_EPOLLIN;
  ev_in.data = 102;
  r = linuxabi::dispatch(c, linuxabi::SYS_epoll_ctl, epfd, linuxabi::L_EPOLL_CTL_ADD, file_fd,
                         long(uintptr_t(&ev_in)));
  CHECK_EQ(r.ret, 0L);

  // Adding already added fd -> EEXIST
  r = linuxabi::dispatch(c, linuxabi::SYS_epoll_ctl, epfd, linuxabi::L_EPOLL_CTL_ADD, 1,
                         long(uintptr_t(&ev_out)));
  CHECK_EQ(r.ret, linuxabi::L_EEXIST);

  // 3. epoll_wait
  linuxabi::EpollEvent events[4];
  r = linuxabi::dispatch(c, linuxabi::SYS_epoll_wait, epfd, long(uintptr_t(events)), 4, 100);
  CHECK_EQ(r.ret, 2L);  // both fd 1 (writable) and file_fd (readable) are ready
  bool saw_out = false, saw_in = false;
  for (int i = 0; i < int(r.ret); ++i) {
    if (events[i].data == 101 && (events[i].events & linuxabi::L_EPOLLOUT)) saw_out = true;
    if (events[i].data == 102 && (events[i].events & linuxabi::L_EPOLLIN)) saw_in = true;
  }
  CHECK(saw_out);
  CHECK(saw_in);

  // 4. epoll_ctl DEL
  r = linuxabi::dispatch(c, linuxabi::SYS_epoll_ctl, epfd, linuxabi::L_EPOLL_CTL_DEL, 1);
  CHECK_EQ(r.ret, 0L);
  // Deleting again -> ENOENT
  r = linuxabi::dispatch(c, linuxabi::SYS_epoll_ctl, epfd, linuxabi::L_EPOLL_CTL_DEL, 1);
  CHECK_EQ(r.ret, linuxabi::L_ENOENT);

  // epoll_ctl on invalid epfd -> EBADF
  r = linuxabi::dispatch(c, linuxabi::SYS_epoll_ctl, 9999, linuxabi::L_EPOLL_CTL_DEL, 1);
  CHECK_EQ(r.ret, linuxabi::L_EBADF);
}

YUI_TEST(linuxabi, nr_from32_maps_mmap_futex_epoll) {
  CHECK_EQ(linuxabi::nr_from32(90), linuxabi::SYS_mmap);
  CHECK_EQ(linuxabi::nr_from32(91), linuxabi::SYS_munmap);
  CHECK_EQ(linuxabi::nr_from32(192), linuxabi::SYS_mmap);
  CHECK_EQ(linuxabi::nr_from32(240), linuxabi::SYS_futex);
  CHECK_EQ(linuxabi::nr_from32(254), linuxabi::SYS_epoll_create);
  CHECK_EQ(linuxabi::nr_from32(255), linuxabi::SYS_epoll_ctl);
  CHECK_EQ(linuxabi::nr_from32(256), linuxabi::SYS_epoll_wait);
  CHECK_EQ(linuxabi::nr_from32(329), linuxabi::SYS_epoll_create1);
}

YUI_TEST_MAIN("test_linuxabi")
