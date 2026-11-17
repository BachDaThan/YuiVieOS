// Guard chống bất thường nền — xem include/yui/guard.hpp.
#include "yui/guard.hpp"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>

#include "yui/sandbox.hpp"  // kRightComputeHeavy (CAP_COMPUTE_HEAVY)

namespace yui::guard {
namespace {

std::string fmt(const char* f, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, f);
  std::vsnprintf(buf, sizeof buf, f, ap);
  va_end(ap);
  return std::string(buf);
}

}  // namespace

std::optional<Alert> Guard::observe(const ProcessSample& s) {
  const bool legit = s.has_window || (s.caps & sandbox::kRightComputeHeavy) != 0;
  if (legit) {
    // Có cửa sổ hoặc mang quyền hợp lệ: không theo dõi, không bóp.
    auto it = tracks_.find(s.pid);
    if (it != tracks_.end()) {
      if (it->second.throttled) {
        // Đã lỡ bóp trước khi người dùng cấp quyền/giao diện — tháo ra.
        tracks_.erase(it);
        return std::nullopt;
      }
      tracks_.erase(it);
    }
    return std::nullopt;
  }

  Track& tr = tracks_[s.pid];
  const bool over = s.cpu_percent > t_.cpu_percent || s.gpu_percent > t_.gpu_percent;
  if (!over) {
    // Tải tụt xuống: chuỗi vi phạm đứt — chờ chuỗi mới.
    tr.over_since_ns = -1;
    return std::nullopt;
  }
  if (tr.over_since_ns < 0) tr.over_since_ns = s.now_ns;
  const int64_t elapsed_ns = s.now_ns - tr.over_since_ns;
  if (tr.alerted || elapsed_ns < t_.sustain_ns) return std::nullopt;

  tr.alerted = true;
  tr.throttled = true;

  Alert a;
  a.pid = s.pid;
  a.name = s.name;
  a.when_ns = s.now_ns;
  a.cpu_percent = s.cpu_percent;
  a.gpu_percent = s.gpu_percent;
  a.throttle_percent = t_.throttle_percent;
  a.demoted = true;
  a.reason_vi = fmt(
      "phát hiện tải CPU %.1f%% / GPU %.1f%% kéo dài %.1f giây ở tiến trình nền "
      "\"%s\" (pid %llu) không có cửa sổ, không có quyền CAP_COMPUTE_HEAVY — "
      "nghi ngờ chạy ngầm (đào coin / tính toán trộm); đã hạ xuống lớp Idle/Background "
      "và bóp xung còn %d%%",
      s.cpu_percent, s.gpu_percent, double(elapsed_ns) / 1e9, s.name.c_str(),
      static_cast<unsigned long long>(s.pid), t_.throttle_percent);
  a.notify_vi = fmt(
      "Tiến trình \"%s\" (pid %llu) chạy nền ở CPU %.1f%% / GPU %.1f%% hơn %.0f giây "
      "mà không có cửa sổ hay quyền CAP_COMPUTE_HEAVY. Hệ thống đã hạ xuống lớp Idle "
      "và bóp xung còn %d%%. Bạn muốn tiêu diệt, gỡ bỏ, hay để yên?",
      s.name.c_str(), static_cast<unsigned long long>(s.pid), s.cpu_percent,
      s.gpu_percent, double(elapsed_ns) / 1e9, t_.throttle_percent);
  alerts_.push_back(a);
  return a;
}

bool Guard::clear(uint64_t pid) { return tracks_.erase(pid) > 0; }

bool Guard::is_throttled(uint64_t pid) const {
  auto it = tracks_.find(pid);
  return it != tracks_.end() && it->second.throttled;
}

Verdict Guard::verdict(uint64_t pid) const {
  auto it = tracks_.find(pid);
  if (it == tracks_.end()) return Verdict::Normal;
  if (it->second.throttled) return Verdict::Throttled;
  return Verdict::Watching;
}

int Guard::throttle_percent(uint64_t pid) const {
  return is_throttled(pid) ? t_.throttle_percent : 100;
}

// ============================================================================
// Shannon entropy & RansomwareDetector (P2.14)
// ============================================================================

double shannon_entropy(const uint8_t* data, size_t len) {
  if (!data || len == 0) return 0.0;
  size_t count[256] = {0};
  for (size_t i = 0; i < len; ++i) {
    count[data[i]]++;
  }
  double entropy = 0.0;
  const double inv_len = 1.0 / double(len);
  for (size_t i = 0; i < 256; ++i) {
    if (count[i] > 0) {
      const double p = double(count[i]) * inv_len;
      entropy -= p * std::log2(p);
    }
  }
  return entropy;
}

std::optional<RansomwareAlert> RansomwareDetector::observe_write(
    const RansomwareEvent& ev, const std::string& rollback_snap) {
  if (ev.size == 0 || !ev.data) return std::nullopt;

  const double ent = shannon_entropy(ev.data, ev.size);
  if (ent < cfg_.min_entropy) {
    return std::nullopt;
  }

  ProcessRecord& rec = records_[ev.pid];
  const int64_t cutoff = ev.now_ns - cfg_.window_ns;
  auto it = std::remove_if(rec.writes.begin(), rec.writes.end(),
                           [cutoff](const WriteRecord& w) { return w.timestamp_ns < cutoff; });
  rec.writes.erase(it, rec.writes.end());

  WriteRecord wr;
  wr.timestamp_ns = ev.now_ns;
  wr.entropy = ent;
  wr.path = ev.file_path;
  rec.writes.push_back(wr);

  if (rec.alerted || rec.writes.size() < cfg_.mass_write_count) {
    return std::nullopt;
  }

  rec.alerted = true;
  rec.throttled = true;

  double sum_ent = 0.0;
  for (const auto& w : rec.writes) sum_ent += w.entropy;
  const double avg_ent = sum_ent / double(rec.writes.size());

  RansomwareAlert a;
  a.pid = ev.pid;
  a.proc_name = ev.proc_name;
  a.files_affected = rec.writes.size();
  a.avg_entropy = avg_ent;
  a.when_ns = ev.now_ns;
  a.throttle_percent = cfg_.throttle_percent;
  a.suggested_snapshot = rollback_snap;
  a.demoted = true;

  a.reason_vi = fmt(
      "phát hiện tiến trình \"%s\" (pid %llu) ghi hàng loạt %zu tệp với entropy cao "
      "%.2f bit/byte trong cửa sổ %.1f giây — nghi ngờ mã độc tống tiền (ransomware) "
      "đang mã hoá dữ liệu; đã hạ ưu tiên xuống Idle và bóp xung còn %d%%",
      ev.proc_name.c_str(), static_cast<unsigned long long>(ev.pid), rec.writes.size(),
      avg_ent, double(cfg_.window_ns) / 1e9, cfg_.throttle_percent);

  a.notify_vi = fmt(
      "Cảnh báo mã độc tống tiền: tiến trình \"%s\" (pid %llu) đang ghi đè hàng loạt tệp "
      "với dữ liệu mã hoá (entropy %.2f). Hệ thống đã bóp xung còn %d%%. "
      "Bạn muốn hoàn tác (rollback) về snapshot \"%s\", tiêu diệt tiến trình, hay để yên?",
      ev.proc_name.c_str(), static_cast<unsigned long long>(ev.pid), avg_ent,
      cfg_.throttle_percent, rollback_snap.empty() ? "gần nhất" : rollback_snap.c_str());

  alerts_.push_back(a);
  return a;
}

bool RansomwareDetector::is_throttled(uint64_t pid) const {
  auto it = records_.find(pid);
  return it != records_.end() && it->second.throttled;
}

bool RansomwareDetector::clear(uint64_t pid) {
  return records_.erase(pid) > 0;
}

// ============================================================================
// SensitiveResource & IdleGuard (P2.15)
// ============================================================================

const char* sensitive_resource_name_vi(SensitiveResource res) {
  switch (res) {
    case SensitiveResource::Microphone: return "microphone (ghi âm)";
    case SensitiveResource::Camera: return "camera (máy ảnh)";
    case SensitiveResource::Contacts: return "danh bạ / tệp nhạy cảm";
    case SensitiveResource::Network: return "kết nối mạng";
  }
  return "tài nguyên nhạy cảm";
}

void IdleGuard::record_user_activity(int64_t now_ns) {
  if (now_ns > last_user_activity_ns_) {
    last_user_activity_ns_ = now_ns;
  }
}

bool IdleGuard::is_user_idle(int64_t now_ns) const {
  return (now_ns - last_user_activity_ns_) >= cfg_.idle_timeout_ns;
}

int64_t IdleGuard::idle_duration_ns(int64_t now_ns) const {
  return (now_ns >= last_user_activity_ns_) ? (now_ns - last_user_activity_ns_) : 0;
}

std::optional<IdleAlert> IdleGuard::observe(const IdleActivitySample& s) {
  // Nếu người dùng đang thao tác hoặc có cửa sổ hiển thị hoặc có tương tác trực tiếp
  if (!is_user_idle(s.now_ns) || s.has_window || s.user_initiated) {
    return std::nullopt;
  }

  // Thu hồi thẻ badge với độ phức tạp O(1)
  if (s.token_badge != 0) {
    revoked_badges_.insert(s.token_badge);
  }

  // Bóp xung tiến trình vi phạm
  throttled_pids_.insert(s.pid);

  const int64_t idle_s = idle_duration_ns(s.now_ns) / 1000000000LL;
  const char* res_name = sensitive_resource_name_vi(s.resource);

  IdleAlert a;
  a.pid = s.pid;
  a.proc_name = s.proc_name;
  a.resource = s.resource;
  a.revoked_badge = s.token_badge;
  a.throttle_percent = cfg_.throttle_percent;
  a.idle_duration_s = idle_s;
  a.when_ns = s.now_ns;

  a.reason_vi = fmt(
      "ứng dụng nền \"%s\" (pid %llu) cố truy cập %s khi máy rảnh %.0f giây "
      "(không có thao tác người dùng, không có cửa sổ hiển thị) — "
      "nghi ngờ thu thập lén / exfiltration; đã thu hồi thẻ badge 0x%llx và bóp xung còn %d%%",
      s.proc_name.c_str(), static_cast<unsigned long long>(s.pid), res_name,
      double(idle_s), static_cast<unsigned long long>(s.token_badge), cfg_.throttle_percent);

  a.notify_vi = fmt(
      "Ứng dụng nền \"%s\" vừa cố mở %s khi bạn không dùng máy (đã rảnh %lld giây). "
      "Hệ thống đã thu hồi quyền và bóp xung còn %d%%. Bạn muốn tiếp tục chặn hay cho phép?",
      s.proc_name.c_str(), res_name, static_cast<long long>(idle_s), cfg_.throttle_percent);

  alerts_.push_back(a);
  return a;
}

bool IdleGuard::is_badge_revoked(uint64_t badge) const {
  return badge != 0 && revoked_badges_.find(badge) != revoked_badges_.end();
}

bool IdleGuard::is_throttled(uint64_t pid) const {
  return throttled_pids_.find(pid) != throttled_pids_.end();
}

bool IdleGuard::clear(uint64_t pid) {
  return throttled_pids_.erase(pid) > 0;
}

}  // namespace yui::guard
