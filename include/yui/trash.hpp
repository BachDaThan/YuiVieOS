// Thùng rác + dọn rác an toàn (Tầng 5, P4.17).
//
// Hai việc, hai nguyên tắc:
//
// 1. XOÁ ⇒ VÀO THÙNG RÁC, KHÔI PHỤC ĐƯỢC.
//    `Trash::put` chuyển tệp/thư mục vào `<root>/files/<tên>` và ghi
//    `<root>/info/<tên>.trashinfo` theo dạng của đặc tả Trash freedesktop.org
//    (Path=, DeletionDate=) cộng hai khoá riêng `X-Yui-Sha256=` (băm cả cây)
//    và `X-Yui-Size=`. Khôi phục kiểm lại băm TRƯỚC khi trả tệp về; lệch ⇒
//    từ chối và để nguyên trong thùng. Chỗ cũ đã có tệp khác ⇒ không ghi đè.
//    Thùng đầy ⇒ `put` từ chối kèm cảnh báo, tệp gốc giữ nguyên: thùng
//    KHÔNG BAO GIỜ tự xoá món cũ để lấy chỗ.
//    Liên kết tượng trưng được chuyển nguyên là liên kết (không đi theo).
//    Trên nhân riêng sẽ nối snapshot `CowStore` của P2.8; trên Linux host
//    dùng thư mục thùng rác riêng này.
//
// 2. "DỌN RÁC" CHỈ XOÁ DANH SÁCH CÓ TÊN.
//    `plan_cleanup` chỉ nhìn trong các `CleanTarget` được khai báo tên (bộ
//    nhớ đệm tải dở của Kho, thư mục tạm của Kho/yui-trim, thư mục tạm của
//    hệ thống), không quét đoán, không đi theo liên kết, không đi ra ngoài
//    thư mục đích. Kế hoạch hiện dung lượng sẽ giải phóng; `apply_cleanup`
//    chỉ chạy khi có xác nhận tin cậy (P4.13) cho ĐÚNG kế hoạch đó (băm kế
//    hoạch nằm trong đích hộp thoại) và kiểm lại từng mục (cùng inode, cùng
//    kích thước, cùng mtime) ngay trước khi xoá.
//
// Xoá vĩnh viễn (`purge`, `empty`) cũng phải qua `ConfirmGate`.
#ifndef YUI_TRASH_HPP
#define YUI_TRASH_HPP

#include <cstdint>
#include <string>
#include <vector>

namespace yui::sandbox {
class TrustedConfirmationHost;
}

namespace yui::trash {

// ------------------------------------------------------------- xác nhận --
// `target` mô tả đúng việc sẽ làm ("trash:empty", "trash:purge:<mã>",
// "trash:clean:<băm kế hoạch>"); một lần bấm không dùng được cho việc khác.
class ConfirmGate {
 public:
  virtual ~ConfirmGate() = default;
  virtual bool confirmed(const std::string& target, const std::string& summary_vi) = 0;
};

class TrustedDialogGate : public ConfirmGate {
 public:
  TrustedDialogGate(const sandbox::TrustedConfirmationHost& host, std::string dialog_id)
      : host_(host), dialog_id_(std::move(dialog_id)) {}
  bool confirmed(const std::string& target, const std::string& summary_vi) override;

 private:
  const sandbox::TrustedConfirmationHost& host_;
  std::string dialog_id_;
};

// ------------------------------------------------------------ thùng rác --
struct Options {
  std::string root;                                   // thư mục thùng rác
  uint64_t capacity_bytes = 2ull * 1024 * 1024 * 1024;  // 2 GiB
  double warn_ratio = 0.9;                            // vượt 90 % ⇒ cảnh báo
  // Không bao giờ đưa vào thùng: chính các thư mục này và mọi thứ bên trong.
  std::vector<std::string> protected_prefixes = {"/bin", "/boot", "/core", "/dev", "/etc", "/lib",
                                                 "/lib32", "/lib64", "/proc", "/run", "/sbin",
                                                 "/sys", "/usr"};
  // Không đưa vào thùng chính các thư mục này (bên trong thì được), vd $HOME.
  std::vector<std::string> protected_exact = {};
  bool force_copy = false;  // test: giả như khác ổ đĩa (EXDEV) ⇒ chép + kiểm + xoá
};

struct Item {
  std::string id;             // tên trong files/ (duy nhất trong thùng)
  std::string original_path;  // đường dẫn tuyệt đối lúc xoá
  std::string deleted_at;     // "YYYY-MM-DDThh:mm:ss" (giờ máy, theo đặc tả)
  std::string sha256;         // băm cây; rỗng nếu món do chương trình khác bỏ vào
  uint64_t size = 0;
  bool is_dir = false;
};

struct Result {
  bool ok = false;
  std::string code;       // ok | not_found | protected | trash_full | exists | hash_mismatch |
                          // unsupported | io_error | not_confirmed | stale_plan
  std::string reason_vi;
  std::string warning_vi;  // vd thùng sắp đầy
  Item item;
  uint64_t freed_bytes = 0;
  size_t removed = 0;
};

// Băm cả cây (tệp: nội dung + quyền; thư mục: danh sách đã sắp xếp; liên
// kết: chuỗi đích). Tệp đặc biệt (fifo, socket, thiết bị) ⇒ false.
bool hash_tree(const std::string& path, std::string& sha256_hex, uint64_t& bytes, std::string* err);

// Mã hoá/giải mã Path= theo kiểu URL của đặc tả (giữ nguyên '/').
std::string encode_path(const std::string& p);
std::string decode_path(const std::string& p);

class Trash {
 public:
  explicit Trash(Options opts);

  Result put(const std::string& path);
  Result restore(const std::string& id, const std::string& to = "");
  Result purge(const std::string& id, ConfirmGate& gate);
  Result empty(ConfirmGate& gate);

  std::vector<Item> list() const;  // mới nhất trước
  uint64_t used_bytes() const;
  const Options& options() const { return opts_; }
  std::string usage_vi() const;

  static std::string purge_target(const std::string& id) { return "trash:purge:" + id; }
  // Đích "đổ thùng" gắn với đúng danh sách món đang có: thùng thay đổi sau
  // khi hỏi ⇒ lần bấm cũ không còn hiệu lực.
  std::string empty_target() const;
  std::string empty_summary_vi() const;

 private:
  bool ensure_layout(std::string* err) const;
  bool is_protected(const std::string& abs, std::string& why) const;
  std::string unique_name(const std::string& base) const;
  bool read_item(const std::string& id, Item& out) const;
  std::string warning_for(uint64_t used) const;

  Options opts_;
};

// --------------------------------------------------------------- dọn rác --
struct CleanTarget {
  std::string label_vi;      // "Bộ nhớ đệm tải dở của Kho"
  std::string dir;           // thư mục, phải là thư mục thật (không phải liên kết)
  std::string suffix;        // chỉ mục có đuôi này ("" = mọi mục)
  bool include_dirs = false;  // được xoá thư mục con (cả cây, không theo liên kết)
  int64_t min_age_s = 0;     // chỉ mục cũ hơn chừng này (tránh đụng việc đang chạy)
};

struct CleanEntry {
  std::string path;
  std::string label_vi;
  uint64_t bytes = 0;
  bool is_dir = false;
  uint64_t dev = 0, ino = 0;
  int64_t mtime = 0;
};

struct CleanPlan {
  std::vector<CleanEntry> entries;
  uint64_t total_bytes = 0;
  std::vector<std::string> skipped_vi;  // mục bỏ qua và lý do (liên kết, quá mới…)
  std::string digest() const;           // băm kế hoạch, gắn vào đích hộp thoại
  std::string target() const { return "trash:clean:" + digest(); }
  std::string summary_vi() const;
};

// Danh sách có tên mặc định: tải dở + thư mục tạm của Kho, thư mục tạm của hệ thống.
std::vector<CleanTarget> default_targets(const std::string& store_root, const std::string& state_dir);

CleanPlan plan_cleanup(const std::vector<CleanTarget>& targets, int64_t now_s);
Result apply_cleanup(const CleanPlan& plan, const std::vector<CleanTarget>& targets, ConfirmGate& gate);

std::string human_bytes(uint64_t n);

}  // namespace yui::trash

#endif  // YUI_TRASH_HPP
