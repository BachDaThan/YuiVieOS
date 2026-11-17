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

YUI_TEST_MAIN("test_redteam")
