// Guard chống bất thường nền — xem include/yui/guard.hpp.
#include "yui/guard.hpp"

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

}  // namespace yui::guard
