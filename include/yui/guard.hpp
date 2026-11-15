// Guard chống bất thường nền (Background Anomaly Guard) — một trong ba cơ chế
// phòng vệ chủ động (docs/SECURITY.md §11).
//
// Luồng: `telemetry` (tải CPU/GPU riêng từng tiến trình, nhiệt, điện) → Guard
// → quyết định → yui-sched (hạ lớp ưu tiên) + yui-power (bóp xung/clock
// gating) + audit + thông báo tiếng Việt cho `yuid` hỏi người dùng
// "tiêu diệt / gỡ bỏ / để yên".
//
// Quy tắc (nhất quán với SECURITY.md):
//   tiến trình KHÔNG có cửa sổ hiển thị (headless) VÀ không giữ thẻ
//   CAP_COMPUTE_HEAVY (`sandbox::kRightComputeHeavy`) mà tải CPU hoặc GPU
//   vượt ngưỡng (mặc định 70 %) liên tục đủ lâu (mặc định 15 s) ⇒ bị nghi
//   ngờ đào coin ngầm / chạy ngầm → hạ xuống lớp Idle/Background, bóp xung
//   (mặc định 20 %), ghi audit, hỏi người dùng.
//
// Mức ưu tiên tối đa của mẫu: mẫu nào có cửa sổ hoặc có quyền thì không
// theo dõi — kể cả trước đó đang bị bóp (người dùng đã biết rồi).
#ifndef YUI_GUARD_HPP
#define YUI_GUARD_HPP

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace yui::guard {

struct ProcessSample {
  uint64_t pid = 0;
  std::string name;
  double cpu_percent = 0.0;  // tải CPU riêng của tiến trình (telemetry)
  double gpu_percent = 0.0;
  bool has_window = false;    // có cửa sổ trên màn hình không
  uint64_t caps = 0;          // bitmask quyền yui::sandbox::Right
  int64_t now_ns = 0;         // mốc thời gian mẫu (monotonic)
};

struct Thresholds {
  double cpu_percent = 70.0;
  double gpu_percent = 70.0;
  int64_t sustain_ns = 15LL * 1000 * 1000 * 1000;  // 15 s
  int throttle_percent = 20;                       // bóp về 20 % xung nhịp
};

struct Alert {
  uint64_t pid = 0;
  std::string name;
  std::string reason_vi;   // lý do audit (tại sao nghi ngờ)
  std::string notify_vi;   // câu hỏi cho người dùng qua yuid
  int throttle_percent = 0;
  bool demoted = true;     // đã hạ xuống lớp Idle/Background
  int64_t when_ns = 0;
  double cpu_percent = 0.0;
  double gpu_percent = 0.0;
};

enum class Verdict { Normal, Watching, Throttled };

/// Bộ Guard: trạng thái theo dõi thuần, không I/O. `yuid` là nơi hiển thị
/// notify_vi và gọi quyết định (tiêu diệt / gỡ bỏ / để yên).
class Guard {
 public:
  explicit Guard(Thresholds t = Thresholds{}) : t_(t) {}

  /// Nạp một mẫu. Trả về Alert khi lần đầu phát hiện (chỉ một lần cho mỗi
  /// chuỗi vi phạm liên tục).
  std::optional<Alert> observe(const ProcessSample& s);

  /// Người dùng chọn "để yên" hoặc tiến trình kết thúc: bỏ bóp xung.
  bool clear(uint64_t pid);

  bool is_throttled(uint64_t pid) const;
  Verdict verdict(uint64_t pid) const;
  int throttle_percent(uint64_t pid) const;
  const std::vector<Alert>& alerts() const { return alerts_; }
  void clear_alerts() { alerts_.clear(); }
  const Thresholds& thresholds() const { return t_; }

 private:
  struct Track {
    int64_t over_since_ns = -1;
    bool alerted = false;
    bool throttled = false;
  };
  Thresholds t_;
  std::map<uint64_t, Track> tracks_;
  std::vector<Alert> alerts_;
};

}  // namespace yui::guard

#endif  // YUI_GUARD_HPP
