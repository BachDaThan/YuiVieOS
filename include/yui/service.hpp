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
  bool offline = true;         // web.search returns an explicit offline notice
  bool verbose = false;
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

  Options opts_;
  graph::Graph graph_;
  json::Value host_ = json::Value(json::Object{});
  sandbox::Enforcer enforcer_;
  guard::Guard guard_;
  bool has_host_ = false;
  size_t vault_ops_ = 0;
};

// Version string shared by yuid, libyui and the MCP server info block.
const char* version();

}  // namespace yui::service

#endif  // YUI_SERVICE_HPP
