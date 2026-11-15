// Host probing for the Hardware Auto-Profiler.  Every file read is optional:
// the profiler must work on a container, a laptop and a board with no sysfs.
#include "yui/profile.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

#include "yui/text.hpp"

namespace yui::profile {
namespace {

std::string read_file(const std::string& path, size_t limit = 8192) {
  std::ifstream f(path);
  if (!f) return "";
  std::string s;
  s.resize(limit);
  f.read(&s[0], std::streamsize(limit));
  s.resize(size_t(f.gcount()));
  return s;
}

bool read_double(const std::string& path, double& out) {
  const std::string s = yui::text::trim(read_file(path, 256));
  if (s.empty()) return false;
  char* end = nullptr;
  const double v = std::strtod(s.c_str(), &end);
  if (end == s.c_str()) return false;
  out = v;
  return true;
}

std::string uname_field(const char* path) {
  return yui::text::trim(read_file(path, 512));
}

void read_cpuinfo(HostProfile& p) {
  std::istringstream in(read_file("/proc/cpuinfo", 1 << 16));
  std::string line;
  int logical = 0;
  while (std::getline(in, line)) {
    if (line.empty()) continue;
    const size_t colon = line.find(':');
    if (colon == std::string::npos) continue;
    const std::string key = yui::text::trim(line.substr(0, colon));
    const std::string value = yui::text::trim(line.substr(colon + 1));
    if (key == "processor") ++logical;
    if (p.cpu_model.empty() && (key == "model name" || key == "Model" || key == "uarch")) p.cpu_model = value;
    if (key == "cpu cores" && p.cores == 0) p.cores = std::atoi(value.c_str());
  }
  if (logical > 0) p.threads = logical;
  if (p.cores == 0) p.cores = p.threads;
}

void read_meminfo(HostProfile& p) {
  std::istringstream in(read_file("/proc/meminfo", 1 << 14));
  std::string line;
  while (std::getline(in, line)) {
    const size_t colon = line.find(':');
    if (colon == std::string::npos) continue;
    const std::string key = line.substr(0, colon);
    const double kb = std::atof(line.c_str() + colon + 1);
    if (key == "MemTotal")
      p.ram_gb = kb / 1024.0 / 1024.0;
    else if (key == "MemAvailable")
      p.ram_available_gb = kb / 1024.0 / 1024.0;
  }
}

void read_battery(HostProfile& p) {
  for (int i = 0; i < 4; ++i) {
    const std::string base = "/sys/class/power_supply/BAT" + std::to_string(i);
    double capacity = 0;
    if (!read_double(base + "/capacity", capacity)) continue;
    p.has_battery = true;
    p.battery_percent = int(capacity);
    const std::string status = yui::text::trim(read_file(base + "/status", 64));
    p.on_battery = (status == "Discharging");
    p.charging = (status == "Charging");
    break;
  }
}

void read_thermal(HostProfile& p) {
  double best = -1;
  for (int i = 0; i < 8; ++i) {
    double raw = 0;
    const std::string zone = "/sys/class/thermal/thermal_zone" + std::to_string(i) + "/temp";
    if (!read_double(zone, raw)) continue;
    const double celsius = raw > 1000.0 ? raw / 1000.0 : raw;
    if (celsius > best && celsius < 150.0) best = celsius;
  }
  if (best > 0) p.temperature_c = best;
}

void read_gpu(HostProfile& p) {
  for (int i = 0; i < 4; ++i) {
    const std::string vendor_path = "/sys/class/drm/card" + std::to_string(i) + "/device/vendor";
    const std::string vendor = yui::text::trim(read_file(vendor_path, 32));
    if (vendor.empty()) continue;
    std::string name;
    if (vendor == "0x10de") name = "NVIDIA";
    else if (vendor == "0x1002") name = "AMD";
    else if (vendor == "0x8086") name = "Intel";
    else name = vendor;
    if (!name.empty()) p.gpu = (p.gpu.empty() ? "" : p.gpu + ", ") + name + " (card" + std::to_string(i) + ")";
  }
  if (p.gpu.empty()) {
    const std::string fb = yui::text::trim(read_file("/sys/class/graphics/fb0/name", 64));
    if (!fb.empty()) p.gpu = fb;
  }
}

}  // namespace

std::string classify(const HostProfile& p) {
  const bool weak = p.ram_gb > 0 && p.ram_gb < 4.0;
  const bool strong = (p.ram_gb >= 16.0 && p.threads >= 8) || (p.threads >= 16 && p.ram_gb >= 8.0);
  if (weak || p.threads <= 2) return "may-yeu";
  if (strong) return "may-manh";
  return "can-bang";
}

std::string summary_vi(const HostProfile& p) {
  std::ostringstream os;
  os << "Máy " << (p.tier == "may-yeu" ? "yếu" : (p.tier == "may-manh" ? "mạnh" : "cân bằng"));
  if (!p.cpu_model.empty()) os << " · " << p.cpu_model;
  os << " · " << p.threads << " luồng";
  if (p.ram_gb > 0) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), " · RAM %.1f GB", p.ram_gb);
    os << buf;
  }
  if (p.has_battery) os << " · pin " << p.battery_percent << "%" << (p.on_battery ? " (đang xả)" : "");
  if (p.temperature_c > 0) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), " · %.1f°C", p.temperature_c);
    os << buf;
  }
  os << " → k=" << p.tune.k << ", cửa sổ " << p.tune.window;
  return os.str();
}

HostProfile probe() {
  HostProfile p;
  p.simd = engine::detect_simd();
  p.arch = p.simd.arch;
  read_cpuinfo(p);
  if (p.cpu_model.empty()) p.cpu_model = p.simd.cpu_name;
  read_meminfo(p);
  read_battery(p);
  read_thermal(p);
  read_gpu(p);

  double load = 0;
  if (read_double("/proc/loadavg", load)) p.load1 = load;

  p.kernel = uname_field("/proc/sys/kernel/osrelease");
  std::istringstream os_release(read_file("/etc/os-release", 4096));
  std::string line;
  while (std::getline(os_release, line)) {
    if (yui::text::starts_with(line, "PRETTY_NAME=")) {
      p.distro = yui::text::trim(line.substr(12));
      if (!p.distro.empty() && p.distro.front() == '"') p.distro = p.distro.substr(1, p.distro.size() - 2);
      break;
    }
  }

  p.tier = classify(p);
  p.tune_input.ram_gb = p.ram_gb > 0 ? p.ram_gb : 8.0;
  p.tune_input.temperature_c = p.temperature_c > 0 ? p.temperature_c : 45.0;
  p.tune_input.on_battery = p.on_battery;
  p.tune_input.cpu_threads = p.threads > 0 ? p.threads : 4;
  p.tune_input.dedicated_gpu = p.gpu.find("NVIDIA") != std::string::npos || p.gpu.find("AMD") != std::string::npos;
  p.tune = engine::auto_tune(p.tune_input);
  p.summary = summary_vi(p);
  return p;
}

json::Value to_json(const HostProfile& p) {
  json::Value v(json::Object{});
  v.set("arch", p.arch);
  v.set("cpu", p.cpu_model);
  v.set("threads", p.threads);
  v.set("cores", p.cores);
  v.set("ram_gb", p.ram_gb);
  v.set("ram_available_gb", p.ram_available_gb);
  v.set("load1", p.load1);
  v.set("gpu", p.gpu);
  v.set("kernel", p.kernel);
  v.set("distro", p.distro);
  v.set("has_battery", p.has_battery);
  v.set("battery_percent", p.battery_percent);
  v.set("on_battery", p.on_battery);
  v.set("charging", p.charging);
  v.set("temperature_c", p.temperature_c);
  json::Value simd(json::Object{});
  simd.set("best", p.simd.best);
  simd.set("avx2", p.simd.avx2);
  simd.set("avx512", p.simd.avx512);
  simd.set("fma", p.simd.fma);
  simd.set("neon", p.simd.neon);
  simd.set("rvv", p.simd.rvv);
  simd.set("vector_bits", int(p.simd.vector_bits));
  v.set("simd", simd);
  v.set("tier", p.tier);
  json::Value tune(json::Object{});
  tune.set("k", p.tune.k);
  tune.set("window", p.tune.window);
  tune.set("threads", p.tune.threads);
  tune.set("profile", p.tune.profile);
  tune.set("reason", p.tune.reason);
  v.set("tune", tune);
  v.set("summary", p.summary);
  return v;
}

}  // namespace yui::profile
