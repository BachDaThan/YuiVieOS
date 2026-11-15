// Hardware Auto-Profiler (Tầng 1): reads the host, classifies it, and feeds the
// elastic k/W autotuner.  Used by `yuid --info`, by the boot service that writes
// /var/lib/yui/profile.json, and by the Tầng 5 shell's status bar.
#ifndef YUI_PROFILE_HPP
#define YUI_PROFILE_HPP

#include <string>

#include "yui/engine.hpp"
#include "yui/json.hpp"

namespace yui::profile {

struct HostProfile {
  std::string arch;
  std::string cpu_model;
  int threads = 0;
  int cores = 0;
  double ram_gb = 0.0;
  double ram_available_gb = 0.0;
  double load1 = 0.0;
  std::string gpu;
  std::string kernel;
  std::string distro;
  bool has_battery = false;
  int battery_percent = -1;
  bool on_battery = false;
  bool charging = false;
  double temperature_c = -1.0;  // -1 when no sensor is readable
  engine::SimdInfo simd;
  std::string tier;      // "may-yeu" | "can-bang" | "may-manh"
  std::string summary;   // Vietnamese one-liner for the shell
  engine::TuneInput tune_input;
  engine::TuneResult tune;
};

HostProfile probe();                      // live host inspection (never throws)
std::string classify(const HostProfile&); // tier decision, exposed for tests
json::Value to_json(const HostProfile&);
std::string summary_vi(const HostProfile&);

}  // namespace yui::profile

#endif  // YUI_PROFILE_HPP
