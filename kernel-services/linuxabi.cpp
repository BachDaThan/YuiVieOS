// Bảng phân phối syscall Linux — xem include/yui/linuxabi.hpp.
#include "yui/linuxabi.hpp"

#include <algorithm>
#include <cstring>

namespace yui::linuxabi {
namespace {

SyscallResult do_read(SyscallCtx& c, long fd, long buf, long len, long, long, long) {
  if (fd < 0 || fd >= long(c.fd_paths.size()) + 3) return {L_EBADF, false, 0};
  if (fd < 3) return {0, false, 0};  // stdin: không có gì để đọc
  const size_t i = size_t(fd) - 3;
  const auto& path = c.fd_paths[i];
  auto it = c.files.find(path);
  if (it == c.files.end()) return {L_EIO, false, 0};
  const uint64_t pos = c.fd_pos[i];
  const uint64_t avail = it->second.size() > pos ? it->second.size() - pos : 0;
  const long n = long(avail < uint64_t(len) ? avail : uint64_t(len));
  if (n > 0 && buf != 0) std::memcpy(reinterpret_cast<void*>(buf), it->second.data() + pos, size_t(n));
  c.fd_pos[i] += uint64_t(n);
  return {n, false, 0};
}

SyscallResult do_write(SyscallCtx& c, long fd, long buf, long len, long, long, long) {
  if (len < 0) return {L_EINVAL, false, 0};
  const char* data = reinterpret_cast<const char*>(buf);
  if (fd == 1 || fd == 2) {
    if (data && len) c.console.append(data, size_t(len));
    return {len, false, 0};
  }
  if (fd < 3 || fd >= long(c.fd_paths.size()) + 3) return {L_EBADF, false, 0};
  const size_t i = size_t(fd) - 3;
  if (!c.fd_writable[i]) return {L_EACCES, false, 0};
  if (data && len) {
    auto& f = c.files[c.fd_paths[i]];
    const size_t pos = size_t(c.fd_pos[i]);
    if (f.size() < pos) f.resize(pos, 0);
    if (f.size() == pos) {
      f.insert(f.end(), data, data + len);
    } else {
      const size_t over = std::min(size_t(len), f.size() - pos);
      std::memcpy(f.data() + pos, data, over);
      if (size_t(len) > over) f.insert(f.end(), data + over, data + len);
    }
    c.fd_pos[i] += uint64_t(len);
  }
  return {len, false, 0};
}

bool under_core(const std::string& p) {
  return p.rfind("/core", 0) == 0 && (p.size() == 5 || p[5] == '/');
}

SyscallResult do_open(SyscallCtx& c, long path_l, long flags, long, long, long, long) {
  const char* path_p = reinterpret_cast<const char*>(path_l);
  if (!path_p) return {L_EINVAL, false, 0};
  const std::string path = path_p;
  const bool want_write = (flags & 3) != 0;  // O_WRONLY|O_RDWR
  if (want_write && under_core(path))
    return {L_EPERM, false, 0};  // bảo vệ chuỗi cung ứng: /core chỉ-đọc
  if (want_write) {
    c.fd_paths.push_back(path);
    c.fd_pos.push_back(c.files[path].size());
    c.fd_writable.push_back(true);
    return {long(c.fd_paths.size() + 2), false, 0};
  }
  auto it = c.files.find(path);
  if (it == c.files.end()) return {L_ENOENT, false, 0};
  c.fd_paths.push_back(path);
  c.fd_pos.push_back(0);
  c.fd_writable.push_back(false);
  return {long(c.fd_paths.size() + 2), false, 0};
}

SyscallResult do_openat(SyscallCtx& c, long dirfd, long path_l, long flags, long, long, long) {
  (void)dirfd;
  // dirfd bị bỏ qua với đường dẫn tuyệt đối; đường dẫn tương đối quy về /state.
  const char* path_p = reinterpret_cast<const char*>(path_l);
  if (!path_p) return {L_EINVAL, false, 0};
  std::string path = path_p;
  if (path.empty()) return {L_EINVAL, false, 0};
  if (path[0] != '/') path = "/state/" + path;
  return do_open(c, long(uintptr_t(path.c_str())), flags, 0, 0, 0, 0);
}

SyscallResult do_close(SyscallCtx& c, long fd, long, long, long, long, long) {
  if (fd < 3 || fd >= long(c.fd_paths.size()) + 3) return {L_EBADF, false, 0};
  const size_t i = size_t(fd) - 3;
  c.fd_paths.erase(c.fd_paths.begin() + long(i));
  c.fd_pos.erase(c.fd_pos.begin() + long(i));
  c.fd_writable.erase(c.fd_writable.begin() + long(i));
  return {0, false, 0};
}

SyscallResult do_mmap(SyscallCtx& c, long addr_l, long len_l, long prot_l, long flags_l, long fd_l, long offset_l) {
  if (len_l <= 0) return {L_EINVAL, false, 0};
  const size_t length = size_t(len_l);
  const int prot = int(prot_l);
  const int flags = int(flags_l);
  const int fd = int(fd_l);
  const uint64_t offset = uint64_t(offset_l);

  // Bảo vệ W^X: cấm vừa ghi vừa thực thi
  if ((prot & L_PROT_WRITE) && (prot & L_PROT_EXEC))
    return {L_EPERM, false, 0};

  const bool is_anon = (flags & L_MAP_ANONYMOUS) != 0;
  std::string file_path;

  if (!is_anon) {
    if (fd < 3 || fd >= long(c.fd_paths.size()) + 3) return {L_EBADF, false, 0};
    const size_t i = size_t(fd) - 3;
    file_path = c.fd_paths[i];
    // Bảo vệ /core: không được ánh xạ có quyền ghi
    if ((prot & L_PROT_WRITE) && under_core(file_path))
      return {L_EPERM, false, 0};
    if (c.files.find(file_path) == c.files.end())
      return {L_ENOENT, false, 0};
  }

  uint64_t addr = 0;
  if ((flags & L_MAP_FIXED) && addr_l != 0) {
    addr = uint64_t(addr_l);
  } else {
    addr = c.next_mmap_addr;
    const size_t aligned_len = ((length + 4095) / 4096) * 4096;
    c.next_mmap_addr += aligned_len;
  }

  Mapping m;
  m.addr = addr;
  m.length = length;
  m.prot = prot;
  m.flags = flags;
  m.file_path = file_path;
  m.data.resize(length, 0);

  if (!is_anon && !file_path.empty()) {
    const auto& f = c.files[file_path];
    if (offset < f.size()) {
      const size_t copy_len = std::min(length, f.size() - size_t(offset));
      std::memcpy(m.data.data(), f.data() + offset, copy_len);
    }
  }

  c.mappings[addr] = std::move(m);
  return {long(addr), false, 0};
}

SyscallResult do_munmap(SyscallCtx& c, long addr_l, long len_l, long, long, long, long) {
  if (addr_l == 0 || len_l <= 0) return {L_EINVAL, false, 0};
  const uint64_t addr = uint64_t(addr_l);
  auto it = c.mappings.find(addr);
  if (it == c.mappings.end()) return {L_EINVAL, false, 0};
  c.mappings.erase(it);
  return {0, false, 0};
}

SyscallResult do_futex(SyscallCtx& c, long uaddr_l, long op_l, long val_l, long, long, long) {
  if (uaddr_l == 0) return {L_EINVAL, false, 0};
  const int cmd = int(op_l) & ~L_FUTEX_PRIVATE_FLAG;

  if (cmd == L_FUTEX_WAIT) {
    const int* ptr = reinterpret_cast<const int*>(uaddr_l);
    if (*ptr != int(val_l)) return {L_EAGAIN, false, 0};
    c.futex_waiters[uint64_t(uaddr_l)]++;
    return {0, false, 0};
  }
  if (cmd == L_FUTEX_WAKE) {
    const int count = std::max(0, int(val_l));
    auto it = c.futex_waiters.find(uint64_t(uaddr_l));
    if (it == c.futex_waiters.end()) return {0, false, 0};
    const int woken = std::min(count, it->second);
    it->second -= woken;
    if (it->second <= 0) c.futex_waiters.erase(it);
    return {long(woken), false, 0};
  }
  return {L_ENOSYS, false, 0};
}

SyscallResult do_epoll_create1(SyscallCtx& c, long, long, long, long, long, long) {
  const int epfd = c.next_epoll_fd++;
  c.epoll_instances[epfd] = EpollInstance{};
  return {long(epfd), false, 0};
}

SyscallResult do_epoll_ctl(SyscallCtx& c, long epfd_l, long op_l, long fd_l, long event_p, long, long) {
  const int epfd = int(epfd_l);
  const int op = int(op_l);
  const int fd = int(fd_l);

  auto it = c.epoll_instances.find(epfd);
  if (it == c.epoll_instances.end()) return {L_EBADF, false, 0};

  if (fd < 0 || (fd >= 3 && size_t(fd - 3) >= c.fd_paths.size()))
    return {L_EBADF, false, 0};

  if (op == L_EPOLL_CTL_ADD) {
    if (it->second.watches.count(fd)) return {L_EEXIST, false, 0};
    EpollWatch w;
    w.fd = fd;
    if (event_p != 0) {
      const auto* ev = reinterpret_cast<const EpollEvent*>(event_p);
      w.events = ev->events;
      w.data = ev->data;
    }
    it->second.watches[fd] = w;
    return {0, false, 0};
  }
  if (op == L_EPOLL_CTL_DEL) {
    if (!it->second.watches.count(fd)) return {L_ENOENT, false, 0};
    it->second.watches.erase(fd);
    return {0, false, 0};
  }
  if (op == L_EPOLL_CTL_MOD) {
    auto wit = it->second.watches.find(fd);
    if (wit == it->second.watches.end()) return {L_ENOENT, false, 0};
    if (event_p != 0) {
      const auto* ev = reinterpret_cast<const EpollEvent*>(event_p);
      wit->second.events = ev->events;
      wit->second.data = ev->data;
    }
    return {0, false, 0};
  }
  return {L_EINVAL, false, 0};
}

SyscallResult do_epoll_wait(SyscallCtx& c, long epfd_l, long events_p, long maxevents_l, long, long, long) {
  const int epfd = int(epfd_l);
  const int maxevents = int(maxevents_l);
  auto it = c.epoll_instances.find(epfd);
  if (it == c.epoll_instances.end()) return {L_EBADF, false, 0};
  if (maxevents <= 0 || events_p == 0) return {L_EINVAL, false, 0};

  auto* out_events = reinterpret_cast<EpollEvent*>(events_p);
  int ready_count = 0;

  for (const auto& [fd, watch] : it->second.watches) {
    if (ready_count >= maxevents) break;
    uint32_t revents = 0;
    if (fd == 1 || fd == 2) {
      if (watch.events & L_EPOLLOUT) revents |= L_EPOLLOUT;
    } else if (fd >= 3 && size_t(fd - 3) < c.fd_paths.size()) {
      const size_t i = size_t(fd) - 3;
      const auto& path = c.fd_paths[i];
      auto fit = c.files.find(path);
      if (fit != c.files.end()) {
        const uint64_t pos = c.fd_pos[i];
        if (pos < fit->second.size() && (watch.events & L_EPOLLIN))
          revents |= L_EPOLLIN;
      }
      if (c.fd_writable[i] && (watch.events & L_EPOLLOUT))
        revents |= L_EPOLLOUT;
    }

    if (revents != 0) {
      out_events[ready_count].events = revents;
      out_events[ready_count].data = watch.data;
      ++ready_count;
    }
  }

  return {long(ready_count), false, 0};
}

SyscallResult do_exit(SyscallCtx&, long code, long, long, long, long, long) {
  return {0, true, int(code & 0xff)};
}

SyscallResult do_perm(SyscallCtx&, long, long, long, long, long, long) {
  return {L_EPERM, false, 0};  // timestomping / fork / mạng: không cấp
}

const SyscallEntry kTable[] = {
    {SYS_read, "read", do_read},
    {SYS_write, "write", do_write},
    {SYS_open, "open", do_open},
    {SYS_close, "close", do_close},
    {SYS_mmap, "mmap", do_mmap},
    {SYS_munmap, "munmap", do_munmap},
    {SYS_socket, "socket", do_perm},
    {SYS_connect, "connect", do_perm},
    {SYS_accept, "accept", do_perm},
    {SYS_fork, "fork", do_perm},
    {SYS_execve, "execve", do_perm},
    {SYS_wait4, "wait4", do_perm},
    {SYS_exit, "exit", do_exit},
    {SYS_futex, "futex", do_futex},
    {SYS_epoll_create, "epoll_create", do_epoll_create1},
    {SYS_epoll_create1, "epoll_create1", do_epoll_create1},
    {SYS_epoll_ctl, "epoll_ctl", do_epoll_ctl},
    {SYS_epoll_wait, "epoll_wait", do_epoll_wait},
    {SYS_epoll_pwait, "epoll_pwait", do_epoll_wait},
    {SYS_exit_group, "exit_group", do_exit},
    {SYS_openat, "openat", do_openat},
    {SYS_utimensat, "utimensat", do_perm},
};

}  // namespace

const SyscallEntry* syscall_table() { return kTable; }
size_t syscall_table_size() { return sizeof kTable / sizeof kTable[0]; }

SyscallResult dispatch(SyscallCtx& ctx, int nr, long a0, long a1, long a2, long a3,
                       long a4, long a5) {
  if (ctx.exited) return {L_EPERM, true, ctx.exit_code};
  for (const auto& e : kTable)
    if (e.nr == nr) {
      SyscallResult r = e.fn(ctx, a0, a1, a2, a3, a4, a5);
      if (r.exited) {
        ctx.exited = true;
        ctx.exit_code = r.exit_code;
      }
      return r;
    }
  return {L_ENOSYS, false, 0};
}

int nr_from32(int nr32) {
  switch (nr32) {
    case 1: return SYS_exit;
    case 2: return SYS_fork;
    case 3: return SYS_read;
    case 4: return SYS_write;
    case 5: return SYS_open;
    case 6: return SYS_close;
    case 11: return SYS_execve;
    case 90: return SYS_mmap;
    case 91: return SYS_munmap;
    case 114: return SYS_wait4;
    case 192: return SYS_mmap;  // mmap2
    case 240: return SYS_futex;
    case 252: return SYS_exit_group;
    case 254: return SYS_epoll_create;
    case 255: return SYS_epoll_ctl;
    case 256: return SYS_epoll_wait;
    case 295: return SYS_openat;
    case 326: return SYS_utimensat;
    case 329: return SYS_epoll_create1;
    default: return nr32;
  }
}

const char* abi_mapping(int nr) {
  switch (nr) {
    case SYS_read: return "read → sysfs::read (fd theo bảng fd)";
    case SYS_write: return "write → sysfs::write (fd 1/2 = console, /core từ chối)";
    case SYS_open: return "open → sysfs::open (ghi vào /core ⇒ EPERM)";
    case SYS_close: return "close → sysfs::close";
    case SYS_openat: return "openat → sysfs::open (đường dẫn tương đối quy về /state)";
    case SYS_mmap: return "mmap → sysmem::map (chặn W^X, /core chỉ-đọc)";
    case SYS_munmap: return "munmap → sysmem::unmap";
    case SYS_futex: return "futex → syssync::futex (FUTEX_WAIT/WAKE)";
    case SYS_epoll_create: return "epoll_create → sysevent::epoll_create";
    case SYS_epoll_create1: return "epoll_create1 → sysevent::epoll_create";
    case SYS_epoll_ctl: return "epoll_ctl → sysevent::epoll_ctl (ADD/MOD/DEL)";
    case SYS_epoll_wait: return "epoll_wait → sysevent::epoll_wait";
    case SYS_epoll_pwait: return "epoll_pwait → sysevent::epoll_wait";
    case SYS_exit: return "exit → sysproc::exit";
    case SYS_exit_group: return "exit_group → sysproc::exit_group";
    case SYS_utimensat: return "utimensat → EPERM (chặn timestomping)";
    case SYS_fork: return "fork → EPERM (không cấp)";
    case SYS_execve: return "execve → EPERM (không cấp)";
    case SYS_wait4: return "wait4 → EPERM (không cấp)";
    case SYS_socket: return "socket → EPERM (mạng qua EgressFilter)";
    case SYS_connect: return "connect → EPERM (mạng qua EgressFilter)";
    case SYS_accept: return "accept → EPERM (mạng qua EgressFilter)";
    default: return "không có trong bảng → ENOSYS";
  }
}

}  // namespace yui::linuxabi
