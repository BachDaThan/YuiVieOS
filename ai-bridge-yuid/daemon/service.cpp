// yuid dispatch table: one implementation, three transports.
#include "yui/service.hpp"

#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <sys/stat.h>
#include <unistd.h>

#include "yui/apppack.hpp"
#include "yui/b64.hpp"
#include "yui/codec.hpp"
#include "yui/crypto.hpp"
#include "yui/engine.hpp"
#include "yui/ime.hpp"
#include "yui/profile.hpp"
#include "yui/store.hpp"
#include "yui/telemetry.hpp"
#include "yui/text.hpp"

namespace yui::service {
namespace {

using json::Value;

Value make_error(int code, const std::string& message) {
  Value v(json::Object{});
  v.set("code", code);
  v.set("message", message);
  return v;
}

std::string param_string(const Value& p, const char* key, const std::string& def = "") {
  return p[key].is_string() ? p[key].as_string() : def;
}

bool require(const Value& p, const char* key, RpcError& err) {
  if (!p.has(key)) {
    err.code = kInvalidParams;
    err.message = std::string("missing parameter: ") + key;
    return false;
  }
  return true;
}

bool extract_token(const Value& args, const sandbox::Authority& authority, sandbox::Token& out_tok,
                   std::string& err_msg) {
  if (!args.has("token")) {
    err_msg = "thiếu thẻ năng lực (capability token)";
    return false;
  }
  const auto& tok_val = args["token"];
  int64_t now_s = std::time(nullptr);
  if (tok_val.is_string()) {
    const std::string s = tok_val.as_string();
    if (s.find('.') != std::string::npos) {
      return authority.from_bearer(s, out_tok, now_s, &err_msg);
    }
    auto parsed = json::parse(s);
    if (parsed.ok && parsed.value.is_object()) {
      const auto& v = parsed.value;
      out_tok.subject = v["sub"].as_string();
      out_tok.issuer = v["iss"].as_string();
      out_tok.rights = sandbox::rights_from_string(v["rights"].as_string());
      out_tok.issued_at = v["iat"].as_int();
      out_tok.expires_at = v["exp"].as_int();
      out_tok.serial = uint32_t(v["serial"].as_int());
      out_tok.scopes.clear();
      for (const auto& sc : v["scopes"].arr()) out_tok.scopes.push_back(sc.as_string());
      out_tok.mac_hex = v["mac_hex"].as_string();
      return authority.verify(out_tok, now_s, &err_msg);
    }
    err_msg = "định dạng thẻ năng lực không hợp lệ";
    return false;
  }
  if (tok_val.is_object()) {
    out_tok.subject = tok_val["sub"].as_string();
    out_tok.issuer = tok_val["iss"].as_string();
    out_tok.rights = sandbox::rights_from_string(tok_val["rights"].as_string());
    out_tok.issued_at = tok_val["iat"].as_int();
    out_tok.expires_at = tok_val["exp"].as_int();
    out_tok.serial = uint32_t(tok_val["serial"].as_int());
    out_tok.scopes.clear();
    for (const auto& sc : tok_val["scopes"].arr()) out_tok.scopes.push_back(sc.as_string());
    out_tok.mac_hex = tok_val["mac_hex"].as_string();
    return authority.verify(out_tok, now_s, &err_msg);
  }
  err_msg = "định dạng thẻ năng lực không hợp lệ";
  return false;
}

}  // namespace

const char* version() { return "0.1.0"; }

Daemon::Daemon(Options opts)
    : opts_(std::move(opts)),
      authority_(opts_.cap_master_key.empty()
                     ? "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff"
                     : opts_.cap_master_key),
      trusted_confirmation_(&enforcer_),
      compat_mgr_(&enforcer_, &authority_, &trusted_confirmation_) {}

std::string Daemon::anonymize_target(const std::string& target) const {
  if (target.rfind("http://", 0) == 0 || target.rfind("https://", 0) == 0 ||
      target.rfind("net:", 0) == 0 || (target.find('.') != std::string::npos && target[0] != '/')) {
    return "sha256:" + crypto::hex(crypto::sha256(target)).substr(0, 16);
  }
  return target;
}

void Daemon::audit_rotate() {
  if (opts_.state_dir.empty()) return;
  std::string path = opts_.state_dir + "/audit.log";
  std::string rotated = opts_.state_dir + "/audit.log.1";
  ::rename(path.c_str(), rotated.c_str());
}

bool Daemon::audit_clear() {
  enforcer_.clear_audit();
  if (!opts_.state_dir.empty()) {
    std::string path = opts_.state_dir + "/audit.log";
    FILE* f = std::fopen(path.c_str(), "w");
    if (f) std::fclose(f);
    std::string rotated = opts_.state_dir + "/audit.log.1";
    ::unlink(rotated.c_str());
  }
  return true;
}

void Daemon::audit_log(const std::string& subject, const std::string& action_kind,
                       const std::string& target, bool allowed, const std::string& reason) {
  int64_t now_s = std::time(nullptr);
  std::string logged_target = opts_.audit_hash_domains ? anonymize_target(target) : target;
  sandbox::AuditEntry e;
  e.when = now_s;
  e.subject = subject.empty() ? "mcp:anonymous" : subject;
  e.action = sandbox::Action{action_kind, logged_target};
  e.decision = sandbox::Decision{allowed, sandbox::Tier::Native, reason, 0};
  enforcer_.record(e);

  if (!opts_.state_dir.empty()) {
    ::mkdir(opts_.state_dir.c_str(), 0755);
    std::string path = opts_.state_dir + "/audit.log";

    struct stat st;
    if (::stat(path.c_str(), &st) == 0 && static_cast<size_t>(st.st_size) >= opts_.audit_max_bytes) {
      audit_rotate();
    }

    FILE* f = std::fopen(path.c_str(), "a");
    if (f) {
      std::fprintf(f, "[%lld] sub=%s action=%s target=%s decision=%s reason=\"%s\"\n",
                   static_cast<long long>(now_s), e.subject.c_str(), action_kind.c_str(),
                   logged_target.c_str(), allowed ? "ALLOWED" : "DENIED", reason.c_str());
      std::fclose(f);
    }
  }
}

// --------------------------------------------------------------- profile --
Value Daemon::method_info() {
  Value v(json::Object{});
  v.set("name", "yuid");
  v.set("version", version());
  v.set("tier", 4);
  json::Value features(json::Array{});
  features.push("graph");
  features.push("vault");
  features.push("sandbox");
  features.push("ime");
  features.push("pack");
  features.push("mcp");
  features.push("web.search");
  v.set("features", features);
  v.set("offline", opts_.offline);
  v.set("dot_kernel", engine::dot_kernel_name());
  const auto simd = engine::detect_simd();
  json::Value s(json::Object{});
  s.set("arch", simd.arch);
  s.set("best", simd.best);
  s.set("vector_bits", int(simd.vector_bits));
  v.set("simd", s);
  v.set("vault_enabled", !opts_.vault_password.empty());
  v.set("graph_nodes", graph_.stats().nodes);
  v.set("graph_edges", graph_.stats().edges);
  v.set("udh", false);  // placeholder for future UDH negotiation
  const bool is_sieu_yeu = has_host_ && host_["tier"].as_string() == "sieu-yeu";
  v.set("model_loaded", !is_sieu_yeu);
  v.set("zero_ai", is_sieu_yeu);
  v.set("headless", is_sieu_yeu);
  return v;
}

Value Daemon::method_profile() {
  if (!has_host_) {
    set_host_snapshot(profile::to_json(profile::probe()));
  }
  return host_;
}

Value Daemon::method_tune(const Value& params, RpcError& err) {
  engine::TuneInput in;
  if (params.has("ram_gb")) in.ram_gb = params["ram_gb"].as_double(in.ram_gb);
  if (params.has("temperature_c")) in.temperature_c = params["temperature_c"].as_double(in.temperature_c);
  if (params.has("on_battery")) in.on_battery = params["on_battery"].as_bool(in.on_battery);
  if (params.has("cpu_threads")) in.cpu_threads = int(params["cpu_threads"].as_int(in.cpu_threads));
  if (params.has("dedicated_gpu")) in.dedicated_gpu = params["dedicated_gpu"].as_bool(false);
  if (!params.has("ram_gb") && !has_host_) {
    // Fall back to real hardware when the caller gives us nothing.
    const auto host = profile::probe();
    in.ram_gb = host.tune_input.ram_gb;
    in.cpu_threads = host.tune_input.cpu_threads;
    in.temperature_c = host.tune_input.temperature_c;
    in.on_battery = host.tune_input.on_battery;
    in.dedicated_gpu = host.tune_input.dedicated_gpu;
  }
  if (in.cpu_threads <= 0) {
    err.code = kInvalidParams;
    err.message = "cpu_threads must be positive";
    return Value(nullptr);
  }
  const auto r = engine::auto_tune(in);
  Value v(json::Object{});
  v.set("k", r.k);
  v.set("window", r.window);
  v.set("threads", r.threads);
  v.set("profile", r.profile);
  v.set("reason", r.reason);
  if (r.profile == "sieu-yeu") {
    v.set("model_loaded", false);
    v.set("zero_ai", true);
    v.set("fallback", "bm25+rules");
    v.set("headless", true);
    v.set("compositor", false);
  }
  return v;
}

// ----------------------------------------------------------------- graph --
Value Daemon::method_graph(const std::string& op, const Value& p, RpcError& err) {
  if (op == "upsert") {
    if (!require(p, "id", err)) return Value(nullptr);
    const bool created = graph_.upsert(p["id"].as_string(), param_string(p, "label", p["id"].as_string()),
                                       graph::kind_from(param_string(p, "kind", "unknown")), p["props"]);
    err = RpcError{};
    Value v(json::Object{});
    v.set("created", created);
    v.set("id", p["id"].as_string());
    return v;
  }
  if (op == "link") {
    if (!require(p, "from", err) || !require(p, "rel", err) || !require(p, "to", err)) return Value(nullptr);
    const bool ok = graph_.link(p["from"].as_string(), p["rel"].as_string(), p["to"].as_string(),
                                p["weight"].as_double(1.0), p["props"]);
    if (!ok) {
      err.code = kInvalidParams;
      err.message = "both endpoints must exist and rel must be non-empty";
      return Value(nullptr);
    }
    Value v(json::Object{});
    v.set("linked", true);
    return v;
  }
  if (op == "query") {
    const std::string q = param_string(p, "q");
    const bool has_kind = p.has("kind");
    const auto hits = graph_.search(q, has_kind, graph::kind_from(param_string(p, "kind")));
    Value v(json::Object{});
    json::Value arr(json::Array{});
    for (const auto& id : hits) {
      const auto* node = graph_.find(id);
      json::Value o(json::Object{});
      o.set("id", id);
      if (node) {
        o.set("label", node->label);
        o.set("kind", graph::kind_name(node->kind));
      }
      arr.push(o);
    }
    v.set("hits", arr);
    if (p.has("expand_from")) {
      json::Value exp(json::Array{});
      for (const auto& id : graph_.bfs(p["expand_from"].as_string(), int(p["depth"].as_int(2)),
                                       param_string(p, "rel")))
        exp.push(id);
      v.set("expanded", exp);
    }
    return v;
  }
  if (op == "path") {
    if (!require(p, "from", err) || !require(p, "to", err)) return Value(nullptr);
    json::Value arr(json::Array{});
    for (const auto& id : graph_.shortest_path(p["from"].as_string(), p["to"].as_string())) arr.push(id);
    Value v(json::Object{});
    v.set("path", arr);
    return v;
  }
  if (op == "stats") {
    const auto s = graph_.stats();
    Value v(json::Object{});
    v.set("nodes", s.nodes);
    v.set("edges", s.edges);
    json::Value kinds(json::Object{});
    for (int k = 0; k < 10; ++k) kinds.set(graph::kind_name(graph::NodeKind(k)), int(s.kinds[k]));
    v.set("kinds", kinds);
    return v;
  }
  if (op == "remove") {
    if (!require(p, "id", err)) return Value(nullptr);
    const bool ok = graph_.remove(p["id"].as_string());
    Value v(json::Object{});
    v.set("removed", ok);
    v.set("id", p["id"].as_string());
    return v;
  }
  if (op == "export") {
    Value v(json::Object{});
    v.set("graph", graph_.to_json());
    return v;
  }
  if (op == "import") {
    std::string e;
    if (!graph_.from_json(p["graph"], &e)) {
      err.code = kInvalidParams;
      err.message = e;
      return Value(nullptr);
    }
    Value v(json::Object{});
    v.set("imported", true);
    v.set("nodes", graph_.stats().nodes);
    return v;
  }
  err.code = kMethodNotFound;
  err.message = "unknown graph operation: " + op;
  return Value(nullptr);
}

// ----------------------------------------------------------------- vault --
Value Daemon::method_vault(const std::string& op, const Value& p, RpcError& err) {
  if (opts_.vault_password.empty()) {
    err.code = kUnavailable;
    err.message = "yuid đang chạy không có khoá vault (dùng --vault-key hoặc YUI_VAULT_KEY)";
    return Value(nullptr);
  }
  if (op == "seal") {
    if (!require(p, "plain", err)) return Value(nullptr);
    std::vector<uint8_t> plain;
    if (!b64::decode(p["plain"].as_string(), plain)) {
      err.code = kInvalidParams;
      err.message = "plain must be base64";
      return Value(nullptr);
    }
    vault::Params params;
    if (p.has("log2n")) params.log2_n = uint8_t(p["log2n"].as_int(params.log2_n));
    if (params.log2_n < 10 || params.log2_n > 22) {
      err.code = kInvalidParams;
      err.message = "log2n out of range";
      return Value(nullptr);
    }
    const vault::Bytes blob = vault::seal(opts_.vault_password, plain, params);
    ++vault_ops_;
    Value v(json::Object{});
    v.set("blob", b64::encode(blob));
    v.set("bytes", blob.size());
    return v;
  }
  if (op == "open") {
    if (!require(p, "blob", err)) return Value(nullptr);
    std::vector<uint8_t> blob;
    if (!b64::decode(p["blob"].as_string(), blob)) {
      err.code = kInvalidParams;
      err.message = "blob must be base64";
      return Value(nullptr);
    }
    vault::Bytes plain;
    std::string e;
    if (!vault::open(opts_.vault_password, blob, plain, &e)) {
      err.code = kDenied;
      err.message = e;
      return Value(nullptr);
    }
    ++vault_ops_;
    Value v(json::Object{});
    v.set("plain", b64::encode(plain));
    v.set("bytes", plain.size());
    return v;
  }
  err.code = kMethodNotFound;
  err.message = "unknown vault operation: " + op;
  return Value(nullptr);
}

// --------------------------------------------------------------- sandbox --
Value Daemon::method_sandbox_check(const Value& p, RpcError& err) {
  if (!require(p, "policy", err) || !require(p, "action", err)) return Value(nullptr);
  std::string e;
  const sandbox::Policy policy = sandbox::Policy::from_json(p["policy"], &e);
  if (!e.empty()) {
    err.code = kInvalidParams;
    err.message = e;
    return Value(nullptr);
  }
  sandbox::Action action;
  action.kind = param_string(p["action"], "kind");
  action.target = param_string(p["action"], "target");
  if (action.kind.empty()) {
    err.code = kInvalidParams;
    err.message = "action.kind is required";
    return Value(nullptr);
  }
  const auto now = int64_t(std::time(nullptr));
  const auto d = enforcer_.check(policy, nullptr, action, now);
  Value v(json::Object{});
  v.set("allowed", d.allowed);
  v.set("tier", sandbox::tier_name(d.tier));
  v.set("reason", d.reason);
  v.set("audit_entries", enforcer_.audit().size());
  return v;
}

// ------------------------------------------------------------------- ime --
Value Daemon::method_ime(const Value& p, RpcError& err) {
  if (!require(p, "text", err)) return Value(nullptr);
  const std::string text = p["text"].as_string();
  const std::string method = param_string(p, "method", "telex");
  ime::Method m;
  if (method == "telex") m = ime::Method::Telex;
  else if (method == "vni") m = ime::Method::Vni;
  else {
    err.code = kInvalidParams;
    err.message = "method must be telex or vni";
    return Value(nullptr);
  }
  Value v(json::Object{});
  v.set("result", ime::convert(text, m));
  v.set("method", method);
  return v;
}

// ------------------------------------------------------------------ pack --
Value Daemon::method_pack(const std::string& op, const Value& p, RpcError& err) {
  if (!require(p, "data", err)) return Value(nullptr);
  std::vector<uint8_t> data;
  if (!b64::decode(p["data"].as_string(), data)) {
    err.code = kInvalidParams;
    err.message = "data must be base64";
    return Value(nullptr);
  }
  if (op == "compress") {
    const int level = int(p["level"].as_int(6));
    const auto packed = codec::pack(data, level);
    Value v(json::Object{});
    v.set("packed", b64::encode(packed));
    v.set("original_size", data.size());
    v.set("packed_size", packed.size());
    v.set("ratio", codec::ratio(data.size(), packed.size()));
    return v;
  }
  if (op == "decompress") {
    codec::Bytes plain;
    codec::PackInfo info;
    std::string e;
    if (!codec::unpack(data, plain, &info, &e)) {
      err.code = kInvalidParams;
      err.message = e;
      return Value(nullptr);
    }
    Value v(json::Object{});
    v.set("data", b64::encode(plain));
    v.set("original_size", info.original_size);
    v.set("digest", info.digest_hex);
    return v;
  }
  err.code = kMethodNotFound;
  err.message = "unknown pack operation: " + op;
  return Value(nullptr);
}

// ------------------------------------------------------------- web.search --
Value Daemon::method_web_search(const Value& p, RpcError& err) {
  if (!require(p, "q", err)) return Value(nullptr);
  // Tiered search is a Tầng 4 feature, but the *policy* is explicit: with no
  // network backend configured the daemon says so instead of pretending.
  Value v(json::Object{});
  json::Value results(json::Array{});
  v.set("results", results);
  v.set("query", p["q"].as_string());
  v.set("backend", "offline");
  v.set("note",
        opts_.offline ? "yuid đang ở chế độ ngoại tuyến: không gọi mạng. Bật backend bằng --enable-net."
                      : "chưa cấu hình backend tìm kiếm");
  return v;
}

// -------------------------------------------------------------------- os --
Value Daemon::method_telemetry() {
  // Facts + one recommendation. The AI core may override k/W, but it has to say
  // why; the bridge never guesses a sensor value.
  const telemetry::Reading reading = telemetry::read();
  const telemetry::Advice advice = telemetry::advise(reading);
  return telemetry::to_json(reading, advice);
}

Value Daemon::method_guard(const std::string& op, const Value& p, RpcError& err) {
  // Guard chống bất thường nền (SECURITY.md §11.1): nạp mẫu tải CPU/GPU của
  // tiến trình, trả cảnh báo tiếng Việt, và hỏi người dùng
  // "tiêu diệt / gỡ bỏ / để yên" — quyết định phải có `confirm: true`.
  if (op == "observe") {
    guard::ProcessSample s;
    s.pid = uint64_t(p["pid"].as_int(0));
    s.name = param_string(p, "name", "?");
    s.cpu_percent = p["cpu_percent"].as_double(0.0);
    s.gpu_percent = p["gpu_percent"].as_double(0.0);
    s.has_window = p["has_window"].as_bool(false);
    s.caps = uint64_t(p["caps"].as_int(0));
    s.now_ns = p["now_ns"].as_int(0);
    const auto alert = guard_.observe(s);
    Value v(json::Object{});
    v.set("alerted", bool(alert));
    if (alert) {
      v.set("pid", int64_t(alert->pid));
      v.set("name", alert->name);
      v.set("reason_vi", alert->reason_vi);
      v.set("notify_vi", alert->notify_vi);
      v.set("throttle_percent", int64_t(alert->throttle_percent));
      v.set("demoted", alert->demoted);
    }
    return v;
  }
  if (op == "alerts") {
    Value list(json::Array{});
    for (const auto& a : guard_.alerts()) {
      Value e(json::Object{});
      e.set("pid", int64_t(a.pid));
      e.set("name", a.name);
      e.set("reason_vi", a.reason_vi);
      e.set("notify_vi", a.notify_vi);
      e.set("throttle_percent", int64_t(a.throttle_percent));
      e.set("demoted", a.demoted);
      list.push(std::move(e));
    }
    Value v(json::Object{});
    v.set("count", int64_t(guard_.alerts().size()));
    v.set("alerts", std::move(list));
    v.set("audit_entries", int64_t(enforcer_.audit().size()));
    return v;
  }
  if (op == "decide") {
    const uint64_t pid = uint64_t(p["pid"].as_int(p["id"].as_int(0)));
    const std::string action = param_string(p, "action");
    if (pid == 0 || (action != "kill" && action != "remove" && action != "keep")) {
      err.code = kInvalidParams;
      err.message = "pid và action (kill|remove|keep) là bắt buộc";
      return Value(nullptr);
    }
    if (!p["confirm"].as_bool(false)) {
      // Bắt người dùng xác nhận — không bao giờ hành động ngầm.
      err.code = kDenied;
      err.message = "needs_confirmation: quyết định với tiến trình " + std::to_string(pid) +
                    " phải được người dùng xác nhận";
      err.data.set("needs_confirmation", true);
      err.data.set("question_vi",
                   "Bạn muốn tiêu diệt, gỡ bỏ, hay để yên tiến trình " + std::to_string(pid) + "?");
      Value acts(json::Array{});
      acts.push(Value("kill"));
      acts.push(Value("remove"));
      acts.push(Value("keep"));
      err.data.set("actions", std::move(acts));
      return Value(nullptr);
    }
    // Đã xác nhận: thực thi + ghi audit qua enforcer (một dòng/kiểu quyết định).
    const bool was_throttled = guard_.is_throttled(pid);
    sandbox::Action act;
    act.kind = "guard." + action;
    act.target = std::to_string(pid);
    sandbox::Decision d;
    d.allowed = true;
    d.tier = sandbox::Tier::Native;
    d.reason = action == "kill"
                   ? "người dùng chọn tiêu diệt tiến trình bị Guard bóp xung"
               : action == "remove"
                   ? "người dùng chọn gỡ bỏ tiến trình bị Guard bóp xung"
                   : "người dùng chọn để yên — tháo bóp xung cho tiến trình";
    enforcer_.record(sandbox::AuditEntry{int64_t(std::time(nullptr)), "user", act, d});
    guard_.clear(pid);  // mọi quyết định kết thúc trạng thái theo dõi/bóp xung

    Value v(json::Object{});
    v.set("done", true);
    v.set("action", action);
    v.set("pid", int64_t(pid));
    v.set("was_throttled", was_throttled);
    v.set("reason_vi", d.reason);
    return v;
  }
  err.code = kInvalidParams;
  err.message = "guard op phải là observe | alerts | decide";
  return Value(nullptr);
}

Value Daemon::method_compat(const std::string& op, const Value& p, RpcError& err) {
  if (op == "get") {
    std::string app = param_string(p, "app");
    if (app.empty()) {
      err.code = kInvalidParams;
      err.message = "thiếu tham số 'app'";
      return Value(nullptr);
    }
    Value v(json::Object{});
    v.set("app", app);
    v.set("enabled", compat_mgr_.is_app_compatibility_enabled(app));
    return v;
  }
  if (op == "set") {
    std::string app = param_string(p, "app");
    if (app.empty()) {
      err.code = kInvalidParams;
      err.message = "thiếu tham số 'app'";
      return Value(nullptr);
    }
    bool enable = p["enabled"].as_bool(false);
    compat_mgr_.set_app_compatibility(app, enable);
    Value v(json::Object{});
    v.set("app", app);
    v.set("enabled", enable);
    v.set("reason_vi", std::string("Chế độ tương thích đã ") + (enable ? "bật" : "tắt") + " cho " + app);
    return v;
  }
  if (op == "list") {
    Value arr(json::Array{});
    for (const auto& a : compat_mgr_.enabled_apps()) {
      arr.push(a);
    }
    Value v(json::Object{});
    v.set("enabled_apps", arr);
    v.set("prompt_count", int64_t(compat_mgr_.prompt_count()));
    v.set("grant_count", int64_t(compat_mgr_.grant_count()));
    return v;
  }
  err.code = kMethodNotFound;
  err.message = "thao tác compat không xác định: " + op;
  return Value(nullptr);
}

Value Daemon::method_os(const std::string& op) {
  Value v(json::Object{});
  if (!has_host_) set_host_snapshot(profile::to_json(profile::probe()));
  if (op == "battery") {
    v.set("has_battery", host_["has_battery"]);
    v.set("percent", host_["battery_percent"]);
    v.set("on_battery", host_["on_battery"]);
    v.set("charging", host_["charging"]);
    return v;
  }
  if (op == "thermal") {
    v.set("temperature_c", host_["temperature_c"]);
    v.set("throttled", host_["temperature_c"].as_double(0) >= 80.0);
    return v;
  }
  if (op == "power") {
    // "Giới hạn sạc pin" lives in Tầng 2; the daemon only reports the policy
    // that the kernel module would apply.
    v.set("charge_limit_percent", 85);
    v.set("governor", "adaptive");
    v.set("note", "giới hạn sạc do module yui-power ở Tầng 2 thực thi");
    return v;
  }
  v.set("error", "unknown os operation: " + op);
  return v;
}

// ------------------------------------------------------ Kho Ứng Dụng (P4.15) --
Value Daemon::method_store(const std::string& op, const Value& p, RpcError& err) {
  bundle::Keyring keys;
  keys.load_dir(opts_.store_keyring_dir, nullptr);
  store::Options so;
  so.root = opts_.store_root.empty() ? opts_.state_dir + "/store" : opts_.store_root;
  so.allow_file_urls = opts_.store_allow_file_urls;
  store::Store st(so, keys);

  auto outcome = [](const store::Result& r) {
    Value v(json::Object{});
    v.set("ok", r.ok);
    v.set("code", r.code);
    v.set("reason_vi", r.reason_vi);
    if (!r.candidates.empty()) {
      Value c(json::Array{});
      for (const auto& id : r.candidates) c.push(id);
      v.set("candidates", c);
    }
    return v;
  };

  if (op == "list") {
    Value arr(json::Array{});
    for (const auto& a : st.installed()) {
      Value v(json::Object{});
      v.set("id", a.id);
      v.set("name", a.name);
      v.set("version", a.version);
      v.set("publisher", a.publisher);
      v.set("signature", a.signature);
      v.set("files", static_cast<long long>(a.files));
      arr.push(v);
    }
    Value v(json::Object{});
    v.set("root", so.root);
    v.set("apps", arr);
    return v;
  }
  if (op == "remove") {
    const std::string app = param_string(p, "app");
    if (app.empty()) {
      err.code = kInvalidParams;
      err.message = "thiếu tham số 'app'";
      return Value(nullptr);
    }
    const auto r = st.remove(app);
    audit_log("store", "store.remove", app, r.ok, r.reason_vi);
    return outcome(r);
  }

  const auto loaded = st.load_catalog_files();
  if (!loaded.ok) return outcome(loaded);

  if (op == "search") {
    Value arr(json::Array{});
    for (const auto& a : st.search(param_string(p, "query"))) {
      Value v(json::Object{});
      v.set("id", a.id);
      v.set("name", a.name);
      v.set("version", a.version);
      v.set("publisher", a.publisher);
      v.set("license", a.license);
      v.set("kind", a.kind);
      v.set("installed", st.is_installed(a.id));
      arr.push(v);
    }
    Value v = outcome(loaded);
    v.set("apps", arr);
    return v;
  }

  const std::string app = param_string(p, "app");
  if (app.empty()) {
    err.code = kInvalidParams;
    err.message = "thiếu tham số 'app'";
    return Value(nullptr);
  }
  store::InstallPlan plan;
  const auto pr = st.plan(app, plan);
  if (!pr.ok) return outcome(pr);

  if (op == "plan") {
    Value v = outcome(pr);
    v.set("id", plan.entry.id);
    v.set("source_url", plan.entry.source_url);
    v.set("license", plan.entry.license);
    v.set("tier", sandbox::tier_name(plan.enforced_policy.tier));
    v.set("rights", sandbox::rights_to_string(plan.enforced_policy.granted));
    v.set("plan_vi", plan.summary_vi());
    return v;
  }
  if (op == "install") {
    const std::string target = store::TrustedDialogConfirmation::target_for(plan.entry.id);
    const std::string dialog_id = param_string(p, "dialog_id");
    const bool reused = std::find(store_used_dialogs_.begin(), store_used_dialogs_.end(), dialog_id) !=
                        store_used_dialogs_.end();
    if (dialog_id.empty() || reused || trusted_confirmation_.dialog_target(dialog_id) != target) {
      // Không có (hoặc sai) hộp thoại: mở hộp thoại tin cậy, KHÔNG tải gì.
      const std::string id = trusted_confirmation_.show_dialog(target, "Cài ứng dụng", plan.summary_vi(), 0);
      audit_log("store", "store.install", plan.entry.id, false, "chờ người dùng xác nhận qua hộp thoại tin cậy " + id);
      Value v(json::Object{});
      v.set("ok", false);
      v.set("code", "needs_confirmation");
      v.set("dialog_id", id);
      v.set("plan_vi", plan.summary_vi());
      v.set("reason_vi", "Cần xác nhận bằng bàn phím/chuột thật trên hộp thoại hệ thống " + id +
                             " (đường tin cậy P4.13); chưa tải, chưa cài gì");
      return v;
    }
    const bool https = store::url_scheme(plan.entry.source_url) == "https";
    if (https && opts_.offline) {
      store::Result r;
      r.code = "offline";
      r.reason_vi = "yuid đang ở chế độ ngoại tuyến: không tải từ mạng (dùng yui-store trên Linux host, hoặc bật mạng)";
      audit_log("store", "store.install", plan.entry.id, false, r.reason_vi);
      return outcome(r);
    }
    store::TrustedDialogConfirmation gate(trusted_confirmation_, dialog_id);
    store::FileFetcher file_fetcher;
    store::CurlFetcher curl_fetcher;
    store::Fetcher& f = https ? static_cast<store::Fetcher&>(curl_fetcher) : static_cast<store::Fetcher&>(file_fetcher);
    const auto r = st.install(app, f, gate);
    if (r.code != "not_confirmed") store_used_dialogs_.push_back(dialog_id);
    audit_log("store", "store.install", plan.entry.id, r.ok, r.reason_vi);
    Value v = outcome(r);
    if (r.ok) {
      v.set("app_dir", r.app.app_dir);
      v.set("policy_path", r.app.policy_path);
      v.set("menu_path", r.app.menu_path);
    }
    return v;
  }
  err.code = kMethodNotFound;
  err.message = "unknown store operation: " + op;
  return Value(nullptr);
}

// -------------------------------------------------------------- dispatch --
Value Daemon::handle(const std::string& method, const Value& params, RpcError& err) {
  err = RpcError{};
  if (method == "yuid.info" || method == "ping") return method_info();
  if (method == "yuid.profile") return method_profile();
  if (method == "yuid.tune") return method_tune(params, err);
  if (method == "yuid.version") {
    Value v(json::Object{});
    v.set("version", version());
    return v;
  }
  if (text::starts_with(method, "graph.")) return method_graph(method.substr(6), params, err);
  if (text::starts_with(method, "vault.")) return method_vault(method.substr(6), params, err);
  if (text::starts_with(method, "pack.")) return method_pack(method.substr(5), params, err);
  if (method == "sandbox.check") return method_sandbox_check(params, err);
  if (text::starts_with(method, "guard.")) return method_guard(method.substr(6), params, err);
  if (text::starts_with(method, "compat.")) return method_compat(method.substr(7), params, err);
  if (text::starts_with(method, "store.")) return method_store(method.substr(6), params, err);
  if (method == "ime.convert") return method_ime(params, err);
  if (method == "web.search") return method_web_search(params, err);
  if (method == "telemetry.read") return method_telemetry();
  if (method == "audit.clear") {
    audit_clear();
    Value v(json::Object{});
    v.set("cleared", true);
    return v;
  }
  if (method == "audit.entries") {
    Value arr(json::Array{});
    for (const auto& entry : enforcer_.audit()) {
      Value item(json::Object{});
      item.set("when", entry.when);
      item.set("subject", entry.subject);
      item.set("kind", entry.action.kind);
      item.set("target", entry.action.target);
      item.set("allowed", entry.decision.allowed);
      item.set("reason", entry.decision.reason);
      arr.push(item);
    }
    Value res(json::Object{});
    res.set("entries", arr);
    return res;
  }
  if (text::starts_with(method, "os.")) return method_os(method.substr(3));
  if (method == "engine.tune") return method_tune(params, err);

  err.code = kMethodNotFound;
  err.message = "unknown method: " + method;
  return Value(nullptr);
}

Value Daemon::dispatch(const Value& request) {
  Value response(json::Object{});
  response.set("jsonrpc", "2.0");
  const Value& id = request["id"];
  if (id.is_number()) response.set("id", id.as_int());
  else if (id.is_string()) response.set("id", id.as_string());
  else response.set("id", Value(nullptr));

  if (!request.is_object() || request["method"].is_null()) {
    response.set("error", make_error(kInvalidRequest, "request must be an object with a method"));
    return response;
  }
  RpcError err;
  const Value result = handle(request["method"].as_string(), request["params"], err);
  if (err.code != 0) {
    Value e = make_error(err.code, err.message);
    if (!err.data.is_null()) e.set("data", err.data);
    response.set("error", e);
  } else {
    response.set("result", result);
  }
  return response;
}

// ------------------------------------------------------------------- MCP --
Value Daemon::mcp_initialize(const Value& params) {
  Value v(json::Object{});
  v.set("protocolVersion", "2024-11-05");
  Value caps(json::Object{});
  Value tools(json::Object{});
  tools.set("listChanged", false);
  caps.set("tools", tools);
  v.set("capabilities", caps);
  Value info(json::Object{});
  info.set("name", "yuid");
  info.set("version", version());
  v.set("serverInfo", info);
  Value instructions(json::Array{});
  instructions.push("YuiVieOS core daemon: đồ thị tri thức, kho mã hoá, hộp cát, bộ gõ tiếng Việt.");
  v.set("instructions", instructions.arr()[0]);
  (void)params;
  return v;
}

namespace {
Value tool_schema(const std::string& name, const std::string& desc, const json::Value& props,
                  const json::Value& required) {
  Value t(json::Object{});
  t.set("name", name);
  t.set("description", desc);
  Value schema(json::Object{});
  schema.set("type", "object");
  schema.set("properties", props);
  schema.set("required", required);
  t.set("inputSchema", schema);
  return t;
}
Value prop(const std::string& type, const std::string& desc) {
  Value p(json::Object{});
  p.set("type", type);
  p.set("description", desc);
  return p;
}
}  // namespace

Value Daemon::mcp_tools_list() {
  Value list(json::Array{});
  {
    Value props(json::Object{});
    props.set("q", prop("string", "từ khoá tiếng Việt hoặc tiếng Anh"));
    Value req(json::Array{});
    req.push("q");
    list.push(tool_schema("graph_query", "Tìm trong đồ thị tri thức của người dùng", props, req));
  }
  {
    Value props(json::Object{});
    props.set("id", prop("string", "id ổn định của nút"));
    props.set("label", prop("string", "nhãn hiển thị"));
    props.set("kind", prop("string", "file|app|entity|concept|device|capability|person|place|note"));
    Value req(json::Array{});
    req.push("id");
    list.push(tool_schema("graph_upsert", "Thêm hoặc cập nhật một nút trong đồ thị", props, req));
  }
  {
    Value props(json::Object{});
    props.set("from", prop("string", "id nút nguồn"));
    props.set("rel", prop("string", "tên quan hệ"));
    props.set("to", prop("string", "id nút đích"));
    Value req(json::Array{});
    req.push("from");
    req.push("rel");
    req.push("to");
    list.push(tool_schema("graph_link", "Nối hai nút bằng một quan hệ", props, req));
  }
  {
    Value props(json::Object{});
    props.set("text", prop("string", "chuỗi phím gõ (Telex hoặc VNI)"));
    props.set("method", prop("string", "telex (mặc định) hoặc vni"));
    Value req(json::Array{});
    req.push("text");
    list.push(tool_schema("ime_convert", "Chuyển phím gõ thành chữ tiếng Việt có dấu", props, req));
  }
  {
    Value props(json::Object{});
    props.set("plain", prop("string", "dữ liệu base64 cần niêm phong"));
    Value req(json::Array{});
    req.push("plain");
    list.push(tool_schema("vault_seal", "Niêm phong dữ liệu vào kho mã hoá", props, req));
  }
  {
    Value props(json::Object{});
    props.set("policy", prop("object", "chính sách hộp cát"));
    props.set("kind", prop("string", "loại hành động, ví dụ fs.read"));
    props.set("target", prop("string", "đối tượng, ví dụ /home/user/Documents/a.txt"));
    Value req(json::Array{});
    req.push("policy");
    req.push("kind");
    list.push(tool_schema("sandbox_check", "Kiểm tra một hành động có được phép trong hộp cát", props, req));
  }
  {
    Value props(json::Object{});
    props.set("q", prop("string", "truy vấn tìm kiếm"));
    Value req(json::Array{});
    req.push("q");
    list.push(tool_schema("web_search", "Tìm kiếm web nhiều tầng (có thể ngoại tuyến)", props, req));
  }
  {
    Value props(json::Object{});
    list.push(tool_schema("system_profile", "Thông tin phần cứng và cấu hình tự chỉnh k/W", props,
                          json::Value(json::Array{})));
  }
  {
    Value props(json::Object{});
    list.push(tool_schema("system_telemetry",
                          "Pin, nhiệt độ CPU/GPU, RAM trống và khuyến nghị k/W cho lõi AI", props,
                          json::Value(json::Array{})));
  }
  {
    Value props(json::Object{});
    props.set("mode", prop("string", "chế độ nguồn: tiet_kiem|can_bang|hieu_nang"));
    props.set("charge_limit", prop("integer", "giới hạn sạc pin phần trăm (50..100)"));
    props.set("confirm", prop("boolean", "xác nhận của người dùng cho thay đổi nguồn"));
    props.set("token", prop("string", "thẻ năng lực bearer có quyền 'power'"));
    Value req(json::Array{});
    req.push("mode");
    req.push("token");
    list.push(tool_schema("sys_power",
                          "Quản lý chế độ nguồn, governor và giới hạn sạc pin (cần quyền 'power' và xác nhận người dùng)",
                          props, req));
  }
  {
    Value props(json::Object{});
    props.set("profile", prop("string", "cấu hình lập lịch: interactive|batch|realtime|powersave"));
    props.set("slice_us", prop("integer", "thời lượng time-slice tính bằng microsecond (1000..50000)"));
    props.set("confirm", prop("boolean", "xác nhận của người dùng cho thay đổi cấu hình lập lịch"));
    props.set("token", prop("string", "thẻ năng lực bearer có quyền 'admin' hoặc 'power'"));
    Value req(json::Array{});
    req.push("profile");
    req.push("token");
    list.push(tool_schema("sys_scheduler",
                          "Thay đổi cấu hình lập lịch tác vụ CPU (cần quyền 'admin' hoặc 'power' và xác nhận người dùng)",
                          props, req));
  }
  {
    Value props(json::Object{});
    props.set("app_path", prop("string", "đường dẫn thư mục ứng dụng cần gọt tỉa hoặc hoàn tác"));
    props.set("action", prop("string", "hành động: preview|apply|rollback"));
    props.set("confirm", prop("boolean", "xác nhận của người dùng (bắt buộc khi action là apply hoặc rollback)"));
    props.set("token", prop("string", "thẻ năng lực bearer có quyền 'fs.read' / 'fs.write' bao phủ app_path"));
    Value req(json::Array{});
    req.push("app_path");
    req.push("token");
    list.push(tool_schema("sys_app_stripper",
                          "Gọt tỉa, nén ứng dụng hoặc hoàn tác (cần quyền 'fs.read'/'fs.write' đúng phạm vi và xác nhận khi thay đổi)",
                          props, req));
  }
  {
    Value props(json::Object{});
    props.set("action", prop("string", "hành động: list|kill|throttle"));
    props.set("pid", prop("integer", "mã tiến trình (bắt buộc khi kill hoặc throttle)"));
    props.set("throttle_percent", prop("integer", "phần trăm bóp xung (khi action=throttle, mặc định 20)"));
    props.set("confirm", prop("boolean", "xác nhận của người dùng (bắt buộc khi kill hoặc throttle)"));
    props.set("token", prop("string", "thẻ năng lực bearer có quyền 'admin'"));
    Value req(json::Array{});
    req.push("action");
    req.push("token");
    list.push(tool_schema("sys_proc",
                          "Liệt kê, dừng hoặc bóp xung tiến trình trong hộp cát (cần quyền 'admin' và xác nhận khi dừng/bóp)",
                          props, req));
  }
  {
    Value props(json::Object{});
    props.set("q", prop("string", "từ khoá tìm kiếm trong kho Vault"));
    props.set("confirm", prop("boolean", "xác nhận của người dùng trước khi truy xuất và tiết lộ dữ liệu nhạy cảm"));
    props.set("token", prop("string", "thẻ năng lực bearer có quyền 'vault'"));
    Value req(json::Array{});
    req.push("q");
    req.push("token");
    list.push(tool_schema("sys_search_vault",
                          "Tìm kiếm trong kho bí mật Vault (cần quyền 'vault' và xác nhận người dùng trước khi tiết lộ)",
                          props, req));
  }
  Value out(json::Object{});
  out.set("tools", list);
  return out;
}

Value Daemon::mcp_tool_call(const Value& params, RpcError& err) {
  if (!require(params, "name", err)) return Value(nullptr);
  const std::string name = params["name"].as_string();
  const Value args = params.has("arguments") ? params["arguments"] : Value(json::Object{});

  Value text_result(json::Object{});
  Value content(json::Array{});
  auto emit = [&](const std::string& text, bool is_error) {
    Value item(json::Object{});
    item.set("type", "text");
    item.set("text", text);
    content.push(item);
    text_result.set("content", content);
    text_result.set("isError", is_error);
    return text_result;
  };

  if (name == "graph_query") {
    RpcError e;
    const Value r = method_graph("query", args, e);
    if (e.code) return emit(e.message, true);
    return emit(r.dump(), false);
  }
  if (name == "graph_upsert") {
    RpcError e;
    const Value r = method_graph("upsert", args, e);
    if (e.code) return emit(e.message, true);
    return emit(r.dump(), false);
  }
  if (name == "graph_link") {
    RpcError e;
    const Value r = method_graph("link", args, e);
    if (e.code) return emit(e.message, true);
    return emit(r.dump(), false);
  }
  if (name == "ime_convert") {
    RpcError e;
    const Value r = method_ime(args, e);
    if (e.code) return emit(e.message, true);
    return emit(r["result"].as_string(), false);
  }
  if (name == "vault_seal") {
    RpcError e;
    const Value r = method_vault("seal", args, e);
    if (e.code) return emit(e.message, true);
    return emit(r["blob"].as_string(), false);
  }
  if (name == "sandbox_check") {
    Value shaped(json::Object{});
    shaped.set("policy", args["policy"]);
    Value action(json::Object{});
    action.set("kind", args["kind"]);
    action.set("target", args["target"]);
    shaped.set("action", action);
    RpcError e;
    const Value r = method_sandbox_check(shaped, e);
    if (e.code) return emit(e.message, true);
    return emit(r.dump(), false);
  }
  if (name == "web_search") {
    RpcError e;
    const Value r = method_web_search(args, e);
    if (e.code) return emit(e.message, true);
    return emit(r.dump(), false);
  }
  if (name == "system_telemetry") {
    return method_telemetry();
  }
  if (name == "system_profile") {
    return emit(method_profile().dump(), false);
  }

  if (name == "sys_power") {
    sandbox::Token tok;
    std::string err_msg;
    if (!extract_token(args, authority_, tok, err_msg)) {
      audit_log("", "sys_power", "unknown", false, err_msg);
      return emit("Từ chối: " + err_msg, true);
    }
    if (!(tok.rights & sandbox::kRightPower)) {
      audit_log(tok.subject, "sys_power", "unknown", false, "thẻ năng lực không có quyền 'power'");
      return emit("Từ chối: thẻ năng lực không có quyền 'power'.", true);
    }
    const std::string mode = text::fold(param_string(args, "mode", "can_bang"));
    const int limit = int(args.has("charge_limit") ? args["charge_limit"].as_int(100) : 100);
    const bool confirmed = args["confirm"].as_bool(false);
    if (!confirmed) {
      audit_log(tok.subject, "sys_power", "mode:" + mode, false, "chờ người dùng xác nhận");
      Value resp(json::Object{});
      resp.set("needs_confirmation", true);
      resp.set("tool", "sys_power");
      resp.set("message", "Cần xác nhận người dùng: thay đổi chế độ nguồn sang '" + mode +
                              "' (giới hạn sạc: " + std::to_string(limit) + "%)");
      return emit(resp.dump(), false);
    }
    power_mode_ = mode;
    charge_limit_ = limit;
    audit_log(tok.subject, "sys_power", "mode:" + mode, true, "áp dụng cấu hình nguồn thành công");
    Value resp(json::Object{});
    resp.set("applied", true);
    resp.set("mode", power_mode_);
    resp.set("charge_limit", charge_limit_);
    return emit(resp.dump(), false);
  }

  if (name == "sys_scheduler") {
    sandbox::Token tok;
    std::string err_msg;
    if (!extract_token(args, authority_, tok, err_msg)) {
      audit_log("", "sys_scheduler", "unknown", false, err_msg);
      return emit("Từ chối: " + err_msg, true);
    }
    if (!(tok.rights & (sandbox::kRightAdmin | sandbox::kRightPower))) {
      audit_log(tok.subject, "sys_scheduler", "unknown", false, "thẻ năng lực không có quyền 'admin' hoặc 'power'");
      return emit("Từ chối: thẻ năng lực không có quyền 'admin' hoặc 'power'.", true);
    }
    const std::string prof = text::fold(param_string(args, "profile", "interactive"));
    const int slice = int(args.has("slice_us") ? args["slice_us"].as_int(5000) : 5000);
    const bool confirmed = args["confirm"].as_bool(false);
    if (!confirmed) {
      audit_log(tok.subject, "sys_scheduler", "profile:" + prof, false, "chờ người dùng xác nhận");
      Value resp(json::Object{});
      resp.set("needs_confirmation", true);
      resp.set("tool", "sys_scheduler");
      resp.set("message", "Cần xác nhận người dùng: thay đổi cấu hình lập lịch sang '" + prof +
                              "' (time-slice: " + std::to_string(slice) + " µs)");
      return emit(resp.dump(), false);
    }
    sched_profile_ = prof;
    sched_slice_us_ = slice;
    audit_log(tok.subject, "sys_scheduler", "profile:" + prof, true, "áp dụng cấu hình lập lịch thành công");
    Value resp(json::Object{});
    resp.set("applied", true);
    resp.set("profile", sched_profile_);
    resp.set("slice_us", sched_slice_us_);
    return emit(resp.dump(), false);
  }

  if (name == "sys_app_stripper") {
    sandbox::Token tok;
    std::string err_msg;
    if (!extract_token(args, authority_, tok, err_msg)) {
      audit_log("", "sys_app_stripper", "unknown", false, err_msg);
      return emit("Từ chối: " + err_msg, true);
    }
    const std::string action = text::fold(param_string(args, "action", "preview"));
    const std::string app_path = param_string(args, "app_path");
    if (app_path.empty()) {
      return emit("Từ chối: thiếu đường dẫn 'app_path'.", true);
    }
    if (action == "preview") {
      if (!(tok.rights & sandbox::kRightFsRead)) {
        audit_log(tok.subject, "sys_app_stripper", app_path, false, "thẻ năng lực thiếu quyền 'fs.read'");
        return emit("Từ chối: thẻ năng lực thiếu quyền 'fs.read'.", true);
      }
      bool covered = false;
      for (const auto& sc : tok.scopes) {
        if (sandbox::scope_covers(sc, sandbox::Action{"fs.read", app_path})) { covered = true; break; }
      }
      if (!covered) {
        audit_log(tok.subject, "sys_app_stripper", app_path, false, "phạm vi không bao phủ app_path");
        return emit("Từ chối: phạm vi thẻ năng lực không bao phủ đường dẫn '" + app_path + "'.", true);
      }
      audit_log(tok.subject, "sys_app_stripper", app_path, true, "xem trước gọt tỉa");
      Value resp(json::Object{});
      resp.set("preview", true);
      resp.set("app_path", app_path);
      resp.set("estimated_saving_pct", 35.0);
      return emit(resp.dump(), false);
    } else {
      const uint64_t needed = sandbox::kRightFsRead | sandbox::kRightFsWrite;
      if ((tok.rights & needed) != needed) {
        audit_log(tok.subject, "sys_app_stripper", app_path, false, "thẻ năng lực thiếu quyền đọc/ghi ('fs.read' / 'fs.write')");
        return emit("Từ chối: thẻ năng lực thiếu quyền đọc/ghi tệp ('fs.read' / 'fs.write').", true);
      }
      bool covered_r = false, covered_w = false;
      for (const auto& sc : tok.scopes) {
        if (sandbox::scope_covers(sc, sandbox::Action{"fs.read", app_path})) covered_r = true;
        if (sandbox::scope_covers(sc, sandbox::Action{"fs.write", app_path})) covered_w = true;
      }
      if (!covered_r || !covered_w) {
        audit_log(tok.subject, "sys_app_stripper", app_path, false, "phạm vi không bao phủ app_path");
        return emit("Từ chối: phạm vi thẻ năng lực không bao phủ đường dẫn '" + app_path + "'.", true);
      }
      const bool confirmed = args["confirm"].as_bool(false);
      if (!confirmed) {
        audit_log(tok.subject, "sys_app_stripper", app_path, false, "chờ người dùng xác nhận cho " + action);
        Value resp(json::Object{});
        resp.set("needs_confirmation", true);
        resp.set("tool", "sys_app_stripper");
        resp.set("message", "Cần xác nhận người dùng: thực hiện " + action + " cho ứng dụng tại '" + app_path + "'");
        return emit(resp.dump(), false);
      }
      audit_log(tok.subject, "sys_app_stripper", app_path, true, "thực hiện " + action + " thành công");
      Value resp(json::Object{});
      resp.set("applied", true);
      resp.set("action", action);
      resp.set("app_path", app_path);
      return emit(resp.dump(), false);
    }
  }

  if (name == "sys_proc") {
    sandbox::Token tok;
    std::string err_msg;
    if (!extract_token(args, authority_, tok, err_msg)) {
      audit_log("", "sys_proc", "unknown", false, err_msg);
      return emit("Từ chối: " + err_msg, true);
    }
    if (!(tok.rights & sandbox::kRightAdmin)) {
      audit_log(tok.subject, "sys_proc", "unknown", false, "thẻ năng lực không có quyền 'admin'");
      return emit("Từ chối: thẻ năng lực không có quyền 'admin'.", true);
    }
    const std::string action = text::fold(param_string(args, "action", "list"));
    if (action == "list") {
      audit_log(tok.subject, "sys_proc", "list", true, "liệt kê tiến trình");
      Value resp(json::Object{});
      Value list(json::Array{});
      for (const auto& a : guard_.alerts()) {
        Value p(json::Object{});
        p.set("pid", int64_t(a.pid));
        p.set("name", a.name);
        p.set("reason_vi", a.reason_vi);
        list.push(std::move(p));
      }
      resp.set("processes", list);
      resp.set("count", int64_t(guard_.alerts().size()));
      return emit(resp.dump(), false);
    }
    const uint64_t pid = uint64_t(args["pid"].as_int(0));
    if (pid == 0) return emit("Từ chối: thiếu 'pid' hợp lệ.", true);
    const bool confirmed = args["confirm"].as_bool(false);
    if (!confirmed) {
      audit_log(tok.subject, "sys_proc", action + ":pid=" + std::to_string(pid), false, "chờ người dùng xác nhận");
      Value resp(json::Object{});
      resp.set("needs_confirmation", true);
      resp.set("tool", "sys_proc");
      resp.set("message", "Cần xác nhận người dùng: " + action + " tiến trình PID " + std::to_string(pid));
      return emit(resp.dump(), false);
    }
    guard_.clear(pid);
    audit_log(tok.subject, "sys_proc", action + ":pid=" + std::to_string(pid), true, "áp dụng " + action + " cho tiến trình");
    Value resp(json::Object{});
    resp.set("applied", true);
    resp.set("action", action);
    resp.set("pid", int64_t(pid));
    return emit(resp.dump(), false);
  }

  if (name == "sys_search_vault") {
    sandbox::Token tok;
    std::string err_msg;
    if (!extract_token(args, authority_, tok, err_msg)) {
      audit_log("", "sys_search_vault", "vault:search", false, err_msg);
      return emit("Từ chối: " + err_msg, true);
    }
    if (opts_.vault_password.empty()) {
      audit_log(tok.subject, "sys_search_vault", "vault:search", false, "kho mã hoá Vault chưa mở khoá");
      return emit("Từ chối: Kho mã hoá Vault chưa được mở khoá (thiếu mật khẩu).", true);
    }
    if (!(tok.rights & sandbox::kRightVault)) {
      audit_log(tok.subject, "sys_search_vault", "vault:search", false, "thẻ năng lực không có quyền 'vault'");
      return emit("Từ chối: thẻ năng lực không có quyền 'vault'.", true);
    }
    const std::string q = param_string(args, "q");
    const bool confirmed = args["confirm"].as_bool(false);
    if (!confirmed) {
      audit_log(tok.subject, "sys_search_vault", "vault:search", false, "chờ người dùng xác nhận trước khi tiết lộ");
      Value resp(json::Object{});
      resp.set("needs_confirmation", true);
      resp.set("tool", "sys_search_vault");
      resp.set("message", "Cần xác nhận người dùng: tìm kiếm và tiết lộ thông tin nhạy cảm trong Vault cho từ khoá '" + q + "'");
      return emit(resp.dump(), false);
    }
    audit_log(tok.subject, "sys_search_vault", "vault:search", true, "tìm kiếm kho vault đã được xác nhận");
    Value resp(json::Object{});
    Value matches(json::Array{});
    resp.set("matches", matches);
    resp.set("query", q);
    resp.set("vault_status", "unlocked");
    return emit(resp.dump(), false);
  }
  err.code = kMethodNotFound;
  err.message = "unknown tool: " + name;
  return Value(nullptr);
}

}  // namespace yui::service
