// Transports for the yuid dispatcher.
#ifndef YUI_SERVER_HPP
#define YUI_SERVER_HPP

#include <atomic>
#include <string>

#include "yui/service.hpp"

namespace yui::server {

// Handles one line of the line-delimited JSON-RPC protocol and returns the
// response line (without the trailing newline).  Empty input -> empty output.
std::string handle_line(service::Daemon& daemon, const std::string& line);

// Serves /run/yuid.sock.  Blocks until `stop` is set; when `single_shot` is true
// it accepts one connection and returns (used by tests).
int run_unix_server(const std::string& path, service::Daemon& daemon, bool single_shot,
                    std::atomic<bool>* stop = nullptr);

// MCP over stdio: Content-Length framed JSON-RPC 2.0.
int run_mcp_stdio(service::Daemon& daemon);

// Reads one Content-Length framed message.  Returns false at EOF.
bool read_framed(std::FILE* in, std::string& body);
void write_framed(std::FILE* out, const std::string& body);

}  // namespace yui::server

#endif  // YUI_SERVER_HPP
