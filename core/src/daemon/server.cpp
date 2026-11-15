// yuid transports: Unix socket (line JSON-RPC) and MCP over stdio.
#include "yui/server.hpp"

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>

#include "yui/text.hpp"

namespace yui::server {

std::string handle_line(service::Daemon& daemon, const std::string& line) {
  const std::string trimmed = text::trim(line);
  if (trimmed.empty()) return "";
  auto parsed = json::parse(trimmed);
  if (!parsed.ok) {
    json::Value err(json::Object{});
    err.set("jsonrpc", "2.0");
    err.set("id", json::Value(nullptr));
    json::Value e(json::Object{});
    e.set("code", int(service::kParseError));
    e.set("message", parsed.error);
    err.set("error", e);
    return err.dump();
  }
  return daemon.dispatch(parsed.value).dump();
}

int run_unix_server(const std::string& path, service::Daemon& daemon, bool single_shot,
                    std::atomic<bool>* stop) {
  ::unlink(path.c_str());
  const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  if (path.size() >= sizeof(addr.sun_path)) {
    ::close(fd);
    return -2;
  }
  std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
  if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    ::close(fd);
    return -3;
  }
  // Only the owning user may talk to the daemon; capabilities are checked on top.
  ::chmod(path.c_str(), 0600);
  if (::listen(fd, 16) != 0) {
    ::close(fd);
    return -4;
  }
  while (true) {
    if (stop && stop->load()) break;
    const int client = ::accept(fd, nullptr, nullptr);
    if (client < 0) {
      if (errno == EINTR) continue;
      break;
    }
    std::string pending;
    char buf[8192];
    while (true) {
      const ssize_t n = ::read(client, buf, sizeof(buf));
      if (n <= 0) break;
      pending.append(buf, size_t(n));
      size_t nl;
      while ((nl = pending.find('\n')) != std::string::npos) {
        const std::string line = pending.substr(0, nl);
        pending.erase(0, nl + 1);
        const std::string reply = handle_line(daemon, line);
        if (!reply.empty()) {
          const std::string out = reply + "\n";
          size_t sent = 0;
          while (sent < out.size()) {
            const ssize_t w = ::write(client, out.data() + sent, out.size() - sent);
            if (w <= 0) break;
            sent += size_t(w);
          }
        }
      }
    }
    ::close(client);
    if (single_shot) break;
  }
  ::close(fd);
  ::unlink(path.c_str());
  return 0;
}

bool read_framed(std::FILE* in, std::string& body) {
  std::string header;
  char buf[512];
  // Read header lines until the blank line separating headers from the body.
  while (std::fgets(buf, sizeof(buf), in)) {
    header += buf;
    if (text::ends_with(header, "\r\n\r\n") || text::ends_with(header, "\n\n")) break;
  }
  if (header.empty()) return false;
  size_t length = 0;
  {
    const std::string lower = text::fold(header);
    const size_t pos = lower.find("content-length:");
    if (pos == std::string::npos) return false;
    length = size_t(std::strtoul(header.c_str() + pos + 15, nullptr, 10));
  }
  if (length == 0 || length > (1u << 24)) return false;
  body.assign(length, '\0');
  if (std::fread(&body[0], 1, length, in) != length) return false;
  return true;
}

void write_framed(std::FILE* out, const std::string& body) {
  std::fprintf(out, "Content-Length: %zu\r\n\r\n", body.size());
  std::fwrite(body.data(), 1, body.size(), out);
  std::fflush(out);
}

int run_mcp_stdio(service::Daemon& daemon) {
  std::string body;
  while (read_framed(stdin, body)) {
    auto parsed = json::parse(body);
    if (!parsed.ok) {
      json::Value err(json::Object{});
      err.set("jsonrpc", "2.0");
      err.set("id", json::Value(nullptr));
      json::Value e(json::Object{});
      e.set("code", int(service::kParseError));
      e.set("message", parsed.error);
      err.set("error", e);
      write_framed(stdout, err.dump());
      continue;
    }
    const json::Value& req = parsed.value;
    const std::string method = req["method"].as_string();
    const bool is_notification = !req.has("id");

    // Notifications never get a response.
    if (is_notification) continue;

    json::Value resp(json::Object{});
    resp.set("jsonrpc", "2.0");
    resp.set("id", req["id"]);

    service::RpcError err;
    json::Value result;
    if (method == "initialize") {
      result = daemon.mcp_initialize(req["params"]);
    } else if (method == "tools/list") {
      result = daemon.mcp_tools_list();
    } else if (method == "tools/call") {
      result = daemon.mcp_tool_call(req["params"], err);
    } else if (method == "shutdown") {
      result = json::Value(json::Object{});
      write_framed(stdout, resp.dump());
      break;
    } else if (method == "ping") {
      result = json::Value(json::Object{});
    } else {
      err.code = service::kMethodNotFound;
      err.message = "unknown MCP method: " + method;
    }
    if (err.code != 0) {
      json::Value e(json::Object{});
      e.set("code", err.code);
      e.set("message", err.message);
      resp.set("error", e);
    } else {
      resp.set("result", result);
    }
    write_framed(stdout, resp.dump());
  }
  return 0;
}

}  // namespace yui::server
