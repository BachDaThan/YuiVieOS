// Hệ tệp bất biến + Ghi gộp — xem include/yui/fsstore.hpp.
#include "yui/fsstore.hpp"

#include <fstream>

namespace yui::fsstore {

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
  std::string why;
  if (!safe_relpath(rel, &why)) return false;
  std::ifstream f(root_ + "/" + rel, std::ios::binary);
  if (!f) return false;
  out->assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  return true;
}

}  // namespace yui::fsstore
