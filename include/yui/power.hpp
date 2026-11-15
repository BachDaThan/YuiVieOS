// Năng lượng tầng nhân (Tầng 2): Full Tickless · Adaptive DVFS · Clock Gating.
//
// Đây là **chính sách**, không phải cơ chế: khung phần cứng (`yui-frame`) giữ
// timer/ngắt/MMIO, còn mọi quyết định "ngủ bao lâu, chạy bao nhiêu MHz, tắt
// xung khối nào" nằm ở đây — Rust an toàn (`kernel-services/yui-power`) và
// bản C++ đối chiếu này dùng chung quy tắc với bảng test.
//
// Nguồn dữ liệu: `telemetry.cpp` (tải CPU/GPU, nhiệt, pin) + mẫu tiến trình
// của `guard.cpp` (mức dùng riêng của từng tiến trình bị bóp xung).
//
// **Mục tiêu đo được** (docs/PLAN.md P2.5 — chưa đo trên phần cứng thật):
//   * khi rảnh: 0 wakeup/s trong 10 s (không còn tick định kỳ);
//   * công suất gói giảm ≥ 20 % so với governor mặc định cùng workload.
#ifndef YUI_POWER_HPP
#define YUI_POWER_HPP

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace yui::power {

// ------------------------------------------------------------- Adaptive DVFS --
struct Opp {
  int freq_mhz = 600;
  int voltage_mv = 800;
};

/// Bảng OPP theo loại máy (may-yeu ít bậc, may-manh nhiều bậc).
const std::vector<Opp>& opp_table(const std::string& machine_class);

struct DvfsInput {
  double cpu_load_percent = 0.0;  // 0..100
  double cpu_c = -1.0;            // -1 = không có cảm biến
  double gpu_c = -1.0;
  bool on_battery = false;
  bool low_battery = false;  // pin < 20 % (ngưỡng telemetry)
  std::string machine_class = "can-bang";
  int process_cap_percent = 100;  // trần xung do Clock Gapping áp cho tiến trình
};

struct DvfsDecision {
  int opp_index = 0;
  int freq_mhz = 0;
  int voltage_mv = 0;
  std::string reason;  // tiếng Việt, hiển thị được cho người dùng
};

/// Chọn OPP theo tải + nhiệt + pin + trần tiến trình. Hàm thuần.
/// Quy tắc (đồng bộ với `telemetry::advise`):
///   nhiệt > 75 °C hoặc pin yếu ⇒ tụt xuống bậc thấp nhất có thể;
///   tải < 25 % ⇒ hạ một bậc (rảnh thì chạy chậm); tải > 80 % ⇒ lên một bậc;
///   process_cap_percent < 100 ⇒ trần tần số ở tỉ lệ đó của mức tối đa.
DvfsDecision decide_dvfs(const DvfsInput& in);

// ------------------------------------------------------------- Full Tickless --
struct IdlePlan {
  bool tickless = true;          // tắt hoàn toàn tick định kỳ khi rảnh
  uint64_t sleep_until_ns = 0;   // 0 = ngủ tới ngắt (không deadline)
  int depth = 1;                 // 0 = halt, 1 = deep (wfi), 2 = suspend
  uint64_t idle_wakeups_per_10s = 0;  // 0 khi không có timer định kỳ nào
};

/// Trạng thái đếm wakeup: chính nhân đếm, năng lượng quyết định.
/// Khi mọi timer đều ở tương lai xa (hoặc không có), kế hoạch là ngủ sâu
/// không giới hạn và **không** có tick đánh thức định kỳ.
class Tickless {
 public:
  IdlePlan plan(uint64_t now_ns, uint64_t next_deadline_ns, int ready_tasks) const;
  void note_wakeup(uint64_t now_ns);
  uint64_t wakeups_between(uint64_t from_ns, uint64_t to_ns) const;
  uint64_t total_wakeups() const { return wakeups_.size(); }
  void reset() { wakeups_.clear(); }

 private:
  std::vector<uint64_t> wakeups_;
};

// ------------------------------------------------------------- Clock Gating --
enum class Block { Cpu = 0, Gpu = 1, Npu = 2, Usb = 3, Display = 4 };
const char* block_name(Block b);

class ClockGate {
 public:
  void gate(Block b);
  void ungate(Block b);
  bool gated(Block b) const;
  int gated_mask() const { return mask_; }
  /// Bóp xung một tiến trình (guard dùng): 1..100 (% mức tối đa).
  void throttle_task(uint64_t task, int percent);
  int throttle_percent(uint64_t task) const;  // mặc định 100
  void release_task(uint64_t task);
  size_t throttled_count() const { return throttle_.size(); }

 private:
  int mask_ = 0;
  std::map<uint64_t, int> throttle_;
};

// ------------------------------------------------------------- Manager -------
struct Actions {
  DvfsDecision dvfs;
  std::vector<Block> gate_now;
  std::string note;  // tóm tắt tiếng Việt cho audit
};

/// Bộ quản lý năng lượng: nạp mẫu telemetry → trả hành động cần làm.
class Manager {
 public:
  Tickless& tickless() { return tickless_; }
  ClockGate& clockgate() { return gates_; }
  const Tickless& tickless() const { return tickless_; }
  const ClockGate& clockgate() const { return gates_; }

  /// Nhiệt > ngưỡng ⇒ tắt xung GPU/NPU rảnh; mát trở lại ⇒ bật lại.
  Actions on_telemetry(double cpu_c, double gpu_c, double cpu_load, bool on_battery,
                       bool low_battery, const std::string& machine_class);

 private:
  Tickless tickless_;
  ClockGate gates_;
};

}  // namespace yui::power

#endif  // YUI_POWER_HPP
