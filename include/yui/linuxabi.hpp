// Bảng phân phối syscall Linux — docs/ARCHITECTURE.md §4.6.
//
// YUI không sao chép kernel Linux: mỗi lời gọi được **dịch có chủ đích** sang
// syscall thuần của YUI (sysfs/sysproc). Số hiệu syscall theo ABI gốc
// (x86_64/ARM64) để binary Linux chạy được, nhưng ngữ nghĩa được thu hẹp:
//   * 0 read · 1 write · 2 open · 3 close · 60 exit · 231 exit_group
//     · 257 openat — tương đương YUI sysfs/sysproc tương ứng;
//   * 280 utimensat (chạm mốc thời gian tệp) → EPERM: ẩn dấu vết thời gian
//     là kỹ thuật lẩn tránh (timestomping) — tuyệt đối chặn;
//   * 57 fork · 58 vfork · 59 execve · 61 wait4 · 62 kill → EPERM (không cấp,
//     trừ khi chính sách riêng cho phép sau);
//   * 41/42/43 (socket/connect/accept) → EPERM: mạng đi qua EgressFilter,
//     không bao giờ qua đường syscall thẳng.
//
// Lỗi trả về theo Linux: -EPERM=1, -ENOENT=2, -EBADF=9, -EINVAL=22,
// -EACCES=13, -ENOSYS=38. Số 32-bit (i386) được quy đổi qua `nr_from32`.
#ifndef YUI_LINUXABI_HPP
#define YUI_LINUXABI_HPP

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace yui::linuxabi {

// Số hiệu syscall Linux x86_64/ARM64 mà YUI thực hiện.
enum : int {
  SYS_read = 0,
  SYS_write = 1,
  SYS_open = 2,
  SYS_close = 3,
  SYS_mmap = 9,
  SYS_munmap = 11,
  SYS_socket = 41,
  SYS_connect = 42,
  SYS_accept = 43,
  SYS_fork = 57,
  SYS_execve = 59,
  SYS_exit = 60,
  SYS_wait4 = 61,
  SYS_futex = 202,
  SYS_epoll_create = 213,
  SYS_exit_group = 231,
  SYS_epoll_wait = 232,
  SYS_epoll_ctl = 233,
  SYS_openat = 257,
  SYS_utimensat = 280,
  SYS_epoll_pwait = 281,
  SYS_epoll_create1 = 291,
};

// Cờ bảo vệ bộ nhớ mmap (PROT_*).
enum : int {
  L_PROT_NONE = 0x0,
  L_PROT_READ = 0x1,
  L_PROT_WRITE = 0x2,
  L_PROT_EXEC = 0x4,
};

// Cờ chế độ mmap (MAP_*).
enum : int {
  L_MAP_SHARED = 0x01,
  L_MAP_PRIVATE = 0x02,
  L_MAP_FIXED = 0x10,
  L_MAP_ANONYMOUS = 0x20,
};

// Lệnh futex (FUTEX_*).
enum : int {
  L_FUTEX_WAIT = 0,
  L_FUTEX_WAKE = 1,
  L_FUTEX_PRIVATE_FLAG = 128,
};

// Thao tác epoll_ctl.
enum : int {
  L_EPOLL_CTL_ADD = 1,
  L_EPOLL_CTL_DEL = 2,
  L_EPOLL_CTL_MOD = 3,
};

// Cờ sự kiện epoll.
enum : uint32_t {
  L_EPOLLIN = 0x001,
  L_EPOLLOUT = 0x004,
  L_EPOLLERR = 0x008,
  L_EPOLLHUP = 0x010,
};

// Cấu trúc sự kiện epoll_event theo Linux ABI.
struct EpollEvent {
  uint32_t events = 0;
  uint64_t data = 0;
};

// Mã lỗi Linux (âm).
enum : long {
  L_EPERM = -1,
  L_ENOENT = -2,
  L_EINTR = -4,
  L_EIO = -5,
  L_EBADF = -9,
  L_EAGAIN = -11,
  L_ENOMEM = -12,
  L_EACCES = -13,
  L_EEXIST = -17,
  L_EINVAL = -22,
  L_ENOSYS = -38,
};

/// Không gian số 32-bit (i386): quy đổi sang x86_64 để dùng chung bảng.
/// 0=read 1=write 2=open 3=close 4=stat 5=fstat 6=lstat 7=poll 8=lseek
/// 11=munmap 90=mmap 197=fstat64 ... 1=exit(32-bit) 2=fork 11=execve ...
int nr_from32(int nr32);

/// Kết quả một syscall.
struct SyscallResult {
  long ret = 0;
  bool exited = false;
  int exit_code = 0;
};

/// Vùng nhớ ánh xạ qua mmap.
struct Mapping {
  uint64_t addr = 0;
  size_t length = 0;
  int prot = 0;
  int flags = 0;
  std::string file_path;  // rỗng nếu MAP_ANONYMOUS
  std::vector<uint8_t> data;
};

/// Bộ theo dõi một fd trong epoll.
struct EpollWatch {
  int fd = 0;
  uint32_t events = 0;
  uint64_t data = 0;
};

/// Phiên epoll.
struct EpollInstance {
  std::map<int, EpollWatch> watches;  // fd -> watch
};

/// Bối cảnh thực thi — bảng fd + FS ảo + mmap + futex + epoll để kiểm chứng không cần đĩa/nhân thật.
struct SyscallCtx {
  uint64_t caps = 0;             // quyền yui::sandbox::Right (kiểm tra khi cần)
  std::map<std::string, std::vector<uint8_t>> files;  // FS ảo (đường dẫn → nộidung)
  std::vector<std::string> fd_paths;  // fd 3.. → đường dẫn đã mở
  std::vector<uint64_t> fd_pos;
  std::vector<bool> fd_writable;
  std::string console;           // stdout/stderr tích luỹ
  bool exited = false;
  int exit_code = 0;

  // Quản lý ánh xạ bộ nhớ ảo (mmap / munmap)
  uint64_t next_mmap_addr = 0x70000000ULL;
  std::map<uint64_t, Mapping> mappings;

  // Quản lý sự kiện epoll
  int next_epoll_fd = 1000;
  std::map<int, EpollInstance> epoll_instances;

  // Hàng đợi futex (địa chỉ uaddr → số lượng tiến trình/luồng đang đợi)
  std::map<uint64_t, int> futex_waiters;
};

/// Hàm mục tiêu của một syscall (tối đa 6 đối số như Linux ABI).
using SyscallFn = SyscallResult (*)(SyscallCtx&, long, long, long, long, long, long);

/// Một hàng bảng phân phối.
struct SyscallEntry {
  int nr;
  const char* name;
  SyscallFn fn;
};

/// Bảng phân phối đầy đủ (theo số hiệu) — dùng cho audit + tra cứu.
const SyscallEntry* syscall_table();
size_t syscall_table_size();

/// Phân phối một lời gọi. Số không có trong bảng ⇒ -ENOSYS.
SyscallResult dispatch(SyscallCtx& ctx, int nr, long a0 = 0, long a1 = 0, long a2 = 0,
                       long a3 = 0, long a4 = 0, long a5 = 0);

/// Mô tả ngắn cho audit: "openat → sysfs::open (chỉ /state ghi được)".
const char* abi_mapping(int nr);

}  // namespace yui::linuxabi

#endif  // YUI_LINUXABI_HPP
