// Telemetry Hook — sensors for the AI bridge (Tầng 4).
//
// Everything here is *policy*, not mechanism: it reads what the kernel already
// exposes and turns it into two things the AI core can act on — facts and a
// recommendation.  Nothing is invented: a missing sensor stays "unknown".
#include "yui/telemetry.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <string>
#include <vector>

namespace yui::telemetry {
namespace {

bool read_first_line(const std::string& path, std::string* out) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  char buf[256];
  const size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
  std::fclose(f);
  if (n == 0) return false;
  buf[n] = '\0';
  std::string s(buf);
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '\t'))
    s.pop_back();
  *out = s;
  return true;
}

bool read_long(const std::string& path, long* out) {
  std::string s;
  if (!read_first_line(path, &s)) return false;
  if (s.empty()) return false;
  char* end = nullptr;
  const long v = std::strtol(s.c_str(), &end, 10);
  if (end == s.c_str()) return false;
  *out = v;
  return true;
}

std::vector<std::string> list_dir(const std::string& path) {
  std::vector<std::string> names;
  DIR* d = ::opendir(path.c_str());
  if (!d) return names;
  while (struct dirent* e = ::readdir(d)) {
    if (e->d_name[0] == '.') continue;
    names.push_back(e->d_name);
  }
  ::closedir(d);
  return names;
}

bool contains_ci(const std::string& haystack, const char* needle) {
  std::string h;
  h.reserve(haystack.size());
  for (char c : haystack) h.push_back(char(std::tolower(static_cast<unsigned char>(c))));
  return h.find(needle) != std::string::npos;
}

// thermal_zone*/temp is millidegrees Celsius on every Linux platform we target.
double zone_temp_c(const std::string& zone) {
  long milli = 0;
  if (!read_long("/sys/class/thermal/" + zone + "/temp", &milli)) return -1.0;
  if (milli <= 0 || milli > 200000) return -1.0;  // nonsense reading
  return double(milli) / 1000.0;
}

std::string zone_type(const std::string& zone) {
  std::string type;
  if (!read_first_line("/sys/class/thermal/" + zone + "/type", &type)) return std::string();
  return type;
}

}  // namespace

double read_gpu_c() {
  // 1) thermal zones whose type mentions a GPU driver.
  static const char* kGpuHints[] = {"gpu", "amdgpu", "nouveau", "i915", "nvidia", "panfrost", "mali"};
  for (const auto& zone : list_dir("/sys/class/thermal")) {
    if (zone.rfind("thermal_zone", 0) != 0) continue;
    const std::string type = zone_type(zone);
    if (type.empty()) continue;
    for (const char* hint : kGpuHints) {
      if (contains_ci(type, hint)) {
        const double t = zone_temp_c(zone);
        if (t > 0) return t;
      }
    }
  }
  // 2) DRM hwmon nodes (AMD/NVIDIA expose temp1_input there).
  for (const auto& card : list_dir("/sys/class/drm")) {
    if (card.rfind("card", 0) != 0 || card.find('-') != std::string::npos) continue;
    for (const auto& hwmon : list_dir("/sys/class/drm/" + card + "/device/hwmon")) {
      long milli = 0;
      if (read_long("/sys/class/drm/" + card + "/device/hwmon/" + hwmon + "/temp1_input", &milli) &&
          milli > 0 && milli <= 200000) {
        return double(milli) / 1000.0;
      }
    }
  }
  return -1.0;
}

double read_cpu_c() {
  // Prefer the package/soc zone, fall back to the hottest readable zone.
  double hottest = -1.0;
  for (const auto& zone : list_dir("/sys/class/thermal")) {
    if (zone.rfind("thermal_zone", 0) != 0) continue;
    const std::string type = zone_type(zone);
    const double t = zone_temp_c(zone);
    if (t <= 0) continue;
    if (contains_ci(type, "pkg") || contains_ci(type, "cpu") || contains_ci(type, "soc") ||
        contains_ci(type, "x86"))
      return t;
    if (t > hottest) hottest = t;
  }
  return hottest;
}

Reading from_profile(const profile::HostProfile& host, const std::string& source) {
  Reading r;
  r.source = source;
  r.battery_present = host.has_battery;
  r.battery_percent = host.has_battery ? host.battery_percent : -1;
  r.charging = host.charging;
  r.cpu_c = host.temperature_c;
  r.gpu_c = -1.0;
  r.ram_total_mb = long(std::lround(host.ram_gb * 1024.0));
  r.ram_free_mb = long(std::lround(host.ram_available_gb * 1024.0));
  if (r.ram_total_mb < 0) r.ram_total_mb = 0;
  if (r.ram_free_mb < 0) r.ram_free_mb = 0;
  r.machine_class = host.tier.empty() ? "can-bang" : host.tier;
  r.simd = host.simd.best.empty() ? "scalar" : host.simd.best;
  r.threads = host.threads > 0 ? host.threads : 1;
  return r;
}

Reading read() {
  const profile::HostProfile host = profile::probe();
  Reading r = from_profile(host, "sysfs");
  const double gpu = read_gpu_c();
  // profile::probe() reports the hottest CPU-side sensor; re-read a dedicated
  // CPU sensor for the field that the AI core actually consumes.
  const double cpu = read_cpu_c();
  r.gpu_c = gpu;
  if (cpu > 0) r.cpu_c = cpu;
  return r;
}

Advice advise(const Reading& r, const Thresholds& t) {
  Advice a;
  const bool battery_low = r.battery_present && r.battery_percent >= 0 &&
                           r.battery_percent < t.low_battery_percent && !r.charging;
  const double heat = std::max(r.cpu_c, r.gpu_c);
  const bool too_hot = heat > t.hot_c;
  const bool ultra_weak = r.machine_class == "sieu-yeu";
  const bool weak = r.machine_class == "may-yeu";
  const bool strong = r.machine_class == "may-manh";

  if (ultra_weak || weak || battery_low || too_hot) {
    a.mode = "tiet-kiem";
    a.k = 1;
    a.window = 16;
    if (ultra_weak) {
      a.reason = "Máy siêu yếu (RAM < 1.5 GB): Headless/TUI, zero-AI fallback (BM25/luật), k=1, W=16";
    } else if (too_hot) {
      char buf[128];
      std::snprintf(buf, sizeof buf, "Nhiệt độ %.1f°C vượt ngưỡng %.0f°C: hạ về k=1, W=16 để giữ máy mát",
                    heat, t.hot_c);
      a.reason = buf;
    } else if (battery_low) {
      char buf[160];
      std::snprintf(buf, sizeof buf, "Pin %d%% và đang xả: hạ về k=1, W=16 để giữ pin", r.battery_percent);
      a.reason = buf;
    } else {
      a.reason = "Máy yếu (RAM/nhân ít): giữ k=1, W=16 để phản hồi nhanh và không tràn RAM";
    }
    return a;
  }

  if (strong) {
    a.mode = "toi-da";
    a.k = r.charging ? 4 : 3;
    a.window = r.charging ? 1024 : 768;
    a.reason = r.charging
                   ? "Máy mạnh và đang cắm sạc: mở k=4, W=1024 (suy luận sâu, đoán trước token)"
                   : "Máy mạnh, đang dùng pin: k=3, W=768 để cân bằng nhiệt và pin";
    return a;
  }

  a.mode = "can-bang";
  a.k = 2;
  a.window = r.charging ? 512 : 256;
  a.reason = r.charging ? "Máy cân bằng và đang sạc: k=2, W=512"
                        : "Máy cân bằng, dùng pin: k=2, W=256";
  return a;
}

json::Value to_json(const Reading& r) {
  json::Value v(json::Object{});
  json::Value battery(json::Object{});
  battery.set("present", r.battery_present);
  battery.set("percent", r.battery_percent);
  battery.set("charging", r.charging);
  v.set("battery", battery);

  json::Value thermal(json::Object{});
  thermal.set("cpu_c", r.cpu_c);
  thermal.set("gpu_c", r.gpu_c);
  thermal.set("throttle", std::max(r.cpu_c, r.gpu_c) > 75.0);
  v.set("thermal", thermal);

  json::Value memory(json::Object{});
  memory.set("total_mb", double(r.ram_total_mb));
  memory.set("free_mb", double(r.ram_free_mb));
  v.set("memory", memory);

  json::Value machine(json::Object{});
  machine.set("class", r.machine_class);
  machine.set("simd", r.simd);
  machine.set("threads", double(r.threads));
  v.set("machine", machine);

  v.set("source", r.source);
  return v;
}

json::Value to_json(const Reading& r, const Advice& a) {
  json::Value v = to_json(r);
  json::Value advice(json::Object{});
  advice.set("k", double(a.k));
  advice.set("window", double(a.window));
  advice.set("mode", a.mode);
  advice.set("reason", a.reason);
  v.set("advice", advice);
  return v;
}

std::string summary_vi(const Reading& r, const Advice& a) {
  std::string out;
  char buf[256];
  if (r.battery_present && r.battery_percent >= 0) {
    std::snprintf(buf, sizeof buf, "Pin %d%% (%s)", r.battery_percent,
                  r.charging ? "đang sạc" : "đang xả");
    out += buf;
  } else {
    out += "Không có pin";
  }
  if (r.cpu_c > 0) {
    std::snprintf(buf, sizeof buf, " · CPU %.1f°C", r.cpu_c);
    out += buf;
  }
  if (r.gpu_c > 0) {
    std::snprintf(buf, sizeof buf, " · GPU %.1f°C", r.gpu_c);
    out += buf;
  }
  std::snprintf(buf, sizeof buf, " · RAM trống %ld/%ld MB · %s", r.ram_free_mb, r.ram_total_mb,
                r.machine_class.c_str());
  out += buf;
  std::snprintf(buf, sizeof buf, " → k=%d, W=%d (%s)", a.k, a.window, a.mode.c_str());
  out += buf;
  return out;
}

}  // namespace yui::telemetry
