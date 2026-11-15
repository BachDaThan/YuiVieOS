// Tests for capability tokens and the three-tier sandbox enforcer.
#include <cstdio>

#include "testutil.hpp"
#include "yui/sandbox.hpp"

using namespace yui;
using namespace yui::sandbox;

static Policy make_policy(const std::string& caps_json, Tier tier = Tier::Isolated,
                          const std::string& app = "app:test") {
  const std::string doc = std::string(R"({"app":")") + app + R"(","tier":")" + tier_name(tier) +
                          R"(","capabilities":)" + caps_json + "}";
  auto parsed = json::parse(doc);
  std::string err;
  Policy p = Policy::from_json(parsed.value, &err);
  CHECK_MSG(err.empty(), "policy parse error: " + err);
  return p;
}

YUI_TEST(caps, rights_roundtrip) {
  const uint64_t mask = kRightFsRead | kRightNetConnect | kRightModel;
  const std::string s = rights_to_string(mask);
  CHECK_EQ(s, std::string("fs.read,net.connect,model"));
  CHECK_EQ(rights_from_string(s), mask);
  CHECK_EQ(rights_from_string("bogus,fs.write"), uint64_t(kRightFsWrite));
  CHECK_EQ(right_for_kind("vault"), uint64_t(kRightVault));
  CHECK_EQ(right_for_kind("nonsense"), uint64_t(kRightNone));
}

YUI_TEST(caps, token_issue_verify_tamper) {
  Authority auth("00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff");
  const int64_t now = 1'700'000'000;
  Token t = auth.issue("app:trim", kRightFsRead | kRightVault, {"/home/user/Documents"}, now, 600);
  std::string err;
  CHECK_MSG(auth.verify(t, now + 10, &err), "fresh token must verify: " + err);
  CHECK_MSG(!auth.verify(t, now + 601, &err), "expired token must fail");
  CHECK_EQ(err, std::string("token expired"));

  Token tampered = t;
  tampered.rights |= kRightAdmin;
  CHECK(!auth.verify(tampered, now, &err));
  CHECK_EQ(err, std::string("bad MAC"));

  Token future = t;
  future.issued_at = now + 3600;  // must re-sign to change, so MAC fails first
  CHECK(!auth.verify(future, now, &err));

  // A token signed by a different authority must not verify.
  Authority other("ffeeddccbbaa99887766554433221100ffeeddccbbaa99887766554433221100");
  CHECK(!other.verify(t, now, &err));
}

YUI_TEST(caps, bearer_roundtrip_and_canonical_check) {
  Authority auth("00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff");
  const int64_t now = 1'700'000'000;
  Token t = auth.issue("app:yui", kRightModel | kRightGraph, {"graph:profile"}, now, 300);
  const std::string bearer = auth.bearer(t);
  CHECK(bearer.find('.') != std::string::npos);

  Token back;
  std::string err;
  CHECK_MSG(auth.from_bearer(bearer, back, now + 5, &err), "bearer must round-trip: " + err);
  CHECK_EQ(back.subject, std::string("app:yui"));
  CHECK_EQ(back.rights, uint64_t(kRightModel | kRightGraph));
  CHECK_EQ(back.serial, t.serial);

  CHECK(!auth.from_bearer("not-a-bearer", back, now, &err));
  CHECK(!auth.from_bearer(bearer + "x", back, now, &err));
}

YUI_TEST(sandbox, fs_scopes) {
  Policy p = make_policy(R"(["fs.read","fs.write","fs:/home/user/Documents:rw","fs:/etc/yui:ro"])");
  Enforcer e;
  const int64_t now = 1'700'000'000;

  CHECK(e.check(p, nullptr, {"fs.read", "/home/user/Documents/notes.txt"}, now).allowed);
  CHECK(e.check(p, nullptr, {"fs.write", "/home/user/Documents/notes.txt"}, now).allowed);
  CHECK(e.check(p, nullptr, {"fs.read", "/etc/yui/policy.json"}, now).allowed);
  CHECK_MSG(!e.check(p, nullptr, {"fs.write", "/etc/yui/policy.json"}, now).allowed,
            "a :ro scope must not permit writes");
  CHECK_MSG(!e.check(p, nullptr, {"fs.read", "/home/user/DocumentsX/a"}, now).allowed,
            "prefix matching must respect path components");
  CHECK_MSG(!e.check(p, nullptr, {"fs.read", "/home/user/Documents/../../etc/shadow"}, now).allowed,
            "path traversal must be refused");
  CHECK_MSG(!e.check(p, nullptr, {"fs.write", "/tmp/x"}, now).allowed, "outside scope must be denied");
}

YUI_TEST(sandbox, net_scopes) {
  Policy p = make_policy(R"(["net.connect","net:*.yui.vn","net:api.github.com:443"])");
  Enforcer e;
  const int64_t now = 1'700'000'000;
  CHECK(e.check(p, nullptr, {"net.connect", "api.yui.vn:443"}, now).allowed);
  CHECK(e.check(p, nullptr, {"net.connect", "api.github.com:443"}, now).allowed);
  CHECK_MSG(!e.check(p, nullptr, {"net.connect", "api.github.com:80"}, now).allowed,
            "port mismatch must be denied");
  CHECK_MSG(!e.check(p, nullptr, {"net.connect", "evil.example.com:443"}, now).allowed,
            "unlisted host must be denied");
  CHECK_MSG(!e.check(p, nullptr, {"net.connect", "notyui.vn:443"}, now).allowed,
            "*.yui.vn must not match notyui.vn");
}

YUI_TEST(sandbox, explicit_deny_wins) {
  auto parsed = json::parse(R"({
    "app": "app:legacy",
    "tier": "isolated",
    "capabilities": ["fs.read", "fs.write", "fs:/home/user:rw"],
    "deny": ["/home/user/.ssh", "/home/user/Documents/private"]
  })");
  std::string err;
  Policy p = Policy::from_json(parsed.value, &err);
  CHECK(err.empty());
  Enforcer e;
  const int64_t now = 1'700'000'000;
  CHECK(e.check(p, nullptr, {"fs.read", "/home/user/Downloads/a.txt"}, now).allowed);
  CHECK_MSG(!e.check(p, nullptr, {"fs.read", "/home/user/.ssh/id_ed25519"}, now).allowed,
            "deny rule must override the grant");
  CHECK_MSG(!e.check(p, nullptr, {"fs.write", "/home/user/Documents/private/x"}, now).allowed,
            "deny rule must apply to nested paths");
}

YUI_TEST(sandbox, tiers_block_dangerous_rights) {
  const int64_t now = 1'700'000'000;
  Enforcer e;
  Policy isolated = make_policy(R"(["*"])", Tier::Isolated);
  CHECK_MSG(!e.check(isolated, nullptr, {"input", ""}, now).allowed,
            "isolated tier must not allow raw input injection");
  CHECK_MSG(!e.check(isolated, nullptr, {"admin", ""}, now).allowed, "isolated tier must not allow admin");

  Policy microvm = make_policy(R"(["*"])", Tier::MicroVm);
  CHECK_MSG(!e.check(microvm, nullptr, {"fs.read", "/etc/passwd"}, now).allowed,
            "micro-VM has no direct filesystem access");
  CHECK_MSG(e.check(microvm, nullptr, {"model", ""}, now).allowed,
            "micro-VM may call brokered model services");

  Policy native = make_policy(R"(["*"])", Tier::Native);
  CHECK(e.check(native, nullptr, {"input", ""}, now).allowed);
}

YUI_TEST(sandbox, missing_grant_is_denied) {
  Policy p = make_policy(R"(["fs.read","fs:/home/user:ro"])");
  Enforcer e;
  const int64_t now = 1'700'000'000;
  CHECK_MSG(!e.check(p, nullptr, {"vault", "profile"}, now).allowed, "no vault grant -> deny");
  CHECK_MSG(!e.check(p, nullptr, {"gpu", ""}, now).allowed, "no gpu grant -> deny");
  CHECK_MSG(!e.check(p, nullptr, {"unknown.kind", ""}, now).allowed, "unknown kinds -> deny");
}

YUI_TEST(sandbox, token_checks_apply_to_isolated_apps) {
  Authority auth("00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff");
  const int64_t now = 1'700'000'000;
  Policy p = make_policy(R"(["fs.read","fs:/home/user/Documents:ro"])");
  Enforcer e;

  // The policy attached at spawn time is authoritative; a token, when present,
  // can only narrow it further (see docs/SECURITY.md).
  CHECK(e.check(p, nullptr, {"fs.read", "/home/user/Documents/a"}, now).allowed);

  Token good = auth.issue("app:test", kRightFsRead, {"/home/user/Documents"}, now, 600);
  CHECK(e.check(p, &good, {"fs.read", "/home/user/Documents/a"}, now).allowed);

  Token weak = auth.issue("app:test", kRightNetConnect, {"/home/user/Documents"}, now, 600);
  CHECK_MSG(!e.check(p, &weak, {"fs.read", "/home/user/Documents/a"}, now).allowed,
            "token lacking fs.read must fail");

  Token narrow = auth.issue("app:test", kRightFsRead, {"/home/user/Downloads"}, now, 600);
  CHECK_MSG(!e.check(p, &narrow, {"fs.read", "/home/user/Documents/a"}, now).allowed,
            "token scope must cover the target");

  Token old = auth.issue("app:test", kRightFsRead, {"/home/user/Documents"}, now - 1000, 600);
  CHECK_MSG(!e.check(p, &old, {"fs.read", "/home/user/Documents/a"}, now).allowed,
            "expired token must fail");
}

YUI_TEST(sandbox, audit_is_recorded) {
  Policy p = make_policy(R"(["fs.read","fs:/home/user:ro"])");
  Enforcer e;
  const int64_t now = 1'700'000'000;
  e.check(p, nullptr, {"fs.read", "/home/user/a"}, now);
  e.check(p, nullptr, {"fs.write", "/home/user/a"}, now);
  CHECK_EQ(e.audit().size(), size_t(2));
  CHECK(e.audit()[0].decision.allowed);
  CHECK(!e.audit()[1].decision.allowed);
  CHECK_EQ(e.audit()[1].subject, std::string("app:test"));
  e.clear_audit();
  CHECK_EQ(e.audit().size(), size_t(0));
}

YUI_TEST(sandbox, policy_json_roundtrip) {
  auto parsed = json::parse(R"({
    "app": "app:game",
    "tier": "microvm",
    "capabilities": ["gpu", "net.connect", "net:*.steampowered.com"],
    "deny": ["net:*"],
    "persistent_storage": true
  })");
  std::string err;
  Policy p = Policy::from_json(parsed.value, &err);
  CHECK(err.empty());
  CHECK_EQ(int(p.tier), int(Tier::MicroVm));
  CHECK(p.granted & kRightGpu);
  CHECK(p.persistent_storage);

  auto again = json::parse(p.to_json().dump());
  Policy q = Policy::from_json(again.value, &err);
  CHECK(err.empty());
  CHECK_EQ(q.app_id, p.app_id);
  CHECK_EQ(q.granted, p.granted);
  CHECK_EQ(q.scopes.size(), p.scopes.size());

  // Invalid documents.
  std::string e2;
  CHECK(Policy::from_json(json::parse(R"({"tier":"isolated"})").value, &e2).app_id.empty());
  CHECK(!e2.empty());
  e2.clear();
  Policy bad = Policy::from_json(json::parse(R"({"app":"a","tier":"quantum"})").value, &e2);
  CHECK(bad.app_id.empty());
  CHECK(!e2.empty());
}

YUI_TEST(sandbox, shipped_policy_files_are_valid) {
  // The policies that ship in sandbox/policies must parse and behave as the
  // documentation claims. YUI_SOURCE_DIR is injected by the Makefile.
  const std::string dir = std::string(YUI_SOURCE_DIR) + "/sandbox/policies/";
  struct Expect {
    const char* file;
    std::string kind;
    std::string target;
    bool allowed;
  };
  const Expect cases[] = {
      {"yui-app.policy.json", "vault", "notes", true},
      {"yui-app.policy.json", "net.connect", "api.yui.vn:443", true},
      {"yui-app.policy.json", "net.connect", "telemetry.evil:443", false},
      {"yui-app.policy.json", "fs.read", "/home/user/.ssh/id_ed25519", false},
      {"yui-app.policy.json", "fs.read", "/home/user/Documents/a.txt", true},
      {"yui-app.policy.json", "fs.write", "/home/user/Documents/a.txt", false},
      {"trim.policy.json", "fs.write", "/home/user/Applications/app.yuip", true},
      {"trim.policy.json", "fs.write", "/home/user/.ssh/authorized_keys", false},
      {"trim.policy.json", "net.connect", "example.com:443", false},
      {"legacy-game.policy.json", "gpu", "", true},
      {"legacy-game.policy.json", "fs.read", "/etc/passwd", false},
      {"legacy-game.policy.json", "input", "", false},
      {"first-party-ipc.policy.json", "admin", "", true},
      {"first-party-ipc.policy.json", "input", "", true},
  };
  for (const auto& c : cases) {
    FILE* f = std::fopen((dir + c.file).c_str(), "rb");
    CHECK_MSG(f != nullptr, std::string("cannot open ") + c.file);
    if (!f) continue;
    std::string text;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
    std::fclose(f);

    auto parsed = json::parse(text);
    CHECK_MSG(parsed.ok, std::string("invalid JSON in ") + c.file + ": " + parsed.error);
    std::string err;
    const Policy policy = Policy::from_json(parsed.value, &err);
    CHECK_MSG(err.empty(), std::string("invalid policy ") + c.file + ": " + err);
    Enforcer e;
    const auto d = e.check(policy, nullptr, {c.kind, c.target}, 1'700'000'000);
    CHECK_MSG(d.allowed == c.allowed, std::string(c.file) + " " + c.kind + " " + c.target +
                                          " expected " + (c.allowed ? "allow" : "deny") + " got " +
                                          (d.allowed ? "allow" : "deny") + " (" + d.reason + ")");
  }
}

YUI_TEST_MAIN("test_sandbox")
