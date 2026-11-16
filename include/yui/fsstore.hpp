// Hệ tệp bất biến + Ghi gộp (Immutable FS + Write Coalescing) — Tầng 2.
//
// Lõi hệ điều hành đọc-ghi trong phân vùng bất biến: mọi lõi (kernel image,
// module, policy) chỉ đọc; ghi hợp lệ duy nhất đi qua `StateStore` ở
// phân vùng trạng thái, và các ghi vật lý được **gộp** (coalescing) — nhiều
// lần ghi cùng trang trong cửa sổ thời gian chỉ chốt một lần xuống đĩa.
//
// Cấu trúc phân vùng (xác minh bởi `scripts/verify.sh`):
//   /core   (chỉ-đọc, hash-locked)  — lõi + module + policy
//   /state  (đọc-ghi, atomic)       — trạng thái, cấu hình người dùng
//
// Mọi đường dẫn đi vào đều phải nằm trong gốc khai báo; đường dẫn tuyệt đối,
// "..", "//" bị từ chối với lý do tiếng Việt (đúng nguyên tắc
// docs/SECURITY.md: giải mã → kiểm tra toàn vẹn → phân tích cú pháp → dùng).
#ifndef YUI_FSSTORE_HPP
#define YUI_FSSTORE_HPP

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace yui::fsstore {

/// Cờ vùng chứa: bất biến hay trạng thái.
enum class Partition { Core, State };

struct WriteOp {
  std::string path;              // tương đối với gốc của phân vùng
  std::vector<uint8_t> data;
  uint32_t mode = 0644;
};

/// Kết quả ghi (gộp): số lần chốt vật lý sau gộp.
struct FlushResult {
  bool ok = false;
  size_t logical_writes = 0;   // số lệnh ghi đã nhận
  size_t physical_writes = 0;   // số lần chốt thực sự xuống đĩa (≤ logical)
  std::string reason_vi;        // khi ok == false
};

/// Bộ ghi gộp: giữ trang ghi trong bộ nhớ, chốt khi flush() hoặc khi
/// vượt giới hạn cửa sổ. Ghi trùng đường dẫn trong cửa sổ được ghi đè
/// (coalesce) — chỉ bản cuối được chốt.
class WriteCoalescer {
 public:
  explicit WriteCoalescer(size_t max_pending = 64) : max_pending_(max_pending) {}

  /// Xếp một lệnh ghi. Ok=false nếu đường dẫn không an toàn.
  bool submit(const WriteOp& op, std::string* err = nullptr);

  size_t pending() const { return pending_.size(); }
  size_t submitted_total() const { return submitted_; }
  size_t flushed_total() const { return flushed_; }

  /// Chốt mọi trang đang giữ (gộp xong) — trả số liệu thật.
  /// `commit` là callback chốt một trang: trả true nếu thành công. Trang
  /// chốt lỗi được GIỮ LẠI để thử lần sau; trang thành công không lặp lại.
  template <typename CommitFn>
  FlushResult flush(CommitFn&& commit) {
    FlushResult r;
    r.logical_writes = submitted_;
    std::map<std::string, WriteOp> failed;
    for (auto& kv : pending_) {
      if (commit(kv.first, kv.second)) {
        ++r.physical_writes;
        flushed_ += 1;
      } else {
        failed.emplace(kv.first, std::move(kv.second));
      }
    }
    pending_ = std::move(failed);
    if (!pending_.empty()) {
      r.reason_vi = "không chốt được " + std::to_string(pending_.size()) +
                    " trang xuống đĩa — giữ lại để thử lại";
      return r;
    }
    r.ok = true;
    return r;
  }

  /// So sánh số lần chốt vật lý với số lệnh ghi — dùng cho test DoD.
  double coalesce_ratio() const {
    return submitted_ ? double(flushed_) / double(submitted_) : 1.0;
  }

 private:
  size_t max_pending_;
  std::map<std::string, WriteOp> pending_;
  size_t submitted_ = 0;
  size_t flushed_ = 0;
};

/// Kiểm tra đường dẫn an toàn (không tuyệt đối, không "..", không "//").
bool safe_relpath(const std::string& p, std::string* err = nullptr);

/// Phân vùng bất biến: đọc được, ghi bị từ chối với lý do tiếng Việt.
class ImmutablePartition {
 public:
  explicit ImmutablePartition(std::string root) : root_(std::move(root)) {}
  bool write(const std::string& rel, const std::vector<uint8_t>& data) const {
    (void)rel;
    (void)data;
    return false;  // luôn từ chối — xem write_denied_reason()
  }
  std::string write_denied_reason() const {
    return "phân vùng lõi /core là chỉ-đọc (bất biến): mọi ghi đều bị từ chối, "
           "chỉ ghi qua /state (WriteCoalescer) mới hợp lệ";
  }
  // Read a regular file beneath root_; reject symlinks at every path component.
  bool read(const std::string& rel, std::vector<uint8_t>* out) const;
  const std::string& root() const { return root_; }

 private:
  std::string root_;
};

/// Trạng thái ghi nhận từ đĩa để test/audit: danh sách trang đã chốt.
struct PageLog {
  std::map<std::string, std::vector<uint8_t>> pages;
  size_t commits = 0;
};

}  // namespace yui::fsstore

#endif  // YUI_FSSTORE_HPP
