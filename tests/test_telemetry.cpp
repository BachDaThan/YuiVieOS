// Telemetry Hook tests (Phase P1.5).
//
// The advice rule is pure, so every threshold is tested exactly at its
// boundary; the live reader is only checked for *plausibility*, never for
// machine-specific values (a test that assumes a battery would fail in CI).
#include <cmath>
#include <string>

#include "testutil.hpp"
#include "yui/service.hpp"
#include "yui/telemetry.hpp"

using yui::telemetry::Advice;
using yui::telemetry::Reading;
using yui::telemetry::Thresholds;

namespace {

Reading laptop() {
  Reading r;
  r.battery_present = true;
  r.battery_percent = 80;
  r.charging = false;
  r.cpu_c = 45.0;
  r.gpu_c = 40.0;
  r.ram_total_mb = 16384;
  r.ram_free_mb = 9000;
  r.machine_class = "can-bang";
  r.simd = "avx2";
  r.threads = 8;
  return r;
}

}  // namespace

// ------------------------------------------------------------- thresholds --
YUI_TEST(telemetry, defaults_match_the_spec) {
  const Thresholds t;
  CHECK_EQ(t.low_battery_percent, 20);
  CHECK(std::fabs(t.hot_c - 75.0) < 1e-9);
}

YUI_TEST(telemetry, low_battery_forces_economy) {
  Reading r = laptop();
  r.battery_percent = 19;
  r.charging = false;
  const Advice a = yui::telemetry::advise(r);
  CHECK_EQ(a.k, 1);
  CHECK_EQ(a.window, 16);
  CHECK_EQ(a.mode, std::string("tiet-kiem"));
  CHECK_MSG(a.reason.find("Pin") != std::string::npos, a.reason);
}

YUI_TEST(telemetry, battery_boundary_is_not_low) {
  Reading r = laptop();
  r.battery_percent = 20;  // not < 20
  const Advice a = yui::telemetry::advise(r);
  CHECK_EQ(a.k, 2);
  CHECK_EQ(a.window, 256);  // can-bang, on battery
  CHECK_EQ(a.mode, std::string("can-bang"));
}

YUI_TEST(telemetry, charging_restores_headroom_even_on_low_battery) {
  Reading r = laptop();
  r.battery_percent = 12;
  r.charging = true;
  const Advice a = yui::telemetry::advise(r);
  CHECK_EQ(a.k, 2);
  CHECK_EQ(a.window, 512);
  CHECK_EQ(a.mode, std::string("can-bang"));
  CHECK_MSG(a.reason.find("sạc") != std::string::npos, a.reason);
}

YUI_TEST(telemetry, heat_boundary) {
  Reading r = laptop();
  r.cpu_c = 75.0;  // strict "> 75" ⇒ still fine
  CHECK_EQ(yui::telemetry::advise(r).window, 256);

  r.cpu_c = 75.01;
  const Advice hot = yui::telemetry::advise(r);
  CHECK_EQ(hot.k, 1);
  CHECK_EQ(hot.window, 16);
  CHECK_EQ(hot.mode, std::string("tiet-kiem"));
  CHECK_MSG(hot.reason.find("°C") != std::string::npos, hot.reason);
}

YUI_TEST(telemetry, gpu_heat_counts_too) {
  Reading r = laptop();
  r.gpu_c = 91.0;  // CPU cool, GPU hot
  const Advice a = yui::telemetry::advise(r);
  CHECK_EQ(a.k, 1);
  CHECK_EQ(a.window, 16);
  CHECK_MSG(a.reason.find("91") != std::string::npos, a.reason);
}

YUI_TEST(telemetry, weak_machine_stays_economy_even_when_cool) {
  Reading r = laptop();
  r.machine_class = "may-yeu";
  r.battery_percent = 95;
  r.charging = true;
  const Advice a = yui::telemetry::advise(r);
  CHECK_EQ(a.k, 1);
  CHECK_EQ(a.window, 16);
  CHECK_EQ(a.mode, std::string("tiet-kiem"));
  CHECK_MSG(a.reason.find("Máy yếu") != std::string::npos, a.reason);
}

YUI_TEST(telemetry, strong_machine_unlocks_k4_when_plugged) {
  Reading r = laptop();
  r.machine_class = "may-manh";
  r.charging = true;
  const Advice a = yui::telemetry::advise(r);
  CHECK_EQ(a.k, 4);
  CHECK_EQ(a.window, 1024);
  CHECK_EQ(a.mode, std::string("toi-da"));
}

YUI_TEST(telemetry, strong_machine_on_battery_is_slower) {
  Reading r = laptop();
  r.machine_class = "may-manh";
  r.charging = false;
  const Advice a = yui::telemetry::advise(r);
  CHECK_EQ(a.k, 3);
  CHECK_EQ(a.window, 768);
}

YUI_TEST(telemetry, advice_bounds_are_inside_the_documented_ranges) {
  const char* classes[] = {"sieu-yeu", "may-yeu", "can-bang", "may-manh", ""};
  const bool charging[] = {false, true};
  const int batteries[] = {-1, 5, 50, 100};
  const double temps[] = {-1.0, 20.0, 74.9, 76.0};
  for (const char* cls : classes) {
    for (bool chg : charging) {
      for (int bat : batteries) {
        for (double t : temps) {
          Reading r = laptop();
          r.machine_class = cls;
          r.charging = chg;
          r.battery_present = bat >= 0;
          r.battery_percent = bat;
          r.cpu_c = t;
          r.gpu_c = t;
          const Advice a = yui::telemetry::advise(r);
          CHECK(a.k >= 1 && a.k <= 4);
          CHECK(a.window == 16 || (a.window >= 256 && a.window <= 1024));
          CHECK(!a.reason.empty());
        }
      }
    }
  }
}

YUI_TEST(telemetry, custom_thresholds_are_honoured) {
  Reading r = laptop();
  r.battery_percent = 30;
  r.cpu_c = 60.0;
  Thresholds t;
  CHECK_EQ(yui::telemetry::advise(r, t).k, 2);
  t.low_battery_percent = 50;
  const Advice strict = yui::telemetry::advise(r, t);
  CHECK_EQ(strict.k, 1);
  CHECK_EQ(strict.window, 16);
}

YUI_TEST(telemetry, no_battery_is_not_low_battery) {
  Reading r = laptop();
  r.battery_present = false;
  r.battery_percent = -1;
  const Advice a = yui::telemetry::advise(r);
  CHECK_EQ(a.k, 2);
  CHECK_MSG(a.reason.find("Pin") == std::string::npos, a.reason);
}

// ----------------------------------------------------------- profile mapping --
YUI_TEST(telemetry, from_profile_maps_every_field) {
  yui::profile::HostProfile host;
  host.arch = "x86_64";
  host.threads = 16;
  host.ram_gb = 32.0;
  host.ram_available_gb = 20.5;
  host.has_battery = true;
  host.battery_percent = 66;
  host.charging = true;
  host.temperature_c = 52.5;
  host.simd.best = "avx512f";
  host.tier = "may-manh";

  const Reading r = yui::telemetry::from_profile(host, "profile");
  CHECK(r.battery_present);
  CHECK_EQ(r.battery_percent, 66);
  CHECK(r.charging);
  CHECK(std::fabs(r.cpu_c - 52.5) < 1e-9);
  CHECK_EQ(r.ram_total_mb, 32768L);
  CHECK_EQ(r.ram_free_mb, 20992L);  // 20.5 GiB
  CHECK_EQ(r.machine_class, std::string("may-manh"));
  CHECK_EQ(r.simd, std::string("avx512f"));
  CHECK_EQ(r.threads, 16);
  CHECK_EQ(r.source, std::string("profile"));
}

YUI_TEST(telemetry, from_profile_keeps_unknowns_unknown) {
  yui::profile::HostProfile host;  // untouched: no battery, no sensor, no tier
  const Reading r = yui::telemetry::from_profile(host);
  CHECK(!r.battery_present);
  CHECK_EQ(r.battery_percent, -1);
  CHECK(r.cpu_c < 0);
  CHECK_EQ(r.ram_total_mb, 0L);
  CHECK_EQ(r.machine_class, std::string("can-bang"));  // documented default
  CHECK_EQ(r.simd, std::string("scalar"));
  CHECK_EQ(r.threads, 1);
}

YUI_TEST(telemetry, from_profile_negative_values_are_clamped) {
  yui::profile::HostProfile host;
  host.ram_gb = -5.0;
  host.ram_available_gb = -1.0;
  host.threads = 0;
  const Reading r = yui::telemetry::from_profile(host);
  CHECK_EQ(r.ram_total_mb, 0L);
  CHECK_EQ(r.ram_free_mb, 0L);
  CHECK_EQ(r.threads, 1);
}

// ------------------------------------------------------------------- live I/O --
YUI_TEST(telemetry, live_read_is_plausible) {
  const Reading r = yui::telemetry::read();
  CHECK(r.ram_total_mb > 0);              // every machine we support has RAM
  CHECK(r.battery_percent >= -1 && r.battery_percent <= 100);
  CHECK(r.threads >= 1);
  CHECK(!r.machine_class.empty());
  CHECK(r.machine_class == "sieu-yeu" || r.machine_class == "may-yeu" ||
        r.machine_class == "can-bang" || r.machine_class == "may-manh");
  if (!r.battery_present) CHECK_EQ(r.battery_percent, -1);
  if (r.cpu_c > 0) CHECK(r.cpu_c < 200.0);
  if (r.gpu_c > 0) CHECK(r.gpu_c < 200.0);
}

YUI_TEST(telemetry, gpu_sensor_is_optional_but_bounded) {
  const double gpu = yui::telemetry::read_gpu_c();
  CHECK(gpu == -1.0 || (gpu > 0.0 && gpu < 200.0));
  const double cpu = yui::telemetry::read_cpu_c();
  CHECK(cpu == -1.0 || (cpu > 0.0 && cpu < 200.0));
}

// ---------------------------------------------------------------- JSON & text --
YUI_TEST(telemetry, to_json_has_the_contract_shape) {
  Reading r = laptop();
  r.battery_percent = 15;
  const Advice a = yui::telemetry::advise(r);
  const auto v = yui::telemetry::to_json(r, a);
  CHECK(v.is_object());
  CHECK(v.has("battery"));
  CHECK(v.has("thermal"));
  CHECK(v.has("memory"));
  CHECK(v.has("machine"));
  CHECK(v.has("advice"));
  CHECK_EQ(v["battery"]["percent"].as_int(), 15);
  CHECK(v["advice"]["k"].as_int() == 1);
  CHECK_EQ(v["advice"]["window"].as_int(), 16);
  CHECK(!v["advice"]["reason"].as_string().empty());
  CHECK_EQ(v["memory"]["total_mb"].as_int(), 16384);
  CHECK_EQ(v["machine"]["class"].as_string(), std::string("can-bang"));
  CHECK(!v["source"].as_string().empty());
}

YUI_TEST(telemetry, to_json_without_advice_is_valid_too) {
  const auto v = yui::telemetry::to_json(laptop());
  CHECK(v.has("battery"));
  CHECK(!v.has("advice"));
}

YUI_TEST(telemetry, summary_is_vietnamese_and_readable) {
  Reading r = laptop();
  r.battery_percent = 15;
  const Advice a = yui::telemetry::advise(r);
  const std::string line = yui::telemetry::summary_vi(r, a);
  CHECK_MSG(line.find("Pin 15%") != std::string::npos, line);
  CHECK_MSG(line.find("RAM trống") != std::string::npos, line);
  CHECK_MSG(line.find("k=1, W=16") != std::string::npos, line);
}

// ------------------------------------------------------ through the RPC layer --
YUI_TEST(telemetry, rpc_telemetry_read_returns_facts_and_advice) {
  yui::service::Options opts;
  opts.state_dir = "/tmp/yui-telemetry-test";
  yui::service::Daemon daemon(opts);
  const auto reply = daemon.dispatch([] {
    yui::json::Value req(yui::json::Object{});
    req.set("jsonrpc", "2.0");
    req.set("id", 1);
    req.set("method", "telemetry.read");
    return req;
  }());
  CHECK(!reply.has("error"));
  const auto result = reply["result"];
  CHECK(result.has("battery"));
  CHECK(result.has("advice"));
  const int k = int(result["advice"]["k"].as_int());
  CHECK(k >= 1 && k <= 4);
  const int w = int(result["advice"]["window"].as_int());
  CHECK(w == 16 || (w >= 256 && w <= 1024));
}

YUI_TEST(telemetry, mcp_exposes_system_telemetry) {
  yui::service::Options opts;
  opts.state_dir = "/tmp/yui-telemetry-test";
  yui::service::Daemon daemon(opts);
  const auto tools = daemon.mcp_tools_list();
  bool found = false;
  for (const auto& t : tools["tools"].arr()) {
    if (t["name"].as_string() == "system_telemetry") found = true;
  }
  CHECK_MSG(found, "system_telemetry phải có trong tools/list");
}

YUI_TEST_MAIN("test_telemetry")
