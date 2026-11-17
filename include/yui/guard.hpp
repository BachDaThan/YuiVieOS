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
// Mở rộng P2.14: Phát hiện mã độc tống tiền (Ransomware):
//   Ghi hàng loạt + Shannon entropy cao (> 7.2 bit/byte) + tốc độ bất thường
//   ⇒ hạ ưu tiên, bóp xung, kích hoạt cảnh báo hoàn tác (rollback CoW).
//
// Mở rộng P2.15: Guard rảnh rỗi (Idle Guard):
//   Ứng dụng nền mở micro/camera, đọc danh bạ hoặc gửi mạng khi không có
//   thao tác người dùng trong N phút ⇒ hạ xung, thu hồi thẻ O(1), audit + hỏi.
#ifndef YUI_GUARD_HPP
#define YUI_GUARD_HPP

#include <cmath>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <unordered_set>
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

// ============================================================================
// Tính entropy Shannon & Phát hiện mã độc tống tiền (Ransomware Detector — P2.14)
// ============================================================================

/// Tính Shannon entropy (bit trên mỗi byte, phạm vi 0.0 .. 8.0).
/// Dữ liệu văn bản thường ~3.5 .. 5.0; mã hoá AES/ChaCha20 hoặc ngẫu nhiên ~7.8 .. 8.0.
double shannon_entropy(const uint8_t* data, size_t len);

struct RansomwareConfig {
  double min_entropy = 7.2;                // Shannon entropy >= 7.2 bit/byte
  size_t mass_write_count = 20;            // số tệp bị ghi dồn dập
  int64_t window_ns = 10LL * 1000 * 1000 * 1000;  // cửa sổ 10 giây
  int throttle_percent = 10;               // bóp xung về 10 %
};

struct RansomwareEvent {
  uint64_t pid = 0;
  std::string proc_name;
  std::string file_path;
  const uint8_t* data = nullptr;
  size_t size = 0;
  int64_t now_ns = 0;
};

struct RansomwareAlert {
  uint64_t pid = 0;
  std::string proc_name;
  size_t files_affected = 0;
  double avg_entropy = 0.0;
  int64_t when_ns = 0;
  int throttle_percent = 10;
  std::string suggested_snapshot;
  std::string reason_vi;
  std::string notify_vi;
  bool demoted = true;
};

/// Bộ phát hiện mã độc tống tiền dựa trên hành vi:
/// Tốc độ ghi nhanh + entropy cao (ciphertext) dồn dập trong cửa sổ thời gian.
///
/// Giới hạn đã biết (theo docs/PLAN.md P2.14):
/// Không chống được ransomware nếu tiến trình chạy với quyền fs.write hợp lệ
/// mà cố tình ghi rất chậm (dưới ngưỡng tốc độ) hoặc ghi dữ liệu entropy thấp.
class RansomwareDetector {
 public:
  explicit RansomwareDetector(RansomwareConfig cfg = RansomwareConfig{}) : cfg_(cfg) {}

  /// Quan sát một thao tác ghi tệp. Trả về Alert nếu phát hiện hành vi tấn công.
  std::optional<RansomwareAlert> observe_write(const RansomwareEvent& ev,
                                              const std::string& rollback_snap = "");

  bool is_throttled(uint64_t pid) const;
  bool clear(uint64_t pid);
  const std::vector<RansomwareAlert>& alerts() const { return alerts_; }
  void clear_alerts() { alerts_.clear(); }
  const RansomwareConfig& config() const { return cfg_; }

 private:
  struct WriteRecord {
    int64_t timestamp_ns = 0;
    double entropy = 0.0;
    std::string path;
  };
  struct ProcessRecord {
    std::vector<WriteRecord> writes;
    bool alerted = false;
    bool throttled = false;
  };
  RansomwareConfig cfg_;
  std::map<uint64_t, ProcessRecord> records_;
  std::vector<RansomwareAlert> alerts_;
};

// ============================================================================
// Guard mở rộng theo quy tắc khi máy rảnh (Idle Guard — P2.15)
// ============================================================================

enum class SensitiveResource {
  Microphone,
  Camera,
  Contacts,
  Network
};

const char* sensitive_resource_name_vi(SensitiveResource res);

struct IdleActivitySample {
  uint64_t pid = 0;
  std::string proc_name;
  SensitiveResource resource = SensitiveResource::Microphone;
  bool has_window = false;        // có cửa sổ giao diện hoạt động không
  bool user_initiated = false;    // do người dùng chủ động kích hoạt
  uint64_t token_badge = 0;       // badge của capability token
  int64_t now_ns = 0;
};

struct IdleAlert {
  uint64_t pid = 0;
  std::string proc_name;
  SensitiveResource resource = SensitiveResource::Microphone;
  uint64_t revoked_badge = 0;
  int throttle_percent = 20;
  int64_t idle_duration_s = 0;
  int64_t when_ns = 0;
  std::string reason_vi;
  std::string notify_vi;
};

struct IdleConfig {
  int64_t idle_timeout_ns = 300LL * 1000 * 1000 * 1000;  // mặc định 5 phút (300 s)
  int throttle_percent = 20;
};

/// Quy tắc bảo vệ tài nguyên nhạy cảm khi máy rảnh:
/// Ứng dụng nền mở micro/camera, đọc danh bạ hoặc gửi mạng khi không có
/// thao tác người dùng trong N phút ⇒ hạ xung, thu hồi thẻ liên quan (O(1)),
/// audit và thông báo hỏi người dùng.
class IdleGuard {
 public:
  using Config = IdleConfig;

  explicit IdleGuard(Config cfg = IdleConfig{}) : cfg_(cfg) {}

  /// Ghi nhận thao tác người dùng (bàn phím, chuột, cảm ứng).
  void record_user_activity(int64_t now_ns);

  /// Kiểm tra xem người dùng hiện có đang rảnh (idle) không.
  bool is_user_idle(int64_t now_ns) const;

  /// Thời gian rảnh tính bằng nanosecond.
  int64_t idle_duration_ns(int64_t now_ns) const;

  /// Quan sát hành vi truy cập tài nguyên nhạy cảm.
  std::optional<IdleAlert> observe(const IdleActivitySample& s);

  /// Kiểm tra badge đã bị thu hồi chưa (tra cứu O(1)).
  bool is_badge_revoked(uint64_t badge) const;

  /// Kiểm tra tiến trình có bị bóp xung không.
  bool is_throttled(uint64_t pid) const;

  /// Bỏ chặn tiến trình (người dùng xác nhận cấp phép).
  bool clear(uint64_t pid);

  const std::vector<IdleAlert>& alerts() const { return alerts_; }
  void clear_alerts() { alerts_.clear(); }
  const Config& config() const { return cfg_; }

 private:
  Config cfg_;
  int64_t last_user_activity_ns_ = 0;
  std::unordered_set<uint64_t> revoked_badges_;  // O(1) tra cứu thu hồi
  std::unordered_set<uint64_t> throttled_pids_;
  std::vector<IdleAlert> alerts_;
};

}  // namespace yui::guard

#endif  // YUI_GUARD_HPP
