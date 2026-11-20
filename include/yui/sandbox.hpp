// Tầng 1 + Tầng 3 boundary: capability tokens and the three-tier sandbox.
//
// Every process that wants to touch a resource presents a *capability token*
// signed by the capability authority (the yui-cap service).  The token names a
// subject, a set of rights and an expiry; the sandbox policy then decides
// whether a concrete action ("read /home/user/notes.txt") is covered by those
// rights.  Default is deny: an action that is not explicitly granted fails.
//
// Three isolation tiers, matching the diagram:
//   Native   — trusted first-party Yui services; still capability-checked.
//   Isolated — legacy Linux/Android-ish apps in a namespace + seccomp profile.
//   MicroVm  — untrusted Windows/game payloads inside a micro-VM; no host calls
//              at all except the virtio channel, so only tier-brokered requests
//              can cross the boundary.
#ifndef YUI_SANDBOX_HPP
#define YUI_SANDBOX_HPP

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "yui/json.hpp"

namespace yui::bundle {
class Keyring;
}

namespace yui::sandbox {

enum class Tier { Native = 0, Isolated = 1, MicroVm = 2 };
const char* tier_name(Tier t);
bool tier_from(const std::string& s, Tier& out);

// Right bits.  Kept as a 64-bit mask so a token can carry a whole profile.
enum Right : uint64_t {
  kRightNone = 0,
  kRightFsRead = 1ull << 0,
  kRightFsWrite = 1ull << 1,
  kRightNetConnect = 1ull << 2,
  kRightNetListen = 1ull << 3,
  kRightSpawn = 1ull << 4,
  kRightGpu = 1ull << 5,
  kRightInput = 1ull << 6,     // keyboard/mouse injection
  kRightCamera = 1ull << 7,
  kRightMic = 1ull << 8,
  kRightLocation = 1ull << 9,
  kRightPower = 1ull << 10,    // battery/thermal control
  kRightVault = 1ull << 11,
  kRightGraph = 1ull << 12,
  kRightModel = 1ull << 13,    // run inference through yuid
  kRightBluetooth = 1ull << 14,
  kRightInputCompose = 1ull << 15,  // input method composition (P3.13)
  kRightComputeHeavy = 1ull << 21,  // được phép tải CPU/GPU cao kéo dài (CAP_COMPUTE_HEAVY)
  kRightAdmin = 1ull << 20,    // policy administration
};
std::string rights_to_string(uint64_t rights);  // "fs.read,net.connect"
uint64_t rights_from_string(const std::string& s);

// ---------------------------------------------------------------- tokens --
struct Token {
  std::string subject;   // e.g. "app:trim" or "uid:1000/pid:4242"
  std::string issuer;    // "yui-cap"
  uint64_t rights = 0;
  int64_t issued_at = 0;   // unix seconds
  int64_t expires_at = 0;  // 0 = no expiry
  uint32_t serial = 0;
  std::vector<std::string> scopes;  // path prefixes / domains this token covers
  uint32_t generation = 0;          // generation / epoch for O(1) revocation
  std::string family_id;            // identifier of token family (defaults to subject if empty)
  std::string mac_hex;              // HMAC-SHA-256 over the canonical form

  std::string canonical() const;  // deterministic serialisation that gets signed
};

class Authority {
 public:
  explicit Authority(std::string master_key_hex);
  // Issues a token bound to `subject`, valid for `ttl_seconds`.
  Token issue(const std::string& subject, uint64_t rights, const std::vector<std::string>& scopes,
              int64_t now, int64_t ttl_seconds = 3600, uint32_t generation = 0,
              const std::string& family_id = "");
  Token derive(const Token& parent, const std::string& child_subject, uint64_t child_rights,
               const std::vector<std::string>& child_scopes, int64_t now, int64_t ttl_seconds = 1800);

  bool revoke_family(const std::string& family_id, const std::string& reason = "");
  bool is_family_revoked(const std::string& family_id, uint32_t token_gen) const;

  bool verify(const Token& t, int64_t now, std::string* error = nullptr) const;
  // Short-lived token handed to a sandboxed app for exactly one request.
  std::string bearer(const Token& t) const;      // base64-url of the canonical form + mac
  bool from_bearer(const std::string& bearer, Token& out, int64_t now, std::string* error = nullptr) const;

 private:
  std::vector<uint8_t> key_;
  std::unordered_map<std::string, uint32_t> family_epochs_;
  std::unordered_map<std::string, std::string> family_revoke_reasons_;
};

// --------------------------------------------------------------- actions --
struct Action {
  // kind: "fs.read" | "fs.write" | "net.connect" | "net.listen" | "spawn" |
  //       "gpu" | "input" | "camera" | "mic" | "location" | "power" | "vault" |
  //       "graph" | "model" | "admin"
  std::string kind;
  std::string target;  // path or host[:port]; empty for boolean rights
};

uint64_t right_for_kind(const std::string& kind);

struct Decision {
  bool allowed = false;
  Tier tier = Tier::Native;
  std::string reason;
  uint64_t matched_right = 0;
};

// ---------------------------------------------------------------- policy --
struct Policy {
  int version = 1;
  std::string app_id;
  Tier tier = Tier::Isolated;
  uint64_t granted = 0;
  std::vector<std::string> scopes;      // allowed path prefixes / domains
  std::vector<std::string> deny;        // explicit denies win over grants
  bool allow_child_processes = false;   // needed for kRightSpawn
  bool persistent_storage = false;
  bool allow_east_west = false;         // east-west traffic between sandboxes (default deny)
  bool allow_direct_dns = false;        // direct port 53 DNS (default deny, internal broker only)
  static Policy from_json(const json::Value& v, std::string* error = nullptr);
  json::Value to_json() const;
};

struct AuditEntry {
  int64_t when = 0;
  std::string subject;
  Action action;
  Decision decision;
};

class Enforcer {
 public:
  // Snapshot the sandbox decision for one action; always audited.
  Decision check(const Policy& policy, const Token* token, const Action& action,
                 int64_t now, const Authority* auth = nullptr) const;
  // Escalation attempt simulation: can `app` move from Isolated to MicroVm, etc.
  static bool tier_allows(Tier tier, const std::string& action_kind);

  // Ghi thẳng một dòng audit (guard dùng cho quyết định tiêu diệt/gỡ bỏ/để yên).
  void record(const AuditEntry& e) const { audit_.push_back(e); }

  // Anomaly detector (Lớp 1 - P3.8): phát hiện hành vi vượt ranh giới hoặc syscall trái phép
  // tự động thu hồi cả họ token O(1) qua authority, ghi audit log kèm lý do tiếng Việt.
  bool detect_anomaly(Authority& auth, const std::string& family_id,
                      const std::string& subject, const std::string& anomaly_type,
                      const std::string& detail, int64_t now = 0) const;

  const std::vector<AuditEntry>& audit() const { return audit_; }
  void clear_audit() { audit_.clear(); }

 private:
  mutable std::vector<AuditEntry> audit_;
};

// Scope matching: "fs" scopes are path prefixes with an optional ":ro"/":rw"
// suffix; "net" scopes are host suffixes with an optional ":port".  Wildcards
// ("*") are allowed only as a full scope.  Exposed for testing.
bool scope_covers(const std::string& scope, const Action& action);

// -------------------------------------------------------- egress filter ---
// Bộ lọc egress theo năng lực (docs/SECURITY.md §11.2): tiến trình ở chế độ
// `isolated`/`microvm` KHÔNG được gửi gói tin ra ngoài danh sách cho phép
// trong `policies/*.json`. MẶC ĐỊNH CHẶN — chỉ rule nạp từ chính sách mới
// mở đường; mỗi quyết định đều có lý do tiếng Việt để audit.
struct EgressRequest {
  std::string domain;         // có thể rỗng nếu chỉ có IP
  std::string ip;             // IPv4 "a.b.c.d" (hoặc IPv6 dạng chữ) — có thể rỗng
  uint16_t port = 0;
  bool is_east_west = false;  // kết nối ngang giữa các hộp cát
  bool is_dns_query = false;  // yêu cầu DNS trực tiếp (cổng 53)
};

struct EgressDecision {
  bool allowed = false;
  std::string reason_vi;
  std::string matched_rule;  // rule khớp (khi allowed)
};

class EgressFilter {
 public:
  // Thêm rule cho phép: domain ("api.trithuc.vn" hoặc "*.trithuc.vn", có thể
  // kèm ":443"), hoặc CIDR IPv4 ("10.0.0.0/8", có thể kèm ":53").
  void allow_domain(const std::string& pattern_with_optional_port);
  void allow_cidr(const std::string& cidr_with_optional_port);
  // Nạp mọi scope "net:..." của một chính sách vào bộ lọc.
  void add_from_policy(const Policy& policy);

  EgressDecision check(const EgressRequest& r) const;
  size_t rule_count() const { return rules_.size(); }

  // "10.0.0.5" ∈ "10.0.0.0/8" — so khớp IPv4/CIDR, /32 khi thiếu prefix.
  static bool ipv4_in_cidr(const std::string& ip, const std::string& cidr);

  // Tính Shannon entropy (bit/ký tự) để nhận diện payload DNS tunneling (P3.10)
  static double shannon_entropy(const std::string& s);

  // Kiểm tra tên miền theo chuẩn RFC 1035 và chống DNS tunneling:
  // Tên miền <= 253 octet, mỗi nhãn 1..63 octet, ký tự hợp lệ, entropy nhãn <= max_entropy
  static bool validate_dns_name(const std::string& domain, std::string* error_vi = nullptr,
                                double max_entropy = 4.2);

  void set_allow_direct_dns(bool allow) { allow_direct_dns_ = allow; }
  bool allow_direct_dns() const { return allow_direct_dns_; }

  void set_allow_east_west(bool allow) { allow_east_west_ = allow; }
  bool allow_east_west() const { return allow_east_west_; }

  void set_max_dns_label_entropy(double max_h) { max_dns_label_entropy_ = max_h; }
  double max_dns_label_entropy() const { return max_dns_label_entropy_; }

 private:
  struct Rule {
    std::string pattern;
    uint16_t port = 0;  // 0 = mọi cổng
    bool is_cidr = false;
  };
  std::vector<Rule> rules_;       // allowlist (mặc định không có gì = chặn)
  std::vector<Rule> deny_rules_;  // deny thắng allow (SECURITY.md)
  bool allow_direct_dns_ = false; // Mặc định cấm DNS trực tiếp cổng 53 (chỉ broker nội bộ)
  bool allow_east_west_ = false;  // Mặc định deny giao tiếp ngang east-west giữa các sandbox
  double max_dns_label_entropy_ = 4.2;
};

// ------------------------------------------------ Secretless Broker (P3.10) --
// Broker giữ bí mật ở vòng ngoài, gắn xác thực sau khi duyệt request
// và không đưa token/secret vào guest, môi trường hay audit.
struct BrokeredRequest {
  std::string service_id;
  std::string target_host;
  uint16_t target_port = 443;
  std::string path;
  std::string payload;
};

struct BrokeredResponse {
  bool success = false;
  std::string data;
  std::string error_vi;
  bool secret_attached = false;
};

class SecretlessBroker {
 public:
  explicit SecretlessBroker(Enforcer* enforcer = nullptr, const Authority* auth = nullptr);

  // Đăng ký bí mật ngoài biên guest (guest không đọc được khoá này)
  void register_service_secret(const std::string& service_id, const std::string& secret);

  // Guest gửi yêu cầu qua broker: broker kiểm tra thẻ năng lực, kiểm tra egress allowlist,
  // tự động gắn bí mật vào header ở biên ngoài và chuyển tiếp.
  BrokeredResponse forward_request(const std::string& app_id, const Token* guest_token,
                                   const EgressFilter& egress, const BrokeredRequest& req,
                                   int64_t now = 0);

  bool has_service_secret(const std::string& service_id) const;

 private:
  Enforcer* enforcer_;
  const Authority* auth_;
  std::unordered_map<std::string, std::string> secrets_;
};

// -------------------------------------------------- app signature gate (P3.12) --
// Chính sách mặc định cho ứng dụng không có chữ ký hợp lệ.
// Dựa trên Ed25519 của P2.4 (Keyring).
enum class AppSignatureStatus {
  ValidTrusted,       // Ký đúng bởi khoá chủ quyền trong keyring
  InvalidSignature,   // Chữ ký sai hoặc nội dung bị can thiệp
  UntrustedKey,       // Ký bằng khoá lạ không nằm trong keyring
  Unsigned,           // Không có chữ ký
};

struct AppInstallPackage {
  std::string app_id;
  std::string manifest_canonical;
  std::string sig_hex;
  std::string signer_key_id;
  Policy declared_policy;
  bool user_confirmed = false;  // Người dùng xác nhận cài qua hộp thoại tin cậy
};

struct AppInstallDecision {
  bool accepted = false;
  AppSignatureStatus sig_status = AppSignatureStatus::Unsigned;
  Policy enforced_policy;
  bool requires_user_confirmation = false;
  std::string reason_vi;
};

class AppSignatureGate {
 public:
  explicit AppSignatureGate(const bundle::Keyring& keyring);

  // Thẩm định yêu cầu cài đặt và áp dụng chính sách hộp cát mặc định
  AppInstallDecision evaluate(const AppInstallPackage& pkg) const;

  // Xác minh chữ ký Ed25519 của manifest
  static AppSignatureStatus verify_signature(
      const std::string& manifest_canonical,
      const std::string& sig_hex,
      const std::string& signer_key_id,
      const bundle::Keyring& keyring);

 private:
  const bundle::Keyring& keyring_;
};

// --------------------------------------------------- third-party IME sandbox (P3.13) --
enum class InputFieldKind {
  NormalText = 0,    // Văn bản thông thường
  Password,          // Mật khẩu
  Pin,               // Mã PIN
  DigitsOnly,        // Số thẻ / OTP
  Url,               // Địa chỉ web
  Email,             // Thư điện tử
};

inline bool is_sensitive_input_field(InputFieldKind kind) {
  return kind != InputFieldKind::NormalText;
}

const char* input_field_kind_name_vi(InputFieldKind kind);

// Giao diện Engine IME bên thứ ba
class IThirdPartyImeEngine {
 public:
  virtual ~IThirdPartyImeEngine() = default;
  virtual const std::string& app_id() const = 0;
  virtual std::string compose_key(uint32_t codepoint) = 0;
  virtual void reset() = 0;
  virtual size_t keystrokes_received() const = 0;
};

// Host hộp cát quản lý tương tác và bảo vệ bàn phím cho IME bên thứ ba
class ThirdPartyImeHost {
 public:
  ThirdPartyImeHost(IThirdPartyImeEngine* engine, Policy policy, Enforcer* enforcer = nullptr);

  // Tạo chính sách chuẩn cho IME bên thứ ba: chỉ input.compose, không net, chỉ fs riêng
  static Policy create_default_policy(const std::string& app_id, const std::string& data_dir);

  // Xử lý một phím gõ. Nếu là trường nhạy cảm (Password/PIN/...),
  // engine ngoài nhận 0 phím, ghi audit log và trả về rỗng.
  std::string process_keystroke(uint32_t codepoint, InputFieldKind field_kind, int64_t now = 0);

  // Kiểm tra hành động do engine cố thực hiện (mạng, đọc file...)
  Decision check_engine_action(const Action& action, int64_t now = 0);

  size_t suppressed_keystrokes_count() const { return suppressed_sensitive_keys_; }
  const Policy& policy() const { return policy_; }

 private:
  IThirdPartyImeEngine* engine_;
  Policy policy_;
  Enforcer* enforcer_;
  size_t suppressed_sensitive_keys_ = 0;
};

// ------------------------------------------------ Micro-VM Lifecycle (P3.9) --
enum class MicroVmState {
  Running,     // Đang hoạt động bình thường
  Quarantined, // Đang bị cách ly / đóng băng (Frozen)
  Destroyed    // Đã bị tiêu huỷ hoàn toàn
};

const char* micro_vm_state_name(MicroVmState s);
const char* micro_vm_state_name_vi(MicroVmState s);

class MicroVmSession {
 public:
  MicroVmSession(std::string vm_id, std::string app_id, Policy policy,
                 std::string family_id = "", int64_t created_at = 0);

  const std::string& vm_id() const { return vm_id_; }
  const std::string& app_id() const { return app_id_; }
  const std::string& family_id() const { return family_id_; }
  const Policy& policy() const { return policy_; }
  MicroVmState state() const { return state_; }
  const std::string& freeze_reason() const { return freeze_reason_; }
  const std::string& destroy_reason() const { return destroy_reason_; }
  int64_t created_at() const { return created_at_; }
  int64_t frozen_at() const { return frozen_at_; }
  int64_t destroyed_at() const { return destroyed_at_; }

  // Idempotent state transitions (Running -> Quarantined/Frozen -> Destroyed)
  bool freeze(const std::string& reason, int64_t now = 0, Enforcer* enforcer = nullptr);
  bool destroy(const std::string& reason, int64_t now = 0, Enforcer* enforcer = nullptr);
  bool quarantine_and_destroy(const std::string& reason, int64_t now = 0, Enforcer* enforcer = nullptr);

  // Guest request gateway: rejects immediately if Quarantined or Destroyed
  Decision handle_guest_request(const Action& action, int64_t now, Enforcer* enforcer,
                                const Authority* auth = nullptr, const Token* token = nullptr);

 private:
  std::string vm_id_;
  std::string app_id_;
  Policy policy_;
  std::string family_id_;
  MicroVmState state_ = MicroVmState::Running;
  std::string freeze_reason_;
  std::string destroy_reason_;
  int64_t created_at_ = 0;
  int64_t frozen_at_ = 0;
  int64_t destroyed_at_ = 0;
};

class MicroVmSupervisor {
 public:
  explicit MicroVmSupervisor(Enforcer* enforcer = nullptr, Authority* auth = nullptr);

  MicroVmSession* create_session(const std::string& vm_id, const std::string& app_id,
                                 Policy policy, const std::string& family_id = "", int64_t now = 0);
  MicroVmSession* find_session(const std::string& vm_id);
  const MicroVmSession* find_session(const std::string& vm_id) const;

  bool freeze_session(const std::string& vm_id, const std::string& reason, int64_t now = 0);
  bool destroy_session(const std::string& vm_id, const std::string& reason, int64_t now = 0);

  // When Layer 1 flags an anomaly, quarantine_and_destroy the corresponding session(s)
  bool on_layer1_anomaly(const std::string& family_id_or_app, const std::string& reason, int64_t now = 0);

  Decision dispatch_guest_request(const std::string& vm_id, const Action& action,
                                  int64_t now, const Token* token = nullptr);

  size_t active_count() const;
  size_t total_count() const { return sessions_.size(); }

 private:
  Enforcer* enforcer_;
  Authority* auth_;
  std::vector<std::unique_ptr<MicroVmSession>> sessions_;
};

// ------------------------------------ Trusted Confirmation Path (P4.13) --
//
// Nhãn nguồn sự kiện nhập (P4.13, mở rộng P2.21). Thứ tự và giá trị số phải
// khớp bản Rust `kernel-cap/src/input.rs` (scripts/verify.py kiểm).
// QUY TẮC CỨNG: chỉ `PhysicalHardware` (driver gốc của host gắn thẻ: USB,
// PS/2, cảm ứng; `hardware_device_id` ≠ 0) mới tính là xác nhận tin cậy.
enum class InputSourceKind {
  PhysicalHardware = 0,  // Thiết bị vật lý do nhân gắn thẻ (bàn phím/chuột phần cứng)
  SyntheticApp = 1,      // Sự kiện tổng hợp từ ứng dụng / guest / background injection
  RemoteAuthorized = 2,  // Phiên từ xa đã được cấp thẻ (yui-remote P6.43, yui-kvm-share P6.42)
  GuestBluetooth = 3     // Bàn phím/chuột BT đi qua ngăn xếp trong khách (P6.46) — không phải vật lý
};

// Mức quyền một sự kiện nhập mang theo.
enum class InputPrivilege {
  None,                 // không được đưa vào ứng dụng khác
  AppControl,           // điều khiển ứng dụng thường (gõ, bấm trong cửa sổ app)
  TrustedConfirmation   // thêm: bấm hộp thoại xác nhận tin cậy, cấp thẻ giữ tỉnh,
                        // mở cổng, cài khoá ký…
};

struct TaggedInputEvent {
  InputSourceKind source = InputSourceKind::SyntheticApp;
  uint64_t hardware_device_id = 0;  // > 0 nếu là thiết bị phần cứng thật
  uint64_t remote_session_id = 0;   // > 0 nếu là phiên từ xa đã cấp thẻ (RemoteAuthorized)
  int x = 0;
  int y = 0;
  bool is_button_down = false;
  uint32_t keycode = 0;
};

const char* input_source_name(InputSourceKind k);
// Mức quyền của sự kiện: PhysicalHardware (id ≠ 0) ⇒ TrustedConfirmation;
// RemoteAuthorized (phiên ≠ 0) và GuestBluetooth ⇒ AppControl; còn lại None.
InputPrivilege input_privilege(const TaggedInputEvent& e);
inline bool counts_as_trusted_confirmation(const TaggedInputEvent& e) {
  return input_privilege(e) == InputPrivilege::TrustedConfirmation;
}
inline bool may_drive_application(const TaggedInputEvent& e) {
  return input_privilege(e) != InputPrivilege::None;
}

struct ConfirmationDialog {
  std::string dialog_id;
  std::string target_app;
  std::string title_vi;
  std::string prompt_vi;
  bool active = false;
  bool confirmed = false;
  bool cancelled = false;
  int64_t created_at = 0;

  int confirm_x1 = 100, confirm_y1 = 200, confirm_x2 = 220, confirm_y2 = 240;
  int cancel_x1 = 240, cancel_y1 = 200, cancel_x2 = 360, cancel_y2 = 240;
};

class TrustedConfirmationHost {
 public:
  explicit TrustedConfirmationHost(Enforcer* enforcer = nullptr);

  std::string show_dialog(const std::string& target_app, const std::string& title_vi,
                          const std::string& prompt_vi, int64_t now = 0);
  bool handle_input(const std::string& dialog_id, const TaggedInputEvent& event, int64_t now = 0);

  bool is_confirmed(const std::string& dialog_id) const;
  bool is_active(const std::string& dialog_id) const;
  bool is_cancelled(const std::string& dialog_id) const;
  // Đích của hộp thoại (vd "store:org.example.app"); rỗng nếu không tồn tại.
  // Bên dùng xác nhận phải so đích để một cú bấm không bị dùng cho việc khác.
  std::string dialog_target(const std::string& dialog_id) const;

  // Mọi sự kiện không-vật-lý bị chặn (tổng hợp + từ xa + BT qua khách).
  size_t synthetic_attacks_blocked() const { return synthetic_attacks_blocked_; }
  // Riêng phần từ phiên từ xa / BT qua khách (đã nằm trong số trên).
  size_t remote_attempts_blocked() const { return remote_attempts_blocked_; }

 private:
  Enforcer* enforcer_;
  std::unordered_map<std::string, ConfirmationDialog> dialogs_;
  size_t synthetic_attacks_blocked_ = 0;
  size_t remote_attempts_blocked_ = 0;
  uint64_t next_dialog_id_ = 1;
};

// ------------------------------ App Compatibility & Sinkhole (P3.16) --
struct DnsResolutionResult {
  bool sinkholed = false;
  std::string ip;        // "0.0.0.0" khi bị sinkhole
  int rcode = 0;         // 3 cho NXDOMAIN, 0 cho NOERROR
  double latency_ms = 0.0;
  std::string reason_vi;
};

class DnsSinkhole {
 public:
  static DnsResolutionResult resolve_or_sinkhole(
      const std::string& domain,
      const EgressFilter& filter,
      int64_t now = 0);
};

struct BlockObservation {
  int64_t timestamp = 0;
  std::string domain;
};

struct TemporaryGrant {
  std::string app_id;
  std::string domain;
  int64_t granted_at = 0;
  int64_t expires_at = 0;  // 0 khi until_app_close = true
  bool until_app_close = false;
  std::string family_id;
  uint32_t generation = 0;
  Token token;
};

class CompatibilityManager {
 public:
  CompatibilityManager(Enforcer* enforcer = nullptr, Authority* auth = nullptr,
                       TrustedConfirmationHost* trusted_host = nullptr);

  // (a) Bật/tắt chế độ tương thích cho từng app (mặc định TẮT)
  void set_app_compatibility(const std::string& app_id, bool enable);
  bool is_app_compatibility_enabled(const std::string& app_id) const;
  std::vector<std::string> enabled_apps() const;

  // (c) Quan sát hành vi bị chặn trên EgressFilter:
  // Nếu bị chặn >= N lần trong M giây (mặc định N=3 trong 10s):
  // Kiểm tra chống làm phiền: tối đa 1 lần hỏi / 5 phút (300s); nếu đã từ chối 3 lần -> không hỏi nữa.
  // Kích hoạt dialog xác nhận tin cậy (P4.13)
  bool record_egress_block(const std::string& app_id, const std::string& domain,
                           int64_t now = 0, std::string* out_dialog_id = nullptr);

  // Cấp quyền tạm thời sau khi người dùng phê duyệt qua đường tin cậy (P4.13)
  // Thời lượng: mặc định 900s (15 phút), trần 28800s (8 giờ), hoặc until_app_close = true
  bool grant_temporary_capability(const std::string& app_id, const std::string& domain,
                                  int64_t duration_sec, bool until_app_close,
                                  const std::string& family_id, int64_t now = 0);

  // Người dùng từ chối cấp quyền (tăng bộ đếm từ chối của app)
  void record_user_denial(const std::string& app_id, const std::string& domain, int64_t now = 0);

  // Kiểm tra domain có đang được cấp quyền tạm thời hợp lệ hay không
  bool is_temporarily_allowed(const std::string& app_id, const std::string& domain, int64_t now = 0) const;

  // (d) Thu hồi O(1) khi hết hạn hoặc khi đóng app
  void check_expirations(int64_t now);
  void on_app_closed(const std::string& app_id, int64_t now = 0);

  // Các phương thức tiện ích ngữ nghĩa (Semantic helper methods)
  bool on_blocked_request(const std::string& app_id, const std::string& kind,
                          const std::string& target, int64_t now = 0);
  void record_denial(const std::string& app_id, int64_t now = 0);
  bool is_permanently_quiet(const std::string& app_id) const;
  TemporaryGrant grant_session_capability(const std::string& app_id, const std::string& kind,
                                          const std::string& target, int64_t duration_sec,
                                          int64_t now = 0, bool until_app_close = false);
  void revoke_app(const std::string& app_id, int64_t now = 0);

  size_t prompt_count() const { return prompt_count_; }
  size_t grant_count() const { return grant_count_; }
  size_t denial_count(const std::string& app_id) const;
  size_t active_grants_count() const { return active_grants_.size(); }

 private:
  Enforcer* enforcer_;
  Authority* auth_;
  TrustedConfirmationHost* trusted_host_;

  std::unordered_map<std::string, bool> compat_enabled_;
  std::unordered_map<std::string, std::vector<BlockObservation>> block_history_;
  std::unordered_map<std::string, int64_t> last_prompt_time_;
  std::unordered_map<std::string, size_t> user_denials_;
  std::vector<TemporaryGrant> active_grants_;

  size_t prompt_count_ = 0;
  size_t grant_count_ = 0;
};

}  // namespace yui::sandbox

#endif  // YUI_SANDBOX_HPP
