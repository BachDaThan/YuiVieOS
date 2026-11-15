// Bảng phân phối syscall Linux — xem include/yui/linuxabi.hpp.
#include "yui/linuxabi.hpp"

#include <algorithm>
#include <cstring>

namespace yui::linuxabi {
namespace {

SyscallResult do_read(SyscallCtx& c, long fd, long buf, long len, long) {
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

SyscallResult do_write(SyscallCtx& c, long fd, long buf, long len, long) {
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

SyscallResult do_open(SyscallCtx& c, long path_l, long flags, long, long) {
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

SyscallResult do_openat(SyscallCtx& c, long dirfd, long path_l, long flags, long) {
  // dirfd bị bỏ qua với đường dẫn tuyệt đối; đường dẫn tương đối quy về /state.
  const char* path_p = reinterpret_cast<const char*>(path_l);
  if (!path_p) return {L_EINVAL, false, 0};
  std::string path = path_p;
  if (path.empty()) return {L_EINVAL, false, 0};
  if (path[0] != '/') path = "/state/" + path;
  return do_open(c, long(uintptr_t(path.c_str())), flags, 0, 0);
}

SyscallResult do_close(SyscallCtx& c, long fd, long, long, long) {
  if (fd < 3 || fd >= long(c.fd_paths.size()) + 3) return {L_EBADF, false, 0};
  const size_t i = size_t(fd) - 3;
  c.fd_paths.erase(c.fd_paths.begin() + long(i));
  c.fd_pos.erase(c.fd_pos.begin() + long(i));
  c.fd_writable.erase(c.fd_writable.begin() + long(i));
  return {0, false, 0};
}

SyscallResult do_exit(SyscallCtx&, long code, long, long, long) {
  return {0, true, int(code & 0xff)};
}

SyscallResult do_perm(SyscallCtx&, long, long, long, long) {
  return {L_EPERM, false, 0};  // timestomping / fork / mạng: không cấp
}

const SyscallEntry kTable[] = {
    {SYS_read, "read", do_read},
    {SYS_write, "write", do_write},
    {SYS_open, "open", do_open},
    {SYS_close, "close", do_close},
    {SYS_socket, "socket", do_perm},
    {SYS_connect, "connect", do_perm},
    {SYS_accept, "accept", do_perm},
    {SYS_fork, "fork", do_perm},
    {SYS_execve, "execve", do_perm},
    {SYS_wait4, "wait4", do_perm},
    {SYS_exit, "exit", do_exit},
    {SYS_exit_group, "exit_group", do_exit},
    {SYS_openat, "openat", do_openat},
    {SYS_utimensat, "utimensat", do_perm},
};

}  // namespace

const SyscallEntry* syscall_table() { return kTable; }
size_t syscall_table_size() { return sizeof kTable / sizeof kTable[0]; }

SyscallResult dispatch(SyscallCtx& ctx, int nr, long a0, long a1, long a2, long a3) {
  if (ctx.exited) return {L_EPERM, true, ctx.exit_code};
  for (const auto& e : kTable)
    if (e.nr == nr) {
      SyscallResult r = e.fn(ctx, a0, a1, a2, a3);
      if (r.exited) {
        ctx.exited = true;
        ctx.exit_code = r.exit_code;
      }
      return r;
    }
  return {L_ENOSYS, false, 0};
}

int nr_from32(int nr32) {
  // Không gian i386: 1=exit 2=fork 3=read 4=write 5=open 6=close 11=execve
  // 114=wait4 252=exit_group 295=openat 326=utimensat.
  switch (nr32) {
    case 1: return SYS_exit;
    case 2: return SYS_fork;
    case 3: return SYS_read;
    case 4: return SYS_write;
    case 5: return SYS_open;
    case 6: return SYS_close;
    case 11: return SYS_execve;
    case 114: return SYS_wait4;
    case 252: return SYS_exit_group;
    case 295: return SYS_openat;
    case 326: return SYS_utimensat;
    default: return nr32;  // để dispatch trả -ENOSYS cho số lạ
  }
}

const char* abi_mapping(int nr) {
  switch (nr) {
    case SYS_read: return "read → sysfs::read (fd theo bảng fd)";
    case SYS_write: return "write → sysfs::write (fd 1/2 = console, /core từ chối)";
    case SYS_open: return "open → sysfs::open (ghi vào /core ⇒ EPERM)";
    case SYS_close: return "close → sysfs::close";
    case SYS_openat: return "openat → sysfs::open (đường dẫn tương đối quy về /state)";
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
