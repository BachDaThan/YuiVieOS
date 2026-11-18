// Tests for capability tokens and the three-tier sandbox enforcer.
#include <cstdio>
#include <cstring>

#include "testutil.hpp"
#include "yui/bundle.hpp"
#include "yui/crypto.hpp"
#include "yui/ed25519.hpp"
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
  // The policies that ship in vmm-sandbox/policies must parse and behave as the
  // documentation claims. YUI_SOURCE_DIR is injected by the Makefile.
  const std::string dir = std::string(YUI_SOURCE_DIR) + "/vmm-sandbox/policies/";
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

// ------------------------------------------------------------- P3.5 escape --
YUI_TEST(sandbox, ten_escape_scenarios_are_all_blocked_with_logged_reason) {
  // Chính sách ứng dụng thông thường bị cô lập (Isolated tier):
  // Chỉ được đọc/ghi trong thư mục dữ liệu riêng và kết nối API nội bộ.
  const Policy policy = make_policy(
      R"(["fs.read", "fs.write", "fs:/home/user/app-data:rw", "net.connect", "net:api.service.vn:443"])",
      Tier::Isolated, "app:untrusted");

  Enforcer enforcer;
  const int64_t now = 1'700'000'000;

  struct EscapeScenario {
    const char* name;
    Action action;
    const char* expected_reason_keyword;
  };

  const EscapeScenario scenarios[] = {
      // 1. Đọc tệp mật khẩu hệ thống /etc/shadow
      {"1. Đọc /etc/shadow", {"fs.read", "/etc/shadow"}, "không bao phủ"},
      // 2. Mở socket C2 ra ngoài trái phép
      {"2. Mở kết nối C2", {"net.connect", "c2.evil-attacker.org:443"}, "không bao phủ"},
      // 3. Can thiệp tiến trình khác qua ptrace
      {"3. ptrace tiến trình khác", {"ptrace", "pid:1"}, "hành động không xác định"},
      // 4. Gắn kết hệ thống tệp đặc quyền (mount)
      {"4. mount hệ tệp đặc quyền", {"mount", "/dev/sda1"}, "hành động không xác định"},
      // 5. Đọc bộ nhớ tiến trình qua /proc/self/mem
      {"5. Đọc /proc/self/mem", {"fs.read", "/proc/self/mem"}, "không bao phủ"},
      // 6. Leo thang đặc quyền admin (setuid/admin)
      {"6. Leo thang admin", {"admin", ""}, "không cho phép"},
      // 7. Bơm sự kiện đầu vào bàn phím/chuột (input injection / keylogging)
      {"7. Bơm input injection", {"input", ""}, "không cho phép"},
      // 8. Nạp module nhân (kernel module / rootkit)
      {"8. Nạp kernel module", {"kernel.module", "/tmp/rootkit.ko"}, "hành động không xác định"},
      // 9. Can thiệp trực tiếp phần cứng / MMIO qua /dev/mem
      {"9. Can thiệp /dev/mem", {"fs.write", "/dev/mem"}, "không bao phủ"},
      // 10. Mở cổng lắng nghe mạng (net.listen / backdoor port)
      {"10. Mở net.listen", {"net.listen", "0.0.0.0:8080"}, "chính sách không cấp quyền"},
  };

  constexpr size_t kScenarioCount = sizeof(scenarios) / sizeof(scenarios[0]);
  CHECK_EQ(kScenarioCount, size_t(10));

  size_t blocked_count = 0;
  for (const auto& s : scenarios) {
    const Decision d = enforcer.check(policy, nullptr, s.action, now);
    if (!d.allowed && !d.reason.empty() &&
        d.reason.find(s.expected_reason_keyword) != std::string::npos) {
      ++blocked_count;
    }
    CHECK(!d.allowed);
    CHECK(!d.reason.empty());
  }

  // 10/10 kịch bản cố thoát hộp cát đều bị chặn!
  CHECK_EQ(blocked_count, size_t(10));

  // Kiểm tra nhật ký kiểm toán (audit log) ghi lại đủ 10 nỗ lực với lý do chi tiết
  CHECK_EQ(enforcer.audit().size(), size_t(10));
  for (size_t i = 0; i < enforcer.audit().size(); ++i) {
    const auto& entry = enforcer.audit()[i];
    CHECK_EQ(entry.subject, std::string("app:untrusted"));
    CHECK(!entry.decision.allowed);
    CHECK(!entry.decision.reason.empty());
  }

  // Kiểm tra thêm biến thể path traversal cố đọc shadow qua đường dẫn tương đối
  const Decision traversal = enforcer.check(
      policy, nullptr, {"fs.read", "/home/user/app-data/../../etc/shadow"}, now);
  CHECK(!traversal.allowed);
  CHECK(traversal.reason.find("không bao phủ") != std::string::npos);

  // Kiểm tra thêm ở lớp EgressFilter: kết nối ra ngoài bị chặn và có lý do tiếng Việt
  EgressFilter egress;
  egress.add_from_policy(policy);
  const auto egress_dec = egress.check({"c2.evil-attacker.org", "", 443});
  CHECK(!egress_dec.allowed);
  CHECK(egress_dec.reason_vi.find("chặn kết nối") != std::string::npos);
  CHECK(egress_dec.reason_vi.find("cô lập egress") != std::string::npos);
}

// ------------------------------------------------------------- P3.12 tests --
YUI_TEST(sandbox, p3_12_app_signature_gate_default_sandbox_policy) {
  // Chuẩn bị khoá chủ quyền sovereign keyring
  ed25519::Seed seed{};
  seed.fill(0x42);
  const ed25519::PublicKey pub = ed25519::public_key(seed);
  bundle::Keyring keyring;
  CHECK(keyring.add("key-sovereign-01", pub));

  AppSignatureGate gate(keyring);
  Enforcer enforcer;
  const int64_t now = 1'700'000'000;

  // Khai báo chính sách mong muốn của app (ví dụ đòi quyền mạng và admin)
  Policy declared;
  declared.app_id = "calc-untrusted";
  declared.tier = Tier::Isolated;
  declared.granted = kRightFsRead | kRightFsWrite | kRightNetConnect | kRightAdmin | kRightPower;
  declared.scopes = {"fs:/home/user/app-data/calc-untrusted:rw", "net:api.evil.com:443"};

  // TEST 1: Ứng dụng không có chữ ký số (Unsigned)
  {
    AppInstallPackage pkg;
    pkg.app_id = "calc-untrusted";
    pkg.manifest_canonical = "{\"app\":\"calc-untrusted\",\"version\":\"1.0\"}";
    pkg.sig_hex = "";
    pkg.signer_key_id = "";
    pkg.declared_policy = declared;
    pkg.user_confirmed = false;

    // 1a. Chưa được người dùng xác nhận => từ chối và yêu cầu xác nhận
    const AppInstallDecision dec_unconfirmed = gate.evaluate(pkg);
    CHECK(!dec_unconfirmed.accepted);
    CHECK(dec_unconfirmed.requires_user_confirmation);
    CHECK_EQ(int(dec_unconfirmed.sig_status), int(AppSignatureStatus::Unsigned));
    CHECK(dec_unconfirmed.reason_vi.find("xác nhận") != std::string::npos);

    // 1b. Người dùng đã xác nhận qua hộp thoại tin cậy => được cài nhưng BẮT BUỘC nhốt vào
    // hộp cát isolated, net: none, tước admin và power
    pkg.user_confirmed = true;
    const AppInstallDecision dec_confirmed = gate.evaluate(pkg);
    CHECK(dec_confirmed.accepted);
    CHECK(!dec_confirmed.requires_user_confirmation);
    CHECK_EQ(int(dec_confirmed.sig_status), int(AppSignatureStatus::Unsigned));

    const Policy& enforced = dec_confirmed.enforced_policy;
    CHECK_EQ(int(enforced.tier), int(Tier::Isolated));
    // Tước toàn bộ quyền mạng
    CHECK((enforced.granted & kRightNetConnect) == 0);
    CHECK((enforced.granted & kRightNetListen) == 0);
    // Tước admin và power
    CHECK((enforced.granted & kRightAdmin) == 0);
    CHECK((enforced.granted & kRightPower) == 0);

    // Thử các hành động trong thực tế để chứng minh sandbox chặn hoàn toàn:
    // Cố kết nối mạng => bị chặn!
    const Decision d_net = enforcer.check(enforced, nullptr, {"net.connect", "api.evil.com:443"}, now);
    CHECK(!d_net.allowed);
    // Cố gọi admin => bị chặn!
    const Decision d_admin = enforcer.check(enforced, nullptr, {"admin", ""}, now);
    CHECK(!d_admin.allowed);
    // Cố gọi power => bị chặn!
    const Decision d_power = enforcer.check(enforced, nullptr, {"power", ""}, now);
    CHECK(!d_power.allowed);
    // Cố đọc file ngoài phạm vi riêng (/etc/shadow) => bị chặn!
    const Decision d_shadow = enforcer.check(enforced, nullptr, {"fs.read", "/etc/shadow"}, now);
    CHECK(!d_shadow.allowed);
    // Được ghi trong thư mục dữ liệu riêng của nó
    const Decision d_ok = enforcer.check(
        enforced, nullptr, {"fs.write", "/home/user/app-data/calc-untrusted/cache.db"}, now);
    CHECK(d_ok.allowed);
  }

  // TEST 2: Chữ ký số sai / bị can thiệp (Invalid Signature)
  {
    AppInstallPackage pkg;
    pkg.app_id = "calc-untrusted";
    pkg.manifest_canonical = "{\"app\":\"calc-untrusted\",\"version\":\"1.0\"}";
    // Ký đúng trên chuỗi gốc
    const auto sig = ed25519::sign(
        seed,
        reinterpret_cast<const uint8_t*>(pkg.manifest_canonical.data()),
        pkg.manifest_canonical.size());
    pkg.sig_hex = crypto::hex(crypto::Bytes(sig.begin(), sig.end()));
    pkg.signer_key_id = "key-sovereign-01";
    pkg.declared_policy = declared;
    pkg.user_confirmed = true;

    // Kẻ tấn công can thiệp sửa 1 byte trong manifest!
    pkg.manifest_canonical += " ";

    const AppInstallDecision dec_tampered = gate.evaluate(pkg);
    CHECK(!dec_tampered.accepted);
    CHECK(!dec_tampered.requires_user_confirmation);
    CHECK_EQ(int(dec_tampered.sig_status), int(AppSignatureStatus::InvalidSignature));
    CHECK(dec_tampered.reason_vi.find("không khớp") != std::string::npos ||
          dec_tampered.reason_vi.find("can thiệp") != std::string::npos);
  }

  // TEST 3: Chữ ký đúng bởi khoá chủ quyền (Valid Trusted)
  {
    AppInstallPackage pkg;
    pkg.app_id = "app-official";
    pkg.manifest_canonical = "{\"app\":\"app-official\",\"version\":\"2.0\"}";
    const auto sig = ed25519::sign(
        seed,
        reinterpret_cast<const uint8_t*>(pkg.manifest_canonical.data()),
        pkg.manifest_canonical.size());
    pkg.sig_hex = crypto::hex(crypto::Bytes(sig.begin(), sig.end()));
    pkg.signer_key_id = "key-sovereign-01";

    Policy official_policy;
    official_policy.app_id = "app-official";
    official_policy.tier = Tier::Isolated;
    official_policy.granted = kRightFsRead | kRightNetConnect;
    official_policy.scopes = {"fs:/home/user/docs:ro", "net:api.service.vn:443"};
    pkg.declared_policy = official_policy;

    const AppInstallDecision dec_valid = gate.evaluate(pkg);
    CHECK(dec_valid.accepted);
    CHECK(!dec_valid.requires_user_confirmation);
    CHECK_EQ(int(dec_valid.sig_status), int(AppSignatureStatus::ValidTrusted));
    CHECK_EQ(dec_valid.enforced_policy.granted, official_policy.granted);
    CHECK_EQ(dec_valid.enforced_policy.scopes.size(), official_policy.scopes.size());
    CHECK(dec_valid.reason_vi.find("khoá chủ quyền") != std::string::npos);
  }
}

// ------------------------------------------------------------- P3.13 tests --
namespace {
class MockThirdPartyImeEngine : public IThirdPartyImeEngine {
 public:
  explicit MockThirdPartyImeEngine(std::string id) : id_(std::move(id)) {}
  const std::string& app_id() const override { return id_; }
  std::string compose_key(uint32_t codepoint) override {
    ++received_keys_;
    buf_ += static_cast<char>(codepoint);
    return std::string("composed:") + buf_;
  }
  void reset() override { buf_.clear(); }
  size_t keystrokes_received() const override { return received_keys_; }

 private:
  std::string id_;
  std::string buf_;
  size_t received_keys_ = 0;
};
}  // namespace

YUI_TEST(sandbox, p3_13_third_party_ime_isolated_and_sensitive_fields_protected) {
  MockThirdPartyImeEngine engine("ime:untrusted-plugin");
  Enforcer enforcer;
  const int64_t now = 1'700'000'000;

  // 1. Khởi tạo chính sách chuẩn cho IME bên thứ ba:
  // Chỉ quyền input.compose, không net, chỉ fs riêng
  const Policy ime_policy = ThirdPartyImeHost::create_default_policy(
      "ime:untrusted-plugin", "/home/user/.config/untrusted-plugin");
  CHECK_EQ(int(ime_policy.tier), int(Tier::Isolated));
  CHECK_EQ(ime_policy.granted, uint64_t(kRightInputCompose | kRightFsRead | kRightFsWrite));

  ThirdPartyImeHost host(&engine, ime_policy, &enforcer);

  // 2. Gõ vào trường văn bản thông thường (NormalText):
  // Host cho phép, engine nhận phím và trả kết quả chuỗi
  const std::string r1 = host.process_keystroke('x', InputFieldKind::NormalText, now);
  CHECK_EQ(r1, std::string("composed:x"));
  CHECK_EQ(engine.keystrokes_received(), size_t(1));
  CHECK_EQ(host.suppressed_keystrokes_count(), size_t(0));

  const std::string r2 = host.process_keystroke('i', InputFieldKind::NormalText, now);
  CHECK_EQ(r2, std::string("composed:xi"));
  CHECK_EQ(engine.keystrokes_received(), size_t(2));

  // 3. Quy tắc cứng OS: gõ vào các trường nhạy cảm:
  // Password, PIN, DigitsOnly, URL, Email
  const InputFieldKind sensitive_fields[] = {
      InputFieldKind::Password,
      InputFieldKind::Pin,
      InputFieldKind::DigitsOnly,
      InputFieldKind::Url,
      InputFieldKind::Email,
  };

  const size_t keys_before = engine.keystrokes_received();
  for (const auto field : sensitive_fields) {
    const std::string out = host.process_keystroke('A', field, now);
    // Tuyệt đối không trả về gì từ engine ngoài
    CHECK(out.empty());
  }

  // Engine ngoài nhận ĐÚNG 0 phím từ tất cả các trường nhạy cảm!
  CHECK_EQ(engine.keystrokes_received(), keys_before);
  CHECK_EQ(host.suppressed_keystrokes_count(), size_t(5));

  // Kiểm tra nhật ký kiểm toán (audit log) ghi lại việc bảo vệ các trường nhạy cảm
  size_t suppressed_audit_count = 0;
  for (const auto& a : enforcer.audit()) {
    if (a.subject == "ime:untrusted-plugin" &&
        a.decision.reason.find("Bảo vệ OS: không chuyển phím") != std::string::npos) {
      ++suppressed_audit_count;
    }
  }
  CHECK_EQ(suppressed_audit_count, size_t(5));

  // 4. Red-team attack test từ mã độc đội lốt engine IME:
  // (a) Mở socket ra ngoài để gửi trộm phím (exfiltration)
  const Decision d_exfil = host.check_engine_action(
      {"net.connect", "telemetry.evil-keylogger.org:443"}, now);
  CHECK(!d_exfil.allowed);
  CHECK(d_exfil.reason.find("chính sách không cấp quyền") != std::string::npos);

  // (b) Đọc lén tệp nhạy cảm của người dùng (/home/user/.ssh/id_ed25519)
  const Decision d_steal = host.check_engine_action(
      {"fs.read", "/home/user/.ssh/id_ed25519"}, now);
  CHECK(!d_steal.allowed);
  CHECK(d_steal.reason.find("không bao phủ") != std::string::npos);

  // (c) Ghi tệp nhật ký phím lén ra ngoài (/home/user/keys.txt)
  const Decision d_log = host.check_engine_action(
      {"fs.write", "/home/user/keys.txt"}, now);
  CHECK(!d_log.allowed);

  // (d) Bơm sự kiện chuột/phím trái phép (input injection)
  const Decision d_inject = host.check_engine_action({"input", ""}, now);
  CHECK(!d_inject.allowed);

  // (e) Thao tác hợp lệ trong thư mục cấu hình riêng của nó
  const Decision d_own_cfg = host.check_engine_action(
      {"fs.write", "/home/user/.config/untrusted-plugin/dict.bin"}, now);
  CHECK(d_own_cfg.allowed);
}

// --------------------------------------------- Layer 2: Micro-VM Lifecycle (P3.9) --

YUI_TEST(sandbox, layer2_micro_vm_quarantine_freeze_and_destruction) {
  Enforcer enforcer;
  const std::string master_key = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
  Authority auth(master_key);
  MicroVmSupervisor supervisor(&enforcer, &auth);

  const int64_t now = 1700000000;

  // 1. Khởi tạo 2 phiên micro-VM độc lập
  Policy game_policy;
  game_policy.app_id = "app:legacy-game";
  game_policy.tier = Tier::MicroVm;
  game_policy.granted = kRightNetConnect | kRightGpu;
  game_policy.scopes = {"game.yui.vn:443"};

  Policy calc_policy;
  calc_policy.app_id = "app:wine-calc";
  calc_policy.tier = Tier::MicroVm;
  calc_policy.granted = kRightNone;

  auto* session_game = supervisor.create_session("vm-game-42", "app:legacy-game", game_policy, "fam-session-game", now);
  auto* session_calc = supervisor.create_session("vm-calc-99", "app:wine-calc", calc_policy, "fam-session-calc", now);

  CHECK(session_game != nullptr);
  CHECK(session_calc != nullptr);
  CHECK_EQ(supervisor.active_count(), size_t(2));
  CHECK_EQ(supervisor.total_count(), size_t(2));

  CHECK_EQ(session_game->state(), MicroVmState::Running);
  CHECK_EQ(session_calc->state(), MicroVmState::Running);
  CHECK_EQ(std::string(micro_vm_state_name(session_game->state())), std::string("running"));
  CHECK_EQ(std::string(micro_vm_state_name_vi(session_game->state())), std::string("đang chạy"));

  // 2. Cấp token và gửi yêu cầu hợp lệ tới vm-game-42
  Token game_token = auth.issue("app:legacy-game", kRightNetConnect, {"game.yui.vn:443"}, now, 3600, 0, "fam-session-game");
  const Decision d_ok = supervisor.dispatch_guest_request("vm-game-42", {"net.connect", "game.yui.vn:443"}, now + 1, &game_token);
  CHECK(d_ok.allowed);

  // 3. Mô phỏng Lớp 1 phát hiện bất thường: vượt ranh giới micro-VM
  const bool anomaly = enforcer.detect_anomaly(
      auth, "fam-session-game", "app:legacy-game", "kvm_ioctl_evasion", "attempted unbrokered KVM escape", now + 10);
  CHECK(anomaly);

  // Lớp 2 phản ứng: chuyển running -> frozen/quarantined -> destroyed trước khi xử lý yêu cầu kế tiếp
  const bool handled = supervisor.on_layer1_anomaly("fam-session-game", "KVM evasion detected by Layer 1", now + 11);
  CHECK(handled);

  // Phiên vm-game-42 bị huỷ; active_count giảm còn 1
  CHECK_EQ(session_game->state(), MicroVmState::Destroyed);
  CHECK_EQ(std::string(micro_vm_state_name(session_game->state())), std::string("destroyed"));
  CHECK_EQ(std::string(micro_vm_state_name_vi(session_game->state())), std::string("đã bị tiêu huỷ"));
  CHECK_EQ(supervisor.active_count(), size_t(1));

  // 4. Mọi yêu cầu guest kế tiếp gửi tới phiên đã huỷ lập tức bị từ chối trước khi thực thi
  const Decision d_after = supervisor.dispatch_guest_request("vm-game-42", {"net.connect", "game.yui.vn:443"}, now + 12, &game_token);
  CHECK(!d_after.allowed);
  CHECK(d_after.reason.find("đã bị tiêu huỷ (Destroyed)") != std::string::npos);

  // 5. Thao tác huỷ phải là idempotent (gọi lại không sinh lỗi hay double-free)
  CHECK(supervisor.destroy_session("vm-game-42", "destroy again", now + 13));
  CHECK_EQ(session_game->state(), MicroVmState::Destroyed);

  // Một phiên đã huỷ không thể chuyển ngược về Quarantined
  CHECK(!session_game->freeze("try un-destroy", now + 14, &enforcer));

  // 6. Tính khoanh vùng: Phiên vm-calc-99 hoàn toàn không bị ảnh hưởng!
  CHECK_EQ(session_calc->state(), MicroVmState::Running);
  CHECK(supervisor.find_session("vm-calc-99") != nullptr);

  // 7. Thao tác freeze thủ công trên session còn lại
  CHECK(supervisor.freeze_session("vm-calc-99", "admin freeze", now + 15));
  CHECK_EQ(session_calc->state(), MicroVmState::Quarantined);
  CHECK_EQ(supervisor.active_count(), size_t(0));

  // Khi đang bị Quarantined, yêu cầu guest cũng bị từ chối ngay lập tức
  const Decision d_calc = supervisor.dispatch_guest_request("vm-calc-99", {"gpu", ""}, now + 16, nullptr);
  CHECK(!d_calc.allowed);
  CHECK(d_calc.reason.find("đang bị đóng băng/cách ly (Quarantined)") != std::string::npos);

  // 8. Kiểm tra nhật ký kiểm toán (audit log) ghi đủ các chuyển trạng thái
  size_t freeze_audits = 0;
  size_t destroy_audits = 0;
  for (const auto& a : enforcer.audit()) {
    if (a.action.kind == "vm.freeze") ++freeze_audits;
    if (a.action.kind == "vm.destroy") ++destroy_audits;
  }
  CHECK(freeze_audits >= 2);
  CHECK(destroy_audits >= 1);
}

// ----------------------------- Layer 3: Egress Zero-Trust & Broker (P3.10) --

YUI_TEST(sandbox, layer3_egress_zero_trust_anti_dns_tunneling_and_secretless_broker) {
  Enforcer enforcer;
  const std::string master_key = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
  Authority auth(master_key);
  SecretlessBroker broker(&enforcer, &auth);

  const int64_t now = 1700000000;

  // 1. Kiểm tra chính sách và EgressFilter: mặc định deny east-west & DNS direct (port 53)
  Policy p;
  p.app_id = "app:weather-service";
  p.tier = Tier::MicroVm;
  p.version = 1;
  p.allow_east_west = false;
  p.allow_direct_dns = false;
  p.scopes = {"api.weather.yui.vn:443", "*.trusted-source.vn:443"};

  EgressFilter filter;
  filter.add_from_policy(p);

  // (a) Giao tiếp ngang (East-West) bị chặn
  EgressRequest r_ew;
  r_ew.domain = "api.weather.yui.vn";
  r_ew.port = 443;
  r_ew.is_east_west = true;
  const EgressDecision d_ew = filter.check(r_ew);
  CHECK(!d_ew.allowed);
  CHECK(d_ew.reason_vi.find("chặn kết nối ngang (east-west)") != std::string::npos);

  // (b) DNS trực tiếp cổng 53 bị cấm (chỉ resolver nội bộ)
  EgressRequest r_dns53;
  r_dns53.ip = "8.8.8.8";
  r_dns53.port = 53;
  const EgressDecision d_dns53 = filter.check(r_dns53);
  CHECK(!d_dns53.allowed);
  CHECK(d_dns53.reason_vi.find("chặn DNS trực tiếp (cổng 53)") != std::string::npos);

  // 2. Chống DNS-tunneling & chuẩn RFC 1035
  // (a) Tên miền vượt quá 253 octet
  std::string huge_domain;
  for (int i = 0; i < 26; ++i) huge_domain += "abcdefghij.";
  huge_domain += "com";  // > 260 chars
  std::string err_huge;
  CHECK(!EgressFilter::validate_dns_name(huge_domain, &err_huge));
  CHECK(err_huge.find("vượt quá giới hạn 253 octet") != std::string::npos);

  // (b) Nhãn DNS vượt quá 63 octet
  std::string long_label = std::string(64, 'a') + ".yui.vn";
  std::string err_label;
  CHECK(!EgressFilter::validate_dns_name(long_label, &err_label));
  CHECK(err_label.find("vượt quá 63 octet") != std::string::npos);

  // (c) Nhãn DNS bắt đầu/kết thúc bằng dấu gạch nối hoặc ký tự lạ
  std::string err_hyphen;
  CHECK(!EgressFilter::validate_dns_name("-badlabel.yui.vn", &err_hyphen));
  CHECK(err_hyphen.find("bắt đầu hoặc kết thúc bằng gạch nối") != std::string::npos);

  // (d) Phát hiện payload DNS tunneling qua entropy cao (H > 4.2 bit/ký tự)
  // Payload điển hình từ dnscat2 / iodine mã hoá base36 ngẫu nhiên
  const std::string high_entropy_tunnel = "3k9m1z8b5f7p4w2q6j0v.attacker-tunnel.com";
  std::string err_tunnel;
  CHECK(!EgressFilter::validate_dns_name(high_entropy_tunnel, &err_tunnel));
  CHECK(err_tunnel.find("entropy nhãn") != std::string::npos);
  CHECK(err_tunnel.find("phát hiện nghi vấn DNS tunneling") != std::string::npos);

  // (e) Tên miền chuẩn tự nhiên (tiếng Việt/Anh) có entropy an toàn
  std::string err_valid;
  CHECK(EgressFilter::validate_dns_name("api.weather.yui.vn", &err_valid));
  CHECK(err_valid.empty());
  CHECK(EgressFilter::validate_dns_name("tin-tuc.trusted-source.vn", &err_valid));

  // 3. Secretless Broker: giữ bí mật ngoài biên guest
  // Broker lưu thông tin xác thực bí mật của nhà cung cấp dịch vụ thời tiết
  const std::string weather_broker_credential = "sample-cred-987654321-xyz";
  broker.register_service_secret("svc:weather", weather_broker_credential);
  CHECK(broker.has_service_secret("svc:weather"));

  // Guest token được cấp quyền net.connect
  Token valid_guest_token = auth.issue("app:weather-service", kRightNetConnect, {"api.weather.yui.vn:443"}, now, 3600);

  // Gửi request hợp lệ qua broker
  BrokeredRequest req_ok;
  req_ok.service_id = "svc:weather";
  req_ok.target_host = "api.weather.yui.vn";
  req_ok.target_port = 443;
  req_ok.path = "/v1/hanoi";
  req_ok.payload = "{\"city\": \"Hanoi\"}";

  const BrokeredResponse resp_ok = broker.forward_request("app:weather-service", &valid_guest_token, filter, req_ok, now + 1);
  CHECK(resp_ok.success);
  CHECK(resp_ok.secret_attached);  // Broker đã gắn bí mật ở ngoài biên
  // Khẳng định: response data trả về cho guest TUYỆT ĐỐI KHÔNG chứa secret!
  CHECK(resp_ok.data.find(weather_broker_credential) == std::string::npos);

  // Kiểm tra nhật ký kiểm toán: Ghi nhận broker gắn xác thực nhưng KHÔNG ĐƯỢC làm lộ chuỗi secret!
  bool audit_found = false;
  for (const auto& a : enforcer.audit()) {
    if (a.action.kind == "broker.forward") {
      audit_found = true;
      CHECK(a.decision.reason.find("[SECRETLESS_BROKER_ATTACHED]") != std::string::npos);
      // Chuỗi bí mật thô tuyệt đối không được xuất hiện trong audit reason
      CHECK(a.decision.reason.find(weather_broker_credential) == std::string::npos);
    }
  }
  CHECK(audit_found);

  // 4. Guest gửi request tới domain không có trong egress allowlist
  BrokeredRequest req_untrusted;
  req_untrusted.service_id = "svc:weather";
  req_untrusted.target_host = "evil-exfiltration-hub.com";
  req_untrusted.target_port = 443;
  const BrokeredResponse resp_blocked = broker.forward_request("app:weather-service", &valid_guest_token, filter, req_untrusted, now + 2);
  CHECK(!resp_blocked.success);
  CHECK(resp_blocked.error_vi.find("ngoài phạm vi cho phép của chính sách") != std::string::npos);

  // 5. Guest không có thẻ năng lực hợp lệ bị broker từ chối ngay lập tức
  Token no_net_token = auth.issue("app:weather-service", kRightFsRead, {}, now, 3600);
  const BrokeredResponse resp_no_perm = broker.forward_request("app:weather-service", &no_net_token, filter, req_ok, now + 3);
  CHECK(!resp_no_perm.success);
  CHECK(resp_no_perm.error_vi.find("thiếu quyền net.connect") != std::string::npos);
}

YUI_TEST(sandbox, trusted_confirmation_anti_clickjacking_p4_13) {
  // P4.13: Kiểm tra đường xác nhận tin cậy chống clickjacking
  Enforcer enforcer;
  TrustedConfirmationHost host(&enforcer);

  // Tạo hộp thoại yêu cầu xác nhận hành vi nhạy cảm
  std::string dlg_id = host.show_dialog(
      "app:photo-editor",
      "Yêu cầu quyền truy cập tệp nhạy cảm",
      "Ứng dụng muốn đọc khóa SSH. Bạn có đồng ý?",
      10000);

  CHECK(!dlg_id.empty());
  CHECK(host.is_active(dlg_id));
  CHECK(!host.is_confirmed(dlg_id));
  CHECK(!host.is_cancelled(dlg_id));

  // Tấn công 1: Click bằng sự kiện tổng hợp (SyntheticApp / synthetic script)
  TaggedInputEvent synth_event;
  synth_event.source = InputSourceKind::SyntheticApp;
  synth_event.hardware_device_id = 0;
  synth_event.x = 150;
  synth_event.y = 210;
  synth_event.is_button_down = true;

  bool delivered = host.handle_input(dlg_id, synth_event, 11000);
  CHECK(!delivered);  // Bị chặn vì là SyntheticApp!
  CHECK(host.is_active(dlg_id));  // Hộp thoại vẫn chưa bị click lừa
  CHECK_EQ(host.synthetic_attacks_blocked(), size_t(1));

  // Tấn công 2: Click phần cứng thật nhưng quá sớm (chưa đủ 500ms cooldown)
  TaggedInputEvent early_event;
  early_event.source = InputSourceKind::PhysicalHardware;
  early_event.hardware_device_id = 1;
  early_event.x = 150;
  early_event.y = 210;
  early_event.is_button_down = true;

  CHECK(!host.handle_input(dlg_id, early_event, 10200));  // 200ms < 500ms
  CHECK(host.is_active(dlg_id));

  // Tấn công 3: Click phần cứng thật sau cooldown nhưng toạ độ nằm ngoài nút
  TaggedInputEvent misaligned_event;
  misaligned_event.source = InputSourceKind::PhysicalHardware;
  misaligned_event.hardware_device_id = 1;
  misaligned_event.x = 50;
  misaligned_event.y = 50;
  misaligned_event.is_button_down = true;

  CHECK(!host.handle_input(dlg_id, misaligned_event, 11000));
  CHECK(host.is_active(dlg_id));

  // Thao tác hợp lệ: Phần cứng thật bấm nút Huỷ (Cancel: 240..360, 200..240)
  TaggedInputEvent valid_cancel;
  valid_cancel.source = InputSourceKind::PhysicalHardware;
  valid_cancel.hardware_device_id = 1;
  valid_cancel.x = 280;
  valid_cancel.y = 220;
  valid_cancel.is_button_down = true;

  CHECK(host.handle_input(dlg_id, valid_cancel, 11000));
  CHECK(!host.is_active(dlg_id));
  CHECK(host.is_cancelled(dlg_id));
  CHECK(!host.is_confirmed(dlg_id));

  // Kiểm tra nhật ký kiểm toán enforcer: Có ghi nhận nỗ lực giả click bị chặn
  bool audit_clickjack = false;
  for (const auto& a : enforcer.audit()) {
    if (a.action.kind == "security.synthetic_click" && !a.decision.allowed) {
      audit_clickjack = true;
      CHECK(a.decision.reason.find("chống clickjacking") != std::string::npos);
    }
  }
  CHECK(audit_clickjack);

  // Tạo hộp thoại thứ 2 và bấm nút Xác nhận thật (Confirm: 100..220, 200..240)
  std::string dlg2_id = host.show_dialog("app:photo-editor", "Yêu cầu", "Đọc ảnh", 20000);
  TaggedInputEvent valid_confirm;
  valid_confirm.source = InputSourceKind::PhysicalHardware;
  valid_confirm.hardware_device_id = 1;
  valid_confirm.x = 150;
  valid_confirm.y = 220;
  valid_confirm.is_button_down = true;

  CHECK(host.handle_input(dlg2_id, valid_confirm, 21000));
  CHECK(!host.is_active(dlg2_id));
  CHECK(host.is_confirmed(dlg2_id));
  CHECK(!host.is_cancelled(dlg2_id));
}

YUI_TEST(sandbox, compatibility_mode_dns_sinkhole_and_session_cap_p3_16) {
  // P3.16: Chế độ tương thích + DNS Sinkhole + Session Capability Grant & Revocation
  // 1. Kiểm thử DNS Sinkhole
  Policy pol;
  pol.scopes = {"api.weather.yui.vn:443", "repo.os.vn:443"};
  EgressFilter filter;
  filter.add_from_policy(pol);

  auto res_allowed = DnsSinkhole::resolve_or_sinkhole("api.weather.yui.vn", filter, 1000);
  CHECK(!res_allowed.sinkholed);
  CHECK_EQ(res_allowed.rcode, 0);
  CHECK(!res_allowed.ip.empty());
  CHECK(res_allowed.latency_ms < 100.0);

  auto res_blocked = DnsSinkhole::resolve_or_sinkhole("adserver.telemetry-tracker.com", filter, 1000);
  CHECK(res_blocked.sinkholed);
  CHECK_EQ(res_blocked.rcode, 3);
  CHECK_EQ(res_blocked.ip, "0.0.0.0");
  CHECK(res_blocked.latency_ms < 100.0);

  // 2. Chế độ tương thích theo ứng dụng (CompatibilityManager)
  Enforcer enf;
  Authority auth("00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff");
  TrustedConfirmationHost host(&enf);
  CompatibilityManager mgr(&enf, &auth, &host);

  // Mặc định TẮT cho toàn bộ ứng dụng
  CHECK(!mgr.is_app_compatibility_enabled("app:demo"));
  CHECK(!mgr.is_app_compatibility_enabled("app:other"));

  // Bật riêng cho app:demo
  mgr.set_app_compatibility("app:demo", true);
  CHECK(mgr.is_app_compatibility_enabled("app:demo"));
  CHECK(!mgr.is_app_compatibility_enabled("app:other"));  // Ứng dụng khác không bị ảnh hưởng

  // Cơ chế chống làm phiền (anti-nuisance throttling):
  // Lần 1 và 2 bị chặn trong cửa sổ 10s -> Chưa đủ ngưỡng 3 lần, không hỏi người dùng
  CHECK(!mgr.on_blocked_request("app:demo", "net.connect", "telemetry.vendor.com", 1000));
  CHECK(!mgr.on_blocked_request("app:demo", "net.connect", "telemetry.vendor.com", 1002));

  // Lần 3 bị chặn tại t=1005 (3 lần trong 10s) -> Đủ điều kiện kích hoạt prompt
  CHECK(mgr.on_blocked_request("app:demo", "net.connect", "telemetry.vendor.com", 1005));
  CHECK_EQ(mgr.prompt_count(), size_t(1));

  // Lần 4 tại t=1008 -> Bị bóp (throttle: tối đa 1 prompt / 5 phút = 300s)
  CHECK(!mgr.on_blocked_request("app:demo", "net.connect", "telemetry.vendor.com", 1008));

  // Thử nghiệm cơ chế yên lặng vĩnh viễn (permanent quiet) sau 3 lần từ chối
  mgr.record_denial("app:demo", 1100);
  mgr.record_denial("app:demo", 1101);
  mgr.record_denial("app:demo", 1102);
  CHECK(mgr.is_permanently_quiet("app:demo"));

  // Khi đã vào trạng thái yên lặng vĩnh viễn, kể cả bị chặn liên tiếp nhiều lần cũng KHÔNG BAO GIỜ hỏi lại
  int64_t future_time = 2000;
  mgr.on_blocked_request("app:demo", "net.connect", "telemetry.vendor.com", future_time);
  mgr.on_blocked_request("app:demo", "net.connect", "telemetry.vendor.com", future_time + 1);
  mgr.on_blocked_request("app:demo", "net.connect", "telemetry.vendor.com", future_time + 2);
  CHECK(!mgr.on_blocked_request("app:demo", "net.connect", "telemetry.vendor.com", future_time + 3));

  // 3. Thẻ phiên (Session Capability) và thu hồi O(1)
  int64_t now_s = 5000;
  mgr.set_app_compatibility("app:office", true);
  TemporaryGrant grant = mgr.grant_session_capability("app:office", "fs.read", "/var/documents/report.pdf", 3600, now_s);
  CHECK_EQ(grant.app_id, "app:office");
  CHECK_EQ(grant.expires_at, now_s + 3600);
  CHECK_EQ(mgr.grant_count(), size_t(1));

  // Thẻ cấp ra có hiệu lực
  std::string verify_err;
  CHECK(auth.verify(grant.token, now_s + 60, &verify_err));

  // Thu hồi tức thì O(1) khi phiên kết thúc hoặc người dùng bấm thu hồi
  mgr.revoke_app("app:office", now_s + 120);

  // Kiểm tra xác thực thẻ sau thu hồi: Phải bị từ chối
  CHECK(!auth.verify(grant.token, now_s + 130, &verify_err));
  CHECK(verify_err.find("đã bị thu hồi") != std::string::npos);
}

YUI_TEST_MAIN("test_sandbox")
