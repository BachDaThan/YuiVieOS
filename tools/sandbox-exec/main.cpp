// yui-sandbox-exec — bệ phóng hộp cát cho lệnh POSIX của yui-shell (P4.12).
//
//   yui-sandbox-exec [--chdir D] [--rw DIR]... [--ro DIR]...
//                    [--stdin F] [--stdout F] [--stdout-append F]
//                    [--stderr F] [--stderr-append F] [--stderr-to-stdout] [--stdout-to-stderr]
//                    (chuyển hướng được áp ĐÚNG THỨ TỰ trên dòng lệnh, như sh)
//                    [--allow-unsandboxed] -- CHƯƠNG_TRÌNH [THAM_SỐ...]
//   yui-sandbox-exec --probe          # in ABI Landlock; mã 3 nếu không có
//
// Thứ tự cố ý:
//   1. chdir;
//   2. PR_SET_NO_NEW_PRIVS — chương trình setuid (sudo, su, pkexec…) không
//      leo quyền được nữa;
//   3. Landlock: chỉ ĐỌC/CHẠY trong các nhánh --ro, ĐỌC/GHI trong các nhánh
//      --rw (cộng /dev/null, /dev/tty), còn lại bị nhân từ chối;
//   4. mở tệp chuyển hướng SAU Landlock ⇒ `> /etc/x` cũng bị nhân chặn chứ
//      không chỉ bị yuid chặn;
//   5. đóng mọi fd ≥ 3 (không rò socket/tệp của yui-shell sang lệnh con);
//   6. execvp.
//
// Không có Landlock (nhân < 5.13 hoặc bị tắt) ⇒ từ chối chạy, trừ khi được gọi
// với --allow-unsandboxed (yui-shell chỉ thêm cờ này khi bạn đặt rõ
// YUI_POSIX_ALLOW_UNSANDBOXED=1).
//
// Giới hạn: Landlock chỉ CHO PHÉP theo nhánh, không trừ được thư mục con —
// nếu $HOME là --rw thì ~/.ssh cũng ghi được ở tầng nhân; yuid chặn chuyển
// hướng vào đó nhưng chương trình tự mở ~/.ssh thì không. Mạng không bị hạn
// chế (Landlock mạng cần ABI 4, nhân 6.7+).
#include <errno.h>
#include <fcntl.h>
#include <linux/landlock.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr int kSetupFailed = 125;

// glibc cũ (Ubuntu 22.04) có thể thiếu tên SYS_landlock_*; số hiệu giống nhau
// trên mọi kiến trúc dùng bảng syscall chung (x86_64, aarch64, riscv64).
#ifndef SYS_landlock_create_ruleset
#define SYS_landlock_create_ruleset 444
#endif
#ifndef SYS_landlock_add_rule
#define SYS_landlock_add_rule 445
#endif
#ifndef SYS_landlock_restrict_self
#define SYS_landlock_restrict_self 446
#endif
#ifndef LANDLOCK_CREATE_RULESET_VERSION
#define LANDLOCK_CREATE_RULESET_VERSION (1U << 0)
#endif

long ll_create(const landlock_ruleset_attr* attr, size_t size, uint32_t flags) {
  return syscall(SYS_landlock_create_ruleset, attr, size, flags);
}
long ll_add(int fd, landlock_rule_type type, const void* attr, uint32_t flags) {
  return syscall(SYS_landlock_add_rule, fd, type, attr, flags);
}
long ll_restrict(int fd, uint32_t flags) { return syscall(SYS_landlock_restrict_self, fd, flags); }

int landlock_abi() {
  const long v = ll_create(nullptr, 0, LANDLOCK_CREATE_RULESET_VERSION);
  return v < 0 ? 0 : int(v);
}

[[noreturn]] void die(const std::string& msg, int code = kSetupFailed) {
  std::fprintf(stderr, "yui-sandbox-exec: %s\n", msg.c_str());
  std::_Exit(code);
}

uint64_t handled_for(int abi) {
  uint64_t a = LANDLOCK_ACCESS_FS_EXECUTE | LANDLOCK_ACCESS_FS_WRITE_FILE | LANDLOCK_ACCESS_FS_READ_FILE |
               LANDLOCK_ACCESS_FS_READ_DIR | LANDLOCK_ACCESS_FS_REMOVE_DIR | LANDLOCK_ACCESS_FS_REMOVE_FILE |
               LANDLOCK_ACCESS_FS_MAKE_CHAR | LANDLOCK_ACCESS_FS_MAKE_DIR | LANDLOCK_ACCESS_FS_MAKE_REG |
               LANDLOCK_ACCESS_FS_MAKE_SOCK | LANDLOCK_ACCESS_FS_MAKE_FIFO | LANDLOCK_ACCESS_FS_MAKE_BLOCK |
               LANDLOCK_ACCESS_FS_MAKE_SYM;
#ifdef LANDLOCK_ACCESS_FS_REFER
  if (abi >= 2) a |= LANDLOCK_ACCESS_FS_REFER;
#endif
#ifdef LANDLOCK_ACCESS_FS_TRUNCATE
  if (abi >= 3) a |= LANDLOCK_ACCESS_FS_TRUNCATE;
#endif
  return a;
}

constexpr uint64_t kRead =
    LANDLOCK_ACCESS_FS_EXECUTE | LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR;

uint64_t file_rights(uint64_t handled) {
  uint64_t f = LANDLOCK_ACCESS_FS_EXECUTE | LANDLOCK_ACCESS_FS_WRITE_FILE | LANDLOCK_ACCESS_FS_READ_FILE;
#ifdef LANDLOCK_ACCESS_FS_TRUNCATE
  f |= LANDLOCK_ACCESS_FS_TRUNCATE;
#endif
  return f & handled;
}

void add_path(int ruleset, const std::string& path, uint64_t access, uint64_t handled) {
  const int fd = ::open(path.c_str(), O_PATH | O_CLOEXEC);
  if (fd < 0) return;  // nhánh chưa tồn tại: bỏ qua
  struct stat st;
  if (::fstat(fd, &st) == 0 && !S_ISDIR(st.st_mode)) access &= file_rights(handled);
  landlock_path_beneath_attr pb{};
  pb.allowed_access = access & handled;
  pb.parent_fd = fd;
  const long r = ll_add(ruleset, LANDLOCK_RULE_PATH_BENEATH, &pb, 0);
  const int e = errno;
  ::close(fd);
  if (r != 0) die("không thêm được quy tắc Landlock cho " + path + ": " + std::strerror(e));
}

void redirect(int target_fd, const std::string& path, int flags) {
  const int fd = ::open(path.c_str(), flags | O_CLOEXEC, 0644);
  if (fd < 0) die("không mở được " + path + ": " + std::strerror(errno), 1);
  if (::dup2(fd, target_fd) < 0) die("dup2 lỗi: " + std::string(std::strerror(errno)));
  ::close(fd);
}

void close_from(int lowfd) {
#ifdef SYS_close_range
  if (syscall(SYS_close_range, lowfd, ~0u, 0) == 0) return;
#endif
  const long maxfd = sysconf(_SC_OPEN_MAX);
  for (long fd = lowfd; fd < (maxfd > 0 ? maxfd : 1024); ++fd) ::close(int(fd));
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> rw, ro;
  std::string chdir_to;
  struct Redir {
    int fd;          // fd đích
    std::string path;  // rỗng ⇒ nhân bản dup_from
    int flags;
    int dup_from;
  };
  std::vector<Redir> redirs;
  bool allow_unsandboxed = false;
  int i = 1;
  auto need = [&](const char* opt) -> std::string {
    if (i + 1 >= argc) die(std::string("thiếu giá trị cho ") + opt, 2);
    return argv[++i];
  };
  for (; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--") {
      ++i;
      break;
    }
    if (a == "--probe") {
      const int abi = landlock_abi();
      if (abi <= 0) {
        std::printf("landlock: không có (%s)\n", std::strerror(errno));
        return 3;
      }
      std::printf("landlock: abi %d\n", abi);
      return 0;
    }
    if (a == "--rw") rw.push_back(need("--rw"));
    else if (a == "--ro") ro.push_back(need("--ro"));
    else if (a == "--chdir") chdir_to = need("--chdir");
    else if (a == "--stdin") redirs.push_back({0, need("--stdin"), O_RDONLY, -1});
    else if (a == "--stdout") redirs.push_back({1, need("--stdout"), O_WRONLY | O_CREAT | O_TRUNC, -1});
    else if (a == "--stdout-append") redirs.push_back({1, need("--stdout-append"), O_WRONLY | O_CREAT | O_APPEND, -1});
    else if (a == "--stderr") redirs.push_back({2, need("--stderr"), O_WRONLY | O_CREAT | O_TRUNC, -1});
    else if (a == "--stderr-append") redirs.push_back({2, need("--stderr-append"), O_WRONLY | O_CREAT | O_APPEND, -1});
    else if (a == "--stderr-to-stdout") redirs.push_back({2, "", 0, 1});
    else if (a == "--stdout-to-stderr") redirs.push_back({1, "", 0, 2});
    else if (a == "--allow-unsandboxed") allow_unsandboxed = true;
    else die("tuỳ chọn lạ: " + a, 2);
  }
  if (i >= argc) die("thiếu chương trình sau --", 2);

  if (!chdir_to.empty() && ::chdir(chdir_to.c_str()) != 0)
    die("không vào được thư mục " + chdir_to + ": " + std::strerror(errno), 1);
  if (::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) die("không bật được no_new_privs");

  const int abi = landlock_abi();
  if (abi > 0) {
    const uint64_t handled = handled_for(abi);
    landlock_ruleset_attr attr{};
    attr.handled_access_fs = handled;
    const int rs = int(ll_create(&attr, sizeof attr, 0));
    if (rs < 0) die(std::string("không tạo được bộ quy tắc Landlock: ") + std::strerror(errno));
    for (const auto& p : ro) add_path(rs, p, kRead, handled);
    for (const auto& p : rw) add_path(rs, p, handled, handled);
    add_path(rs, "/dev/null", handled, handled);
    add_path(rs, "/dev/tty", handled, handled);
    if (ll_restrict(rs, 0) != 0) die(std::string("không áp được Landlock: ") + std::strerror(errno));
    ::close(rs);
  } else if (!allow_unsandboxed) {
    die("nhân không có Landlock nên KHÔNG chạy lệnh ngoài hộp cát (đặt YUI_POSIX_ALLOW_UNSANDBOXED=1 nếu chấp nhận "
        "chỉ có kiểm tra chính sách của yuid)");
  }

  for (const auto& r : redirs) {
    if (r.path.empty()) {
      if (::dup2(r.dup_from, r.fd) < 0) die("dup2 lỗi: " + std::string(std::strerror(errno)));
    } else {
      redirect(r.fd, r.path, r.flags);
    }
  }
  close_from(3);

  ::execvp(argv[i], argv + i);
  const int e = errno;
  std::fprintf(stderr, "yui-sandbox-exec: không chạy được %s: %s\n", argv[i], std::strerror(e));
  return e == ENOENT ? 127 : 126;
}
