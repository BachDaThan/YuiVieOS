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
  SYS_socket = 41,
  SYS_connect = 42,
  SYS_accept = 43,
  SYS_exit = 60,
  SYS_fork = 57,
  SYS_execve = 59,
  SYS_wait4 = 61,
  SYS_exit_group = 231,
  SYS_openat = 257,
  SYS_utimensat = 280,
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

/// Bối cảnh thực thi — bảng fd + FS ảo để kiểm chứng không cần đĩa thật.
struct SyscallCtx {
  uint64_t caps = 0;             // quyền yui::sandbox::Right (kiểm tra khi cần)
  std::map<std::string, std::vector<uint8_t>> files;  // FS ảo (đường dẫn → nộidung)
  std::vector<std::string> fd_paths;  // fd 3.. → đường dẫn đã mở
  std::vector<uint64_t> fd_pos;
  std::vector<bool> fd_writable;
  std::string console;           // stdout/stderr tích luỹ
  bool exited = false;
  int exit_code = 0;
};

/// Hàm mục tiêu của một syscall.
using SyscallFn = SyscallResult (*)(SyscallCtx&, long, long, long, long);

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
                       long a3 = 0);

/// Mô tả ngắn cho audit: "openat → sysfs::open (chỉ /state ghi được)".
const char* abi_mapping(int nr);

}  // namespace yui::linuxabi

#endif  // YUI_LINUXABI_HPP
