// Capability tokens + three-tier sandbox enforcement.
#include "yui/sandbox.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <sstream>

#include "yui/crypto.hpp"
#include "yui/text.hpp"

namespace yui::sandbox {
namespace {
using crypto::Bytes;

const char* kRightNames[] = {
    "fs.read", "fs.write", "net.connect", "net.listen", "spawn", "gpu",
    "input",   "camera",    "mic",         "location",     "power", "vault",
    "graph",   "model",     "admin",       "compute.heavy",
};
const uint64_t kRightBits[] = {
    kRightFsRead, kRightFsWrite, kRightNetConnect, kRightNetListen, kRightSpawn, kRightGpu,
    kRightInput,  kRightCamera,  kRightMic,        kRightLocation,  kRightPower, kRightVault,
    kRightGraph,  kRightModel,   kRightAdmin,      kRightComputeHeavy,
};
constexpr size_t kRightCount = sizeof(kRightBits) / sizeof(kRightBits[0]);

std::string b64url_encode(const Bytes& in) {
  static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  std::string out;
  out.reserve((in.size() + 2) / 3 * 4);
  size_t i = 0;
  while (i + 3 <= in.size()) {
    const uint32_t v = (uint32_t(in[i]) << 16) | (uint32_t(in[i + 1]) << 8) | in[i + 2];
    out += tbl[(v >> 18) & 63];
    out += tbl[(v >> 12) & 63];
    out += tbl[(v >> 6) & 63];
    out += tbl[v & 63];
    i += 3;
  }
  if (i + 1 == in.size()) {
    const uint32_t v = uint32_t(in[i]) << 16;
    out += tbl[(v >> 18) & 63];
    out += tbl[(v >> 12) & 63];
  } else if (i + 2 == in.size()) {
    const uint32_t v = (uint32_t(in[i]) << 16) | (uint32_t(in[i + 1]) << 8);
    out += tbl[(v >> 18) & 63];
    out += tbl[(v >> 12) & 63];
    out += tbl[(v >> 6) & 63];
  }
  return out;
}

bool b64url_decode(const std::string& in, Bytes& out) {
  auto val = [](char c) -> int {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-') return 62;
    if (c == '_') return 63;
    return -1;
  };
  out.clear();
  uint32_t buf = 0;
  int bits = 0;
  for (char c : in) {
    const int v = val(c);
    if (v < 0) return false;
    buf = (buf << 6) | uint32_t(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(uint8_t((buf >> bits) & 0xff));
    }
  }
  return true;
}

bool path_within(const std::string& target, const std::string& prefix) {
  // Normalise obvious traversal attempts before comparing.
  std::string t = target;
  if (t.find("/../") != std::string::npos || t == ".." || yui::text::starts_with(t, "../"))
    return false;
  if (prefix == "/") return yui::text::starts_with(t, "/");
  if (t == prefix) return true;
  return t.size() > prefix.size() && t.compare(0, prefix.size(), prefix) == 0 && t[prefix.size()] == '/';
}

bool host_matches(const std::string& host, const std::string& pattern) {
  if (pattern == "*") return true;
  if (pattern.size() > 2 && pattern.compare(0, 2, "*.") == 0) {
    const std::string suffix = pattern.substr(1);  // ".example.com"
    return host.size() > suffix.size() && host.compare(host.size() - suffix.size(), suffix.size(), suffix) == 0;
  }
  return yui::text::fold(host) == yui::text::fold(pattern);
}

}  // namespace

const char* tier_name(Tier t) {
  switch (t) {
    case Tier::Native: return "native";
    case Tier::Isolated: return "isolated";
    case Tier::MicroVm: return "microvm";
  }
  return "isolated";
}

bool tier_from(const std::string& s, Tier& out) {
  if (s == "native" || s == "0") { out = Tier::Native; return true; }
  if (s == "isolated" || s == "1") { out = Tier::Isolated; return true; }
  if (s == "microvm" || s == "micro-vm" || s == "2") { out = Tier::MicroVm; return true; }
  return false;
}

std::string rights_to_string(uint64_t rights) {
  std::string out;
  for (size_t i = 0; i < kRightCount; ++i) {
    if (!(rights & kRightBits[i])) continue;
    if (!out.empty()) out += ',';
    out += kRightNames[i];
  }
  return out;
}

uint64_t rights_from_string(const std::string& s) {
  uint64_t mask = 0;
  for (const auto& part : yui::text::split(s, ',')) {
    const std::string name = yui::text::trim(part);
    if (name.empty()) continue;
    for (size_t i = 0; i < kRightCount; ++i)
      if (name == kRightNames[i]) mask |= kRightBits[i];
  }
  return mask;
}

uint64_t right_for_kind(const std::string& kind) {
  for (size_t i = 0; i < kRightCount; ++i)
    if (kind == kRightNames[i]) return kRightBits[i];
  if (kind == "fs.read") return kRightFsRead;
  return kRightNone;
}

// ---------------------------------------------------------------- tokens --
std::string Token::canonical() const {
  // Field order is frozen: changing it invalidates every issued token.
  json::Value v(json::Object{});
  v.set("sub", subject);
  v.set("iss", issuer);
  v.set("rights", rights_to_string(rights));
  v.set("iat", issued_at);
  v.set("exp", expires_at);
  v.set("serial", int64_t(serial));
  json::Value scopes_v(json::Array{});
  for (const auto& s : scopes) scopes_v.push(std::string(s));
  v.set("scopes", scopes_v);
  return v.dump();
}

Authority::Authority(std::string master_key_hex) { key_ = crypto::from_hex(master_key_hex); }

Token Authority::issue(const std::string& subject, uint64_t rights, const std::vector<std::string>& scopes,
                       int64_t now, int64_t ttl_seconds) {
  Token t;
  t.subject = subject;
  t.issuer = "yui-cap";
  t.rights = rights;
  t.issued_at = now;
  t.expires_at = ttl_seconds > 0 ? now + ttl_seconds : 0;
  const Bytes serial_bytes = crypto::random_bytes(4);
  t.serial = (uint32_t(serial_bytes[0]) << 24) | (uint32_t(serial_bytes[1]) << 16) |
             (uint32_t(serial_bytes[2]) << 8) | uint32_t(serial_bytes[3]);
  t.scopes = scopes;
  crypto::HmacSha256 h(key_);
  const std::string canon = t.canonical();
  h.update(canon.data(), canon.size());
  t.mac_hex = crypto::hex(h.final().data(), 32);
  return t;
}

bool Authority::verify(const Token& t, int64_t now, std::string* error) const {
  auto fail = [&](const char* m) {
    if (error) *error = m;
    return false;
  };
  if (t.issuer != "yui-cap") return fail("unknown issuer");
  if (t.expires_at != 0 && now > t.expires_at) return fail("token expired");
  if (t.issued_at > now + 300) return fail("token issued in the future");
  if (t.mac_hex.size() != 64) return fail("malformed MAC");
  const Bytes mac = crypto::from_hex(t.mac_hex);
  if (mac.size() != 32) return fail("malformed MAC");
  crypto::HmacSha256 h(key_);
  const std::string canon = t.canonical();
  h.update(canon.data(), canon.size());
  const auto expect = h.final();
  if (!crypto::ct_equal(expect.data(), mac.data(), 32)) return fail("bad MAC");
  return true;
}

std::string Authority::bearer(const Token& t) const {
  const std::string canon = t.canonical();
  const Bytes body(canon.begin(), canon.end());
  return b64url_encode(body) + "." + t.mac_hex;
}

bool Authority::from_bearer(const std::string& bearer, Token& out, int64_t now, std::string* error) const {
  auto fail = [&](const char* m) {
    if (error) *error = m;
    return false;
  };
  const size_t dot = bearer.find('.');
  if (dot == std::string::npos) return fail("malformed bearer");
  Bytes body;
  if (!b64url_decode(bearer.substr(0, dot), body)) return fail("malformed bearer body");
  const std::string canon(body.begin(), body.end());
  auto parsed = json::parse(canon);
  if (!parsed.ok || !parsed.value.is_object()) return fail("malformed token payload");
  const auto& v = parsed.value;
  out.subject = v["sub"].as_string();
  out.issuer = v["iss"].as_string();
  out.rights = rights_from_string(v["rights"].as_string());
  out.issued_at = v["iat"].as_int();
  out.expires_at = v["exp"].as_int();
  out.serial = uint32_t(v["serial"].as_int());
  out.scopes.clear();
  for (const auto& s : v["scopes"].arr()) out.scopes.push_back(s.as_string());
  out.mac_hex = bearer.substr(dot + 1);
  // Re-canonicalise so a reordered or padded payload cannot smuggle changes in.
  if (out.canonical() != canon) return fail("non-canonical token payload");
  return verify(out, now, error);
}

// --------------------------------------------------------------- actions --
bool scope_covers(const std::string& scope, const Action& action) {
  if (scope == "*") return true;
  // A scope that names a right (e.g. "fs.read", "input") matches exactly that
  // kind — this is how deny lists switch off a whole capability.
  if (right_for_kind(scope) != kRightNone) return scope == action.kind;
  if (yui::text::starts_with(action.kind, "fs.")) {
    std::string prefix = scope;
    bool readonly = false;
    if (yui::text::ends_with(scope, ":ro")) {
      prefix = scope.substr(0, scope.size() - 3);
      readonly = true;
    } else if (yui::text::ends_with(scope, ":rw")) {
      prefix = scope.substr(0, scope.size() - 3);
    }
    if (action.kind == "fs.write" && readonly) return false;
    return path_within(action.target, prefix);
  }
  if (yui::text::starts_with(action.kind, "net.")) {
    std::string host = action.target;
    std::string port;
    const size_t colon = host.rfind(':');
    if (colon != std::string::npos) {
      port = host.substr(colon + 1);
      host = host.substr(0, colon);
    }
    std::string pattern = scope;
    std::string scope_port;
    const size_t scolon = pattern.rfind(':');
    if (scolon != std::string::npos && pattern.find('.') != std::string::npos) {
      scope_port = pattern.substr(scolon + 1);
      pattern = pattern.substr(0, scolon);
    }
    if (!scope_port.empty() && !port.empty() && scope_port != port) return false;
    return host_matches(host, pattern);
  }
  // Boolean rights: a scope equal to the action kind enables it.
  return scope == action.kind || scope == "all";
}

bool Enforcer::tier_allows(Tier tier, const std::string& kind) {
  // The micro-VM has no host syscalls at all: everything goes through the
  // virtio channel, so only brokered high-level services are reachable.
  if (tier == Tier::MicroVm) {
    // No host syscalls at all: only brokered services and a passed-through GPU.
    return kind == "vault" || kind == "graph" || kind == "model" || kind == "net.connect" ||
           kind == "power" || kind == "gpu";
  }
  if (tier == Tier::Isolated) {
    // Legacy apps never get raw input devices or admin rights.
    return kind != "input" && kind != "admin";
  }
  return true;
}

Decision Enforcer::check(const Policy& policy, const Token* token, const Action& action, int64_t now) const {
  Decision d;
  d.tier = policy.tier;

  auto deny = [&](const std::string& why) {
    Decision r;
    r.allowed = false;
    r.tier = policy.tier;
    r.reason = why;
    audit_.push_back(AuditEntry{now, policy.app_id, action, r});
    return r;
  };

  const uint64_t needed = right_for_kind(action.kind);
  if (needed == kRightNone) return deny("hành động không xác định: " + action.kind);

  if (!tier_allows(policy.tier, action.kind))
    return deny(std::string("tầng cách ly ") + tier_name(policy.tier) + " không cho phép " + action.kind);

  if ((policy.granted & needed) == 0) return deny("chính sách không cấp quyền " + action.kind);

  // Explicit denies always win, and are matched with the same scope rules.
  for (const auto& scope : policy.deny)
    if (scope_covers(scope, action)) return deny("bị chặn bởi quy tắc từ chối: " + scope);

  // Scopes only constrain resource-shaped actions (files and hosts). Boolean
  // rights such as "vault" or "model" are governed by the grant itself.
  const bool scoped_action = yui::text::starts_with(action.kind, "fs.") ||
                             yui::text::starts_with(action.kind, "net.");
  bool covered = !scoped_action || policy.scopes.empty();
  std::string matched;
  if (scoped_action) {
    for (const auto& scope : policy.scopes) {
      if (scope_covers(scope, action)) {
        covered = true;
        matched = scope;
        break;
      }
    }
  }
  if (!covered) return deny("phạm vi không bao phủ: " + action.kind + " " + action.target);

  if (token) {
    if (token->expires_at != 0 && now > token->expires_at)
      return deny("thẻ năng lực đã hết hạn");
    if ((token->rights & needed) == 0)
      return deny("thẻ năng lực thiếu quyền " + action.kind);
    bool token_scope_ok = token->scopes.empty();
    for (const auto& scope : token->scopes)
      if (scope_covers(scope, action)) token_scope_ok = true;
    if (!token_scope_ok) return deny("thẻ năng lực không bao phủ " + action.target);
  }

  Decision ok;
  ok.allowed = true;
  ok.tier = policy.tier;
  ok.matched_right = needed;
  ok.reason = matched.empty() ? std::string("được cấp quyền ") + action.kind
                              : std::string("khớp phạm vi: ") + matched;
  audit_.push_back(AuditEntry{now, policy.app_id, action, ok});
  return ok;
}

// ---------------------------------------------------------------- policy --
Policy Policy::from_json(const json::Value& v, std::string* error) {
  Policy p;
  auto fail = [&](const char* m) {
    if (error) *error = m;
    return Policy{};
  };
  if (!v.is_object()) return fail("policy must be an object");
  p.app_id = v["app"].as_string();
  if (p.app_id.empty()) return fail("policy requires an \"app\" id");
  if (!tier_from(v["tier"].as_string("isolated"), p.tier)) return fail("unknown tier");

  uint64_t granted = 0;
  // Capability strings come in three shapes:
  //   "net.connect"                 -> bare right, implicit wildcard scope
  //   "fs:/home/user/Documents:rw"  -> right + path scope
  //   "net:*.yui.vn"                -> right + host scope
  //   "*"                           -> everything (micro-VM test fixtures only)
  for (const auto& c : v["capabilities"].arr()) {
    const std::string s = c.as_string();
    if (s == "*") {
      for (size_t i = 0; i < kRightCount; ++i) granted |= kRightBits[i];
      p.scopes.push_back("*");
      continue;
    }
    const size_t colon = s.find(':');
    if (colon == std::string::npos) {
      const uint64_t bits = rights_from_string(s);
      if (bits == 0) return fail(("unknown capability: " + s).c_str());
      // A bare right grants the capability but adds no scope: scopes are the
      // allow-list, and a policy with no scopes at all is a blanket grant.
      granted |= bits;
      continue;
    }
    const std::string head = s.substr(0, colon);
    const std::string scope = s.substr(colon + 1);
    if (head == "fs") {
      granted |= kRightFsRead | kRightFsWrite;
    } else if (head == "net") {
      granted |= kRightNetConnect;
    } else {
      const uint64_t bits = rights_from_string(head);
      if (bits == 0) return fail(("unknown capability: " + s).c_str());
      granted |= bits;
    }
    p.scopes.push_back(scope);
  }
  for (const auto& s : v["scopes"].arr()) p.scopes.push_back(s.as_string());
  for (const auto& s : v["deny"].arr()) p.deny.push_back(s.as_string());
  p.granted = granted;
  p.allow_child_processes = v["allow_child_processes"].as_bool(false);
  p.persistent_storage = v["persistent_storage"].as_bool(false);
  if (!p.allow_child_processes) p.granted &= ~kRightSpawn;
  return p;
}

json::Value Policy::to_json() const {
  json::Value v(json::Object{});
  v.set("app", app_id);
  v.set("tier", tier_name(tier));
  json::Value caps(json::Array{});
  for (const auto& n : yui::text::split(rights_to_string(granted), ','))
    if (!n.empty()) caps.push(n);
  v.set("capabilities", caps);
  json::Value scopes_v(json::Array{});
  for (const auto& s : scopes) scopes_v.push(std::string(s));
  v.set("scopes", scopes_v);
  json::Value deny_v(json::Array{});
  for (const auto& s : deny) deny_v.push(std::string(s));
  v.set("deny", deny_v);
  v.set("allow_child_processes", allow_child_processes);
  v.set("persistent_storage", persistent_storage);
  return v;
}

// ---------------------------------------------------------- egress filter --
void EgressFilter::allow_domain(const std::string& s) {
  Rule r;
  std::string pat = s;
  const size_t col = s.rfind(':');
  if (col != std::string::npos && col + 1 < s.size() &&
      s.find_first_not_of("0123456789", col + 1) == std::string::npos) {
    r.port = uint16_t(std::strtoul(s.c_str() + col + 1, nullptr, 10));
    pat = s.substr(0, col);
  }
  r.pattern = pat;
  rules_.push_back(r);
}

void EgressFilter::allow_cidr(const std::string& s) {
  Rule r;
  r.is_cidr = true;
  std::string pat = s;
  // Cổng tùy chọn sau CIDR: "10.0.0.0/8:53" — cắt ở ':' cuối nếu phần sau
  // toàn chữ số (CIDR IPv4 không chứa ':').
  const size_t col = s.rfind(':');
  if (col != std::string::npos && col + 1 < s.size() &&
      s.find_first_not_of("0123456789", col + 1) == std::string::npos) {
    r.port = uint16_t(std::strtoul(s.c_str() + col + 1, nullptr, 10));
    pat = s.substr(0, col);
  }
  r.pattern = pat;
  rules_.push_back(r);
}

void EgressFilter::add_from_policy(const Policy& policy) {
  // Scope trong Policy đã bị Policy::from_json cắt tiền tố ("net:"/"fs:"),
  // nhưng test/chính sách viết tay có thể giữ nguyên "net:" — chấp nhận cả
  // hai. Đường dẫn ("/...") là phạm vi fs: không phải egress.
  auto is_net_spec = [](const std::string& s, std::string* spec) {
    if (s.rfind("net:", 0) == 0) {
      *spec = s.substr(4);
      return !spec->empty();
    }
    if (s.rfind("fs:", 0) == 0 || (!s.empty() && (s[0] == '/' || s[0] == '~'))) return false;
    *spec = s;
    return !s.empty();
  };
  for (const auto& scope : policy.scopes) {
    std::string spec;
    if (!is_net_spec(scope, &spec)) continue;
    // "1.2.3.0/24[:port]" ⇒ CIDR; còn lại là domain (có thể "*.suffix[:port]").
    if (spec.size() > 3 && spec.find('/') != std::string::npos &&
        std::isdigit(static_cast<unsigned char>(spec[0])))
      allow_cidr(spec);
    else
      allow_domain(spec);
  }
  // deny của chính sách: giữ nguyên tắc "deny thắng grant" — đích nằm trong
  // deny bị phủ quyết ngay cả khi có allow khớp.
  for (const auto& d : policy.deny) {
    std::string spec;
    if (!is_net_spec(d, &spec)) continue;
    Rule r;
    const size_t col = spec.rfind(':');
    if (col != std::string::npos && col + 1 < spec.size() &&
        spec.find_first_not_of("0123456789", col + 1) == std::string::npos) {
      r.port = uint16_t(std::strtoul(spec.c_str() + col + 1, nullptr, 10));
      spec = spec.substr(0, col);
    }
    r.pattern = spec;
    r.is_cidr =
        spec.find('/') != std::string::npos && std::isdigit(static_cast<unsigned char>(spec[0]));
    deny_rules_.push_back(r);
  }
}

bool EgressFilter::ipv4_in_cidr(const std::string& ip, const std::string& cidr) {
  auto parse_v4 = [](const std::string& s, uint32_t* out) -> bool {
    unsigned a[4];
    char dot;
    std::istringstream ss(s);
    if (!(ss >> a[0] >> dot >> a[1] >> dot >> a[2] >> dot >> a[3])) return false;
    if (a[0] > 255 || a[1] > 255 || a[2] > 255 || a[3] > 255) return false;
    std::string extra;
    if (ss >> extra) return false;
    *out = (uint32_t(a[0]) << 24) | (uint32_t(a[1]) << 16) | (uint32_t(a[2]) << 8) | a[3];
    return true;
  };
  uint32_t addr = 0, base = 0;
  if (!parse_v4(ip, &addr)) return false;
  std::string base_s = cidr;
  unsigned prefix = 32;
  const size_t slash = cidr.find('/');
  if (slash != std::string::npos) {
    base_s = cidr.substr(0, slash);
    prefix = unsigned(std::strtoul(cidr.c_str() + slash + 1, nullptr, 10));
    if (prefix > 32) return false;
  }
  if (!parse_v4(base_s, &base)) return false;
  const uint32_t mask = prefix == 0 ? 0 : (0xFFFFFFFFu << (32 - prefix));
  return (addr & mask) == (base & mask);
}

EgressDecision EgressFilter::check(const EgressRequest& r) const {
  auto match_rule = [&](const Rule& rule) -> bool {
    if (rule.port != 0 && r.port != 0 && rule.port != r.port) return false;
    if (rule.is_cidr) return !r.ip.empty() && ipv4_in_cidr(r.ip, rule.pattern);
    const std::string host = r.domain.empty() ? r.ip : r.domain;
    return !host.empty() && host_matches(host, rule.pattern);
  };
  const std::string target = r.domain.empty() ? r.ip : r.domain;
  const std::string port_s = r.port ? ":" + std::to_string(r.port) : std::string();

  // Deny thắng allow (docs/SECURITY.md).
  for (const auto& rule : deny_rules_) {
    if (match_rule(rule)) {
      EgressDecision d;
      d.allowed = false;
      d.matched_rule = rule.pattern;
      d.reason_vi = "chặn kết nối tới " + (target.empty() ? std::string("<rỗng>") : target) +
                    port_s + " — đích nằm trong danh sách deny của chính sách (deny thắng allow)";
      return d;
    }
  }
  for (const auto& rule : rules_) {
    if (match_rule(rule)) {
      EgressDecision d;
      d.allowed = true;
      d.matched_rule =
          rule.pattern + (rule.port ? ":" + std::to_string(rule.port) : std::string());
      d.reason_vi = "cho phép kết nối tới " + (target.empty() ? std::string("<rỗng>") : target) +
                    port_s + " — khớp allowlist chính sách (net:" + d.matched_rule + ")";
      return d;
    }
  }
  EgressDecision d;
  d.allowed = false;
  d.reason_vi =
      "chặn kết nối tới " + (target.empty() ? std::string("<rỗng>") : target) + port_s +
      " — ngoài phạm vi cho phép của chính sách (cô lập egress, mặc định chặn)";
  return d;
}

}  // namespace yui::sandbox
