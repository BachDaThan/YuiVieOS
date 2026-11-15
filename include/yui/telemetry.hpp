// Telemetry Hook — the sensor contract between YuiVieOS and the AI core
// (Tầng 4, "cổng chờ" `/ai-bridge-yuid`).
//
// The bridge never decides how the model runs; it publishes *facts* about the
// machine (battery, charging, CPU/GPU heat, free RAM) and one *recommendation*
// (k / W) that the AI core may follow or override with a reason.
//
// Two rules keep this honest:
//   * every unreadable sensor is reported as unknown (-1 / false), never guessed;
//   * the advice function is pure, so it can be unit tested on any machine.
#ifndef YUI_TELEMETRY_HPP
#define YUI_TELEMETRY_HPP

#include <string>

#include "yui/json.hpp"
#include "yui/profile.hpp"

namespace yui::telemetry {

/// Ngưỡng quyết định chế độ tiết kiệm. Mặc định đúng đặc tả Phase P1:
/// pin < 20 % hoặc nhiệt độ > 75 °C ⇒ hạ xuống k=1, W=16.
struct Thresholds {
  int low_battery_percent = 20;
  double hot_c = 75.0;
};

/// Một lần đọc cảm biến. Trường không đọc được giữ -1 (số) hoặc false (cờ).
struct Reading {
  bool battery_present = false;
  int battery_percent = -1;
  bool charging = false;
  double cpu_c = -1.0;
  double gpu_c = -1.0;
  long ram_total_mb = 0;
  long ram_free_mb = 0;
  std::string machine_class = "can-bang";  // may-yeu | can-bang | may-manh
  std::string simd = "scalar";
  int threads = 1;
  std::string source = "sysfs";  // sysfs | fake | profile
};

/// Khuyến nghị cho lõi AI: k (số chuyên gia/kênh), W (cửa sổ ngữ cảnh), lý do.
struct Advice {
  int k = 1;
  int window = 16;
  std::string mode;    // "tiet-kiem" | "can-bang" | "toi-da"
  std::string reason;  // tiếng Việt, hiển thị được cho người dùng
};

/// Đọc cảm biến thật của máy (sysfs + /proc qua profile::probe). Không ném.
Reading read();

/// Chuyển hồ sơ phần cứng đã dò sẵn thành Reading (dùng cho test và cho boot).
Reading from_profile(const profile::HostProfile& host, const std::string& source = "profile");

/// Quy tắc khuyến nghị — hàm thuần, không đọc gì bên ngoài.
///   tiet-kiem : máy yếu, hoặc pin < ngưỡng khi không sạc, hoặc nhiệt > ngưỡng
///   toi-da    : máy mạnh và mát
///   can-bang  : còn lại
Advice advise(const Reading& reading, const Thresholds& thresholds = {});

/// Nhiệt độ GPU tính bằng °C, -1 nếu không có cảm biến.
double read_gpu_c();

/// Nhiệt độ CPU (gói/SOC) tính bằng °C, -1 nếu không có cảm biến.
double read_cpu_c();

json::Value to_json(const Reading& reading, const Advice& advice);
json::Value to_json(const Reading& reading);

/// Một dòng tiếng Việt cho shell/log, ví dụ:
/// "Pin 37% (đang xả) · CPU 71.5°C · RAM trống 402/1843 MB · máy yếu → k=1, W=16"
std::string summary_vi(const Reading& reading, const Advice& advice);

}  // namespace yui::telemetry

#endif  // YUI_TELEMETRY_HPP
