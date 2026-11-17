// Hệ tệp bất biến + Ghi gộp — xem include/yui/fsstore.hpp.
#include "yui/fsstore.hpp"

#include <algorithm>
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

// ============================================================================
// CowStore implementation
// ============================================================================

bool CowStore::write_file(const std::string& relpath, std::vector<uint8_t> data, std::string* err) {
  std::string why;
  if (!safe_relpath(relpath, &why)) {
    if (err) *err = why;
    return false;
  }
  current_files_[relpath] = std::make_shared<const std::vector<uint8_t>>(std::move(data));
  return true;
}

bool CowStore::read_file(const std::string& relpath, std::vector<uint8_t>* out) const {
  std::string why;
  if (!safe_relpath(relpath, &why)) return false;
  auto it = current_files_.find(relpath);
  if (it == current_files_.end()) return false;
  if (out) *out = *it->second;
  return true;
}

bool CowStore::delete_file(const std::string& relpath, std::string* err) {
  std::string why;
  if (!safe_relpath(relpath, &why)) {
    if (err) *err = why;
    return false;
  }
  auto it = current_files_.find(relpath);
  if (it == current_files_.end()) {
    if (err) *err = "tệp không tồn tại: " + relpath;
    return false;
  }
  current_files_.erase(it);
  return true;
}

bool CowStore::has_file(const std::string& relpath) const {
  return current_files_.find(relpath) != current_files_.end();
}

std::vector<std::string> CowStore::list_files() const {
  std::vector<std::string> res;
  res.reserve(current_files_.size());
  for (const auto& kv : current_files_) {
    res.push_back(kv.first);
  }
  std::sort(res.begin(), res.end());
  return res;
}

std::string CowStore::create_snapshot(const std::string& tag, int64_t now_ns, double* elapsed_ms_out) {
  const auto t0 = std::chrono::high_resolution_clock::now();

  InternalSnapshot snap;
  snap.meta.id = "snap-" + std::to_string(next_snapshot_idx_++);
  snap.meta.tag = tag.empty() ? snap.meta.id : tag;
  snap.meta.created_at_ns = (now_ns > 0) ? now_ns :
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count();
  snap.meta.file_count = current_files_.size();

  size_t bytes = 0;
  for (const auto& kv : current_files_) {
    bytes += kv.second->size();
  }
  snap.meta.bytes = bytes;
  snap.files = current_files_;  // CoW: sao chép shallow các shared_ptr

  const auto t1 = std::chrono::high_resolution_clock::now();
  const double elapsed_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
  snap.meta.creation_time_ms = elapsed_ms;
  if (elapsed_ms_out) *elapsed_ms_out = elapsed_ms;

  const std::string id = snap.meta.id;
  snapshots_[id] = std::move(snap);
  return id;
}

bool CowStore::rollback_to(const std::string& snapshot_id, std::string* err) {
  auto it = snapshots_.find(snapshot_id);
  if (it == snapshots_.end()) {
    if (err) *err = "không tìm thấy snapshot ID: " + snapshot_id;
    return false;
  }
  // Khôi phục lại toàn bộ trạng thái tệp tại thời điểm snapshot được chụp
  current_files_ = it->second.files;
  return true;
}

bool CowStore::drop_snapshot(const std::string& snapshot_id) {
  return snapshots_.erase(snapshot_id) > 0;
}

bool CowStore::has_snapshot(const std::string& snapshot_id) const {
  return snapshots_.find(snapshot_id) != snapshots_.end();
}

bool CowStore::get_snapshot_meta(const std::string& snapshot_id, SnapshotMeta* meta) const {
  auto it = snapshots_.find(snapshot_id);
  if (it == snapshots_.end()) return false;
  if (meta) *meta = it->second.meta;
  return true;
}

std::vector<std::string> CowStore::list_snapshots() const {
  std::vector<std::string> res;
  res.reserve(snapshots_.size());
  for (const auto& kv : snapshots_) {
    res.push_back(kv.first);
  }
  return res;
}

StorageAccounting CowStore::compute_accounting() const {
  StorageAccounting acc;
  acc.file_count = current_files_.size();

  std::unordered_map<const std::vector<uint8_t>*, size_t> seen;

  // Active state
  for (const auto& kv : current_files_) {
    acc.logical_bytes += kv.second->size();
    seen[kv.second.get()]++;
  }

  // All snapshots
  for (const auto& s : snapshots_) {
    for (const auto& kv : s.second.files) {
      acc.logical_bytes += kv.second->size();
      seen[kv.second.get()]++;
    }
  }

  for (const auto& kv : seen) {
    const size_t sz = kv.first->size();
    acc.physical_bytes += sz;
    if (kv.second > 1) {
      acc.shared_bytes += sz;
    } else {
      acc.exclusive_bytes += sz;
    }
  }

  if (acc.logical_bytes > 0 && acc.logical_bytes >= acc.physical_bytes) {
    acc.savings_ratio = double(acc.logical_bytes - acc.physical_bytes) / double(acc.logical_bytes);
  } else {
    acc.savings_ratio = 0.0;
  }

  return acc;
}

}  // namespace yui::fsstore
