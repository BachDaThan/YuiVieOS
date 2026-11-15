// Năng lượng tầng nhân — xem include/yui/power.hpp.
#include "yui/power.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace yui::power {

const std::vector<Opp>& opp_table(const std::string& machine_class) {
  // Ba bảng OPP: máy yếu ít bậc (điện áp thấp), máy mạnh nhiều bậc (turbo).
  static const std::vector<Opp> kWeak = {{600, 800}, {1000, 850}, {1400, 900}};
  static const std::vector<Opp> kBalanced = {{600, 800}, {1000, 850}, {1600, 900}, {2200, 1000}};
  static const std::vector<Opp> kStrong = {{800, 800}, {1400, 850}, {2200, 950}, {3000, 1050},
                                           {3800, 1150}};
  if (machine_class == "may-yeu") return kWeak;
  if (machine_class == "may-manh") return kStrong;
  return kBalanced;
}

DvfsDecision decide_dvfs(const DvfsInput& in) {
  const auto& table = opp_table(in.machine_class);
  const int max_idx = int(table.size()) - 1;

  // Tải quyết định điểm bắt đầu: 0 % → OPP thấp nhất, 100 % → cao nhất.
  double load = in.cpu_load_percent;
  if (load < 0) load = 0;
  if (load > 100) load = 100;
  int idx = int(load / 100.0 * max_idx + 0.5);

  const double heat = std::max(in.cpu_c, in.gpu_c);
  const bool hot = heat > 75.0;  // cùng ngưỡng với telemetry::advise

  std::string reason;
  if (hot) {
    idx = std::min(idx, max_idx / 2);
    char buf[128];
    std::snprintf(buf, sizeof buf, "Nhiệt độ %.1f°C vượt ngưỡng 75°C: hạ xuống OPP %d để giữ máy mát",
                  heat, idx);
    reason = buf;
  } else if (in.low_battery) {
    idx = std::min(idx, max_idx / 4);
    reason = "Pin dưới 20%: giữ OPP thấp nhất có thể để kéo dài thời gian dùng";
  } else if (in.on_battery) {
    idx = std::min(idx, max_idx > 0 ? max_idx - 1 : 0);
    reason = "Đang dùng pin: không mở turbo, giữ OPP vừa phải";
  } else if (load < 25.0) {
    reason = "Tải dưới 25%: hạ xung để tiết kiệm điện khi rảnh";
  } else {
    reason = "Tải và nhiệt bình thường: chạy theo tải CPU";
  }

  // Trần xung theo tiến trình (Clock Gating của guard): chọn OPP cao nhất
  // không vượt tỉ lệ phần trăm của mức tối đa.
  if (in.process_cap_percent < 100) {
    const int cap_freq = table[max_idx].freq_mhz * std::max(1, in.process_cap_percent) / 100;
    while (idx > 0 && table[idx].freq_mhz > cap_freq) --idx;
    reason += " · trần tiến trình " + std::to_string(in.process_cap_percent) + "%";
  }
  if (idx < 0) idx = 0;
  if (idx > max_idx) idx = max_idx;

  DvfsDecision d;
  d.opp_index = idx;
  d.freq_mhz = table[idx].freq_mhz;
  d.voltage_mv = table[idx].voltage_mv;
  d.reason = reason;
  return d;
}

// ------------------------------------------------------------- Full Tickless --
IdlePlan Tickless::plan(uint64_t now_ns, uint64_t next_deadline_ns, int ready_tasks) const {
  IdlePlan p;
  if (ready_tasks > 0) {
    // Còn việc: không ngủ, và chắc chắn không phải tickless.
    p.tickless = false;
    p.depth = 0;
    p.sleep_until_ns = now_ns;
    p.idle_wakeups_per_10s = 1;  // bận thì phải có wakeup — 0 là dấu hiệu sai
    return p;
  }
  // Rảnh: KHÔNG có tick định kỳ (full tickless) — chỉ deadline của timer gần
  // nhất mới được đánh thức nhân.
  p.tickless = true;
  p.idle_wakeups_per_10s = 0;
  if (next_deadline_ns == 0 || next_deadline_ns <= now_ns) {
    // Không timer nào: ngủ sâu chờ ngắt (wfi), không giới hạn.
    p.sleep_until_ns = 0;
    p.depth = 1;
    return p;
  }
  const uint64_t gap = next_deadline_ns - now_ns;
  p.sleep_until_ns = next_deadline_ns;
  // > 1 s rảnh ⇒ suspend (tắt clock ngoại vi); > 10 ms ⇒ deep; còn lại halt.
  p.depth = gap > 1'000'000'000ull ? 2 : (gap > 10'000'000ull ? 1 : 0);
  return p;
}

void Tickless::note_wakeup(uint64_t now_ns) { wakeups_.push_back(now_ns); }

uint64_t Tickless::wakeups_between(uint64_t from_ns, uint64_t to_ns) const {
  uint64_t n = 0;
  for (uint64_t t : wakeups_) {
    if (t >= from_ns && t <= to_ns) ++n;
  }
  return n;
}

// ------------------------------------------------------------- Clock Gating --
const char* block_name(Block b) {
  switch (b) {
    case Block::Cpu: return "cpu";
    case Block::Gpu: return "gpu";
    case Block::Npu: return "npu";
    case Block::Usb: return "usb";
    case Block::Display: return "display";
  }
  return "?";
}

void ClockGate::gate(Block b) { mask_ |= 1 << int(b); }
void ClockGate::ungate(Block b) { mask_ &= ~(1 << int(b)); }
bool ClockGate::gated(Block b) const { return (mask_ & (1 << int(b))) != 0; }

void ClockGate::throttle_task(uint64_t task, int percent) {
  if (percent < 1) percent = 1;
  if (percent > 100) percent = 100;
  throttle_[task] = percent;
}

int ClockGate::throttle_percent(uint64_t task) const {
  auto it = throttle_.find(task);
  return it == throttle_.end() ? 100 : it->second;
}

void ClockGate::release_task(uint64_t task) { throttle_.erase(task); }

// ------------------------------------------------------------- Manager -------
Actions Manager::on_telemetry(double cpu_c, double gpu_c, double cpu_load, bool on_battery,
                              bool low_battery, const std::string& machine_class) {
  Actions a;
  DvfsInput in;
  in.cpu_load_percent = cpu_load;
  in.cpu_c = cpu_c;
  in.gpu_c = gpu_c;
  in.on_battery = on_battery;
  in.low_battery = low_battery;
  in.machine_class = machine_class;
  a.dvfs = decide_dvfs(in);

  const double heat = std::max(cpu_c, gpu_c);
  if (heat > 75.0) {
    // Khối tăng tốc bị bỏ trống khi quá nhiệt: tắt xung cho tới khi mát.
    if (!gates_.gated(Block::Gpu)) {
      gates_.gate(Block::Gpu);
      a.gate_now.push_back(Block::Gpu);
    }
    if (!gates_.gated(Block::Npu)) {
      gates_.gate(Block::Npu);
      a.gate_now.push_back(Block::Npu);
    }
    a.note = "Quá nhiệt: clock-gating GPU + NPU, hạ xung CPU";
  } else {
    bool changed = false;
    if (gates_.gated(Block::Gpu)) {
      gates_.ungate(Block::Gpu);
      changed = true;
    }
    if (gates_.gated(Block::Npu)) {
      gates_.ungate(Block::Npu);
      changed = true;
    }
    a.note = changed ? "Nhiệt trở lại bình thường: bật lại xung GPU/NPU"
                     : "Nhiệt bình thường: giữ clock-gating theo tiến trình rảnh";
  }
  return a;
}

}  // namespace yui::power
