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
#include <string>
#include <vector>

#include "yui/json.hpp"

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
  std::string mac_hex;              // HMAC-SHA-256 over the canonical form

  std::string canonical() const;  // deterministic serialisation that gets signed
};

class Authority {
 public:
  explicit Authority(std::string master_key_hex);
  // Issues a token bound to `subject`, valid for `ttl_seconds`.
  Token issue(const std::string& subject, uint64_t rights, const std::vector<std::string>& scopes,
              int64_t now, int64_t ttl_seconds = 3600);
  bool verify(const Token& t, int64_t now, std::string* error = nullptr) const;
  // Short-lived token handed to a sandboxed app for exactly one request.
  std::string bearer(const Token& t) const;      // base64-url of the canonical form + mac
  bool from_bearer(const std::string& bearer, Token& out, int64_t now, std::string* error = nullptr) const;

 private:
  std::vector<uint8_t> key_;
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
  std::string app_id;
  Tier tier = Tier::Isolated;
  uint64_t granted = 0;
  std::vector<std::string> scopes;      // allowed path prefixes / domains
  std::vector<std::string> deny;        // explicit denies win over grants
  bool allow_child_processes = false;   // needed for kRightSpawn
  bool persistent_storage = false;
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
  Decision check(const Policy& policy, const Token* token, const Action& action, int64_t now) const;
  // Escalation attempt simulation: can `app` move from Isolated to MicroVm, etc.
  static bool tier_allows(Tier tier, const std::string& action_kind);

  // Ghi thẳng một dòng audit (guard dùng cho quyết định tiêu diệt/gỡ bỏ/để yên).
  void record(const AuditEntry& e) const { audit_.push_back(e); }

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
  std::string domain;  // có thể rỗng nếu chỉ có IP
  std::string ip;      // IPv4 "a.b.c.d" (hoặc IPv6 dạng chữ) — có thể rỗng
  uint16_t port = 0;
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

 private:
  struct Rule {
    std::string pattern;
    uint16_t port = 0;  // 0 = mọi cổng
    bool is_cidr = false;
  };
  std::vector<Rule> rules_;       // allowlist (mặc định không có gì = chặn)
  std::vector<Rule> deny_rules_;  // deny thắng allow (SECURITY.md)
};

}  // namespace yui::sandbox

#endif  // YUI_SANDBOX_HPP
