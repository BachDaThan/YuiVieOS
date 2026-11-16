// Bài test Red-Team (P2.13 + SECURITY.md §11): mô phỏng tấn công thật và
// chứng minh ba cơ chế phòng vệ chủ động hoạt động:
//   1. Đào coin ngầm  → Guard phát hiện, hạ Idle, bóp xung, hỏi người dùng;
//   2. Rò rỉ dữ liệu  → EgressFilter chặn đích ngoài allowlist của chính sách;
//   3. Chuỗi quyết định qua yuid: guard.alerts + guard.decide (confirm).
#include <cstring>
#include <string>
#include <vector>

#include "testutil.hpp"
#include "yui/guard.hpp"
#include "yui/power.hpp"
#include "yui/sandbox.hpp"
#include "yui/service.hpp"

using namespace yui;
using service::RpcError;

namespace {

guard::ProcessSample miner_sample(uint64_t pid, int64_t now_ns, double cpu) {
  guard::ProcessSample s;
  s.pid = pid;
  s.name = "xmrig-ngam";
  s.cpu_percent = cpu;
  s.gpu_percent = 0.0;
  s.has_window = false;
  s.caps = 0;
  s.now_ns = now_ns;
  return s;
}

constexpr int64_t kSec = 1000 * 1000 * 1000;

}  // namespace

// ------------------------------------------------- 1. đào coin ngầm -------

YUI_TEST(redteam, hidden_miner_is_detected_after_15s) {
  guard::Guard g;
  // Tiến trình nền 95 % CPU liên tục, không cửa sổ, không CAP_COMPUTE_HEAVY.
  std::optional<guard::Alert> alert;
  for (int64_t t = 0; t <= 20; t += 5) {
    alert = g.observe(miner_sample(4242, t * kSec, 95.0));
    if (alert) break;
  }
  CHECK(bool(alert));
  CHECK(g.is_throttled(4242));
  CHECK(g.verdict(4242) == guard::Verdict::Throttled);
  CHECK(alert->demoted);
  CHECK_EQ(alert->throttle_percent, 20);
  CHECK(alert->reason_vi.find("nghi ngờ") != std::string::npos);
  CHECK(alert->reason_vi.find("CAP_COMPUTE_HEAVY") != std::string::npos);
  CHECK(alert->notify_vi.find("tiêu diệt") != std::string::npos);
  CHECK(alert->notify_vi.find("gỡ bỏ") != std::string::npos);
  CHECK(alert->notify_vi.find("để yên") != std::string::npos);
  CHECK_EQ(g.alerts().size(), size_t(1));
}

YUI_TEST(redteam, miner_alert_fires_only_after_sustained_load) {
  guard::Guard g;
  // 14 s cao rồi tụt xuống: chuỗi vi phạm đứt — không cảnh báo.
  CHECK(!g.observe(miner_sample(7, 0, 95.0)));
  CHECK(!g.observe(miner_sample(7, 14 * kSec, 95.0)));
  CHECK(!g.observe(miner_sample(7, 15 * kSec, 10.0)));
  CHECK(!g.observe(miner_sample(7, 29 * kSec, 95.0)));
  CHECK(!g.observe(miner_sample(7, 30 * kSec, 95.0)));  // mới 15 s tính từ mốc sau
  CHECK(!g.is_throttled(7));
  CHECK(g.observe(miner_sample(7, 44 * kSec, 95.0)));  // đủ 15 s liên tục
  CHECK(g.is_throttled(7));
}

YUI_TEST(redteam, legit_processes_are_untouched) {
  guard::Guard g;
  // Có cửa sổ: không theo dõi — kể cả tải cao.
  guard::ProcessSample w = miner_sample(1, 0, 99.0);
  w.has_window = true;
  for (int64_t t = 0; t <= 30; t += 10) {
    w.now_ns = t * kSec;
    CHECK(!g.observe(w));
  }
  CHECK(!g.is_throttled(1));
  // Mang quyền CAP_COMPUTE_HEAVY: được phép tính toán nặng.
  guard::ProcessSample cap = miner_sample(2, 0, 99.0);
  cap.caps = sandbox::kRightComputeHeavy;
  for (int64_t t = 0; t <= 30; t += 10) {
    cap.now_ns = t * kSec;
    CHECK(!g.observe(cap));
  }
  CHECK(!g.is_throttled(2));
}

YUI_TEST(redteam, throttled_miner_is_clock_gated_and_demoted) {
  guard::Guard g;
  power::ClockGate gates;  // yui-power: bóp xung theo lệnh Guard
  std::optional<guard::Alert> alert;
  for (int64_t t = 0; t <= 20 && !alert; t += 5) alert = g.observe(miner_sample(99, t * kSec, 92.0));
  CHECK(bool(alert));
  gates.throttle_task(99, alert->throttle_percent);
  CHECK_EQ(gates.throttle_percent(99), 20);  // hạ xung nhịp còn 20 %
  CHECK(alert->demoted);                     // hạ xuống lớp Idle/Background
  // Người dùng chọn "để yên" ⇒ tháo bóp.
  CHECK(g.clear(99));
  gates.release_task(99);
  CHECK(!g.is_throttled(99));
  CHECK_EQ(gates.throttle_percent(99), 100);
}

// --------------------------------------------- 2. rò rỉ dữ liệu -----------

YUI_TEST(redteam, egress_is_default_deny) {
  sandbox::EgressFilter f;  // không policy nào: không đường ra nào
  const auto d = f.check({"api.trithuc.vn", "", 443});
  CHECK(!d.allowed);
  CHECK(d.reason_vi.find("mặc định chặn") != std::string::npos);
}

YUI_TEST(redteam, egress_allows_only_policy_scopes) {
  sandbox::Policy policy;
  policy.app_id = "app.exfil";          // ứng dụng bị nghi vấn
  policy.tier = sandbox::Tier::Isolated;
  policy.granted = sandbox::kRightNetConnect;
  policy.scopes = {"net:api.trithuc.vn:443", "net:*.trithuc.vn", "net:10.20.0.0/16"};
  sandbox::EgressFilter f;
  f.add_from_policy(policy);

  // Trong allowlist: đi được.
  CHECK(f.check({"api.trithuc.vn", "", 443}).allowed);
  CHECK(f.check({"dl.trithuc.vn", "", 443}).allowed);  // wildcard *.trithuc.vn
  CHECK(f.check({"", "10.20.30.40", 0}).allowed);      // CIDR trong chính sách

  // Ngoài allowlist: chặn — kể cả "gần giống" được phép.
  CHECK(!f.check({"evil.example", "", 443}).allowed);
  CHECK(!f.check({"api.trithuc.vn.evil.example", "", 443}).allowed);
  CHECK(!f.check({"", "8.8.8.8", 53}).allowed);          // ngoài CIDR

  // Cổng bị chặn khi rule CHỈ định cổng (wildcard "*.trithuc.vn" là mọi cổng).
  sandbox::Policy strict;
  strict.granted = sandbox::kRightNetConnect;
  strict.scopes = {"net:api.trithuc.vn:443"};
  sandbox::EgressFilter f_strict;
  f_strict.add_from_policy(strict);
  CHECK(f_strict.check({"api.trithuc.vn", "", 443}).allowed);
  CHECK(!f_strict.check({"api.trithuc.vn", "", 22}).allowed);
  const auto bad = f.check({"collector.attacker.io", "", 443});
  CHECK(!bad.allowed);
  CHECK(bad.reason_vi.find("chặn") != std::string::npos);
  CHECK(bad.reason_vi.find("cô lập egress") != std::string::npos);
}

YUI_TEST(redteam, egress_deny_overrides_allow) {
  sandbox::Policy policy;
  policy.granted = sandbox::kRightNetConnect;
  policy.scopes = {"net:*.trithuc.vn"};
  policy.deny = {"net:mat.trithuc.vn"};  // trừ một miền cụ thể
  sandbox::EgressFilter f;
  f.add_from_policy(policy);
  CHECK(f.check({"tot.trithuc.vn", "", 443}).allowed);
  const auto d = f.check({"mat.trithuc.vn", "", 443});
  CHECK(!d.allowed);
  CHECK(d.reason_vi.find("deny") != std::string::npos);
}

YUI_TEST(redteam, cidr_matching_is_exact_on_prefix) {
  CHECK(sandbox::EgressFilter::ipv4_in_cidr("10.20.30.40", "10.20.0.0/16"));
  CHECK(sandbox::EgressFilter::ipv4_in_cidr("10.20.255.255", "10.20.0.0/16"));
  CHECK(!sandbox::EgressFilter::ipv4_in_cidr("10.21.0.1", "10.20.0.0/16"));
  CHECK(!sandbox::EgressFilter::ipv4_in_cidr("192.168.1.1", "10.20.0.0/16"));
  CHECK(sandbox::EgressFilter::ipv4_in_cidr("1.2.3.4", "1.2.3.4"));  // /32 ngầm
  CHECK(!sandbox::EgressFilter::ipv4_in_cidr("không-phải-ip", "1.2.3.0/24"));
}

YUI_TEST(redteam, exfiltration_is_blocked_at_the_sandbox_too) {
  // Lớp năng lực: token cấp net.connect CHỈ cho api.trithuc.vn:443 —
  // lời gọi tới đích khác bị Enforcer từ chối và có audit.
  // (Policy::from_json cắt tiền tố "net:" — scopes ở dạng đã cắt.)
  sandbox::Policy policy;
  policy.app_id = "app.exfil";
  policy.tier = sandbox::Tier::Isolated;
  policy.granted = sandbox::kRightNetConnect;
  policy.scopes = {"api.trithuc.vn:443"};
  sandbox::Enforcer enforcer;
  sandbox::Action ok{ "net.connect", "api.trithuc.vn:443" };
  sandbox::Action leak{ "net.connect", "collector.attacker.io:443" };
  const auto d1 = enforcer.check(policy, nullptr, ok, 1000);
  const auto d2 = enforcer.check(policy, nullptr, leak, 1001);
  CHECK(d1.allowed);
  CHECK(!d2.allowed);
  CHECK(d2.reason.find("phạm vi") != std::string::npos);
  CHECK_EQ(enforcer.audit().size(), size_t(2));  // cả hai đều vào sổ audit
}

// ------------------------------------- 3. chuỗi quyết định qua yuid --------

YUI_TEST(redteam, yuid_guard_flow_alert_then_confirmed_kill) {
  service::Options opts;
  opts.state_dir = "/tmp/yui-redteam-state";
  service::Daemon d(opts);
  RpcError err;

  // Nạp mẫu đào coin: cảnh báo phải nổi lên sau 15 s tải cao.
  for (int64_t t = 0; t <= 20; t += 5) {
    json::Value p(json::Object{});
    p.set("pid", int64_t(4242));
    p.set("name", "xmrig-ngam");
    p.set("cpu_percent", 95.0);
    p.set("now_ns", t * kSec);
    d.handle("guard.observe", p, err);
    CHECK_EQ(err.code, 0);
  }
  json::Value alerts = d.handle("guard.alerts", json::Value(json::Object{}), err);
  CHECK_EQ(alerts["count"].as_int(), int64_t(1));
  CHECK(alerts["alerts"].arr()[0]["notify_vi"].as_string().find("tiêu diệt") !=
        std::string::npos);

  // Quyết định thiếu confirm: bị chặn với needs_confirmation.
  json::Value dec(json::Object{});
  dec.set("pid", int64_t(4242));
  dec.set("action", "kill");
  json::Value r = d.handle("guard.decide", dec, err);
  CHECK_EQ(err.code, service::kDenied);
  CHECK(err.message.find("needs_confirmation") != std::string::npos);
  CHECK(err.data["needs_confirmation"].as_bool());
  CHECK_EQ(err.data["actions"].arr().size(), size_t(3));  // kill / remove / keep

  // Xác nhận: tiêu diệt + audit.
  const size_t audit_before = d.enforcer().audit().size();
  dec.set("confirm", true);
  err = RpcError{};
  r = d.handle("guard.decide", dec, err);
  CHECK_EQ(err.code, 0);
  CHECK(r["done"].as_bool());
  CHECK_EQ(r["action"].as_string(), std::string("kill"));
  CHECK(r["reason_vi"].as_string().find("tiêu diệt") != std::string::npos);
  CHECK_EQ(d.enforcer().audit().size(), audit_before + 1);
  CHECK(!d.guard().is_throttled(4242));
}

YUI_TEST(redteam, yuid_guard_keep_releases_throttle) {
  service::Options opts;
  opts.state_dir = "/tmp/yui-redteam-state";
  service::Daemon d(opts);
  RpcError err;
  for (int64_t t = 0; t <= 20; t += 5) {
    json::Value p(json::Object{});
    p.set("pid", int64_t(7));
    p.set("name", "trinh-thu");
    p.set("gpu_percent", 88.0);
    p.set("now_ns", t * kSec);
    d.handle("guard.observe", p, err);
  }
  CHECK(d.guard().is_throttled(7));
  json::Value dec(json::Object{});
  dec.set("pid", int64_t(7));
  dec.set("action", "keep");
  dec.set("confirm", true);
  const json::Value r = d.handle("guard.decide", dec, err);
  CHECK_EQ(err.code, 0);
  CHECK(r["done"].as_bool());
  CHECK(!d.guard().is_throttled(7));
  CHECK(r["reason_vi"].as_string().find("để yên") != std::string::npos);
}

YUI_TEST_MAIN("test_redteam")
