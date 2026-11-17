// Sovereign Keyring & gói cập nhật ngoại tuyến `.bundle` (Tầng 1, kernel-cap).
//
// Đặc tả (docs/SECURITY.md §6):
//   bundle/
//     manifest.json   { "version", "key_id", "files":[{"path","sha256","mode"}] }
//     manifest.sig    Ed25519(manifest_canonical) — 64 byte thô
//     payload/        tệp thật
//
// Ba nguyên tắc:
//   1. **Khoá chủ quyền**: khoá phát hành cài một lần (USB) vào
//      `/etc/yui/trusted-keys/`, không bao giờ lấy từ mạng.
//   2. **Chuẩn hoá trước khi ký**: bên xác minh TỰ dựng lại chuỗi chuẩn từ
//      các trường đã parse rồi mới so chữ ký — không bao giờ ký lại chuỗi
//      nhận được (chống canonicalisation như token năng lực).
//   3. **Hai lớp toàn vẹn**: chữ ký Ed25519 toàn manifest + SHA-256 từng tệp
//      payload sau khi chép. Sửa 1 byte ở bất kỳ đâu ⇒ từ chối, có lý do
//      tiếng Việt.
//
// Cặp đối chiếu Rust: `kernel-cap/src/{keyring,bundle}.rs`.
#ifndef YUI_BUNDLE_HPP
#define YUI_BUNDLE_HPP

#include <array>
#include <map>
#include <string>
#include <vector>

#include "yui/ed25519.hpp"

namespace yui::bundle {

// ------------------------------------------------------ Sovereign Keyring --
class Keyring {
 public:
  // Trả về false nếu key_id rỗng/đã tồn tại (không ghi đè im lặng).
  bool add(const std::string& key_id, const ed25519::PublicKey& pub);
  bool remove(const std::string& key_id);
  bool contains(const std::string& key_id) const;
  bool get(const std::string& key_id, ed25519::PublicKey& out) const;
  size_t size() const { return keys_.size(); }
  std::vector<std::string> ids() const;

  // Mỗi tệp `<key_id>.pub` chứa 64 hex (32 byte public key).
  bool load_dir(const std::string& dir, std::string* err = nullptr);
  bool save_dir(const std::string& dir, std::string* err = nullptr) const;

 private:
  std::map<std::string, ed25519::PublicKey> keys_;
};

// ------------------------------------------------------------- Manifest ----
struct ManifestFile {
  std::string path;        // đường dẫn tương đối trong payload/ (chống ../)
  std::string sha256_hex;  // 64 hex thường
  std::string mode;        // "0644", "0755"…
};

struct Manifest {
  std::string version;
  std::string key_id;
  std::string stream = "os";  // "os", "models", "apps"
  std::vector<ManifestFile> files;  // sắp xếp theo path khi chuẩn hoá
};

/// Chuỗi chuẩn hoá để ký: khoá cố định, file sắp theo path, không khoảng
/// trắng thừa. Verifier DỰNG LẠI chuỗi này từ trường đã parse.
std::string canonical(const Manifest& m);

/// Parse manifest.json (định dạng người–máy đọc được); kiểm đường dẫn file.
bool parse_manifest(const std::string& json_text, Manifest& out, std::string* err = nullptr);

// ------------------------------------------------------------- Sign (P4.7) -
struct SignOptions {
  std::string version = "1.0.0";
  std::string key_id;
  std::string stream = "os";  // "os", "models", "apps"
};

/// Ký số ngoại tuyến: duyệt payload_dir, tính SHA-256 từng tệp, tạo manifest.json,
/// ký Ed25519 bằng seed tạo manifest.sig (64 byte thô), chép tệp vào bundle_dir/payload/.
bool sign_bundle(const std::string& payload_dir,
                 const std::string& bundle_dir,
                 const SignOptions& opts,
                 const ed25519::Seed& seed,
                 std::string* err = nullptr);

// ------------------------------------------------------------- Verify -----
struct VerifyResult {
  bool ok = false;
  std::string reason_vi;  // lý do tiếng Việt cho audit/log
  std::string key_id;     // khoá đã khớp (khi ok)
  bool signature_ok = false;
  bool payload_ok = false;
};

/// Xác minh bundle: chữ ký manifest bằng khoá trong keyring, rồi SHA-256 từng
/// tệp trong payload/. Offline hoàn toàn — không mở mạng.
VerifyResult verify(const std::string& bundle_dir, const Keyring& keys);

// ------------------------------------------------------------- Multi-stream & A/B (P4.9)
struct UpdateOptions {
  std::string stream = "os";  // "os", "models", "apps"
  bool defer_reboot = false;  // nếu true: hoãn kích hoạt (đặt cờ chờ khởi động lại, không ngắt phiên làm việc)
};

struct UpdateStatus {
  std::string stream = "os";
  std::string active_slot = "A";  // "A" hoặc "B"
  std::string backup_slot = "B";  // "B" hoặc "A"
  bool has_pending = false;
  std::string pending_slot;       // khe chờ kích hoạt ở boot tới (khi defer_reboot)
  std::string active_version;
  std::string backup_version;
  std::string active_key_id;
  size_t file_count = 0;
};

// ------------------------------------------------------------- Install ----
struct InstallResult {
  bool ok = false;
  std::string slot;       // "A" hoặc "B" vừa được ghi
  std::string reason_vi;
  std::string boot_pointer_path;
  bool pending = false;   // true nếu được ghi vào khe chờ khởi động lại (defer_reboot)
};

/// Đọc trạng thái cập nhật của một luồng (os, models, apps).
UpdateStatus update_status(const std::string& state_dir, const std::string& stream = "os");

/// Cài bundle theo luồng A/B với tuỳ chọn hoãn kích hoạt (không ngắt phiên làm việc).
InstallResult update_apply(const std::string& bundle_dir,
                           const Keyring& keys,
                           const std::string& state_dir,
                           const UpdateOptions& opts = {});

/// Quay lui (rollback) về khe trước đó của luồng.
InstallResult update_rollback(const std::string& state_dir,
                              const std::string& stream = "os");

/// Kích hoạt khe đang chờ (boot apply) cho luồng.
bool update_commit_pending(const std::string& state_dir,
                           const std::string& stream = "os",
                           std::string* err = nullptr);

/// Khe đang hoạt động của luồng ("A" nếu chưa từng cài).
std::string stream_active_slot(const std::string& state_dir, const std::string& stream = "os");

/// Đường dẫn thư mục khe của một luồng.
std::string stream_slot_dir(const std::string& state_dir, const std::string& stream,
                            const std::string& slot);

/// Cài bundle vào slot KHÔNG hoạt động (A/B), kiểm hash sau khi chép, rồi
/// đổi con trỏ boot bằng ghi nguyên tử (tmp → rename). Cấu trúc state_dir:
///   slots/A/  slots/B/  boot_pointer ("A" hoặc "B")
InstallResult install(const std::string& bundle_dir, const Keyring& keys,
                      const std::string& state_dir);

/// Slot đang hoạt động ("A" nếu chưa từng cài).
std::string active_slot(const std::string& state_dir);

}  // namespace yui::bundle

#endif  // YUI_BUNDLE_HPP
