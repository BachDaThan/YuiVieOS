// Vault, graph and engine tests.
#include <cmath>
#include <random>

#include "testutil.hpp"
#include "yui/engine.hpp"
#include "yui/graph.hpp"
#include "yui/profile.hpp"
#include "yui/vault.hpp"

using namespace yui;
using Bytes = std::vector<uint8_t>;

// ------------------------------------------------------------------ vault --
static vault::Params fast_params() {
  vault::Params p;
  p.log2_n = 10;  // 1024 -> quick in tests; production default is 2^15
  p.r = 8;
  p.p = 1;
  return p;
}

YUI_TEST(vault, roundtrip) {
  const std::string secret = "Mật khẩu mở kho: 42 — YuiVieOS";
  const Bytes blob = vault::seal("khẩu-lệnh-2026", secret, fast_params());
  CHECK(vault::is_vault(blob));
  CHECK_MSG(blob.size() > secret.size(), "sealed blob must include a 74-byte header");

  std::string out;
  std::string err;
  CHECK(vault::open_string("khẩu-lệnh-2026", blob, out, &err));
  CHECK_EQ(out, secret);
}

YUI_TEST(vault, wrong_password_and_tamper) {
  const Bytes blob = vault::seal("dung-mat-khau", std::string("payload"), fast_params());
  Bytes out;
  std::string err;
  CHECK(!vault::open("sai-mat-khau", blob, out, &err));
  CHECK_MSG(err.find("wrong password") != std::string::npos, "expected auth error, got: " + err);

  // Flip a ciphertext byte.
  Bytes t = blob;
  t.back() ^= 0x40;
  CHECK(!vault::open("dung-mat-khau", t, out, &err));

  // Flip a header byte (salt) — parameters and salt are authenticated too.
  Bytes t2 = blob;
  t2[12] ^= 0x01;
  CHECK(!vault::open("dung-mat-khau", t2, out, &err));

  // Truncate.
  Bytes t3(blob.begin(), blob.end() - 1);
  CHECK(!vault::open("dung-mat-khau", t3, out, &err));
}

YUI_TEST(vault, param_downgrade_is_detected) {
  const Bytes blob = vault::seal("pw", std::string("abc"), fast_params());
  Bytes out;
  std::string err;
  // An attacker flipping log2_n to 1 must not be able to make us run a weak KDF
  // and accept the result: the header is inside the MAC.
  Bytes t = blob;
  t[5] = 4;
  CHECK(!vault::open("pw", t, out, &err));
  // Out-of-range parameters are refused before any KDF work happens.
  Bytes t2 = blob;
  t2[5] = 30;
  CHECK(!vault::open("pw", t2, out, &err));
  CHECK_MSG(err.find("KDF parameters") != std::string::npos, "got: " + err);
}

YUI_TEST(vault, nonce_makes_ciphertexts_unique) {
  const std::string msg = "same plaintext every time";
  const Bytes a = vault::seal("pw", msg, fast_params());
  const Bytes b = vault::seal("pw", msg, fast_params());
  CHECK_MSG(a != b, "two seals of the same plaintext must differ (random salt+nonce)");
  std::string out;
  CHECK(vault::open_string("pw", a, out));
  CHECK(vault::open_string("pw", b, out));
  CHECK_EQ(out, msg);
}

YUI_TEST(vault, empty_plaintext_and_peek) {
  const Bytes blob = vault::seal("pw", Bytes{}, fast_params());
  Bytes out;
  CHECK(vault::open("pw", blob, out));
  CHECK_EQ(out.size(), size_t(0));
  vault::Params p;
  CHECK(vault::peek_params(blob, p));
  CHECK_EQ(int(p.log2_n), 10);
  CHECK_MSG(!vault::is_vault(Bytes{1, 2, 3}), "random bytes are not a vault blob");
}

// ------------------------------------------------------------------ graph --
static graph::Graph sample_graph() {
  graph::Graph g;
  g.upsert("os:yui", "YuiVieOS", graph::NodeKind::Concept);
  g.upsert("tier:5", "Tầng 5 — Giao diện", graph::NodeKind::Concept);
  g.upsert("app:yui", "App Yui", graph::NodeKind::App);
  g.upsert("app:trim", "Trình Gọt Tỉa", graph::NodeKind::App);
  g.upsert("cpu:t480", "Intel i5-8250U", graph::NodeKind::Device);
  g.link("os:yui", "has_tier", "tier:5");
  g.link("app:yui", "runs_on", "os:yui");
  g.link("app:trim", "runs_on", "os:yui");
  g.link("app:yui", "depends_on", "app:trim", 0.8);
  g.link("os:yui", "measured_on", "cpu:t480");
  return g;
}

YUI_TEST(graph, upsert_link_and_stats) {
  graph::Graph g = sample_graph();
  const auto st = g.stats();
  CHECK_EQ(st.nodes, size_t(5));
  CHECK_EQ(st.edges, size_t(5));
  CHECK_EQ(st.kinds[size_t(graph::NodeKind::App)], size_t(2));

  CHECK_MSG(!g.upsert("os:yui", "YuiVieOS 2", graph::NodeKind::Concept), "second upsert updates, not creates");
  CHECK_EQ(g.find("os:yui")->label, std::string("YuiVieOS 2"));

  CHECK_MSG(!g.link("os:yui", "x", "missing:node"), "linking to a missing node fails");
  CHECK_MSG(!g.link("os:yui", "", "tier:5"), "empty relationship is rejected");
}

YUI_TEST(graph, traversal) {
  graph::Graph g = sample_graph();
  const auto nb = g.neighbours("app:yui");
  CHECK_EQ(nb.size(), size_t(2));
  const auto deps = g.neighbours("app:yui", "depends_on");
  CHECK_EQ(deps.size(), size_t(1));
  CHECK_EQ(deps[0], std::string("app:trim"));

  const auto reach = g.bfs("app:trim", 2);
  CHECK_EQ(reach.size(), size_t(4));  // trim -> os -> tier:5, cpu, app:yui
  CHECK_EQ(reach[0], std::string("app:trim"));

  const auto path = g.shortest_path("app:trim", "cpu:t480");
  CHECK_EQ(path.size(), size_t(3));
  CHECK_EQ(path.front(), std::string("app:trim"));
  CHECK_EQ(path.back(), std::string("cpu:t480"));
  CHECK(g.shortest_path("app:trim", "nope").empty());
}

YUI_TEST(graph, search_and_remove) {
  graph::Graph g = sample_graph();
  const auto hits = g.search("TRÌNH");
  CHECK_EQ(hits.size(), size_t(1));
  CHECK_EQ(hits[0], std::string("app:trim"));
  const auto apps = g.search("", true, graph::NodeKind::App);
  CHECK_EQ(apps.size(), size_t(2));

  CHECK(g.remove("app:trim"));
  CHECK_MSG(!g.find("app:trim"), "removed node is gone");
  CHECK_EQ(g.stats().edges, size_t(3));  // both edges touching app:trim disappear
  CHECK(!g.remove("app:trim"));
}

YUI_TEST(graph, json_roundtrip) {
  graph::Graph g = sample_graph();
  const std::string doc = g.dump();
  graph::Graph h;
  std::string err;
  CHECK(h.from_json(json::parse(doc).value, &err));
  CHECK_EQ(h.stats().nodes, g.stats().nodes);
  CHECK_EQ(h.stats().edges, g.stats().edges);
  CHECK_EQ(h.find("app:yui")->label, std::string("App Yui"));
  CHECK_EQ(h.shortest_path("app:trim", "cpu:t480").size(), size_t(3));

  graph::Graph bad;
  CHECK(!bad.from_json(json::parse(R"({"format":"other/9","nodes":[]})").value, &err));
  CHECK(!bad.from_json(json::parse(R"({"nodes":[{"label":"no id"}]})").value, &err));
  CHECK(!bad.from_json(json::parse(R"({"nodes":[],"edges":[{"from":"a","rel":"r","to":"b"}]})").value, &err));
}

// ----------------------------------------------------------------- engine --
YUI_TEST(engine, detect_simd_is_sane) {
  const auto info = engine::detect_simd();
  CHECK_MSG(!info.arch.empty(), "arch must be reported");
  CHECK_MSG(!info.best.empty(), "best ISA must be reported");
  CHECK_MSG(info.vector_bits >= 64, "vector width must be at least 64 bits");
  CHECK_MSG(engine::dot_kernel_name() == "avx2" || engine::dot_kernel_name() == "scalar",
            "dot kernel must be avx2 or scalar");
}

YUI_TEST(engine, q4_roundtrip_error_bound) {
  std::mt19937 rng(42);
  std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
  std::vector<float> x(320);
  for (auto& v : x) v = dist(rng);

  std::vector<engine::Q4Block> blocks;
  engine::quantize_q4(x.data(), x.size(), blocks);
  CHECK_EQ(blocks.size(), size_t(10));

  std::vector<float> back(x.size());
  engine::dequantize_q4(blocks, back.data(), back.size());
  float max_err = 0.0f;
  for (size_t i = 0; i < x.size(); ++i) max_err = std::max(max_err, std::fabs(back[i] - x[i]));
  // 4-bit symmetric quantisation with 32-value blocks: error <= scale/2.
  CHECK_MSG(max_err <= 0.08f, "q4 max abs error too large: " + std::to_string(max_err));

  // Exact representation of a zero vector.
  std::vector<float> zeros(64, 0.0f);
  std::vector<engine::Q4Block> zb;
  engine::quantize_q4(zeros.data(), zeros.size(), zb);
  std::vector<float> zback(64);
  engine::dequantize_q4(zb, zback.data(), zback.size());
  for (float v : zback) CHECK(std::fabs(v) < 1e-9f);
}

YUI_TEST(engine, dot_q4_matches_fp32_reference) {
  std::mt19937 rng(7);
  std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
  const size_t n = 32 * 50;
  std::vector<float> w(n), y(n), wq(n);
  for (auto& v : w) v = dist(rng);
  for (auto& v : y) v = dist(rng);

  std::vector<engine::Q4Block> blocks;
  engine::quantize_q4(w.data(), n, blocks);
  engine::dequantize_q4(blocks, wq.data(), n);

  double ref = 0.0;
  for (size_t i = 0; i < n; ++i) ref += double(wq[i]) * double(y[i]);
  const float got = engine::dot_q4(blocks.data(), y.data(), n);
  const double tol = 1e-3 * std::max(1.0, std::fabs(ref));
  CHECK_MSG(std::fabs(double(got) - ref) <= tol,
            "dot mismatch: got " + std::to_string(got) + " ref " + std::to_string(ref) +
                " kernel=" + engine::dot_kernel_name());
}

YUI_TEST(engine, t2_quantisation) {
  const float x[32] = {1.0f, -1.0f, 0.05f, 0.9f, -0.9f, 0.0f, 0.02f, 1.0f,
                       -1.0f, 1.0f, 0.0f, -0.95f, 0.98f, 0.01f, -0.02f, 0.5f,
                       0.5f, -0.5f, 0.0f, 0.0f, 1.0f, -1.0f, 0.1f, -0.1f,
                       0.7f, -0.7f, 0.0f, 0.0f, 0.9f, -0.9f, 0.0f, 0.0f};
  std::vector<engine::T2Block> blocks;
  engine::quantize_t2(x, 32, blocks);
  CHECK_EQ(blocks.size(), size_t(1));
  float back[32];
  engine::dequantize_t2(blocks, back, 32);
  // Signs must survive; magnitude is only 2-bit so compare loosely.
  for (int i = 0; i < 32; ++i) {
    if (x[i] > 0.4f) CHECK(back[i] > 0.0f);
    if (x[i] < -0.4f) CHECK(back[i] < 0.0f);
    if (std::fabs(x[i]) < 0.05f) CHECK(std::fabs(back[i]) < 1e-6f);
  }
}

YUI_TEST(engine, elastic_autotune_profiles) {
  engine::TuneInput weak;
  weak.ram_gb = 2.0;
  weak.cpu_threads = 2;
  weak.temperature_c = 50;
  const auto rw = engine::auto_tune(weak);
  CHECK_EQ(rw.profile, std::string("may-yeu"));
  CHECK_EQ(rw.k, 2);
  CHECK_MSG(rw.window <= 1024, "weak machines must keep the window small");

  engine::TuneInput strong;
  strong.ram_gb = 32.0;
  strong.cpu_threads = 16;
  strong.temperature_c = 45;
  strong.dedicated_gpu = true;
  const auto rs = engine::auto_tune(strong);
  CHECK_EQ(rs.profile, std::string("may-manh"));
  CHECK_MSG(rs.k >= 8, "strong machines should use a larger k");
  CHECK_MSG(rs.window >= 8192, "strong machines should use a large window");

  engine::TuneInput hot = strong;
  hot.temperature_c = 85.0;
  const auto rh = engine::auto_tune(hot);
  CHECK_MSG(rh.k < rs.k, "throttling must reduce k");
  CHECK_MSG(rh.threads < rs.threads, "throttling must reduce threads");
  CHECK_MSG(rh.reason.find("Nhiệt độ") != std::string::npos, "reason should explain thermal throttling");

  engine::TuneInput battery = strong;
  battery.on_battery = true;
  const auto rb = engine::auto_tune(battery);
  CHECK_MSG(rb.window < rs.window, "battery mode must shrink the window");
}

YUI_TEST(profile, tier_sieu_yeu_and_ram_boundary_checks) {
  // Test ranh giới: 1.4 / 1.5 / 3.9 / 4.0 GB theo DoD P6.2
  profile::HostProfile p14;
  p14.ram_gb = 1.4;
  p14.threads = 4;
  CHECK_EQ(profile::classify(p14), std::string("sieu-yeu"));

  profile::HostProfile p15;
  p15.ram_gb = 1.5;
  p15.threads = 4;
  CHECK_EQ(profile::classify(p15), std::string("may-yeu"));

  profile::HostProfile p39;
  p39.ram_gb = 3.9;
  p39.threads = 4;
  CHECK_EQ(profile::classify(p39), std::string("may-yeu"));

  profile::HostProfile p40;
  p40.ram_gb = 4.0;
  p40.threads = 4;
  CHECK_EQ(profile::classify(p40), std::string("can-bang"));

  // Kiểm tra autotune cho tier sieu-yeu (RAM < 1.5 GB): k=1, window=16, zero-AI fallback
  engine::TuneInput ultra_weak;
  ultra_weak.ram_gb = 1.4;
  ultra_weak.cpu_threads = 2;
  const auto r_uw = engine::auto_tune(ultra_weak);
  CHECK_EQ(r_uw.profile, std::string("sieu-yeu"));
  CHECK_EQ(r_uw.k, 1);
  CHECK_EQ(r_uw.window, 16);
  CHECK(r_uw.reason.find("Headless/TUI") != std::string::npos);
  CHECK(r_uw.reason.find("zero-AI fallback") != std::string::npos);

  // Kiểm tra summary_vi cho máy siêu yếu
  p14.tier = "sieu-yeu";
  p14.tune = r_uw;
  const std::string s_vi = profile::summary_vi(p14);
  CHECK(s_vi.find("siêu yếu") != std::string::npos);
  CHECK(s_vi.find("Headless/TUI") != std::string::npos);
}

YUI_TEST_MAIN("test_core")
