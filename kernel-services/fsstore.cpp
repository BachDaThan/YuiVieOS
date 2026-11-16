// Hệ tệp bất biến + Ghi gộp — xem include/yui/fsstore.hpp.
#include "yui/fsstore.hpp"

#include <array>
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace yui::fsstore {
namespace {

class ScopedFd {
 public:
  explicit ScopedFd(int fd) : fd_(fd) {}
  ~ScopedFd() {
    if (fd_ >= 0) ::close(fd_);
  }
  ScopedFd(const ScopedFd&) = delete;
  ScopedFd& operator=(const ScopedFd&) = delete;

  int get() const { return fd_; }
  void reset(int fd) {
    if (fd_ >= 0) ::close(fd_);
    fd_ = fd;
  }

 private:
  int fd_;
};

}  // namespace

bool safe_relpath(const std::string& p, std::string* err) {
  auto fail = [&](const std::string& why) {
    if (err) *err = why;
    return false;
  };
  if (p.empty()) return fail("đường dẫn rỗng");
  if (p[0] == '/' || (p.size() > 1 && p[1] == ':')) return fail("từ chối đường dẫn tuyệt đối: " + p);
  if (p.find("//") != std::string::npos) return fail("từ chối đường dẫn chứa '//': " + p);
  size_t i = 0;
  while (i < p.size()) {
    size_t j = p.find('/', i);
    if (j == std::string::npos) j = p.size();
    const std::string part = p.substr(i, j - i);
    if (part == "..") return fail("từ chối thành phần \"..\" trong đường dẫn: " + p);
    i = j + 1;
  }
  return true;
}

bool WriteCoalescer::submit(const WriteOp& op, std::string* err) {
  std::string why;
  if (!safe_relpath(op.path, &why)) {
    if (err) *err = why;
    return false;
  }
  if (pending_.find(op.path) == pending_.end() && pending_.size() >= max_pending_) {
    if (err) *err = "cửa sổ gộp đầy (" + std::to_string(max_pending_) +
                    " trang chưa chốt): cần flush() trước";
    return false;
  }
  pending_[op.path] = op;  // ghi đè = gộp: chỉ bản cuối được chốt
  ++submitted_;
  return true;
}

bool ImmutablePartition::read(const std::string& rel, std::vector<uint8_t>* out) const {
  if (!out) return false;
  std::string why;
  if (!safe_relpath(rel, &why)) return false;

  // Walk from an open root directory instead of concatenating strings. O_NOFOLLOW
  // at every component prevents a symlink inside /core from escaping its root.
  ScopedFd directory(::open(root_.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
  if (directory.get() < 0) return false;

  size_t begin = 0;
  for (;;) {
    const size_t slash = rel.find('/', begin);
    if (slash != std::string::npos) {
      const std::string component = rel.substr(begin, slash - begin);
      const int next = ::openat(directory.get(), component.c_str(),
                                O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
      if (next < 0) return false;
      directory.reset(next);
      begin = slash + 1;
      if (begin == rel.size()) return false;  // a trailing slash names a directory
      continue;
    }

    const std::string leaf = rel.substr(begin);
    struct stat expected {};
    if (::fstatat(directory.get(), leaf.c_str(), &expected, AT_SYMLINK_NOFOLLOW) != 0 ||
        !S_ISREG(expected.st_mode))
      return false;

    ScopedFd file(::openat(directory.get(), leaf.c_str(),
                           O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    if (file.get() < 0) return false;
    struct stat st {};
    if (::fstat(file.get(), &st) != 0 || !S_ISREG(st.st_mode) ||
        st.st_dev != expected.st_dev || st.st_ino != expected.st_ino)
      return false;

    std::vector<uint8_t> contents;
    std::array<uint8_t, 8192> buffer{};
    for (;;) {
      const ssize_t count = ::read(file.get(), buffer.data(), buffer.size());
      if (count == 0) break;
      if (count < 0) {
        if (errno == EINTR) continue;
        return false;
      }
      contents.insert(contents.end(), buffer.data(), buffer.data() + count);
    }
    out->swap(contents);
    return true;
  }
}

}  // namespace yui::fsstore
