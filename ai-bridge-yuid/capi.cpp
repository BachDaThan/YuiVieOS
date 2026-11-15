// libyui C ABI implementation.
#include "yui/yui.h"

#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>

#include "yui/codec.hpp"
#include "yui/crypto.hpp"
#include "yui/engine.hpp"
#include "yui/graph.hpp"
#include "yui/ime.hpp"
#include "yui/json.hpp"
#include "yui/sandbox.hpp"
#include "yui/telemetry.hpp"
#include "yui/service.hpp"
#include "yui/vault.hpp"

using yui::engine::detect_simd;
using yui::graph::Graph;
using yui::json::Value;

struct yui_graph {
  Graph g;
};

namespace {

// Copies into a fixed-size C buffer, always NUL terminated, never overflows.
void copy_fixed(char* out, size_t cap, const std::string& s) {
  if (!out || cap == 0) return;
  const size_t n = s.size() < cap - 1 ? s.size() : cap - 1;
  std::memcpy(out, s.data(), n);
  out[n] = '\0';
}

void copy_out(const std::string& s, char* out, size_t cap, int* rc) {
  if (cap <= s.size()) {
    if (out && cap) out[0] = '\0';
    *rc = YUI_E_TOO_SMALL;
    return;
  }
  std::memcpy(out, s.data(), s.size());
  out[s.size()] = '\0';
  *rc = int(s.size());
}

}  // namespace

extern "C" {

const char* yui_version(void) { return yui::service::version(); }
const char* yui_arch(void) {
  // Static storage: the string literals live for the lifetime of the process.
  static const std::string arch = detect_simd().arch;
  return arch.c_str();
}
int yui_abi_version(void) { return YUI_ABI_VERSION; }

int yui_simd(char* out, size_t cap) {
  if (!out || cap == 0) return YUI_E_INVALID;
  const std::string best = detect_simd().best;
  int rc = 0;
  copy_out(best, out, cap, &rc);
  return rc;
}

int yui_sha256(const void* data, size_t len, char out_hex[65]) {
  if (!out_hex) return YUI_E_INVALID;
  const auto d = yui::crypto::sha256(data, len);
  std::memcpy(out_hex, yui::crypto::hex(d).c_str(), 64);
  out_hex[64] = '\0';
  return YUI_OK;
}

int yui_pack(const void* data, size_t len, int level, yui_buf* out) {
  if (!out) return YUI_E_INVALID;
  out->data = nullptr;
  out->len = 0;
  if (!data && len) return YUI_E_INVALID;
  const yui::codec::Bytes in(static_cast<const uint8_t*>(data),
                             static_cast<const uint8_t*>(data) + len);
  const auto packed = yui::codec::pack(in, level);
  uint8_t* buf = static_cast<uint8_t*>(std::malloc(packed.size()));
  if (!buf) return YUI_E_NOMEM;
  std::memcpy(buf, packed.data(), packed.size());
  out->data = buf;
  out->len = packed.size();
  return YUI_OK;
}

int yui_unpack(const void* data, size_t len, yui_buf* out, char* err, size_t err_cap) {
  if (!out) return YUI_E_INVALID;
  out->data = nullptr;
  out->len = 0;
  if (!data) return YUI_E_INVALID;
  const yui::codec::Bytes in(static_cast<const uint8_t*>(data),
                             static_cast<const uint8_t*>(data) + len);
  yui::codec::Bytes plain;
  std::string e;
  if (!yui::codec::unpack(in, plain, nullptr, &e)) {
    if (err && err_cap) {
      const size_t n = e.size() < err_cap - 1 ? e.size() : err_cap - 1;
      std::memcpy(err, e.data(), n);
      err[n] = '\0';
    }
    return YUI_E_CORRUPT;
  }
  uint8_t* buf = static_cast<uint8_t*>(std::malloc(plain.empty() ? 1 : plain.size()));
  if (!buf) return YUI_E_NOMEM;
  if (!plain.empty()) std::memcpy(buf, plain.data(), plain.size());
  out->data = buf;
  out->len = plain.size();
  return YUI_OK;
}

void yui_buf_free(yui_buf* buf) {
  if (!buf) return;
  std::free(buf->data);
  buf->data = nullptr;
  buf->len = 0;
}

int yui_tune_auto(int ram_decigb, int temperature_c, int on_battery, int threads, int has_gpu,
                  yui_tune* out) {
  if (!out) return YUI_E_INVALID;
  yui::engine::TuneInput in;
  in.ram_gb = ram_decigb / 10.0;
  in.temperature_c = temperature_c;
  in.on_battery = on_battery != 0;
  in.cpu_threads = threads;
  in.dedicated_gpu = has_gpu != 0;
  const auto r = yui::engine::auto_tune(in);
  out->k = r.k;
  out->window = r.window;
  out->threads = r.threads;
  std::snprintf(out->profile, sizeof(out->profile), "%s", r.profile.c_str());
  std::snprintf(out->reason, sizeof(out->reason), "%s", r.reason.c_str());
  return YUI_OK;
}

yui_graph* yui_graph_new(void) {
  try {
    return new yui_graph();
  } catch (const std::bad_alloc&) {
    return nullptr;
  }
}

void yui_graph_free(yui_graph* g) { delete g; }

int yui_graph_upsert(yui_graph* g, const char* id, const char* label, const char* kind) {
  if (!g || !id) return YUI_E_INVALID;
  g->g.upsert(id, label ? label : id, yui::graph::kind_from(kind ? kind : "unknown"));
  return YUI_OK;
}

int yui_graph_link(yui_graph* g, const char* from, const char* rel, const char* to, double weight) {
  if (!g || !from || !rel || !to) return YUI_E_INVALID;
  return g->g.link(from, rel, to, weight) ? YUI_OK : YUI_E_INVALID;
}

char* yui_graph_dump(yui_graph* g) {
  if (!g) return nullptr;
  const std::string doc = g->g.dump(false);
  char* out = static_cast<char*>(std::malloc(doc.size() + 1));
  if (!out) return nullptr;
  std::memcpy(out, doc.data(), doc.size());
  out[doc.size()] = '\0';
  return out;
}

int yui_graph_import(yui_graph* g, const char* json_doc) {
  if (!g || !json_doc) return YUI_E_INVALID;
  auto parsed = yui::json::parse(json_doc);
  if (!parsed.ok) return YUI_E_INVALID;
  std::string err;
  return g->g.from_json(parsed.value, &err) ? YUI_OK : YUI_E_INVALID;
}

size_t yui_graph_nodes(const yui_graph* g) { return g ? g->g.stats().nodes : 0; }

int yui_graph_bfs(yui_graph* g, const char* start, int max_depth, char* out, size_t cap) {
  if (!g || !start || !out) return YUI_E_INVALID;
  const auto ids = g->g.bfs(start, max_depth);
  std::string s;
  for (size_t i = 0; i < ids.size(); ++i) {
    if (i) s += '\n';
    s += ids[i];
  }
  int rc = 0;
  copy_out(s, out, cap, &rc);
  return rc;
}

int yui_telex(const char* keys, char* out, size_t cap) {
  if (!keys || !out) return YUI_E_INVALID;
  int rc = 0;
  copy_out(yui::ime::telex(keys), out, cap, &rc);
  return rc;
}

int yui_vni(const char* keys, char* out, size_t cap) {
  if (!keys || !out) return YUI_E_INVALID;
  int rc = 0;
  copy_out(yui::ime::vni(keys), out, cap, &rc);
  return rc;
}

int yui_vault_seal(const char* password, const void* plain, size_t len, int log2n, yui_buf* out) {
  if (!password || !out || (!plain && len)) return YUI_E_INVALID;
  out->data = nullptr;
  out->len = 0;
  yui::vault::Params params;
  if (log2n > 0) params.log2_n = uint8_t(log2n);
  const yui::vault::Bytes in(static_cast<const uint8_t*>(plain),
                             static_cast<const uint8_t*>(plain) + len);
  try {
    const auto blob = yui::vault::seal(password, in, params);
    uint8_t* buf = static_cast<uint8_t*>(std::malloc(blob.size()));
    if (!buf) return YUI_E_NOMEM;
    std::memcpy(buf, blob.data(), blob.size());
    out->data = buf;
    out->len = blob.size();
    return YUI_OK;
  } catch (...) {
    return YUI_E_INVALID;
  }
}

int yui_vault_open(const char* password, const void* blob, size_t len, yui_buf* out, char* err,
                   size_t err_cap) {
  if (!password || !out || !blob) return YUI_E_INVALID;
  out->data = nullptr;
  out->len = 0;
  const yui::vault::Bytes in(static_cast<const uint8_t*>(blob),
                             static_cast<const uint8_t*>(blob) + len);
  yui::vault::Bytes plain;
  std::string e;
  if (!yui::vault::open(password, in, plain, &e)) {
    if (err && err_cap) {
      const size_t n = e.size() < err_cap - 1 ? e.size() : err_cap - 1;
      std::memcpy(err, e.data(), n);
      err[n] = '\0';
    }
    return YUI_E_AUTH;
  }
  uint8_t* buf = static_cast<uint8_t*>(std::malloc(plain.empty() ? 1 : plain.size()));
  if (!buf) return YUI_E_NOMEM;
  if (!plain.empty()) std::memcpy(buf, plain.data(), plain.size());
  out->data = buf;
  out->len = plain.size();
  return YUI_OK;
}

int yui_telemetry(yui_telemetry_snapshot* out) {
  if (!out) return YUI_E_INVALID;
  const yui::telemetry::Reading r = yui::telemetry::read();
  const yui::telemetry::Advice a = yui::telemetry::advise(r);
  out->battery_present = r.battery_present ? 1 : 0;
  out->battery_percent = r.battery_percent;
  out->charging = r.charging ? 1 : 0;
  out->cpu_c = float(r.cpu_c);
  out->gpu_c = float(r.gpu_c);
  out->ram_total_mb = int(r.ram_total_mb);
  out->ram_free_mb = int(r.ram_free_mb);
  copy_fixed(out->machine_class, sizeof out->machine_class, r.machine_class);
  copy_fixed(out->simd, sizeof out->simd, r.simd);
  out->threads = r.threads;
  out->advised_k = a.k;
  out->advised_window = a.window;
  copy_fixed(out->reason, sizeof out->reason, a.reason);
  return YUI_OK;
}

int yui_telemetry_advise(const char* machine_class, int battery_percent, int charging, float cpu_c,
                         float gpu_c, int* out_k, int* out_window, char* reason, size_t reason_cap) {
  if (!out_k || !out_window) return YUI_E_INVALID;
  yui::telemetry::Reading r;
  r.machine_class = (machine_class && *machine_class) ? machine_class : "can-bang";
  r.battery_present = battery_percent >= 0;
  r.battery_percent = battery_percent;
  r.charging = charging != 0;
  r.cpu_c = cpu_c;
  r.gpu_c = gpu_c;
  const yui::telemetry::Advice a = yui::telemetry::advise(r);
  *out_k = a.k;
  *out_window = a.window;
  if (reason && reason_cap) {
    copy_fixed(reason, reason_cap, a.reason);
  }
  return YUI_OK;
}

int yui_sandbox_check(const char* policy_json, const char* kind, const char* target, char* reason,
                      size_t reason_cap) {
  if (!policy_json || !kind) return YUI_E_INVALID;
  auto parsed = yui::json::parse(policy_json);
  if (!parsed.ok) return YUI_E_INVALID;
  std::string err;
  const auto policy = yui::sandbox::Policy::from_json(parsed.value, &err);
  if (!err.empty()) return YUI_E_INVALID;
  yui::sandbox::Action action;
  action.kind = kind;
  action.target = target ? target : "";
  yui::sandbox::Enforcer enforcer;
  const auto d = enforcer.check(policy, nullptr, action, 0);
  if (reason && reason_cap) {
    const size_t n = d.reason.size() < reason_cap - 1 ? d.reason.size() : reason_cap - 1;
    std::memcpy(reason, d.reason.data(), n);
    reason[n] = '\0';
  }
  return d.allowed ? 1 : 0;
}

void yui_free(void* ptr) { std::free(ptr); }

}  // extern "C"
