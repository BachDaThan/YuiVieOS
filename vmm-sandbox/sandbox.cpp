// Capability tokens + three-tier sandbox enforcement.
#include "yui/sandbox.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <sstream>

#include "yui/bundle.hpp"
#include "yui/crypto.hpp"
#include "yui/ed25519.hpp"
#include "yui/text.hpp"

namespace yui::sandbox {
namespace {
using crypto::Bytes;

const char* kRightNames[] = {
    "fs.read", "fs.write", "net.connect", "net.listen", "spawn", "gpu",
    "input",   "camera",    "mic",         "location",     "power", "vault",
    "graph",   "model",     "admin",       "compute.heavy", "bluetooth", "input.compose",
};
const uint64_t kRightBits[] = {
    kRightFsRead, kRightFsWrite, kRightNetConnect, kRightNetListen, kRightSpawn, kRightGpu,
    kRightInput,  kRightCamera,  kRightMic,        kRightLocation,  kRightPower, kRightVault,
    kRightGraph,  kRightModel,   kRightAdmin,      kRightComputeHeavy, kRightBluetooth, kRightInputCompose,
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
  if (generation > 0) v.set("gen", int64_t(generation));
  if (!family_id.empty()) v.set("fam", family_id);
  json::Value scopes_v(json::Array{});
  for (const auto& s : scopes) scopes_v.push(std::string(s));
  v.set("scopes", scopes_v);
  return v.dump();
}

Authority::Authority(std::string master_key_hex) { key_ = crypto::from_hex(master_key_hex); }

Token Authority::issue(const std::string& subject, uint64_t rights, const std::vector<std::string>& scopes,
                       int64_t now, int64_t ttl_seconds, uint32_t generation,
                       const std::string& family_id) {
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
  t.family_id = family_id.empty() ? subject : family_id;
  auto it = family_epochs_.find(t.family_id);
  t.generation = (it != family_epochs_.end() && it->second > generation) ? it->second : generation;
  crypto::HmacSha256 h(key_);
  const std::string canon = t.canonical();
  h.update(canon.data(), canon.size());
  t.mac_hex = crypto::hex(h.final().data(), 32);
  return t;
}

Token Authority::derive(const Token& parent, const std::string& child_subject, uint64_t child_rights,
                        const std::vector<std::string>& child_scopes, int64_t now, int64_t ttl_seconds) {
  uint64_t restricted_rights = child_rights & parent.rights;
  std::string fid = parent.family_id.empty() ? parent.subject : parent.family_id;
  int64_t exp = ttl_seconds > 0 ? now + ttl_seconds : parent.expires_at;
  if (parent.expires_at > 0 && (exp == 0 || exp > parent.expires_at)) exp = parent.expires_at;
  int64_t ttl = exp > now ? (exp - now) : 0;
  return issue(child_subject, restricted_rights, child_scopes, now, ttl, parent.generation, fid);
}

bool Authority::revoke_family(const std::string& family_id, const std::string& reason) {
  family_epochs_[family_id] = family_epochs_[family_id] + 1;
  if (!reason.empty()) {
    family_revoke_reasons_[family_id] = reason;
  }
  return true;
}

bool Authority::is_family_revoked(const std::string& family_id, uint32_t token_gen) const {
  auto it = family_epochs_.find(family_id);
  if (it == family_epochs_.end()) return false;
  return token_gen < it->second;
}

bool Authority::verify(const Token& t, int64_t now, std::string* error) const {
  auto fail = [&](const char* m) {
    if (error) *error = m;
    return false;
  };
  if (t.issuer != "yui-cap") return fail("unknown issuer");
  if (t.expires_at != 0 && now > t.expires_at) return fail("token expired");
  if (t.issued_at > now + 300) return fail("token issued in the future");

  // O(1) Transitive Revocation check by family epoch
  std::string fid = t.family_id.empty() ? t.subject : t.family_id;
  if (is_family_revoked(fid, t.generation)) {
    return fail("họ thẻ năng lực đã bị thu hồi (epoch mismatch)");
  }

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
  out.generation = v.has("gen") ? uint32_t(v["gen"].as_int()) : 0;
  out.family_id = v.has("fam") ? v["fam"].as_string() : "";
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

Decision Enforcer::check(const Policy& policy, const Token* token, const Action& action,
                         int64_t now, const Authority* auth) const {
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
    if (auth) {
      std::string auth_err;
      if (!auth->verify(*token, now, &auth_err)) {
        return deny(std::string("thẻ năng lực không hợp lệ: ") + auth_err);
      }
    }
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

bool Enforcer::detect_anomaly(Authority& auth, const std::string& family_id,
                              const std::string& subject, const std::string& anomaly_type,
                              const std::string& detail, int64_t now) const {
  if (!family_id.empty()) {
    auth.revoke_family(family_id);
  }
  AuditEntry e;
  e.when = now;
  e.subject = subject;
  e.action = Action{"anomaly." + anomaly_type, detail};
  e.decision = Decision{false, Tier::Isolated,
      "Phát hiện bất thường Lớp 1: " + anomaly_type + " (" + detail + ") — Tự động thu hồi họ token " + family_id, 0};
  audit_.push_back(e);
  return true;
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
  p.version = static_cast<int>(v["version"].as_int(1));
  p.allow_east_west = v["allow_east_west"].as_bool(false);
  p.allow_direct_dns = v["allow_direct_dns"].as_bool(false);
  if (!p.allow_child_processes) p.granted &= ~kRightSpawn;
  return p;
}

json::Value Policy::to_json() const {
  json::Value v(json::Object{});
  v.set("version", version);
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
  v.set("allow_east_west", allow_east_west);
  v.set("allow_direct_dns", allow_direct_dns);
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
  allow_east_west_ = policy.allow_east_west;
  allow_direct_dns_ = policy.allow_direct_dns;

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

double EgressFilter::shannon_entropy(const std::string& s) {
  if (s.empty()) return 0.0;
  std::unordered_map<char, size_t> counts;
  for (char c : s) {
    counts[static_cast<char>(std::tolower(static_cast<unsigned char>(c)))]++;
  }
  double entropy = 0.0;
  const double total = static_cast<double>(s.size());
  for (const auto& kv : counts) {
    const double p = static_cast<double>(kv.second) / total;
    if (p > 0.0) {
      entropy -= p * (std::log(p) / std::log(2.0));
    }
  }
  return entropy;
}

bool EgressFilter::validate_dns_name(const std::string& domain, std::string* error_vi, double max_entropy) {
  if (domain.empty()) return true;
  if (domain.size() > 253) {
    if (error_vi) {
      *error_vi = "từ chối tên miền: độ dài (" + std::to_string(domain.size()) +
                  " octet) vượt quá giới hạn 253 octet (RFC 1035 / chống DNS tunneling)";
    }
    return false;
  }

  std::string d = domain;
  if (!d.empty() && d.back() == '.') d.pop_back();

  size_t start = 0;
  while (start < d.size()) {
    size_t dot = d.find('.', start);
    if (dot == std::string::npos) dot = d.size();
    std::string label = d.substr(start, dot - start);
    start = dot + 1;

    if (label.empty()) {
      if (error_vi) *error_vi = "từ chối tên miền: nhãn DNS rỗng hoặc chứa hai dấu chấm liên tiếp";
      return false;
    }
    if (label.size() > 63) {
      if (error_vi) {
        *error_vi = "từ chối nhãn DNS: độ dài nhãn (" + label + ", " +
                    std::to_string(label.size()) + " octet) vượt quá 63 octet (chống DNS tunneling)";
      }
      return false;
    }

    if (label == "*") continue;

    if (label.front() == '-' || label.back() == '-') {
      if (error_vi) *error_vi = "từ chối nhãn DNS: nhãn không được bắt đầu hoặc kết thúc bằng gạch nối";
      return false;
    }

    for (char c : label) {
      if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-') {
        if (error_vi) *error_vi = "từ chối nhãn DNS: ký tự '" + std::string(1, c) + "' không hợp lệ theo chuẩn RFC 1035";
        return false;
      }
    }

    if (label.size() >= 12) {
      const double h = shannon_entropy(label);
      if (h > max_entropy) {
        if (error_vi) {
          std::ostringstream ss;
          ss << "từ chối tên miền: entropy nhãn (" << label << ") vượt ngưỡng an toàn (H = "
             << std::fixed << std::setprecision(2) << h << " > " << max_entropy
             << " bit/ký tự) — phát hiện nghi vấn DNS tunneling";
          *error_vi = ss.str();
        }
        return false;
      }
    }
  }
  return true;
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
  EgressDecision d;
  d.allowed = false;

  // 1. Kiểm tra kết nối ngang (east-west) giữa các hộp cát (mặc định deny)
  if (r.is_east_west && !allow_east_west_) {
    d.reason_vi = "chặn kết nối ngang (east-west) giữa các hộp cát: mặc định deny (zero-trust)";
    return d;
  }

  // 2. Kiểm tra cấm DNS trực tiếp cổng 53 (chỉ cho phép broker nội bộ)
  if ((r.port == 53 || r.is_dns_query) && !allow_direct_dns_) {
    d.reason_vi = "chặn DNS trực tiếp (cổng 53): chỉ cho phép resolver/broker nội bộ phân giải tên";
    return d;
  }

  // 3. Kiểm tra anti-DNS-tunneling và chuẩn tên miền RFC 1035
  if (!r.domain.empty()) {
    std::string dns_err;
    if (!validate_dns_name(r.domain, &dns_err, max_dns_label_entropy_)) {
      d.reason_vi = dns_err;
      return d;
    }
  }

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
      d.matched_rule = rule.pattern;
      d.reason_vi = "chặn kết nối tới " + (target.empty() ? std::string("<rỗng>") : target) +
                    port_s + " — đích nằm trong danh sách deny của chính sách (deny thắng allow)";
      return d;
    }
  }
  for (const auto& rule : rules_) {
    if (match_rule(rule)) {
      d.allowed = true;
      d.matched_rule =
          rule.pattern + (rule.port ? ":" + std::to_string(rule.port) : std::string());
      d.reason_vi = "cho phép kết nối tới " + (target.empty() ? std::string("<rỗng>") : target) +
                    port_s + " — khớp allowlist chính sách (net:" + d.matched_rule + ")";
      return d;
    }
  }

  d.reason_vi =
      "chặn kết nối tới " + (target.empty() ? std::string("<rỗng>") : target) + port_s +
      " — ngoài phạm vi cho phép của chính sách (cô lập egress, mặc định chặn)";
  return d;
}

// ------------------------------------------------ Secretless Broker (P3.10) --
SecretlessBroker::SecretlessBroker(Enforcer* enforcer, const Authority* auth)
    : enforcer_(enforcer), auth_(auth) {}

void SecretlessBroker::register_service_secret(const std::string& service_id, const std::string& secret) {
  secrets_[service_id] = secret;
}

bool SecretlessBroker::has_service_secret(const std::string& service_id) const {
  return secrets_.find(service_id) != secrets_.end();
}

BrokeredResponse SecretlessBroker::forward_request(const std::string& app_id, const Token* guest_token,
                                                   const EgressFilter& egress, const BrokeredRequest& req,
                                                   int64_t now) {
  BrokeredResponse resp;
  // 1. Kiểm tra xác thực thẻ năng lực guest (nếu có Authority)
  if (auth_) {
    if (!guest_token) {
      resp.error_vi = "Yêu cầu bị từ chối: thiếu thẻ năng lực guest";
      return resp;
    }
    std::string tok_err;
    if (!auth_->verify(*guest_token, now, &tok_err)) {
      resp.error_vi = "Yêu cầu bị từ chối: thẻ năng lực không hợp lệ (" + tok_err + ")";
      return resp;
    }
    if ((guest_token->rights & kRightNetConnect) == 0) {
      resp.error_vi = "Yêu cầu bị từ chối: thiếu quyền net.connect";
      return resp;
    }
  }

  // 2. Kiểm tra bộ lọc egress zero-trust (domain/port)
  EgressRequest er;
  er.domain = req.target_host;
  er.port = req.target_port;
  const EgressDecision ed = egress.check(er);
  if (!ed.allowed) {
    resp.error_vi = "Yêu cầu kết nối ra ngoài bị chặn: " + ed.reason_vi;
    if (enforcer_) {
      enforcer_->record(AuditEntry{now, app_id, Action{"net.connect", req.target_host + ":" + std::to_string(req.target_port)},
                                   Decision{false, Tier::MicroVm, ed.reason_vi, 0}});
    }
    return resp;
  }

  // 3. Broker gắn bí mật ở vòng ngoài mà KHÔNG đưa token/secret vào guest, môi trường hay audit
  const auto it = secrets_.find(req.service_id);
  if (it != secrets_.end()) {
    resp.secret_attached = true;
  }

  // Ghi audit: Ghi nhận gắn secretless broker nhưng KHÔNG BAO GIỜ ghi giá trị bí mật
  if (enforcer_) {
    Decision d;
    d.allowed = true;
    d.tier = Tier::MicroVm;
    d.matched_right = kRightNetConnect;
    d.reason = "Broker đã xác thực và gắn bí mật ngoài biên [SECRETLESS_BROKER_ATTACHED] cho dịch vụ " +
               req.service_id + " (đích: " + req.target_host + ")";
    enforcer_->record(AuditEntry{now, app_id, Action{"broker.forward", req.service_id + "@" + req.target_host}, d});
  }

  resp.success = true;
  resp.data = "{\"status\": 200, \"brokered\": true, \"service\": \"" + req.service_id + "\"}";
  return resp;
}

// -------------------------------------------------- app signature gate (P3.12) --
AppSignatureStatus AppSignatureGate::verify_signature(
    const std::string& manifest_canonical,
    const std::string& sig_hex,
    const std::string& signer_key_id,
    const bundle::Keyring& keyring) {
  if (manifest_canonical.empty() || sig_hex.empty() || signer_key_id.empty()) {
    return AppSignatureStatus::Unsigned;
  }
  if (!keyring.contains(signer_key_id)) {
    return AppSignatureStatus::UntrustedKey;
  }
  ed25519::PublicKey pub;
  if (!keyring.get(signer_key_id, pub)) {
    return AppSignatureStatus::UntrustedKey;
  }
  const auto sig_bytes = crypto::from_hex(sig_hex);
  if (sig_bytes.size() != 64) {
    return AppSignatureStatus::InvalidSignature;
  }
  ed25519::Signature sig;
  std::memcpy(sig.data(), sig_bytes.data(), 64);

  const uint8_t* msg = reinterpret_cast<const uint8_t*>(manifest_canonical.data());
  if (!ed25519::verify(pub, msg, manifest_canonical.size(), sig)) {
    return AppSignatureStatus::InvalidSignature;
  }
  return AppSignatureStatus::ValidTrusted;
}

AppSignatureGate::AppSignatureGate(const bundle::Keyring& keyring) : keyring_(keyring) {}

AppInstallDecision AppSignatureGate::evaluate(const AppInstallPackage& pkg) const {
  AppInstallDecision dec;
  dec.sig_status = verify_signature(pkg.manifest_canonical, pkg.sig_hex, pkg.signer_key_id, keyring_);

  if (dec.sig_status == AppSignatureStatus::InvalidSignature) {
    dec.accepted = false;
    dec.requires_user_confirmation = false;
    dec.reason_vi = "Chữ ký số Ed25519 không khớp hoặc gói cài đặt đã bị can thiệp: từ chối cài đặt";
    return dec;
  }

  if (dec.sig_status == AppSignatureStatus::Unsigned || dec.sig_status == AppSignatureStatus::UntrustedKey) {
    if (!pkg.user_confirmed) {
      dec.accepted = false;
      dec.requires_user_confirmation = true;
      dec.reason_vi = (dec.sig_status == AppSignatureStatus::Unsigned)
          ? "Ứng dụng không có chữ ký số: người dùng phải xác nhận cài đặt vào hộp cát cách ly"
          : "Chữ ký từ khoá chưa được tin cậy: người dùng phải xác nhận cài đặt vào hộp cát cách ly";
      return dec;
    }

    // Người dùng đã xác nhận: đưa vào hộp cát isolated, net: none, tước admin/power
    dec.accepted = true;
    dec.requires_user_confirmation = false;
    Policy restricted;
    restricted.app_id = pkg.app_id.empty() ? "untrusted.app" : pkg.app_id;
    restricted.tier = Tier::Isolated;
    // Tước toàn bộ quyền mạng (net.connect, net.listen), admin, và power
    uint64_t stripped = pkg.declared_policy.granted;
    stripped &= ~(kRightNetConnect | kRightNetListen | kRightAdmin | kRightPower);
    restricted.granted = stripped;
    restricted.allow_child_processes = false;
    restricted.persistent_storage = false;

    // Chỉ cho phép thư mục dữ liệu riêng của app
    restricted.scopes.clear();
    restricted.scopes.push_back("/home/user/app-data/" + restricted.app_id + ":rw");

    // Thêm rõ ràng deny cho net, admin, power
    restricted.deny.push_back("net:*");
    restricted.deny.push_back("admin");
    restricted.deny.push_back("power");

    dec.enforced_policy = restricted;
    dec.reason_vi = "Ứng dụng không có chữ ký hợp lệ được người dùng xác nhận: cách ly trong hộp cát net:none, tước quyền admin và power";
    return dec;
  }

  // Chữ ký hợp lệ từ khoá chủ quyền:
  dec.accepted = true;
  dec.requires_user_confirmation = false;
  dec.enforced_policy = pkg.declared_policy;
  dec.reason_vi = "Chữ ký số Ed25519 hợp lệ từ khoá chủ quyền: áp dụng chính sách riêng đã thẩm định";
  return dec;
}

// --------------------------------------------------- third-party IME sandbox (P3.13) --
const char* input_field_kind_name_vi(InputFieldKind kind) {
  switch (kind) {
    case InputFieldKind::NormalText: return "văn bản";
    case InputFieldKind::Password: return "mật khẩu";
    case InputFieldKind::Pin: return "mã PIN";
    case InputFieldKind::DigitsOnly: return "số thẻ/OTP";
    case InputFieldKind::Url: return "địa chỉ web";
    case InputFieldKind::Email: return "email";
  }
  return "không rõ";
}

Policy ThirdPartyImeHost::create_default_policy(const std::string& app_id, const std::string& data_dir) {
  Policy p;
  p.app_id = app_id;
  p.tier = Tier::Isolated;
  p.granted = kRightInputCompose;
  p.allow_child_processes = false;
  p.persistent_storage = false;
  if (!data_dir.empty()) {
    p.granted |= kRightFsRead | kRightFsWrite;
    p.scopes.push_back(data_dir + ":rw");
  }
  p.deny.push_back("net:*");
  p.deny.push_back("admin");
  p.deny.push_back("power");
  return p;
}

ThirdPartyImeHost::ThirdPartyImeHost(IThirdPartyImeEngine* engine, Policy policy, Enforcer* enforcer)
    : engine_(engine), policy_(std::move(policy)), enforcer_(enforcer) {}

std::string ThirdPartyImeHost::process_keystroke(uint32_t codepoint, InputFieldKind field_kind, int64_t now) {
  if (is_sensitive_input_field(field_kind)) {
    ++suppressed_sensitive_keys_;
    if (enforcer_) {
      AuditEntry entry;
      entry.when = now;
      entry.subject = policy_.app_id;
      entry.action = Action{"input.compose", input_field_kind_name_vi(field_kind)};
      entry.decision = Decision{false, policy_.tier,
          std::string("Bảo vệ OS: không chuyển phím từ trường nhạy cảm (") +
              input_field_kind_name_vi(field_kind) + ") cho bộ gõ bên thứ ba", 0};
      enforcer_->record(entry);
    }
    // Quy tắc cứng: engine ngoài nhận 0 phím
    return "";
  }

  // Trường văn bản thông thường: kiểm tra quyền input.compose
  if (enforcer_) {
    const Decision d = enforcer_->check(policy_, nullptr, Action{"input.compose", ""}, now);
    if (!d.allowed) return "";
  }

  if (!engine_) return "";
  return engine_->compose_key(codepoint);
}

Decision ThirdPartyImeHost::check_engine_action(const Action& action, int64_t now) {
  if (!enforcer_) return Decision{false, policy_.tier, "không có enforcer", 0};
  return enforcer_->check(policy_, nullptr, action, now);
}

// ------------------------------------------------ Micro-VM Lifecycle (P3.9) --
const char* micro_vm_state_name(MicroVmState s) {
  switch (s) {
    case MicroVmState::Running: return "running";
    case MicroVmState::Quarantined: return "quarantined";
    case MicroVmState::Destroyed: return "destroyed";
  }
  return "unknown";
}

const char* micro_vm_state_name_vi(MicroVmState s) {
  switch (s) {
    case MicroVmState::Running: return "đang chạy";
    case MicroVmState::Quarantined: return "đang đóng băng/cách ly";
    case MicroVmState::Destroyed: return "đã bị tiêu huỷ";
  }
  return "không rõ";
}

MicroVmSession::MicroVmSession(std::string vm_id, std::string app_id, Policy policy,
                               std::string family_id, int64_t created_at)
    : vm_id_(std::move(vm_id)),
      app_id_(std::move(app_id)),
      policy_(std::move(policy)),
      family_id_(family_id.empty() ? app_id_ : std::move(family_id)),
      state_(MicroVmState::Running),
      created_at_(created_at) {}

bool MicroVmSession::freeze(const std::string& reason, int64_t now, Enforcer* enforcer) {
  if (state_ == MicroVmState::Destroyed) return false;
  if (state_ == MicroVmState::Quarantined) return true;  // idempotent
  state_ = MicroVmState::Quarantined;
  frozen_at_ = now;
  freeze_reason_ = reason;
  if (enforcer) {
    AuditEntry e;
    e.when = now;
    e.subject = app_id_;
    e.action = Action{"vm.freeze", vm_id_};
    e.decision = Decision{false, policy_.tier,
        "Lớp 2: Đóng băng/cách ly micro-VM (" + vm_id_ + "): " + reason, 0};
    enforcer->record(e);
  }
  return true;
}

bool MicroVmSession::destroy(const std::string& reason, int64_t now, Enforcer* enforcer) {
  if (state_ == MicroVmState::Destroyed) return true;  // idempotent
  // Chuyển running -> frozen/quarantined -> destroyed
  if (state_ == MicroVmState::Running) {
    freeze(reason, now, enforcer);
  }
  state_ = MicroVmState::Destroyed;
  destroyed_at_ = now;
  destroy_reason_ = reason;
  if (enforcer) {
    AuditEntry e;
    e.when = now;
    e.subject = app_id_;
    e.action = Action{"vm.destroy", vm_id_};
    e.decision = Decision{false, policy_.tier,
        "Lớp 2: Tiêu huỷ phiên micro-VM (" + vm_id_ + "): " + reason, 0};
    enforcer->record(e);
  }
  return true;
}

bool MicroVmSession::quarantine_and_destroy(const std::string& reason, int64_t now, Enforcer* enforcer) {
  freeze(reason, now, enforcer);
  return destroy(reason, now, enforcer);
}

Decision MicroVmSession::handle_guest_request(const Action& action, int64_t now, Enforcer* enforcer,
                                              const Authority* auth, const Token* token) {
  if (state_ == MicroVmState::Quarantined) {
    Decision d;
    d.allowed = false;
    d.tier = policy_.tier;
    d.reason = "Yêu cầu guest bị từ chối: micro-VM đang bị đóng băng/cách ly (Quarantined) do: " + freeze_reason_;
    if (enforcer) {
      enforcer->record(AuditEntry{now, app_id_, action, d});
    }
    return d;
  }
  if (state_ == MicroVmState::Destroyed) {
    Decision d;
    d.allowed = false;
    d.tier = policy_.tier;
    d.reason = "Yêu cầu guest bị từ chối: micro-VM đã bị tiêu huỷ (Destroyed) do: " + destroy_reason_;
    if (enforcer) {
      enforcer->record(AuditEntry{now, app_id_, action, d});
    }
    return d;
  }
  if (!enforcer) {
    return Decision{false, policy_.tier, "không có enforcer", 0};
  }
  const Decision d = enforcer->check(policy_, token, action, now, auth);
  if (!d.allowed && (d.reason.find("thu hồi") != std::string::npos ||
                     d.reason.find("tầng cách ly") != std::string::npos)) {
    // Phát hiện vi phạm nghiêm trọng: đóng băng và huỷ phiên lập tức
    quarantine_and_destroy("Vi phạm chính sách cách ly: " + d.reason, now, enforcer);
  }
  return d;
}

MicroVmSupervisor::MicroVmSupervisor(Enforcer* enforcer, Authority* auth)
    : enforcer_(enforcer), auth_(auth) {}

MicroVmSession* MicroVmSupervisor::create_session(const std::string& vm_id, const std::string& app_id,
                                                  Policy policy, const std::string& family_id, int64_t now) {
  if (find_session(vm_id)) return nullptr;
  sessions_.push_back(std::make_unique<MicroVmSession>(vm_id, app_id, std::move(policy), family_id, now));
  return sessions_.back().get();
}

MicroVmSession* MicroVmSupervisor::find_session(const std::string& vm_id) {
  for (auto& s : sessions_) {
    if (s->vm_id() == vm_id) return s.get();
  }
  return nullptr;
}

const MicroVmSession* MicroVmSupervisor::find_session(const std::string& vm_id) const {
  for (const auto& s : sessions_) {
    if (s->vm_id() == vm_id) return s.get();
  }
  return nullptr;
}

bool MicroVmSupervisor::freeze_session(const std::string& vm_id, const std::string& reason, int64_t now) {
  auto* s = find_session(vm_id);
  if (!s) return false;
  return s->freeze(reason, now, enforcer_);
}

bool MicroVmSupervisor::destroy_session(const std::string& vm_id, const std::string& reason, int64_t now) {
  auto* s = find_session(vm_id);
  if (!s) return false;
  return s->destroy(reason, now, enforcer_);
}

bool MicroVmSupervisor::on_layer1_anomaly(const std::string& family_id_or_app, const std::string& reason, int64_t now) {
  bool found = false;
  for (auto& s : sessions_) {
    if (s->family_id() == family_id_or_app || s->app_id() == family_id_or_app || s->vm_id() == family_id_or_app) {
      s->quarantine_and_destroy("Lớp 1 kích hoạt huỷ phiên: " + reason, now, enforcer_);
      found = true;
    }
  }
  return found;
}

Decision MicroVmSupervisor::dispatch_guest_request(const std::string& vm_id, const Action& action,
                                                   int64_t now, const Token* token) {
  auto* s = find_session(vm_id);
  if (!s) {
    Decision d;
    d.allowed = false;
    d.tier = Tier::MicroVm;
    d.reason = "Phiên micro-VM không tồn tại: " + vm_id;
    if (enforcer_) {
      enforcer_->record(AuditEntry{now, "unknown", action, d});
    }
    return d;
  }
  return s->handle_guest_request(action, now, enforcer_, auth_, token);
}

size_t MicroVmSupervisor::active_count() const {
  size_t c = 0;
  for (const auto& s : sessions_) {
    if (s->state() == MicroVmState::Running) ++c;
  }
  return c;
}

// ------------------------------------ Trusted Confirmation Path (P4.13) --
TrustedConfirmationHost::TrustedConfirmationHost(Enforcer* enforcer) : enforcer_(enforcer) {}

std::string TrustedConfirmationHost::show_dialog(const std::string& target_app, const std::string& title_vi,
                                                 const std::string& prompt_vi, int64_t now) {
  std::string id = "dlg-" + std::to_string(next_dialog_id_++);
  ConfirmationDialog dlg;
  dlg.dialog_id = id;
  dlg.target_app = target_app;
  dlg.title_vi = title_vi;
  dlg.prompt_vi = prompt_vi;
  dlg.active = true;
  dlg.confirmed = false;
  dlg.cancelled = false;
  dlg.created_at = now;
  dialogs_[id] = dlg;
  return id;
}

bool TrustedConfirmationHost::handle_input(const std::string& dialog_id, const TaggedInputEvent& event, int64_t now) {
  auto it = dialogs_.find(dialog_id);
  if (it == dialogs_.end() || !it->second.active) return false;
  auto& dlg = it->second;

  // Chặn sự kiện tổng hợp từ ứng dụng / guest (anti-clickjacking)
  if (event.source != InputSourceKind::PhysicalHardware || event.hardware_device_id == 0) {
    ++synthetic_attacks_blocked_;
    if (enforcer_) {
      AuditEntry e;
      e.when = now;
      e.subject = dlg.target_app;
      e.action = Action{"security.synthetic_click", dialog_id};
      e.decision = Decision{false, Tier::Isolated,
          "Bảo vệ OS: phát hiện sự kiện tổng hợp (synthetic click) cố can thiệp hộp thoại xác nhận tin cậy — từ chối (chống clickjacking)", 0};
      enforcer_->record(e);
    }
    return false;
  }

  // Kiểm tra thời gian chờ tối thiểu (cooldown / countdown >= 500 ms) chống bấm nhầm/lừa click nhanh
  if (now > 0 && dlg.created_at > 0 && (now - dlg.created_at < 500)) {
    return false;
  }

  // Sự kiện từ thiết bị vật lý thật do nhân gắn thẻ
  if (event.is_button_down) {
    if (event.x >= dlg.confirm_x1 && event.x <= dlg.confirm_x2 &&
        event.y >= dlg.confirm_y1 && event.y <= dlg.confirm_y2) {
      dlg.confirmed = true;
      dlg.active = false;
      if (enforcer_) {
        AuditEntry e;
        e.when = now;
        e.subject = dlg.target_app;
        e.action = Action{"security.physical_confirm", dialog_id};
        e.decision = Decision{true, Tier::Isolated, "Xác nhận từ thiết bị phần cứng vật lý hợp lệ", 0};
        enforcer_->record(e);
      }
      return true;
    }
    if (event.x >= dlg.cancel_x1 && event.x <= dlg.cancel_x2 &&
        event.y >= dlg.cancel_y1 && event.y <= dlg.cancel_y2) {
      dlg.cancelled = true;
      dlg.active = false;
      if (enforcer_) {
        AuditEntry e;
        e.when = now;
        e.subject = dlg.target_app;
        e.action = Action{"security.physical_cancel", dialog_id};
        e.decision = Decision{false, Tier::Isolated, "Người dùng đã bấm huỷ bỏ từ thiết bị vật lý", 0};
        enforcer_->record(e);
      }
      return true;
    }
  }
  return false;
}

bool TrustedConfirmationHost::is_confirmed(const std::string& dialog_id) const {
  auto it = dialogs_.find(dialog_id);
  return it != dialogs_.end() && it->second.confirmed;
}

bool TrustedConfirmationHost::is_active(const std::string& dialog_id) const {
  auto it = dialogs_.find(dialog_id);
  return it != dialogs_.end() && it->second.active;
}

bool TrustedConfirmationHost::is_cancelled(const std::string& dialog_id) const {
  auto it = dialogs_.find(dialog_id);
  return it != dialogs_.end() && it->second.cancelled;
}

std::string TrustedConfirmationHost::dialog_target(const std::string& dialog_id) const {
  auto it = dialogs_.find(dialog_id);
  return it == dialogs_.end() ? std::string() : it->second.target_app;
}

// ------------------------------ App Compatibility & Sinkhole (P3.16) --
DnsResolutionResult DnsSinkhole::resolve_or_sinkhole(
    const std::string& domain,
    const EgressFilter& filter,
    int64_t now) {
  EgressRequest r;
  r.domain = domain;
  r.port = 443;
  const EgressDecision dec = filter.check(r);
  DnsResolutionResult res;
  if (dec.allowed) {
    res.sinkholed = false;
    res.ip = "1.2.3.4";
    res.rcode = 0;  // NOERROR
    res.latency_ms = 1.2;
    res.reason_vi = "NOERROR: phân giải hợp lệ theo allowlist chính sách";
  } else {
    // Sinkhole ngay lập tức tại tầng DNS: trả NXDOMAIN / địa chỉ 0.0.0.0
    // Độ trễ phản hồi < 100 ms (0.5 ms) giúp app nhận lỗi ngay thay vì bị treo
    res.sinkholed = true;
    res.ip = "0.0.0.0";
    res.rcode = 3;  // NXDOMAIN
    res.latency_ms = 0.5;
    res.reason_vi = "NXDOMAIN: bị chặn và sinkhole (" + dec.reason_vi + ")";
  }
  return res;
}

CompatibilityManager::CompatibilityManager(Enforcer* enforcer, Authority* auth,
                                           TrustedConfirmationHost* trusted_host)
    : enforcer_(enforcer), auth_(auth), trusted_host_(trusted_host) {}

void CompatibilityManager::set_app_compatibility(const std::string& app_id, bool enable) {
  compat_enabled_[app_id] = enable;
  if (enforcer_) {
    AuditEntry e;
    e.when = 0;
    e.subject = app_id;
    e.action = Action{"compat.set", enable ? "bật" : "tắt"};
    e.decision = Decision{true, Tier::MicroVm,
        std::string("Chế độ tương thích đã được ") + (enable ? "bật" : "tắt") + " cho " + app_id, 0};
    enforcer_->record(e);
  }
}

bool CompatibilityManager::is_app_compatibility_enabled(const std::string& app_id) const {
  auto it = compat_enabled_.find(app_id);
  return it != compat_enabled_.end() && it->second;
}

std::vector<std::string> CompatibilityManager::enabled_apps() const {
  std::vector<std::string> res;
  for (const auto& kv : compat_enabled_) {
    if (kv.second) res.push_back(kv.first);
  }
  return res;
}

bool CompatibilityManager::record_egress_block(const std::string& app_id, const std::string& domain,
                                               int64_t now, std::string* out_dialog_id) {
  if (!is_app_compatibility_enabled(app_id)) return false;

  // Chống làm phiền: nếu đã từ chối 3 lần -> không hỏi nữa
  if (user_denials_[app_id] >= 3) return false;

  // Tối đa 1 lần hỏi / 5 phút (300 giây)
  auto time_it = last_prompt_time_.find(app_id);
  if (time_it != last_prompt_time_.end() && (now - time_it->second < 300)) {
    return false;
  }

  // Ghi nhận lần chặn vào lịch sử
  block_history_[app_id].push_back({now, domain});

  // Kiểm tra điều kiện: bị chặn >= 3 lần trong 10 giây qua (M=10s)
  size_t count = 0;
  for (const auto& b : block_history_[app_id]) {
    if (b.domain == domain && (now - b.timestamp <= 10)) {
      ++count;
    }
  }
  if (count < 3) return false;

  // Kích hoạt đường xác nhận tin cậy (P4.13)
  last_prompt_time_[app_id] = now;
  ++prompt_count_;
  if (trusted_host_) {
    std::string title = "Yêu cầu kết nối mạng (Chế độ tương thích)";
    std::string prompt = "Ứng dụng " + app_id + " cần kết nối tới " + domain +
                         ". Mở quyền tạm thời 15 phút (mặc định)?";
    std::string dlg_id = trusted_host_->show_dialog(app_id, title, prompt, now);
    if (out_dialog_id) *out_dialog_id = dlg_id;
    if (enforcer_) {
      AuditEntry e;
      e.when = now;
      e.subject = app_id;
      e.action = Action{"compat.prompt", domain};
      e.decision = Decision{false, Tier::MicroVm,
          "Hiện hộp thoại xác nhận tin cậy mở quyền tạm thời tới " + domain, 0};
      enforcer_->record(e);
    }
    return true;
  }
  return false;
}

bool CompatibilityManager::grant_temporary_capability(const std::string& app_id, const std::string& domain,
                                                      int64_t duration_sec, bool until_app_close,
                                                      const std::string& family_id, int64_t now) {
  // Áp dụng trần thời gian 8 giờ (28800 giây)
  const int64_t clamped_duration = std::min<int64_t>(std::max<int64_t>(60, duration_sec), 28800);

  TemporaryGrant g;
  g.app_id = app_id;
  g.domain = domain;
  g.granted_at = now;
  g.expires_at = until_app_close ? 0 : (now + clamped_duration);
  g.until_app_close = until_app_close;
  g.family_id = family_id;
  g.generation = 0;

  active_grants_.push_back(g);
  ++grant_count_;

  if (enforcer_) {
    AuditEntry e;
    e.when = now;
    e.subject = app_id;
    e.action = Action{"compat.grant", domain};
    e.decision = Decision{true, Tier::MicroVm,
        "Cấp thẻ phiên tạm thời cho đích " + domain +
            (until_app_close ? " (đến khi đóng app)" : " (" + std::to_string(clamped_duration) + "s)"),
        kRightNetConnect};
    enforcer_->record(e);
  }
  return true;
}

void CompatibilityManager::record_user_denial(const std::string& app_id, const std::string& domain, int64_t now) {
  user_denials_[app_id]++;
  if (enforcer_) {
    AuditEntry e;
    e.when = now;
    e.subject = app_id;
    e.action = Action{"compat.deny", domain};
    e.decision = Decision{false, Tier::MicroVm,
        "Người dùng từ chối cấp quyền kết nối tới " + domain +
            " (từ chối lần thứ " + std::to_string(user_denials_[app_id]) + ")",
        0};
    enforcer_->record(e);
  }
}

bool CompatibilityManager::is_temporarily_allowed(const std::string& app_id, const std::string& domain, int64_t now) const {
  for (const auto& g : active_grants_) {
    if (g.app_id == app_id && host_matches(domain, g.domain)) {
      if (g.until_app_close || (now <= g.expires_at)) {
        return true;
      }
    }
  }
  return false;
}

void CompatibilityManager::check_expirations(int64_t now) {
  std::vector<TemporaryGrant> remaining;
  for (const auto& g : active_grants_) {
    if (!g.until_app_close && now > g.expires_at) {
      if (auth_ && !g.family_id.empty()) {
        auth_->revoke_family(g.family_id, "session expired");
      }
      if (enforcer_) {
        AuditEntry e;
        e.when = now;
        e.subject = g.app_id;
        e.action = Action{"compat.expire", g.domain};
        e.decision = Decision{false, Tier::MicroVm,
            "Hết hạn thẻ phiên tạm thời cho " + g.domain + " — thu hồi O(1) theo epoch", 0};
        enforcer_->record(e);
      }
    } else {
      remaining.push_back(g);
    }
  }
  active_grants_ = std::move(remaining);
}

void CompatibilityManager::on_app_closed(const std::string& app_id, int64_t now) {
  std::vector<TemporaryGrant> remaining;
  for (const auto& g : active_grants_) {
    if (g.app_id == app_id) {
      if (auth_ && !g.family_id.empty()) {
        auth_->revoke_family(g.family_id, "app closed");
      }
      if (enforcer_) {
        AuditEntry e;
        e.when = now;
        e.subject = g.app_id;
        e.action = Action{"compat.app_close", g.domain};
        e.decision = Decision{false, Tier::MicroVm,
            "Đóng ứng dụng " + app_id + " — huỷ toàn bộ thẻ phiên tạm thời của app", 0};
        enforcer_->record(e);
      }
    } else {
      remaining.push_back(g);
    }
  }
  active_grants_ = std::move(remaining);
}

size_t CompatibilityManager::denial_count(const std::string& app_id) const {
  auto it = user_denials_.find(app_id);
  return it != user_denials_.end() ? it->second : 0;
}

bool CompatibilityManager::is_permanently_quiet(const std::string& app_id) const {
  return denial_count(app_id) >= 3;
}

void CompatibilityManager::record_denial(const std::string& app_id, int64_t now) {
  record_user_denial(app_id, "*", now);
}

bool CompatibilityManager::on_blocked_request(const std::string& app_id, const std::string& kind,
                                              const std::string& target, int64_t now) {
  (void)kind;
  return record_egress_block(app_id, target, now);
}

TemporaryGrant CompatibilityManager::grant_session_capability(const std::string& app_id,
                                                              const std::string& kind,
                                                              const std::string& target,
                                                              int64_t duration_sec,
                                                              int64_t now,
                                                              bool until_app_close) {
  if (now == 0) now = std::time(nullptr);
  if (duration_sec <= 0) duration_sec = 900;
  if (duration_sec > 28800) duration_sec = 28800;

  TemporaryGrant g;
  g.app_id = app_id;
  g.domain = target;
  g.granted_at = now;
  g.until_app_close = until_app_close;
  g.expires_at = until_app_close ? 0 : (now + duration_sec);
  g.family_id = app_id;

  if (auth_) {
    uint64_t rights = right_for_kind(kind);
    if (rights == kRightNone) {
      if (yui::text::starts_with(kind, "net")) rights = kRightNetConnect;
      else if (yui::text::starts_with(kind, "fs")) rights = kRightFsRead;
      else rights = kRightNone;
    }
    g.token = auth_->issue(app_id, rights, {target}, now, until_app_close ? 86400 : duration_sec, 0, g.family_id);
    g.generation = g.token.generation;
  }

  active_grants_.push_back(g);
  grant_count_++;

  if (enforcer_) {
    AuditEntry e;
    e.when = now;
    e.subject = app_id;
    e.action = Action{"compat.grant", target};
    e.decision = Decision{true, Tier::MicroVm,
        "Cấp thẻ năng lực phiên tạm thời cho " + app_id + " truy cập " + target, 0};
    enforcer_->record(e);
  }

  return g;
}

void CompatibilityManager::revoke_app(const std::string& app_id, int64_t now) {
  if (auth_) {
    auth_->revoke_family(app_id, "user revoked compatibility");
  }
  on_app_closed(app_id, now);
}

}  // namespace yui::sandbox
