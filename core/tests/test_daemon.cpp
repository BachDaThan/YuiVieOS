// Integration tests for the yuid service surface: JSON-RPC dispatch, the Unix
// socket transport, the MCP handshake and the C ABI header contract.
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <cstring>
#include <string>
#include <thread>

#include "testutil.hpp"
#include "yui/b64.hpp"
#include "yui/server.hpp"
#include "yui/service.hpp"
#include "yui/yui.h"

using namespace yui;

static service::Daemon make_daemon(bool with_vault = false) {
  service::Options opts;
  opts.state_dir = "/tmp/yui-test";
  if (with_vault) opts.vault_password = "khoa-thu-nghiem";
  service::Daemon d(opts);
  d.set_host_snapshot(json::Value(json::Object{}));
  return d;
}

static json::Value call(service::Daemon& d, const std::string& method, const std::string& params_json) {
  json::Value req(json::Object{});
  req.set("jsonrpc", "2.0");
  req.set("id", 7);
  req.set("method", method);
  req.set("params", json::parse(params_json).value);
  return d.dispatch(req);
}

YUI_TEST(daemon, info_and_version) {
  auto d = make_daemon();
  const auto r = call(d, "yuid.info", "{}");
  CHECK(!r.has("error"));
  CHECK_EQ(r["id"].as_int(), int64_t(7));
  CHECK_EQ(r["result"]["name"].as_string(), std::string("yuid"));
  CHECK_EQ(r["result"]["tier"].as_int(), int64_t(4));
  CHECK_EQ(r["result"]["offline"].as_bool(), true);
  CHECK(!r["result"]["vault_enabled"].as_bool());
}

YUI_TEST(daemon, graph_workflow_over_rpc) {
  auto d = make_daemon();
  CHECK(!call(d, "graph.upsert", R"({"id":"os:yui","label":"YuiVieOS","kind":"concept"})").has("error"));
  CHECK(!call(d, "graph.upsert", R"({"id":"app:yui","label":"App Yui","kind":"app"})").has("error"));
  CHECK(!call(d, "graph.link", R"({"from":"app:yui","rel":"runs_on","to":"os:yui"})").has("error"));

  const auto q = call(d, "graph.query", R"({"q":"yui","expand_from":"app:yui","depth":1})");
  CHECK(!q.has("error"));
  CHECK_EQ(q["result"]["hits"].size(), size_t(2));
  CHECK_EQ(q["result"]["expanded"].size(), size_t(2));

  const auto p = call(d, "graph.path", R"({"from":"app:yui","to":"os:yui"})");
  CHECK_EQ(p["result"]["path"].size(), size_t(2));

  const auto s = call(d, "graph.stats", "{}");
  CHECK_EQ(s["result"]["nodes"].as_int(), int64_t(2));
  CHECK_EQ(s["result"]["edges"].as_int(), int64_t(1));

  const auto dump = call(d, "graph.export", "{}");
  const std::string doc = dump["result"]["graph"].dump();
  auto d2 = make_daemon();
  const auto imp = call(d2, "graph.import", std::string("{\"graph\":") + doc + "}");
  CHECK(!imp.has("error"));
  CHECK_EQ(d2.graph().stats().nodes, d.graph().stats().nodes);
}

YUI_TEST(daemon, error_reporting) {
  auto d = make_daemon();
  const auto unknown = call(d, "does.not.exist", "{}");
  CHECK_EQ(unknown["error"]["code"].as_int(), int64_t(service::kMethodNotFound));

  const auto missing = call(d, "graph.upsert", "{}");
  CHECK_EQ(missing["error"]["code"].as_int(), int64_t(service::kInvalidParams));

  const auto bad_link = call(d, "graph.link", R"({"from":"a","rel":"r","to":"b"})");
  CHECK_EQ(bad_link["error"]["code"].as_int(), int64_t(service::kInvalidParams));

  // Vault without a key is explicitly unavailable, not silently broken.
  const auto vault = call(d, "vault.seal", R"({"plain":"aGk="})");
  CHECK_EQ(vault["error"]["code"].as_int(), int64_t(service::kUnavailable));

  // Malformed envelope.
  json::Value notobj(json::Array{});
  const auto bad = d.dispatch(notobj);
  CHECK_EQ(bad["error"]["code"].as_int(), int64_t(service::kInvalidRequest));
}

YUI_TEST(daemon, vault_over_rpc) {
  auto d = make_daemon(true);
  const std::string plain = "Thông tin riêng tư: 42";
  const std::string plain_b64 = b64::encode(reinterpret_cast<const uint8_t*>(plain.data()), plain.size());
  const auto sealed = call(d, "vault.seal", "{\"plain\":\"" + plain_b64 + "\",\"log2n\":10}");
  CHECK(!sealed.has("error"));
  const std::string blob = sealed["result"]["blob"].as_string();
  CHECK(!blob.empty());

  const auto opened = call(d, "vault.open", "{\"blob\":\"" + blob + "\"}");
  CHECK(!opened.has("error"));
  std::string round;
  CHECK(b64::decode_string(opened["result"]["plain"].as_string(), round));
  CHECK_EQ(round, plain);

  // A tampered blob must be refused with kDenied.
  std::vector<uint8_t> raw;
  CHECK(b64::decode(blob, raw));
  raw.back() ^= 0x01;
  const auto bad = call(d, "vault.open", "{\"blob\":\"" + b64::encode(raw) + "\"}");
  CHECK_EQ(bad["error"]["code"].as_int(), int64_t(service::kDenied));
}

YUI_TEST(daemon, ime_and_pack_and_policy) {
  auto d = make_daemon();
  const auto ime = call(d, "ime.convert", R"({"text":"Tieengs Vieetj","method":"telex"})");
  CHECK_EQ(ime["result"]["result"].as_string(), std::string("Tiếng Việt"));

  const auto vni = call(d, "ime.convert", R"({"text":"Vie65t Nam","method":"vni"})");
  CHECK_EQ(vni["result"]["result"].as_string(), std::string("Việt Nam"));

  const auto bad_ime = call(d, "ime.convert", R"({"text":"x","method":"klingon"})");
  CHECK_EQ(bad_ime["error"]["code"].as_int(), int64_t(service::kInvalidParams));

  // A realistic payload: the container has a 60-byte header, so ratios only make
  // sense for data that is actually worth compressing.
  std::string payload;
  for (int i = 0; i < 60; ++i) payload += "Tầng 4 — dịch vụ trí tuệ lõi của YuiVieOS. ";
  const std::string data = b64::encode(reinterpret_cast<const uint8_t*>(payload.data()), payload.size());
  const auto packed = call(d, "pack.compress", "{\"data\":\"" + data + "\"}");
  CHECK(!packed.has("error"));
  CHECK(packed["result"]["ratio"].as_double() > 0.5);
  const auto back = call(d, "pack.decompress",
                         "{\"data\":\"" + packed["result"]["packed"].as_string() + "\"}");
  CHECK_EQ(back["result"]["data"].as_string(), data);

  const auto decision = call(d, "sandbox.check", R"({
      "policy": {"app":"app:x","tier":"isolated","capabilities":["fs.read","fs:/home/user:ro"]},
      "action": {"kind":"fs.write","target":"/home/user/a"}})");
  CHECK_EQ(decision["result"]["allowed"].as_bool(), false);
  CHECK_EQ(decision["result"]["tier"].as_string(), std::string("isolated"));

  const auto search = call(d, "web.search", R"({"q":"YuiVieOS"})");
  CHECK_EQ(search["result"]["backend"].as_string(), std::string("offline"));
  CHECK(search["result"]["note"].as_string().find("ngoại tuyến") != std::string::npos);

  const auto tune = call(d, "yuid.tune", R"({"ram_gb":2,"cpu_threads":2,"temperature_c":50})");
  CHECK_EQ(tune["result"]["profile"].as_string(), std::string("may-yeu"));
  const auto bad_tune = call(d, "yuid.tune", R"({"ram_gb":8,"cpu_threads":0})");
  CHECK_EQ(bad_tune["error"]["code"].as_int(), int64_t(service::kInvalidParams));
}

YUI_TEST(daemon, unix_socket_roundtrip) {
  const std::string path = "/tmp/yui-test-" + std::to_string(::getpid()) + ".sock";
  auto d = make_daemon();

  // The server runs in a thread and accepts exactly one connection.
  std::thread server([&] { server::run_unix_server(path, d, true, nullptr); });
  for (int i = 0; i < 400 && ::access(path.c_str(), F_OK) != 0; ++i) {
    timespec ts{0, 5 * 1000 * 1000};
    ::nanosleep(&ts, nullptr);
  }
  CHECK(::access(path.c_str(), F_OK) == 0);

  const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  CHECK(fd >= 0);
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
  CHECK(::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);

  const std::string line = R"({"jsonrpc":"2.0","id":1,"method":"yuid.info"})" "\n";
  CHECK(::write(fd, line.data(), line.size()) == ssize_t(line.size()));

  std::string got;
  char buf[4096];
  ssize_t n;
  while ((n = ::read(fd, buf, sizeof(buf))) > 0) {
    got.append(buf, size_t(n));
    if (got.find('\n') != std::string::npos) break;
  }
  ::close(fd);
  server.join();

  CHECK(got.find("yuid") != std::string::npos);
  auto parsed = json::parse(got);
  CHECK(parsed.ok);
  CHECK_EQ(parsed.value["result"]["name"].as_string(), std::string("yuid"));
}

YUI_TEST(daemon, mcp_handshake_and_tools) {
  auto d = make_daemon();
  const auto init = d.mcp_initialize(json::Value(json::Object{}));
  CHECK_EQ(init["protocolVersion"].as_string(), std::string("2024-11-05"));
  CHECK_EQ(init["serverInfo"]["name"].as_string(), std::string("yuid"));
  CHECK(init.has("capabilities"));
  CHECK(init["capabilities"].has("tools"));

  const auto tools = d.mcp_tools_list();
  CHECK(tools["tools"].size() >= 6);
  bool saw_ime = false, saw_graph = false, saw_sandbox = false;
  for (const auto& t : tools["tools"].arr()) {
    if (t["name"].as_string() == "ime_convert") saw_ime = true;
    if (t["name"].as_string() == "graph_query") saw_graph = true;
    if (t["name"].as_string() == "sandbox_check") saw_sandbox = true;
    CHECK(t["inputSchema"]["type"].as_string() == "object");
  }
  CHECK(saw_ime && saw_graph && saw_sandbox);

  service::RpcError err;
  const auto r = d.mcp_tool_call(json::parse(R"({"name":"ime_convert",
       "arguments":{"text":"chaof banj","method":"telex"}})").value, err);
  CHECK_EQ(err.code, 0);
  CHECK_EQ(r["isError"].as_bool(), false);
  CHECK_EQ(r["content"].arr()[0]["text"].as_string(), std::string("chào bạn"));

  service::RpcError err2;
  const auto bad = d.mcp_tool_call(json::parse(R"({"name":"nope","arguments":{}})").value, err2);
  CHECK_EQ(err2.code, int(service::kMethodNotFound));
  CHECK(bad.is_null());
}

YUI_TEST(daemon, c_abi_contract) {
  CHECK(std::string(yui_version()).size() > 0);
  CHECK_EQ(yui_abi_version(), YUI_ABI_VERSION);
  CHECK(std::string(yui_arch()).size() > 0);

  char simd[32];
  CHECK(yui_simd(simd, sizeof(simd)) > 0);
  CHECK(std::strlen(simd) > 0);

  char hexbuf[65];
  CHECK_EQ(yui_sha256("abc", 3, hexbuf), YUI_OK);
  CHECK_EQ(std::string(hexbuf),
           std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));

  std::string big;
  for (int i = 0; i < 50; ++i) big += "YuiVieOS nén dữ liệu bằng YUI-LZ. ";
  const char* text = big.c_str();
  yui_buf packed{};
  CHECK_EQ(yui_pack(text, big.size(), 6, &packed), YUI_OK);
  CHECK(packed.len > 0);
  CHECK(packed.len < big.size() / 2);
  yui_buf plain{};
  char err[128];
  CHECK_EQ(yui_unpack(packed.data, packed.len, &plain, err, sizeof(err)), YUI_OK);
  CHECK_EQ(std::string(reinterpret_cast<char*>(plain.data), plain.len), big);
  yui_buf_free(&packed);
  yui_buf_free(&plain);

  yui_tune tune{};
  CHECK_EQ(yui_tune_auto(20, 50, 0, 2, 0, &tune), YUI_OK);
  CHECK_EQ(std::string(tune.profile), std::string("may-yeu"));
  CHECK(tune.k > 0);

  yui_graph* g = yui_graph_new();
  CHECK(g != nullptr);
  CHECK_EQ(yui_graph_upsert(g, "a", "Nút A", "concept"), YUI_OK);
  CHECK_EQ(yui_graph_upsert(g, "b", "Nút B", "concept"), YUI_OK);
  CHECK_EQ(yui_graph_link(g, "a", "to", "b", 1.0), YUI_OK);
  CHECK_EQ(yui_graph_nodes(g), size_t(2));
  char* doc = yui_graph_dump(g);
  CHECK(doc != nullptr);
  CHECK(std::string(doc).find("Nút A") != std::string::npos);
  char ids[64];
  CHECK(yui_graph_bfs(g, "a", 2, ids, sizeof(ids)) > 0);
  CHECK(std::string(ids).find('b') != std::string::npos);
  yui_free(doc);
  yui_graph_free(g);

  char out[64];
  CHECK(yui_telex("Tieengs Vieetj", out, sizeof(out)) > 0);
  CHECK_EQ(std::string(out), std::string("Tiếng Việt"));
  CHECK(yui_vni("cha2o", out, sizeof(out)) > 0);
  CHECK_EQ(std::string(out), std::string("chào"));
  CHECK_EQ(yui_telex("a", out, 1), YUI_E_TOO_SMALL);

  yui_buf sealed{};
  CHECK_EQ(yui_vault_seal("pw", "secret", 6, 10, &sealed), YUI_OK);
  yui_buf opened{};
  CHECK_EQ(yui_vault_open("pw", sealed.data, sealed.len, &opened, err, sizeof(err)), YUI_OK);
  CHECK_EQ(std::string(reinterpret_cast<char*>(opened.data), opened.len), std::string("secret"));
  yui_buf wrong{};
  CHECK_EQ(yui_vault_open("nope", sealed.data, sealed.len, &wrong, err, sizeof(err)), YUI_E_AUTH);
  yui_buf_free(&sealed);
  yui_buf_free(&opened);

  char reason[256];
  const char* policy = R"({"app":"x","tier":"isolated","capabilities":["fs.read","fs:/etc/yui:ro"]})";
  CHECK_EQ(yui_sandbox_check(policy, "fs.read", "/etc/yui/p.yaml", reason, sizeof(reason)), 1);
  CHECK_EQ(yui_sandbox_check(policy, "fs.write", "/etc/yui/p.yaml", reason, sizeof(reason)), 0);
}

YUI_TEST_MAIN("test_daemon")
