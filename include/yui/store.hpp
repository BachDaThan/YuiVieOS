// Yui Store — lõi Kho Ứng Dụng (Tầng 5, P4.15).
//
// Luồng một ứng dụng đi qua, theo đúng thứ tự, mỗi bước có thể từ chối:
//
//   catalog.json + catalog.sig      danh mục ký Ed25519 bằng khoá trong keyring
//        │  (chưa ký / sai chữ ký / khoá lạ ⇒ TỪ CHỐI cả danh mục)
//        ▼
//   kế hoạch cài (InstallPlan)      nguồn tải, giấy phép, quyền sẽ cấp — hiện
//        │                          cho người dùng TRƯỚC khi xác nhận
//        ▼
//   xác nhận tin cậy (P4.13)        không có ⇒ không tải, không cài
//        ▼
//   tải từ URL nguồn CHÍNH THỨC     host phải nằm trong `allowed_hosts` của danh
//        │                          mục (kể cả sau chuyển hướng); tải vào
//        │                          cache/<id>@<ver>.part, tải dở thì giữ để nối
//        ▼
//   kiểm kích thước + SHA-256       lệch ⇒ xoá .part, từ chối
//        ▼
//   bung vào staging                tar (ustar/pax) · yuip · tệp đơn; chặn
//        │                          đường dẫn tuyệt đối, `..`, liên kết (zip-slip)
//        ▼
//   gọt cục bộ (yui-trim)           chỉ trên bản vừa tải, chỉ khi danh mục cho
//        │                          phép; KHÔNG phân phối lại bản đã gọt
//        ▼
//   hộp cát (P3.12 + P3.15)         app có chữ ký nhà phát hành tin cậy ⇒ chính
//        │                          sách khai báo; không chữ ký ⇒ Isolated,
//        │                          net:none, tước admin/power
//        ▼
//   apps/<id>/ + policies/<id>.policy.json + menu/<id>.desktop + installed.json
//
// Không có gì ở đây tự mở mạng: việc tải đi qua `Fetcher` do bên gọi đưa vào.
// `CurlFetcher` (Linux host, P1) gọi chương trình `curl` của hệ thống, chỉ
// https, chỉ chuyển hướng sang https; yuid ở chế độ offline không dùng nó.
//
// Giới hạn đã biết (ghi thẳng, không tô hồng):
//   * chưa có bộ giải nén DEFLATE: chưa nhận `.tar.gz`/`.zip`/`.deb`; tệp đơn
//     (AppImage, `.exe`, `.apk`) được lưu nguyên vẹn, chạy chúng là việc của P3;
//   * xác nhận ở terminal (`TtyConfirmation`) là đường tạm trên Linux host — kẻ
//     chiếm được terminal của bạn vẫn gõ thay được; đường tin cậy thật là hộp
//     thoại của compositor (P4.1 + P4.13).
#ifndef YUI_STORE_HPP
#define YUI_STORE_HPP

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "yui/apppack.hpp"
#include "yui/bundle.hpp"
#include "yui/ed25519.hpp"
#include "yui/sandbox.hpp"

namespace yui::store {

// ---------------------------------------------------------------- danh mục --
struct AppEntry {
  std::string id;           // reverse-DNS: [a-z0-9.-], 3..64 ký tự, vd "org.example.hello"
  std::string name;         // tên hiển thị (có thể trùng giữa hai app khác id)
  std::string version;
  std::string publisher;
  std::string kind = "native";   // "native" | "wine" | "apk"
  std::string format = "tar";    // "tar" | "yuip" | "file"
  std::string source_url;        // URL nguồn chính thức (không mirror)
  std::string sha256;            // 64 hex thường
  uint64_t size = 0;             // byte, khớp chính xác
  std::string license;
  std::string exec;              // đường dẫn tương đối của tệp chạy trong apps/<id>/
  bool trim = true;              // danh mục cho phép gọt cục bộ (false cho app có DRM/kiểm toàn vẹn)
  std::string low_ram_args;      // tham số tiết kiệm RAM nhà phát hành cho phép ("" = không có)
  sandbox::Policy policy;        // chính sách gợi ý (P3.15); app_id luôn bị ép = id
  // Chữ ký của nhà phát hành trên `entry_canonical()` (không gồm hai trường này).
  std::string signer_key_id;
  std::string signature_hex;     // 128 hex; rỗng = app không chữ ký ⇒ P3.12
};

struct Catalog {
  std::string format = "yui-store-catalog/1";
  std::string key_id;                       // khoá đã ký danh mục
  std::string issued;                       // ngày phát hành (YYYY-MM-DD)
  std::vector<std::string> allowed_hosts;   // host được phép tải (khớp chính xác)
  std::vector<AppEntry> apps;
};

/// Chuỗi chuẩn hoá của một mục (để nhà phát hành ký). Không gồm chữ ký.
std::string entry_canonical(const AppEntry& e);
/// Chuỗi chuẩn hoá của cả danh mục (để chủ kho ký). Gồm chữ ký từng mục.
/// Bên xác minh DỰNG LẠI chuỗi này từ trường đã parse, không ký lại byte nhận được.
std::string canonical(const Catalog& c);

/// Parse + kiểm cấu trúc: id hợp lệ và không trùng, sha256 64 hex, URL có
/// scheme https:// hoặc file://, exec là đường dẫn tương đối an toàn…
bool parse_catalog(const std::string& json_text, Catalog& out, std::string* err = nullptr);
std::string catalog_to_json(const Catalog& c, bool pretty = true);

std::string sign_catalog(const Catalog& c, const ed25519::Seed& seed);      // 128 hex
std::string sign_entry(const AppEntry& e, const ed25519::Seed& seed);       // 128 hex

// URL: "https://host[:port]/path" → host (chữ thường, bỏ cổng). Rỗng nếu sai.
std::string url_host(const std::string& url);
std::string url_scheme(const std::string& url);

// Bỏ dấu tiếng Việt + chữ thường, để "tim" tìm được "Tìm".
std::string fold_unaccent(const std::string& s);

// ------------------------------------------------------------------- tải ----
class Fetcher {
 public:
  virtual ~Fetcher() = default;
  /// Tải `url` NỐI TIẾP vào cuối `dest` (tệp có thể đã có phần đầu từ lần tải
  /// dở trước). Trả về false nếu chưa xong; `effective_url` là URL cuối cùng
  /// sau chuyển hướng (Store kiểm lại host của nó).
  virtual bool fetch(const std::string& url, const std::string& dest, std::string& effective_url,
                     std::string* err) = 0;
};

/// file:///đường/dẫn — chỉ dùng khi Options.allow_file_urls (thử nghiệm, bản
/// bạn đã tự tải về từ nguồn chính thức).
class FileFetcher : public Fetcher {
 public:
  bool fetch(const std::string& url, const std::string& dest, std::string& effective_url,
             std::string* err) override;
};

/// https qua chương trình `curl` của Linux host (fork/execvp, không qua shell):
/// --proto =https --proto-redir =https --max-redirs 5 --continue-at -.
class CurlFetcher : public Fetcher {
 public:
  explicit CurlFetcher(std::string curl_path = "curl") : curl_(std::move(curl_path)) {}
  bool fetch(const std::string& url, const std::string& dest, std::string& effective_url,
             std::string* err) override;

 private:
  std::string curl_;
};

// ------------------------------------------------------------- kế hoạch ----
struct InstallPlan {
  AppEntry entry;
  sandbox::AppSignatureStatus app_signature = sandbox::AppSignatureStatus::Unsigned;
  sandbox::Policy enforced_policy;   // chính sách SẼ áp (sau P3.12)
  std::string source_host;
  bool already_installed = false;
  std::string installed_version;
  std::string summary_vi() const;    // khối chữ hiện trước khi xác nhận
};

/// Cổng xác nhận: Store chỉ tải/cài khi `confirmed(plan)` trả true.
class ConfirmationGate {
 public:
  virtual ~ConfirmationGate() = default;
  virtual bool confirmed(const InstallPlan& plan) = 0;
};

/// Hộp thoại của đường tin cậy P4.13: đúng hộp thoại, đúng đích "store:<id>",
/// và đã được bấm bằng thiết bị vật lý.
class TrustedDialogConfirmation : public ConfirmationGate {
 public:
  TrustedDialogConfirmation(const sandbox::TrustedConfirmationHost& host, std::string dialog_id)
      : host_(host), dialog_id_(std::move(dialog_id)) {}
  bool confirmed(const InstallPlan& plan) override;
  static std::string target_for(const std::string& app_id) { return "store:" + app_id; }

 private:
  const sandbox::TrustedConfirmationHost& host_;
  std::string dialog_id_;
};

/// Linux host: in kế hoạch ra /dev/tty và bắt gõ lại ĐÚNG mã ứng dụng. Không
/// đọc stdin (ống dẫn từ script không xác nhận được). Không có /dev/tty ⇒ từ chối.
class TtyConfirmation : public ConfirmationGate {
 public:
  bool confirmed(const InstallPlan& plan) override;
};

// ----------------------------------------------------------------- Store ----
struct Options {
  std::string root;                 // vd /var/lib/yui/store
  bool allow_file_urls = false;     // nhận file:// (thử nghiệm / bản tự tải)
  uint64_t max_package_bytes = 4ull * 1024 * 1024 * 1024;
  apppack::TrimOptions trim;        // locale giữ lại: vi, en
};

struct InstalledApp {
  std::string id, name, version, publisher, kind, sha256, signature;  // signature: valid|unsigned
  std::string app_dir, policy_path, menu_path;
  int64_t installed_at = 0;
  uint64_t files = 0, bytes = 0, bytes_trimmed = 0;
};

struct Result {
  bool ok = false;
  std::string code;        // mã máy đọc: "ok", "bad_signature", "hash_mismatch", "download_incomplete",
                           // "host_not_allowed", "not_confirmed", "unsafe_path", "ambiguous", ...
  std::string reason_vi;
  std::vector<std::string> candidates;   // khi "ambiguous": các id trùng tên
  InstalledApp app;                      // khi cài xong
};

class Store {
 public:
  Store(Options opts, const bundle::Keyring& keyring);

  /// Nạp danh mục; chỉ thành công khi chữ ký hợp lệ bởi khoá trong keyring.
  Result load_catalog(const std::string& catalog_json, const std::string& sig_hex);
  /// Nạp từ <root>/catalog.json + <root>/catalog.sig (hex).
  Result load_catalog_files();
  /// Lưu danh mục đã xác minh vào <root> (để lần sau nạp không cần mạng).
  bool save_catalog(const std::string& catalog_json, const std::string& sig_hex, std::string* err = nullptr);
  bool has_catalog() const { return loaded_; }
  const Catalog& catalog() const { return catalog_; }

  /// Tìm không dấu trên tên, id, nhà phát hành. Sắp theo tên rồi id.
  std::vector<AppEntry> search(const std::string& query) const;
  /// id chính xác, hoặc tên duy nhất. Hai app cùng tên ⇒ "ambiguous" + danh sách id.
  Result resolve(const std::string& name_or_id, AppEntry& out) const;

  Result plan(const std::string& name_or_id, InstallPlan& out) const;
  Result install(const std::string& name_or_id, Fetcher& fetcher, ConfirmationGate& gate);
  Result remove(const std::string& id);

  std::vector<InstalledApp> installed() const;
  bool is_installed(const std::string& id, InstalledApp* out = nullptr) const;

  std::string cache_part_path(const AppEntry& e) const;
  const Options& options() const { return opts_; }

 private:
  bool write_installed(const std::vector<InstalledApp>& apps, std::string* err) const;

  Options opts_;
  const bundle::Keyring& keyring_;
  Catalog catalog_;
  bool loaded_ = false;
};

// ---------------------------------------------------------- tar (ustar) ----
/// Parse tar không nén (ustar + pax 'x' + GNU 'L'). Chỉ nhận tệp thường và thư
/// mục; liên kết/thiết bị/đường dẫn thoát ⇒ false + lý do (chống zip-slip).
bool parse_tar(const std::vector<uint8_t>& data, std::vector<apppack::Entry>& out, std::string* err = nullptr);
/// Tạo tar ustar tối giản (dùng cho test và cho nhà phát hành thử nghiệm).
std::vector<uint8_t> build_tar(const std::vector<apppack::Entry>& entries);

/// Đường dẫn tương đối an toàn: không rỗng, không '/', không thành phần ".."/".", không '\\'.
bool safe_relative_path(const std::string& p);

}  // namespace yui::store

#endif  // YUI_STORE_HPP
