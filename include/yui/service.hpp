// yuid service surface.  One dispatcher serves three transports:
//   * the Unix socket at /run/yuid.sock (line-delimited JSON-RPC 2.0)
//   * the MCP bridge (Content-Length framed JSON-RPC 2.0 on stdio)
//   * the C-ABI (libyui) for apps that link the library instead
//
// Keeping the dispatch table in one place is what makes "same brain, three
// doors" true instead of aspirational.
#ifndef YUI_SERVICE_HPP
#define YUI_SERVICE_HPP

#include <map>
#include <string>
#include <vector>

#include "yui/guard.hpp"
#include "yui/graph.hpp"
#include "yui/json.hpp"
#include "yui/sandbox.hpp"
#include "yui/vault.hpp"

namespace yui::service {

struct Options {
  std::string state_dir = "/var/lib/yui";
  std::string vault_password;  // empty -> vault methods refuse to run
  std::string cap_master_key = "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";
  bool offline = true;         // web.search returns an explicit offline notice
  bool verbose = false;
  size_t audit_max_bytes = 1024 * 1024;  // 1 MB default
  int audit_retention_days = 30;         // 30 days retention
  bool audit_hash_domains = false;       // optional domain anonymization
  // Kho Ứng Dụng (P4.15). store_root rỗng ⇒ <state_dir>/store.
  std::string store_root;
  std::string store_keyring_dir = "/etc/yui/trusted-keys";
  bool store_allow_file_urls = false;    // file:// chỉ cho thử nghiệm / bản tự tải
  // Thùng rác + dọn rác (P4.17). trash_root rỗng ⇒ <state_dir>/trash.
  std::string trash_root;
  uint64_t trash_capacity_bytes = 2ull * 1024 * 1024 * 1024;
  // Chế độ POSIX của shell (P4.12): nhánh đọc/ghi cho hộp cát của lệnh con.
  std::vector<std::string> posix_rw_dirs;               // rỗng ⇒ {$HOME, /tmp}
  std::vector<std::string> posix_ro_dirs = {"/"};
  std::vector<std::string> posix_deny;                  // rỗng ⇒ {~/.ssh, ~/.gnupg, /etc/shadow, /etc/sudoers, state_dir}
};

// JSON-RPC 2.0 error codes (subset).
enum ErrorCode {
  kParseError = -32700,
  kInvalidRequest = -32600,
  kMethodNotFound = -32601,
  kInvalidParams = -32602,
  kInternalError = -32603,
  kDenied = -32000,
  kUnavailable = -32001,
};

struct RpcError {
  int code = 0;
  std::string message;
  json::Value data;
};

class Daemon {
 public:
  explicit Daemon(Options opts);

  // Returns the `result` value; on failure fills `err`.
  json::Value handle(const std::string& method, const json::Value& params, RpcError& err);

  // JSON-RPC envelope: request {jsonrpc,id,method,params} -> response object.
  json::Value dispatch(const json::Value& request);

  // MCP ---------------------------------------------------------------
  json::Value mcp_initialize(const json::Value& params);
  json::Value mcp_tools_list();
  json::Value mcp_tool_call(const json::Value& params, RpcError& err);

  const graph::Graph& graph() const { return graph_; }
  graph::Graph& graph() { return graph_; }
  const Options& options() const { return opts_; }
  // Guard + audit: cho phép test/shell nạp mẫu và xem quyết định.
  guard::Guard& guard() { return guard_; }
  const sandbox::Enforcer& enforcer() const { return enforcer_; }
  sandbox::Authority& authority() { return authority_; }
  const sandbox::Authority& authority() const { return authority_; }
  sandbox::TrustedConfirmationHost& trusted_confirmation() { return trusted_confirmation_; }
  const sandbox::TrustedConfirmationHost& trusted_confirmation() const { return trusted_confirmation_; }
  sandbox::CompatibilityManager& compatibility() { return compat_mgr_; }
  const sandbox::CompatibilityManager& compatibility() const { return compat_mgr_; }

  const std::string& power_mode() const { return power_mode_; }
  int charge_limit() const { return charge_limit_; }
  const std::string& sched_profile() const { return sched_profile_; }
  int sched_slice_us() const { return sched_slice_us_; }

  void audit_log(const std::string& subject, const std::string& action_kind,
                 const std::string& target, bool allowed, const std::string& reason);
  void audit_rotate();
  bool audit_clear();
  std::string anonymize_target(const std::string& target) const;

  // Tests and the shell can inject a canned host snapshot.
  void set_host_snapshot(json::Value v) {
    host_ = std::move(v);
    has_host_ = true;
  }

 private:
  json::Value method_info();
  json::Value method_profile();
  json::Value method_tune(const json::Value& params, RpcError& err);
  json::Value method_graph(const std::string& op, const json::Value& params, RpcError& err);
  json::Value method_vault(const std::string& op, const json::Value& params, RpcError& err);
  json::Value method_sandbox_check(const json::Value& params, RpcError& err);
  json::Value method_ime(const json::Value& params, RpcError& err);
  json::Value method_pack(const std::string& op, const json::Value& params, RpcError& err);
  json::Value method_web_search(const json::Value& params, RpcError& err);
  json::Value method_os(const std::string& op);
  json::Value method_telemetry();
  // Guard chống bất thường nền: guard.observe / guard.alerts / guard.decide.
  json::Value method_guard(const std::string& op, const json::Value& params, RpcError& err);
  json::Value method_compat(const std::string& op, const json::Value& params, RpcError& err);
  // Kho Ứng Dụng (P4.15): store.search / store.plan / store.install / store.remove / store.list.
  json::Value method_store(const std::string& op, const json::Value& params, RpcError& err);
  // Thùng rác (P4.17): trash.list / put / restore / purge / empty / clean_plan / clean.
  json::Value method_trash(const std::string& op, const json::Value& params, RpcError& err);
  // Shell POSIX (P4.12): shell.posix_check / shell.posix_policy.
  json::Value method_shell(const std::string& op, const json::Value& params, RpcError& err);
  sandbox::Policy posix_policy() const;
  // Đường tin cậy P4.13 cho việc không đảo ngược được: true nếu dialog_id đúng
  // đích, chưa dùng; nếu không, mở hộp thoại mới và điền `out` (needs_confirmation).
  bool trusted_dialog_ready(const std::string& target, const std::string& title_vi, const std::string& summary_vi,
                            const std::string& dialog_id, const std::string& audit_kind, json::Value& out);

  Options opts_;
  graph::Graph graph_;
  json::Value host_ = json::Value(json::Object{});
  sandbox::Enforcer enforcer_;
  sandbox::Authority authority_;
  sandbox::TrustedConfirmationHost trusted_confirmation_;
  sandbox::CompatibilityManager compat_mgr_;
  guard::Guard guard_;
  bool has_host_ = false;
  size_t vault_ops_ = 0;
  std::string power_mode_ = "can_bang";
  int charge_limit_ = 100;
  std::string sched_profile_ = "interactive";
  int sched_slice_us_ = 5000;
  // Hộp thoại xác nhận cài đã dùng một lần thì không dùng lại được (chống phát lại).
  std::vector<std::string> used_dialogs_;
};

// Version string shared by yuid, libyui and the MCP server info block.
const char* version();

}  // namespace yui::service

#endif  // YUI_SERVICE_HPP
