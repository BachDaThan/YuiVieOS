// Năng lượng tầng nhân (P2.5): Tickless + Adaptive DVFS + Clock Gating.
#include <cstring>
#include <string>

#include "testutil.hpp"
#include "yui/power.hpp"

using namespace yui;

YUI_TEST(power, opp_tables_per_machine_class) {
  CHECK_EQ(power::opp_table("may-yeu").size(), size_t(3));
  CHECK_EQ(power::opp_table("can-bang").size(), size_t(4));
  CHECK_EQ(power::opp_table("may-manh").size(), size_t(5));
  CHECK(power::opp_table("may-manh").back().freq_mhz > power::opp_table("may-yeu").back().freq_mhz);
}

YUI_TEST(power, dvfs_follows_load) {
  power::DvfsInput in;
  in.machine_class = "can-bang";
  in.cpu_load_percent = 100;
  const auto high = power::decide_dvfs(in);
  in.cpu_load_percent = 5;
  const auto low = power::decide_dvfs(in);
  CHECK(high.opp_index > low.opp_index);
  CHECK(high.freq_mhz > low.freq_mhz);
  CHECK(!low.reason.empty());
}

YUI_TEST(power, dvfs_heat_wins_over_load) {
  power::DvfsInput in;
  in.machine_class = "may-manh";
  in.cpu_load_percent = 100;
  in.cpu_c = 80.0;  // quá nhiệt: tải cao không được mở turbo
  const auto d = power::decide_dvfs(in);
  const int max_idx = int(power::opp_table("may-manh").size()) - 1;
  CHECK(d.opp_index <= max_idx / 2);
  CHECK(d.reason.find("75") != std::string::npos);
}

YUI_TEST(power, dvfs_battery_rules) {
  power::DvfsInput in;
  in.machine_class = "may-manh";
  in.cpu_load_percent = 100;
  in.on_battery = true;
  const auto bat = power::decide_dvfs(in);
  CHECK(bat.opp_index < int(power::opp_table("may-manh").size()) - 1);
  CHECK(bat.reason.find("pin") != std::string::npos ||
        bat.reason.find("Pin") != std::string::npos);
  in.low_battery = true;
  const auto low = power::decide_dvfs(in);
  CHECK(low.opp_index <= bat.opp_index);
}

YUI_TEST(power, dvfs_process_cap_caps_frequency) {
  power::DvfsInput in;
  in.machine_class = "may-manh";
  in.cpu_load_percent = 100;
  in.process_cap_percent = 25;
  const auto d = power::decide_dvfs(in);
  const int full = power::opp_table("may-manh").back().freq_mhz;
  CHECK(d.freq_mhz <= full / 2);  // 25 % trần ⇒ tối đa nửa xung của mức đầy đủ
  CHECK(d.reason.find("trần tiến trình") != std::string::npos);
}

YUI_TEST(power, tickless_idle_has_no_periodic_wakeup) {
  power::Tickless t;
  // Rảnh tuyệt đối: không timer → ngủ sâu, 0 wakeup định kỳ (DoD).
  const auto p = t.plan(1000, 0, 0);
  CHECK(p.tickless);
  CHECK_EQ(p.sleep_until_ns, uint64_t(0));
  CHECK_EQ(p.idle_wakeups_per_10s, uint64_t(0));
  CHECK(p.depth >= 1);
}

YUI_TEST(power, tickless_waits_only_for_the_next_deadline) {
  power::Tickless t;
  const auto short_gap = t.plan(1000, 1000 + 5'000'000ull, 0);   // 5 ms
  const auto long_gap = t.plan(1000, 1000 + 5'000'000'000ull, 0);  // 5 s
  CHECK(short_gap.tickless);
  CHECK_EQ(short_gap.sleep_until_ns, 1000 + 5'000'000ull);
  CHECK_EQ(short_gap.depth, 0);   // halt là đủ cho khoảng ngắn
  CHECK_EQ(long_gap.depth, 2);    // rảnh lâu ⇒ suspend
}

YUI_TEST(power, tickless_busy_tasks_disable_sleep) {
  power::Tickless t;
  const auto p = t.plan(0, 0, 3);
  CHECK(!p.tickless);
  CHECK_EQ(p.depth, 0);
  CHECK(p.idle_wakeups_per_10s > 0);
}

YUI_TEST(power, wakeup_accounting) {
  power::Tickless t;
  t.note_wakeup(10);
  t.note_wakeup(20);
  t.note_wakeup(30);
  CHECK_EQ(t.total_wakeups(), size_t(3));
  CHECK_EQ(t.wakeups_between(15, 25), uint64_t(1));
  t.reset();
  CHECK_EQ(t.total_wakeups(), size_t(0));
}

YUI_TEST(power, clock_gate_blocks_and_unblocks) {
  power::ClockGate g;
  CHECK(!g.gated(power::Block::Gpu));
  g.gate(power::Block::Gpu);
  g.gate(power::Block::Npu);
  CHECK(g.gated(power::Block::Gpu));
  CHECK(g.gated(power::Block::Npu));
  CHECK(!g.gated(power::Block::Usb));
  g.ungate(power::Block::Gpu);
  CHECK(!g.gated(power::Block::Gpu));
  CHECK_EQ(std::string(power::block_name(power::Block::Npu)), std::string("npu"));
}

YUI_TEST(power, throttle_task_clamps_percent) {
  power::ClockGate g;
  g.throttle_task(42, 20);
  CHECK_EQ(g.throttle_percent(42), 20);
  CHECK_EQ(g.throttle_percent(7), 100);  // chưa bóp: chạy đầy đủ
  g.throttle_task(42, 0);
  CHECK_EQ(g.throttle_percent(42), 1);   // ép xuống tối thiểu 1 %
  g.throttle_task(42, 500);
  CHECK_EQ(g.throttle_percent(42), 100); // không vượt 100 %
  g.release_task(42);
  CHECK_EQ(g.throttle_percent(42), 100);
  CHECK_EQ(g.throttled_count(), size_t(0));
}

YUI_TEST(power, manager_gates_gpu_npu_when_hot) {
  power::Manager m;
  const auto hot = m.on_telemetry(85.0, -1.0, 90.0, false, false, "can-bang");
  CHECK_EQ(hot.gate_now.size(), size_t(2));
  CHECK(m.clockgate().gated(power::Block::Gpu));
  CHECK(m.clockgate().gated(power::Block::Npu));
  CHECK(hot.note.find("Quá nhiệt") != std::string::npos);
  const auto cool = m.on_telemetry(45.0, -1.0, 10.0, false, false, "can-bang");
  CHECK(!m.clockgate().gated(power::Block::Gpu));
  CHECK(!m.clockgate().gated(power::Block::Npu));
  CHECK(cool.note.find("bình thường") != std::string::npos);
}

YUI_TEST_MAIN("test_power")
