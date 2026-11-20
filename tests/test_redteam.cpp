// Bài test Red-Team (P2.13 + SECURITY.md §11): mô phỏng tấn công thật và
// chứng minh ba cơ chế phòng vệ chủ động hoạt động:
//   1. Đào coin ngầm  → Guard phát hiện, hạ Idle, bóp xung, hỏi người dùng;
//   2. Rò rỉ dữ liệu  → EgressFilter chặn đích ngoài allowlist của chính sách;
//   3. Chuỗi quyết định qua yuid: guard.alerts + guard.decide (confirm).
#include <cstring>
#include <string>
#include <vector>

#include "testutil.hpp"
#include "yui/crypto.hpp"
#include "yui/fsstore.hpp"
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

// --------------------------------- 4. phát hiện ransomware & rollback 10.000 tệp (P2.14) --

YUI_TEST(redteam, shannon_entropy_distinguishes_plaintext_from_ciphertext) {
  // 1. Dữ liệu toàn 0 -> entropy = 0.0
  std::vector<uint8_t> zeros(1024, 0);
  CHECK_EQ(guard::shannon_entropy(zeros.data(), zeros.size()), 0.0);

  // 2. Dữ liệu văn bản -> entropy ~ 3.5 .. 5.5
  const std::string text = "YuiVieOS Beta la he dieu hanh chu quyen toi gian thiet ke cho nguoi Viet.";
  const double text_ent = guard::shannon_entropy(reinterpret_cast<const uint8_t*>(text.data()), text.size());
  CHECK(text_ent > 3.0 && text_ent < 5.5);

  // 3. Dữ liệu giả lập mã hoá (pseudo-random / ciphertext) -> entropy > 7.5
  std::vector<uint8_t> cipher(1024);
  uint32_t state = 0x12345678;
  for (size_t i = 0; i < cipher.size(); ++i) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    cipher[i] = uint8_t(state & 0xFF);
  }
  const double cipher_ent = guard::shannon_entropy(cipher.data(), cipher.size());
  CHECK(cipher_ent > 7.5);
}

YUI_TEST(redteam, ransomware_attack_10000_files_detected_and_rolled_back) {
  // Chuẩn bị kho lưu trữ CoW
  fsstore::CowStore store;
  constexpr size_t kFileCount = 10000;
  std::vector<std::string> paths;
  paths.reserve(kFileCount);
  std::vector<crypto::Digest32> original_hashes;
  original_hashes.reserve(kFileCount);

  // 1. Tạo 10.000 tệp ban đầu với dữ liệu hợp lệ và băm SHA-256 từng tệp
  for (size_t i = 0; i < kFileCount; ++i) {
    std::string path = "docs/file_" + std::to_string(i) + ".dat";
    paths.push_back(path);
    std::string content = "YuiVieOS-File-Content-#" + std::to_string(i) + " -- Du lieu nguoi dung an toan";
    std::vector<uint8_t> data(content.begin(), content.end());
    original_hashes.push_back(crypto::sha256(data));
    store.write_file(path, std::move(data));
  }
  CHECK_EQ(store.file_count(), kFileCount);

  // 2. Chụp snapshot điểm giao dịch sạch ban đầu (pre-attack)
  double elapsed_ms = -1.0;
  const std::string snap_id = store.create_snapshot("pre-attack-clean", 1000, &elapsed_ms);
  CHECK(store.has_snapshot(snap_id));
  CHECK(elapsed_ms >= 0.0);  // số thật đo được

  // Kiểm tra bù trừ dung lượng ban đầu: toàn bộ 10.000 tệp được chia sẻ qua CoW
  auto acc_before = store.compute_accounting();
  CHECK_EQ(acc_before.file_count, kFileCount);
  CHECK(acc_before.shared_bytes > 0);
  CHECK_EQ(acc_before.exclusive_bytes, size_t(0));

  // 3. Mô phỏng mã độc tống tiền (Ransomware PID 666) tấn công ghi đè 10.000 tệp bằng ciphertext
  guard::RansomwareDetector detector;
  std::optional<guard::RansomwareAlert> fired_alert;

  uint32_t rng = 0xDEADBEEF;
  for (size_t i = 0; i < kFileCount; ++i) {
    // Sinh khối dữ liệu entropy cao (ciphertext mã hoá 512 byte)
    std::vector<uint8_t> encrypted_payload(512);
    for (size_t b = 0; b < encrypted_payload.size(); ++b) {
      rng ^= rng << 13;
      rng ^= rng >> 17;
      rng ^= rng << 5;
      encrypted_payload[b] = uint8_t(rng & 0xFF);
    }

    // Ghi đè vào file store
    store.write_file(paths[i], encrypted_payload);

    // Báo sự kiện ghi cho Guard
    guard::RansomwareEvent ev;
    ev.pid = 666;
    ev.proc_name = "ransomware.exe";
    ev.file_path = paths[i];
    ev.data = encrypted_payload.data();
    ev.size = encrypted_payload.size();
    ev.now_ns = 2000 + int64_t(i) * 1000000LL;  // ghi nhanh trong cửa sổ

    auto alert = detector.observe_write(ev, snap_id);
    if (alert && !fired_alert) {
      fired_alert = alert;
    }
  }

  // 4. Kiểm tra Guard phát hiện mã độc tống tiền
  CHECK(bool(fired_alert));
  CHECK_EQ(fired_alert->pid, uint64_t(666));
  CHECK_EQ(fired_alert->proc_name, std::string("ransomware.exe"));
  CHECK(fired_alert->avg_entropy > 7.2);
  CHECK(fired_alert->reason_vi.find("mã độc tống tiền") != std::string::npos);
  CHECK(fired_alert->notify_vi.find("hoàn tác (rollback)") != std::string::npos);
  CHECK(detector.is_throttled(666));

  // Xác nhận các tệp trong store hiện đã bị hỏng/mã hoá (hash không khớp)
  std::vector<uint8_t> sample;
  CHECK(store.read_file(paths[0], &sample));
  CHECK(crypto::sha256(sample) != original_hashes[0]);

  // 5. Người dùng đồng ý kích hoạt Hoàn tác (Rollback) về snapshot sạch
  std::string err;
  CHECK(store.rollback_to(snap_id, &err));

  // 6. Kiểm tra lại toàn bộ 10.000 tệp: băm SHA-256 khớp 100% không sót một tệp nào!
  size_t restored_matches = 0;
  for (size_t i = 0; i < kFileCount; ++i) {
    std::vector<uint8_t> file_buf;
    if (store.read_file(paths[i], &file_buf) && crypto::sha256(file_buf) == original_hashes[i]) {
      ++restored_matches;
    }
  }
  CHECK_EQ(restored_matches, kFileCount);
  CHECK_EQ(store.file_count(), kFileCount);

  // Kiểm tra bù trừ dung lượng sau rollback
  auto acc_after = store.compute_accounting();
  CHECK_EQ(acc_after.exclusive_bytes, size_t(0));
  CHECK_EQ(acc_after.shared_bytes, acc_before.shared_bytes);
}

// --------------------------------- 5. Guard rảnh rỗi: 6 kịch bản Red-Team (P2.15) --

YUI_TEST(redteam, idle_guard_6_scenarios) {
  guard::IdleConfig cfg;
  cfg.idle_timeout_ns = 300LL * kSec;  // 300 s (5 phút)
  cfg.throttle_percent = 20;
  guard::IdleGuard idle_guard(cfg);

  // Kịch bản 1: Máy rảnh (> 300 s), ứng dụng nền mở micro ghi âm
  // -> Guard phát hiện, bóp xung còn 20%, thu hồi token badge O(1), phát cảnh báo.
  idle_guard.record_user_activity(0);  // người dùng thao tác ở t = 0
  guard::IdleActivitySample s1;
  s1.pid = 1001;
  s1.proc_name = "spy-mic";
  s1.resource = guard::SensitiveResource::Microphone;
  s1.has_window = false;
  s1.user_initiated = false;
  s1.token_badge = 0xAA01;
  s1.now_ns = 350LL * kSec;  // đã rảnh 350 s > 300 s

  auto alert1 = idle_guard.observe(s1);
  CHECK(bool(alert1));
  CHECK_EQ(alert1->pid, uint64_t(1001));
  CHECK_EQ(alert1->resource, guard::SensitiveResource::Microphone);
  CHECK(alert1->reason_vi.find("microphone") != std::string::npos);
  CHECK(alert1->notify_vi.find("350 giây") != std::string::npos);
  CHECK(idle_guard.is_badge_revoked(0xAA01));  // O(1) tra cứu thu hồi thẻ
  CHECK(idle_guard.is_throttled(1001));

  // Kịch bản 2: Người dùng đang thao tác (active), ứng dụng mở camera (video call)
  // -> Cho phép hợp lệ, không cảnh báo, không bóp xung.
  idle_guard.record_user_activity(360LL * kSec);  // người dùng vừa gõ phím ở t = 360 s
  guard::IdleActivitySample s2;
  s2.pid = 1002;
  s2.proc_name = "video-call";
  s2.resource = guard::SensitiveResource::Camera;
  s2.has_window = true;
  s2.user_initiated = true;
  s2.token_badge = 0xAA02;
  s2.now_ns = 365LL * kSec;  // mới trôi qua 5 s < 300 s

  auto alert2 = idle_guard.observe(s2);
  CHECK(!bool(alert2));
  CHECK(!idle_guard.is_badge_revoked(0xAA02));
  CHECK(!idle_guard.is_throttled(1002));

  // Kịch bản 3: Máy rảnh, ứng dụng nền đọc danh bạ / file nhạy cảm
  // -> Bị chặn, thu hồi thẻ O(1).
  guard::IdleActivitySample s3;
  s3.pid = 1003;
  s3.proc_name = "contacts-stealer";
  s3.resource = guard::SensitiveResource::Contacts;
  s3.has_window = false;
  s3.user_initiated = false;
  s3.token_badge = 0xAA03;
  s3.now_ns = 700LL * kSec;  // đã rảnh 700 - 360 = 340 s > 300 s

  auto alert3 = idle_guard.observe(s3);
  CHECK(bool(alert3));
  CHECK_EQ(alert3->resource, guard::SensitiveResource::Contacts);
  CHECK(idle_guard.is_badge_revoked(0xAA03));
  CHECK(idle_guard.is_throttled(1003));

  // Kịch bản 4: Máy rảnh, ứng dụng nền gửi gói tin ra mạng
  // -> Bị chặn, thu hồi thẻ O(1).
  guard::IdleActivitySample s4;
  s4.pid = 1004;
  s4.proc_name = "exfil-net";
  s4.resource = guard::SensitiveResource::Network;
  s4.has_window = false;
  s4.user_initiated = false;
  s4.token_badge = 0xAA04;
  s4.now_ns = 720LL * kSec;

  auto alert4 = idle_guard.observe(s4);
  CHECK(bool(alert4));
  CHECK_EQ(alert4->resource, guard::SensitiveResource::Network);
  CHECK(idle_guard.is_badge_revoked(0xAA04));
  CHECK(idle_guard.is_throttled(1004));

  // Kịch bản 5: Tiến trình có cửa sổ hoạt động (has_window) hoặc do người dùng chủ động (user_initiated)
  // kể cả khi máy đã lâu không có thao tác bàn phím (ví dụ: trình xem phim/nghe nhạc đang chạy)
  // -> Cho phép, không cảnh báo.
  guard::IdleActivitySample s5;
  s5.pid = 1005;
  s5.proc_name = "music-player";
  s5.resource = guard::SensitiveResource::Network;
  s5.has_window = true;  // cửa sổ đang mở
  s5.user_initiated = true;
  s5.token_badge = 0xAA05;
  s5.now_ns = 800LL * kSec;

  auto alert5 = idle_guard.observe(s5);
  CHECK(!bool(alert5));
  CHECK(!idle_guard.is_badge_revoked(0xAA05));
  CHECK(!idle_guard.is_throttled(1005));

  // Kịch bản 6: Tải nhấp nháy / thao tác chập chờn (người dùng chạm phím ở giây 1000 trước ngưỡng 300)
  // -> Không kích hoạt giả mạo ở giây 1005 (vì mới chỉ rảnh 5 s); nhưng khi không thao tác đến giây 1350
  // (> 300 s kể từ 1000) thì kích hoạt chính xác.
  idle_guard.record_user_activity(1000LL * kSec);  // thao tác lúc 1000 s

  guard::IdleActivitySample s6_early;
  s6_early.pid = 1006;
  s6_early.proc_name = "bg-sync";
  s6_early.resource = guard::SensitiveResource::Microphone;
  s6_early.has_window = false;
  s6_early.user_initiated = false;
  s6_early.token_badge = 0xAA06;
  s6_early.now_ns = 1005LL * kSec;  // 1005 - 1000 = 5 s < 300 s

  auto alert6_early = idle_guard.observe(s6_early);
  CHECK(!bool(alert6_early));  // Không bị báo nhầm
  CHECK(!idle_guard.is_badge_revoked(0xAA06));

  guard::IdleActivitySample s6_late;
  s6_late.pid = 1006;
  s6_late.proc_name = "bg-sync";
  s6_late.resource = guard::SensitiveResource::Microphone;
  s6_late.has_window = false;
  s6_late.user_initiated = false;
  s6_late.token_badge = 0xAA06;
  s6_late.now_ns = 1350LL * kSec;  // 1350 - 1000 = 350 s > 300 s -> Đủ điều kiện rảnh

  auto alert6_late = idle_guard.observe(s6_late);
  CHECK(bool(alert6_late));
  CHECK(idle_guard.is_badge_revoked(0xAA06));
  CHECK(idle_guard.is_throttled(1006));

  // Thử clear gỡ bóp xung sau khi người dùng cho phép
  CHECK(idle_guard.clear(1006));
  CHECK(!idle_guard.is_throttled(1006));
}

// ------------------------------------------------------------- P3.13 red-team --
// Tấn công giả lập: Bộ gõ bên thứ ba (IBus/Fcitx5 plugin giả) cố đánh cắp
// mật khẩu, ghi lén nhật ký phím và mở socket exfiltration ra C2 server.
namespace {
class EvilKeyloggerImeEngine : public sandbox::IThirdPartyImeEngine {
 public:
  const std::string& app_id() const override { return id_; }
  std::string compose_key(uint32_t codepoint) override {
    ++intercepted_keystrokes_;
    stolen_keystrokes_ += static_cast<char>(codepoint);
    return std::string(1, static_cast<char>(codepoint));
  }
  void reset() override { stolen_keystrokes_.clear(); }
  size_t keystrokes_received() const override { return intercepted_keystrokes_; }
  const std::string& stolen() const { return stolen_keystrokes_; }

 private:
  std::string id_ = "ime:evil-keylogger-plugin";
  std::string stolen_keystrokes_;
  size_t intercepted_keystrokes_ = 0;
};
}  // namespace

YUI_TEST(redteam, third_party_ime_keylogger_attack_is_thwarted_and_audited) {
  EvilKeyloggerImeEngine evil_engine;
  sandbox::Enforcer enforcer;
  const int64_t now = 1'700'000'000;

  // IME độc hại bị cô lập trong hộp cát với chính sách chuẩn:
  // Chỉ quyền input.compose, không net, chỉ fs riêng
  const sandbox::Policy policy = sandbox::ThirdPartyImeHost::create_default_policy(
      "ime:evil-keylogger-plugin", "/home/user/.config/evil-ime");
  sandbox::ThirdPartyImeHost host(&evil_engine, policy, &enforcer);

  // Giai đoạn 1: Người dùng gõ mật khẩu ngân hàng và mã PIN
  const std::string sensitive_inputs[] = {
      "P@ssw0rd2026!",  // Password
      "123456",         // PIN
      "9876543210",     // OTP / DigitsOnly
  };

  for (char c : sensitive_inputs[0]) {
    host.process_keystroke(c, sandbox::InputFieldKind::Password, now);
  }
  for (char c : sensitive_inputs[1]) {
    host.process_keystroke(c, sandbox::InputFieldKind::Pin, now);
  }
  for (char c : sensitive_inputs[2]) {
    host.process_keystroke(c, sandbox::InputFieldKind::DigitsOnly, now);
  }

  // Kết quả: Mã độc nhận đúng 0 phím từ tất cả các trường nhạy cảm!
  CHECK_EQ(evil_engine.keystrokes_received(), size_t(0));
  CHECK(evil_engine.stolen().empty());
  CHECK_EQ(host.suppressed_keystrokes_count(),
           sensitive_inputs[0].size() + sensitive_inputs[1].size() + sensitive_inputs[2].size());

  // Giai đoạn 2: Người dùng gõ văn bản thông thường ("Xin chao Yui")
  const std::string text = "Xin chao";
  for (char c : text) {
    host.process_keystroke(c, sandbox::InputFieldKind::NormalText, now);
  }
  CHECK_EQ(evil_engine.keystrokes_received(), text.size());
  CHECK_EQ(evil_engine.stolen(), text);

  // Giai đoạn 3: Mã độc cố mở socket gửi dữ liệu về C2
  const sandbox::Decision d_c2 = host.check_engine_action(
      {"net.connect", "c2.evil-stealer.org:443"}, now);
  CHECK(!d_c2.allowed);
  CHECK(d_c2.reason.find("chính sách không cấp quyền") != std::string::npos);

  // Giai đoạn 4: Mã độc cố đọc /home để tìm SSH private key
  const sandbox::Decision d_ssh = host.check_engine_action(
      {"fs.read", "/home/user/.ssh/id_ed25519"}, now);
  CHECK(!d_ssh.allowed);
  CHECK(d_ssh.reason.find("không bao phủ") != std::string::npos);

  // Giai đoạn 5: Mã độc cố ghi nhật ký phím lén vào /home/user/keystrokes.log
  const sandbox::Decision d_log = host.check_engine_action(
      {"fs.write", "/home/user/keystrokes.log"}, now);
  CHECK(!d_log.allowed);
  CHECK(d_log.reason.find("không bao phủ") != std::string::npos);

  // Giai đoạn 6: Mã độc cố nạp module nhân hoặc leo thang quyền
  const sandbox::Decision d_mod = host.check_engine_action(
      {"kernel.module", "/tmp/rootkit.ko"}, now);
  CHECK(!d_mod.allowed);

  // Toàn bộ các nỗ lực xâm phạm đều được ghi nhận vào nhật ký kiểm toán (audit log)
  CHECK(enforcer.audit().size() > 0);
  size_t denied_audits = 0;
  size_t allowed_audits = 0;
  for (const auto& a : enforcer.audit()) {
    CHECK_EQ(a.subject, std::string("ime:evil-keylogger-plugin"));
    if (!a.decision.allowed) {
      ++denied_audits;
    } else {
      ++allowed_audits;
    }
    CHECK(!a.decision.reason.empty());
  }
  // 8 phím văn bản thường được phép
  CHECK_EQ(allowed_audits, text.size());
  // Mọi phím nhạy cảm và mọi hành động xâm phạm (mạng, đọc trộm ssh, ghi trộm file...) đều bị chặn
  CHECK(denied_audits >= 29 + 4);
}

// ------------------------------------------------ 4. Lớp 1 Capability Boundary & O(1) Revocation -------

YUI_TEST(redteam, layer1_capability_boundary_derivation_o1_revocation_and_anomaly) {
  const std::string master_key = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
  sandbox::Authority auth(master_key);
  sandbox::Enforcer enforcer;
  const int64_t now = 1700000000;

  // 1. Cấp thẻ gốc (Root Token) cho phiên làm việc Alpha
  const std::string fam_alpha = "session-alpha-42";
  const uint64_t root_rights = sandbox::kRightFsRead | sandbox::kRightFsWrite | sandbox::kRightNetConnect;
  const std::vector<std::string> root_scopes = {"/data", "api.yui.vn"};
  const sandbox::Token root_token = auth.issue("app:session-alpha", root_rights, root_scopes, now, 3600, 0, fam_alpha);

  CHECK_EQ(root_token.subject, std::string("app:session-alpha"));
  CHECK_EQ(root_token.family_id, fam_alpha);
  CHECK_EQ(root_token.generation, 0u);
  CHECK(auth.verify(root_token, now));

  // Kiểm tra tính toàn vẹn Bearer token serialization / deserialization
  const std::string bearer = auth.bearer(root_token);
  sandbox::Token parsed_tok;
  CHECK(auth.from_bearer(bearer, parsed_tok, now));
  CHECK_EQ(parsed_tok.subject, root_token.subject);
  CHECK_EQ(parsed_tok.family_id, fam_alpha);
  CHECK_EQ(parsed_tok.generation, 0u);
  CHECK(auth.verify(parsed_tok, now));

  // 2. Dẫn xuất thẻ con (Capability Attenuation / Derivation)
  // Con 1: Thu hẹp quyền chỉ đọc và thu hẹp phạm vi con /data/sub
  const sandbox::Token child1 = auth.derive(root_token, "app:session-alpha:worker", sandbox::kRightFsRead, {"/data/sub"}, now + 10, 1800);
  CHECK_EQ(child1.family_id, fam_alpha);
  CHECK_EQ(child1.generation, 0u);
  CHECK_EQ(child1.rights, sandbox::kRightFsRead);
  CHECK(auth.verify(child1, now + 10));

  // Con 2: Thu hẹp quyền kết nối mạng tới endpoint cụ thể
  const sandbox::Token child2 = auth.derive(root_token, "app:session-alpha:net", sandbox::kRightNetConnect, {"api.yui.vn:443"}, now + 10, 1800);
  CHECK_EQ(child2.family_id, fam_alpha);
  CHECK(auth.verify(child2, now + 10));

  // 3. Thử nghiệm xin vượt quyền: derive tự động tước (attenuate) các bit không có ở thẻ cha
  const sandbox::Token evil_child = auth.derive(root_token, "app:session-alpha:evil", sandbox::kRightAdmin | sandbox::kRightFsRead, {"/data"}, now + 10, 1800);
  CHECK((evil_child.rights & sandbox::kRightAdmin) == 0);
  CHECK_EQ(evil_child.rights, sandbox::kRightFsRead);

  // 4. Tạo phiên làm việc Beta độc lập (unrelated family)
  const std::string fam_beta = "session-beta-99";
  const sandbox::Token beta_token = auth.issue("app:session-beta", sandbox::kRightNetConnect, {"update.yui.vn"}, now, 3600, 0, fam_beta);
  CHECK(auth.verify(beta_token, now));

  // 5. Kiểm tra quyền qua Enforcer trước khi phát hiện bất thường
  sandbox::Policy policy_alpha;
  policy_alpha.app_id = "app:session-alpha:worker";
  policy_alpha.tier = sandbox::Tier::Isolated;
  policy_alpha.granted = root_rights;
  policy_alpha.scopes = {"/data", "api.yui.vn"};

  // Thẻ con 1 đọc trong phạm vi hợp lệ -> cho phép
  const sandbox::Decision dec1 = enforcer.check(policy_alpha, &child1, {"fs.read", "/data/sub/report.txt"}, now + 20, &auth);
  CHECK(dec1.allowed);

  // Thẻ con 1 cố ghi -> bị từ chối do thẻ con thiếu quyền ghi
  const sandbox::Decision dec2 = enforcer.check(policy_alpha, &child1, {"fs.write", "/data/sub/report.txt"}, now + 20, &auth);
  CHECK(!dec2.allowed);
  CHECK(dec2.reason.find("thiếu quyền") != std::string::npos);

  // 6. Phát hiện bất thường Lớp 1 (Layer 1 Anomaly Detection)
  // Enforcer phát hiện tiến trình thuộc session-alpha cố gọi syscall ngoài ranh giới
  const bool anomaly_detected = enforcer.detect_anomaly(
      auth, fam_alpha, "app:session-alpha", "syscall_boundary_violation", "attempted unbrokered ptrace to host pid 1", now + 30);
  CHECK(anomaly_detected);

  // Xác nhận nhật ký kiểm toán ghi nhận bất thường kèm lý do tiếng Việt
  CHECK(!enforcer.audit().empty());
  const auto& last_audit = enforcer.audit().back();
  CHECK_EQ(last_audit.subject, std::string("app:session-alpha"));
  CHECK_EQ(last_audit.action.kind, std::string("anomaly.syscall_boundary_violation"));
  CHECK(last_audit.decision.reason.find("Phát hiện bất thường Lớp 1") != std::string::npos);
  CHECK(last_audit.decision.reason.find("Tự động thu hồi họ token") != std::string::npos);

  // 7. Xác nhận thu hồi bắc cầu O(1) theo epoch (Transitive O(1) Revocation)
  // Toàn bộ họ session-alpha (cả gốc và tất cả con) bị vô hiệu hoá ngay lập tức
  CHECK(auth.is_family_revoked(fam_alpha, root_token.generation));
  CHECK(auth.is_family_revoked(fam_alpha, child1.generation));
  CHECK(auth.is_family_revoked(fam_alpha, child2.generation));

  std::string verify_err;
  CHECK(!auth.verify(root_token, now + 35, &verify_err));
  CHECK(verify_err.find("họ thẻ năng lực đã bị thu hồi") != std::string::npos);

  CHECK(!auth.verify(child1, now + 35, &verify_err));
  CHECK(verify_err.find("họ thẻ năng lực đã bị thu hồi") != std::string::npos);

  CHECK(!auth.verify(child2, now + 35, &verify_err));
  CHECK(verify_err.find("họ thẻ năng lực đã bị thu hồi") != std::string::npos);

  // Lần kiểm tra tiếp theo của Enforcer trên token con lập tức từ chối và ghi log
  const sandbox::Decision dec_after = enforcer.check(policy_alpha, &child1, {"fs.read", "/data/sub/report.txt"}, now + 35, &auth);
  CHECK(!dec_after.allowed);
  CHECK(dec_after.reason.find("thẻ năng lực không hợp lệ") != std::string::npos);
  CHECK(dec_after.reason.find("họ thẻ năng lực đã bị thu hồi") != std::string::npos);

  // Quan trọng: Phiên làm việc khác (session-beta) hoàn toàn không bị ảnh hưởng!
  CHECK(!auth.is_family_revoked(fam_beta, beta_token.generation));
  CHECK(auth.verify(beta_token, now + 35));

  sandbox::Policy policy_beta;
  policy_beta.app_id = "app:session-beta";
  policy_beta.tier = sandbox::Tier::Isolated;
  policy_beta.granted = sandbox::kRightNetConnect;
  policy_beta.scopes = {"update.yui.vn"};

  const sandbox::Decision dec_beta = enforcer.check(policy_beta, &beta_token, {"net.connect", "update.yui.vn:443"}, now + 35, &auth);
  CHECK(dec_beta.allowed);
}

// --------------------------------- 7. Red-Team tích hợp ba lớp phòng thủ (P3.11) --

YUI_TEST(redteam, three_layer_integrated_defense_containment) {
  sandbox::Enforcer enforcer;
  const std::string master_k = "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789";
  sandbox::Authority auth(master_k);
  sandbox::MicroVmSupervisor supervisor(&enforcer, &auth);
  sandbox::SecretlessBroker broker(&enforcer, &auth);

  const int64_t now = 1700000000;
  const std::string fam_target = "fam-microvm-agent";
  const std::string fam_other = "fam-microvm-daemon";

  // Thiết lập 2 micro-VM
  sandbox::Policy pol_target;
  pol_target.version = 1;
  pol_target.app_id = "app:agent-worker";
  pol_target.tier = sandbox::Tier::MicroVm;
  pol_target.granted = sandbox::kRightNetConnect | sandbox::kRightFsRead;
  pol_target.scopes = {"api.target-hub.yui.vn:443"};
  pol_target.allow_east_west = false;
  pol_target.allow_direct_dns = false;

  sandbox::Policy pol_other;
  pol_other.version = 1;
  pol_other.app_id = "app:other-worker";
  pol_other.tier = sandbox::Tier::MicroVm;
  pol_other.granted = sandbox::kRightNetConnect;
  pol_other.scopes = {"service.yui.vn:443"};

  auto* session_target = supervisor.create_session("vm-target-1", "app:agent-worker", pol_target, fam_target, now);
  auto* session_other = supervisor.create_session("vm-other-2", "app:other-worker", pol_other, fam_other, now);

  CHECK(session_target != nullptr);
  CHECK(session_other != nullptr);
  CHECK_EQ(supervisor.active_count(), size_t(2));

  // Cấp token gốc và token con
  sandbox::Token root_target = auth.issue("app:agent-worker", sandbox::kRightNetConnect | sandbox::kRightFsRead,
                                          {"api.target-hub.yui.vn:443"}, now, 3600, 0, fam_target);
  sandbox::Token child_target = auth.derive(root_target, "app:agent-sub", sandbox::kRightNetConnect,
                                            {"api.target-hub.yui.vn:443"}, now, 1800);

  // [LỚP 1 + LỚP 2]: Phát hiện bất thường Lớp 1 -> Revoke O(1) + Freeze/Huỷ Lớp 2
  const bool anomaly = enforcer.detect_anomaly(
      auth, fam_target, "app:agent-worker", "vm_kvm_escape_ioctl", "unauthorized hypercall bypass attempt", now + 10);
  CHECK(anomaly);

  // Kiểm tra Lớp 1 đã thu hồi tức thì toàn bộ họ token theo generation
  CHECK(auth.is_family_revoked(fam_target, root_target.generation));
  CHECK(auth.is_family_revoked(fam_target, child_target.generation));
  std::string rev_err;
  CHECK(!auth.verify(child_target, now + 11, &rev_err));
  CHECK(rev_err.find("họ thẻ năng lực đã bị thu hồi") != std::string::npos);

  // Lớp 2 phản ứng: chuyển running -> frozen -> destroyed
  const bool l2_handled = supervisor.on_layer1_anomaly(fam_target, "anomaly detected by L1", now + 12);
  CHECK(l2_handled);

  CHECK_EQ(session_target->state(), sandbox::MicroVmState::Destroyed);
  CHECK_EQ(supervisor.active_count(), size_t(1));

  // Mọi request kế tiếp từ guest tới phiên bị huỷ đều fail-closed trước khi xử lý
  const sandbox::Decision dec_blocked = supervisor.dispatch_guest_request("vm-target-1", {"net.connect", "api.target-hub.yui.vn:443"}, now + 13, &child_target);
  CHECK(!dec_blocked.allowed);
  CHECK(dec_blocked.reason.find("đã bị tiêu huỷ (Destroyed)") != std::string::npos);

  // Phiên khác (vm-other-2) không bị ảnh hưởng, vẫn Running
  CHECK_EQ(session_other->state(), sandbox::MicroVmState::Running);

  // [LỚP 3]: Egress Zero-Trust, Anti-DNS-Tunneling, Secretless Broker
  sandbox::EgressFilter egress;
  egress.add_from_policy(pol_target);

  // 1. Chặn peer-to-peer / East-West giữa các sandbox
  sandbox::EgressRequest r_p2p;
  r_p2p.domain = "vm-other-2.local";
  r_p2p.port = 8080;
  r_p2p.is_east_west = true;
  const sandbox::EgressDecision d_p2p = egress.check(r_p2p);
  CHECK(!d_p2p.allowed);
  CHECK(d_p2p.reason_vi.find("chặn kết nối ngang (east-west)") != std::string::npos);

  // 2. Chặn truy vấn DNS trực tiếp ra cổng 53
  sandbox::EgressRequest r_dns;
  r_dns.ip = "1.1.1.1";
  r_dns.port = 53;
  const sandbox::EgressDecision d_dns = egress.check(r_dns);
  CHECK(!d_dns.allowed);
  CHECK(d_dns.reason_vi.find("chặn DNS trực tiếp (cổng 53)") != std::string::npos);

  // 3. Chặn DNS Tunnel: nhãn dài > 63 octet
  const std::string malformed_long = std::string(64, 'x') + ".tunnel.net";
  std::string err_long;
  CHECK(!sandbox::EgressFilter::validate_dns_name(malformed_long, &err_long));
  CHECK(err_long.find("vượt quá 63 octet") != std::string::npos);

  // 4. Chặn DNS Tunnel: payload entropy cao (> 4.2 bit/char)
  const std::string high_entropy_tunnel = "w9k1p4z7x2b8m5c3v0q.exfil.org";
  std::string err_h;
  CHECK(!sandbox::EgressFilter::validate_dns_name(high_entropy_tunnel, &err_h));
  CHECK(err_h.find("entropy nhãn") != std::string::npos);

  // 5. Chặn kết nối tới domain ngoài allowlist
  sandbox::EgressRequest r_disallowed;
  r_disallowed.domain = "malicious-c2.org";
  r_disallowed.port = 443;
  const sandbox::EgressDecision d_c2 = egress.check(r_disallowed);
  CHECK(!d_c2.allowed);
  CHECK(d_c2.reason_vi.find("ngoài phạm vi cho phép") != std::string::npos);

  // 6. Tên miền hợp lệ đi qua EgressFilter bình thường (không bị regression)
  std::string err_valid;
  CHECK(sandbox::EgressFilter::validate_dns_name("api.target-hub.yui.vn", &err_valid));
  CHECK(err_valid.empty());

  sandbox::EgressRequest r_valid;
  r_valid.domain = "api.target-hub.yui.vn";
  r_valid.port = 443;
  const sandbox::EgressDecision d_valid = egress.check(r_valid);
  CHECK(d_valid.allowed);
  CHECK(d_valid.reason_vi.find("khớp allowlist") != std::string::npos);

  // 7. Broker giữ bí mật ngoài biên, gắn xác thực cho request hợp lệ
  const std::string mock_upstream_cred = "mock-auth-token-sample-12345";
  broker.register_service_secret("svc:hub", mock_upstream_cred);

  // Cấp token cho other worker để test request hợp lệ
  sandbox::Token valid_other_token = auth.issue("app:other-worker", sandbox::kRightNetConnect,
                                                {"service.yui.vn:443"}, now + 20, 3600, 0, fam_other);

  sandbox::EgressFilter egress_other;
  egress_other.add_from_policy(pol_other);

  sandbox::BrokeredRequest b_req;
  b_req.service_id = "svc:hub";
  b_req.target_host = "service.yui.vn";
  b_req.target_port = 443;
  b_req.path = "/api/v1/ping";
  b_req.payload = "{}";

  const sandbox::BrokeredResponse b_resp = broker.forward_request("app:other-worker", &valid_other_token, egress_other, b_req, now + 21);
  CHECK(b_resp.success);
  CHECK(b_resp.secret_attached);
  CHECK(b_resp.data.find(mock_upstream_cred) == std::string::npos);

  // 8. Toàn bộ kịch bản kiểm toán ghi lại đầy đủ allow và deny
  bool has_allow_audit = false;
  bool has_deny_audit = false;
  for (const auto& a : enforcer.audit()) {
    if (a.decision.allowed) has_allow_audit = true;
    else has_deny_audit = true;
    CHECK(!a.decision.reason.empty());
    // Bí mật không bao giờ lọt vào audit
    CHECK(a.decision.reason.find(mock_upstream_cred) == std::string::npos);
  }
  CHECK(has_allow_audit);
  CHECK(has_deny_audit);
}

YUI_TEST(redteam, clickjacking_and_compatibility_evasion_attempts) {
  // Redteam Scenario: Kẻ tấn công cố tình vượt rào chống giả click (clickjacking)
  // và lạm dụng cơ chế tương thích (compatibility mode)
  sandbox::Enforcer enforcer;
  sandbox::Authority auth("fedcba98765432100123456789abcdef0123456789abcdef0123456789abcdef");
  sandbox::TrustedConfirmationHost conf_host(&enforcer);
  sandbox::CompatibilityManager compat_mgr(&enforcer, &auth, &conf_host);

  // 1. Thử tấn công giả lập click bằng chuỗi 50 sự kiện nhân tạo (SyntheticApp)
  std::string dlg_id = conf_host.show_dialog(
      "app:malicious-app",
      "Cấp quyền kết nối ra máy chủ C2?",
      "Cấp quyền kết nối ra máy chủ C2?",
      50000);

  CHECK(!dlg_id.empty());
  CHECK(conf_host.is_active(dlg_id));

  for (int i = 0; i < 50; ++i) {
    sandbox::TaggedInputEvent ev;
    ev.source = sandbox::InputSourceKind::SyntheticApp;
    ev.hardware_device_id = 0;
    ev.x = 150;
    ev.y = 220;
    ev.is_button_down = true;
    CHECK(!conf_host.handle_input(dlg_id, ev, 50100 + i * 10));  // Toàn bộ 50 sự kiện giả mạo đều bị chặn đứng
  }
  CHECK(conf_host.is_active(dlg_id));  // Hộp thoại không hề bị xác nhận tự động
  CHECK_EQ(conf_host.synthetic_attacks_blocked(), size_t(50));

  // 2. Thử tấn công làm phiền (Nuisance spamming DoS): Gửi dồn dập hàng loạt kết nối bị chặn
  // nhằm làm tràn giao diện xác nhận (UI prompt flooding)
  for (int i = 0; i < 100; ++i) {
    compat_mgr.on_blocked_request("app:malicious-app", "net.connect", "evil.c2.com", 60000 + i);
  }
  // Nhờ cơ chế bóp xung (throttle) và yên lặng, số lần hỏi người dùng bị giới hạn nghiêm ngặt
  CHECK(compat_mgr.prompt_count() <= 1);

  // Sau khi người dùng bấm từ chối 3 lần:
  compat_mgr.record_denial("app:malicious-app", 65000);
  compat_mgr.record_denial("app:malicious-app", 65001);
  compat_mgr.record_denial("app:malicious-app", 65002);
  CHECK(compat_mgr.is_permanently_quiet("app:malicious-app"));

  // Kẻ tấn công spam tiếp 50 lần nữa -> 0 prompt mới nào được phép sinh ra
  size_t prompts_before = compat_mgr.prompt_count();
  for (int i = 0; i < 50; ++i) {
    compat_mgr.on_blocked_request("app:malicious-app", "net.connect", "evil.c2.com", 70000 + i);
  }
  CHECK_EQ(compat_mgr.prompt_count(), prompts_before);

  // 3. Thử tái sử dụng thẻ phiên đã bị thu hồi (Revoked session capability reuse)
  int64_t now_s = 80000;
  compat_mgr.set_app_compatibility("app:untrusted-plugin", true);
  auto grant = compat_mgr.grant_session_capability("app:untrusted-plugin", "net.connect", "192.168.1.50:80", 300, now_s);

  // Thẻ ban đầu hợp lệ
  std::string v_err;
  CHECK(auth.verify(grant.token, now_s + 10, &v_err));

  // Kẻ tấn công kích hoạt hành vi bất thường -> Thu hồi tức thời O(1)
  compat_mgr.revoke_app("app:untrusted-plugin", now_s + 15);

  // Thử tái sử dụng thẻ -> Thẻ bị từ chối ngay lập tức tại cổng xác thực
  CHECK(!auth.verify(grant.token, now_s + 20, &v_err));
  CHECK(v_err.find("đã bị thu hồi") != std::string::npos);
}

// ============================================================================
// P2.21 — nhãn nguồn RemoteAuthorized / GuestBluetooth: điều khiển app được,
// KHÔNG BAO GIỜ bấm được hộp thoại xác nhận tin cậy.
// ============================================================================

namespace {

sandbox::TaggedInputEvent click(sandbox::InputSourceKind src, int x, int y) {
  sandbox::TaggedInputEvent ev;
  ev.source = src;
  ev.x = x;
  ev.y = y;
  ev.is_button_down = true;
  if (src == sandbox::InputSourceKind::PhysicalHardware) ev.hardware_device_id = 0x1001;
  if (src == sandbox::InputSourceKind::RemoteAuthorized) ev.remote_session_id = 42;
  return ev;
}

size_t count_audit(const sandbox::Enforcer& enf, const std::string& kind, bool allowed) {
  size_t n = 0;
  for (const auto& e : enf.audit())
    if (e.action.kind == kind && e.decision.allowed == allowed) ++n;
  return n;
}

}  // namespace

YUI_TEST(redteam, remote_authorized_input_never_confirms_trusted_dialog) {
  using sandbox::InputPrivilege;
  using sandbox::InputSourceKind;
  // Bảng quyền: chỉ thiết bị vật lý (mã thiết bị ≠ 0) mới xác nhận được.
  CHECK(sandbox::input_privilege(click(InputSourceKind::PhysicalHardware, 0, 0)) ==
        InputPrivilege::TrustedConfirmation);
  CHECK(sandbox::input_privilege(click(InputSourceKind::RemoteAuthorized, 0, 0)) == InputPrivilege::AppControl);
  CHECK(sandbox::input_privilege(click(InputSourceKind::GuestBluetooth, 0, 0)) == InputPrivilege::AppControl);
  CHECK(sandbox::input_privilege(click(InputSourceKind::SyntheticApp, 0, 0)) == InputPrivilege::None);
  auto unbound = click(InputSourceKind::RemoteAuthorized, 0, 0);
  unbound.remote_session_id = 0;  // không gắn phiên đã cấp thẻ ⇒ không quyền gì
  CHECK(sandbox::input_privilege(unbound) == InputPrivilege::None);
  auto fake_phys = click(InputSourceKind::PhysicalHardware, 0, 0);
  fake_phys.hardware_device_id = 0;
  CHECK(!sandbox::counts_as_trusted_confirmation(fake_phys));
  auto weird = click(InputSourceKind::PhysicalHardware, 0, 0);
  weird.source = static_cast<InputSourceKind>(7);  // giá trị lạ từ dây ⇒ không quyền gì
  CHECK(sandbox::input_privilege(weird) == InputPrivilege::None);
  CHECK(sandbox::may_drive_application(click(InputSourceKind::RemoteAuthorized, 1, 1)));
  CHECK(sandbox::may_drive_application(click(InputSourceKind::GuestBluetooth, 1, 1)));
  CHECK(!sandbox::may_drive_application(click(InputSourceKind::SyntheticApp, 1, 1)));
  CHECK_EQ(std::string(sandbox::input_source_name(InputSourceKind::RemoteAuthorized)), std::string("RemoteAuthorized"));

  sandbox::Enforcer enforcer;
  sandbox::TrustedConfirmationHost host(&enforcer);
  const std::string dlg = host.show_dialog("store:org.example.app", "Cài ứng dụng?", "Cài ứng dụng?", 1000);
  // 50 cú bấm từ phiên từ xa đã cấp thẻ, đúng toạ độ nút Xác nhận, sau thời gian chờ.
  for (int i = 0; i < 50; ++i) CHECK(!host.handle_input(dlg, click(InputSourceKind::RemoteAuthorized, 150, 220), 2000 + i));
  // 10 cú bấm từ bàn phím/chuột Bluetooth đi qua ngăn xếp trong khách.
  for (int i = 0; i < 10; ++i) CHECK(!host.handle_input(dlg, click(InputSourceKind::GuestBluetooth, 150, 220), 3000 + i));
  CHECK(!host.handle_input(dlg, weird, 3100));
  CHECK(host.is_active(dlg));
  CHECK(!host.is_confirmed(dlg));
  CHECK_EQ(host.remote_attempts_blocked(), size_t(60));
  CHECK_EQ(host.synthetic_attacks_blocked(), size_t(61));
  CHECK_EQ(count_audit(enforcer, "security.remote_confirm", false), size_t(60));
  bool reason_ok = false;
  for (const auto& e : enforcer.audit())
    if (e.action.kind == "security.remote_confirm" && e.decision.reason.find("RemoteAuthorized") != std::string::npos)
      reason_ok = true;
  CHECK(reason_ok);

  // Thiết bị vật lý vẫn xác nhận được như cũ (P4.13 không đổi).
  CHECK(host.handle_input(dlg, click(InputSourceKind::PhysicalHardware, 150, 220), 4000));
  CHECK(host.is_confirmed(dlg));
  CHECK_EQ(count_audit(enforcer, "security.physical_confirm", true), size_t(1));
}

// ============================================================================
// P2.20 — thẻ giữ tỉnh session.inhibit: ngoại lệ hẹp, có hạn, qua xác nhận
// tin cậy, đèn báo do host vẽ là điều kiện.
// ============================================================================

namespace {

struct Indicators {
  bool working = true;          // compositor vẽ được đèn không
  bool on[4] = {false, false, false, false};
  int draws = 0;
  guard::IndicatorRenderer renderer() {
    return [this](guard::SensitiveResource r, bool want) {
      ++draws;
      if (!working && want) {
        on[static_cast<int>(r)] = false;
        return false;
      }
      on[static_cast<int>(r)] = want;
      return true;
    };
  }
};

guard::IdleActivitySample bg(uint64_t pid, const std::string& sb, guard::SensitiveResource r, int64_t now) {
  guard::IdleActivitySample s;
  s.pid = pid;
  s.proc_name = "may-chu-game";
  s.sandbox_id = sb;
  s.resource = r;
  s.has_window = false;
  s.user_initiated = false;
  s.token_badge = 0xB000 + pid;
  s.now_ns = now;
  return s;
}

// Host hiện hộp thoại cho đúng yêu cầu rồi người dùng bấm bằng chuột thật.
std::string confirm_physically(sandbox::TrustedConfirmationHost& host, const std::string& target,
                               sandbox::InputSourceKind src = sandbox::InputSourceKind::PhysicalHardware) {
  const std::string id = host.show_dialog(target, "Giữ tỉnh dịch vụ nền?", "Cho phép chạy nền khi máy rảnh?", 1000);
  host.handle_input(id, click(src, 150, 220), 2000);
  return id;
}

}  // namespace

YUI_TEST(redteam, session_inhibit_card_scenarios) {
  using guard::SensitiveResource;
  sandbox::Enforcer enforcer;
  sandbox::TrustedConfirmationHost host(&enforcer);
  guard::IdleConfig cfg;
  guard::IdleGuard ig(cfg);
  Indicators lamp;
  ig.set_audit(&enforcer);
  ig.set_indicator_renderer(lamp.renderer());
  ig.record_user_activity(0);
  const int64_t t0 = 400 * kSec;  // đã rảnh 400 s > 300 s

  // (1) Dịch vụ nền không cửa sổ, không thẻ ⇒ vẫn bị chặn (quy tắc P2.15 không đổi).
  CHECK(bool(ig.observe(bg(500, "sb:server", SensitiveResource::Network, t0))));
  CHECK(ig.is_throttled(500));
  CHECK(ig.is_badge_revoked(0xB000 + 500));
  ig.clear(500);

  // (4) Ứng dụng tự xin thẻ, không qua hộp thoại ⇒ từ chối + audit.
  guard::InhibitRequest req;
  req.pid = 501;
  req.sandbox_id = "sb:server";
  req.proc_name = "may-chu-game";
  req.resources = guard::resource_bit(SensitiveResource::Network);
  auto r4 = ig.grant_inhibit(req, nullptr, "", t0);
  CHECK(!r4.ok);
  CHECK(r4.reason_vi.find("xác nhận tin cậy") != std::string::npos);
  auto r4b = ig.grant_inhibit(req, &host, "dlg-không-có", t0);  // mã hộp thoại bịa
  CHECK(!r4b.ok);
  // Hộp thoại có thật nhưng chỉ bị bấm bởi phiên từ xa (P2.21) ⇒ chưa xác nhận ⇒ từ chối.
  const std::string remote_dlg =
      confirm_physically(host, guard::inhibit_dialog_target(req, cfg), sandbox::InputSourceKind::RemoteAuthorized);
  CHECK(!host.is_confirmed(remote_dlg));
  CHECK(!ig.grant_inhibit(req, &host, remote_dlg, t0).ok);
  CHECK_EQ(ig.inhibit_count(), size_t(0));
  CHECK(count_audit(enforcer, "session.inhibit.grant", false) >= size_t(3));

  // (2) Thẻ chỉ-mạng qua xác nhận vật lý ⇒ mạng chạy, micro vẫn bị chặn.
  const std::string dlg = confirm_physically(host, guard::inhibit_dialog_target(req, cfg));
  auto r2 = ig.grant_inhibit(req, &host, dlg, t0);
  CHECK(r2.ok);
  CHECK(r2.reason_vi.find("240 phút") != std::string::npos);  // mặc định 4 giờ
  CHECK(lamp.on[static_cast<int>(SensitiveResource::Network)]);
  CHECK(ig.indicator_lit(SensitiveResource::Network));
  CHECK(!lamp.on[static_cast<int>(SensitiveResource::Microphone)]);
  CHECK(!bool(ig.observe(bg(501, "sb:server", SensitiveResource::Network, t0 + kSec))));
  CHECK(!ig.is_throttled(501));
  CHECK(bool(ig.observe(bg(501, "sb:server", SensitiveResource::Microphone, t0 + 2 * kSec))));  // không tự kèm micro
  CHECK(ig.is_throttled(501));
  ig.clear(501);
  // Thẻ gắn đúng tiến trình + hộp cát: tiến trình khác hay hộp cát khác không dùng ké được.
  CHECK(bool(ig.observe(bg(502, "sb:server", SensitiveResource::Network, t0 + 3 * kSec))));
  CHECK(bool(ig.observe(bg(501, "sb:khac", SensitiveResource::Network, t0 + 3 * kSec))));
  ig.clear(501);
  ig.clear(502);
  // Một lần xác nhận chỉ cấp một thẻ.
  CHECK(!ig.grant_inhibit(req, &host, dlg, t0).ok);
  // Hộp thoại xác nhận cho "mạng" không dùng được để xin "mạng + camera".
  guard::InhibitRequest wider = req;
  wider.resources |= guard::resource_bit(SensitiveResource::Camera);
  const std::string dlg_net = confirm_physically(host, guard::inhibit_dialog_target(req, cfg));
  CHECK(!ig.grant_inhibit(wider, &host, dlg_net, t0).ok);
  // Không có thẻ vĩnh viễn: thời hạn vượt trần 4 giờ ⇒ từ chối.
  guard::InhibitRequest forever = req;
  forever.duration_ns = 5LL * 3600 * kSec;
  const std::string dlg_forever = confirm_physically(host, guard::inhibit_dialog_target(forever, cfg));
  CHECK(!ig.grant_inhibit(forever, &host, dlg_forever, t0).ok);
  guard::InhibitRequest none = req;
  none.resources = 0;
  CHECK(!ig.grant_inhibit(none, &host, confirm_physically(host, guard::inhibit_dialog_target(none, cfg)), t0).ok);

  // (3) Thẻ hết hạn ⇒ thu hồi, quy tắc cũ áp lại, đèn tắt.
  const int64_t expiry = t0 + cfg.inhibit_default_ns;
  CHECK(!bool(ig.observe(bg(501, "sb:server", SensitiveResource::Network, expiry - kSec))));
  CHECK(bool(ig.observe(bg(501, "sb:server", SensitiveResource::Network, expiry))));
  CHECK_EQ(ig.inhibit_count(), size_t(0));
  CHECK(!lamp.on[static_cast<int>(SensitiveResource::Network)]);
  CHECK(count_audit(enforcer, "session.inhibit.revoke", true) >= size_t(1));
  ig.clear(501);

  // (5) Thẻ micro: đèn báo là điều kiện. Host không vẽ được đèn ⇒ không cấp;
  //     đèn tắt giữa chừng ⇒ thẻ không có hiệu lực ⇒ micro bị chặn.
  guard::InhibitRequest mic = req;
  mic.pid = 600;
  mic.duration_ns = 3600 * kSec;
  mic.resources = guard::resource_bit(SensitiveResource::Microphone);
  lamp.working = false;
  CHECK(!ig.grant_inhibit(mic, &host, confirm_physically(host, guard::inhibit_dialog_target(mic, cfg)), t0).ok);
  lamp.working = true;
  auto r5 = ig.grant_inhibit(mic, &host, confirm_physically(host, guard::inhibit_dialog_target(mic, cfg)), t0);
  CHECK(r5.ok);
  CHECK(lamp.on[static_cast<int>(SensitiveResource::Microphone)]);
  CHECK(!bool(ig.observe(bg(600, "sb:server", SensitiveResource::Microphone, t0 + kSec))));
  lamp.working = false;  // compositor mất đèn
  CHECK(bool(ig.observe(bg(600, "sb:server", SensitiveResource::Microphone, t0 + 2 * kSec))));
  CHECK(ig.is_throttled(600));
  CHECK(!ig.indicator_lit(SensitiveResource::Microphone));
  CHECK(count_audit(enforcer, "session.inhibit.indicator_missing", false) >= size_t(1));
  lamp.working = true;
  ig.clear(600);

  // Dịch vụ tắt ⇒ thu hồi thẻ của tiến trình đó; đổi người dùng ⇒ thu hồi hết.
  CHECK_EQ(ig.revoke_inhibits_for_pid(600, "dịch vụ đã tắt", t0 + 3 * kSec), size_t(1));
  CHECK(!lamp.on[static_cast<int>(SensitiveResource::Microphone)]);
  guard::InhibitRequest a = req, b = req;
  a.pid = 701;
  b.pid = 702;
  CHECK(ig.grant_inhibit(a, &host, confirm_physically(host, guard::inhibit_dialog_target(a, cfg)), t0).ok);
  CHECK(ig.grant_inhibit(b, &host, confirm_physically(host, guard::inhibit_dialog_target(b, cfg)), t0).ok);
  CHECK_EQ(ig.revoke_all_inhibits("đổi người dùng", t0 + 4 * kSec), size_t(2));
  CHECK_EQ(ig.inhibit_count(), size_t(0));
  CHECK(bool(ig.observe(bg(701, "sb:server", SensitiveResource::Network, t0 + 5 * kSec))));

  // (6) 50 yêu cầu thẻ giả liên tiếp (không hộp thoại / bấm tổng hợp / bấm từ xa) ⇒ đều bị chặn.
  const size_t refused_before = ig.inhibit_requests_refused();
  for (int i = 0; i < 50; ++i) {
    guard::InhibitRequest f = req;
    f.pid = 800 + static_cast<uint64_t>(i);
    std::string id;
    if (i % 3 == 1) id = confirm_physically(host, guard::inhibit_dialog_target(f, cfg), sandbox::InputSourceKind::SyntheticApp);
    if (i % 3 == 2) id = confirm_physically(host, guard::inhibit_dialog_target(f, cfg), sandbox::InputSourceKind::RemoteAuthorized);
    CHECK(!ig.grant_inhibit(f, &host, id, t0).ok);
  }
  CHECK_EQ(ig.inhibit_requests_refused() - refused_before, size_t(50));
  CHECK_EQ(ig.inhibit_count(), size_t(0));
}

YUI_TEST_MAIN("test_redteam")
