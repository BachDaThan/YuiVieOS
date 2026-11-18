// Integration tests for the yuid service surface: JSON-RPC dispatch, the Unix
// socket transport, the MCP handshake and the C ABI header contract.
#include <algorithm>
#include <chrono>
#include <fstream>
#include <sys/socket.h>
#include <sys/stat.h>
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
#include "yui/shell.hpp"
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
  const auto tune_sieu_yeu = call(d, "yuid.tune", R"({"ram_gb":1.4,"cpu_threads":2,"temperature_c":45})");
  CHECK_EQ(tune_sieu_yeu["result"]["profile"].as_string(), std::string("sieu-yeu"));
  CHECK_EQ(tune_sieu_yeu["result"]["model_loaded"].as_bool(), false);
  CHECK_EQ(tune_sieu_yeu["result"]["zero_ai"].as_bool(), true);
  CHECK_EQ(tune_sieu_yeu["result"]["headless"].as_bool(), true);
  CHECK_EQ(tune_sieu_yeu["result"]["compositor"].as_bool(), false);
  CHECK_EQ(tune_sieu_yeu["result"]["fallback"].as_string(), std::string("bm25+rules"));
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

YUI_TEST(daemon, mcp_os_tools_catalog_and_schemas) {
  auto d = make_daemon();
  const auto tools = d.mcp_tools_list();
  CHECK(tools["tools"].size() >= 14);

  std::map<std::string, const json::Value*> tool_map;
  for (const auto& t : tools["tools"].arr()) {
    tool_map[t["name"].as_string()] = &t;
  }

  const char* kOsTools[] = {"sys_power", "sys_scheduler", "sys_app_stripper", "sys_proc",
                            "sys_search_vault"};
  for (const char* name : kOsTools) {
    CHECK_MSG(tool_map.count(name) > 0, std::string("phải có công cụ ") + name);
    const auto& t = *tool_map[name];
    CHECK_EQ(t["inputSchema"]["type"].as_string(), "object");
    CHECK(t["inputSchema"]["properties"].is_object());
    CHECK(t["inputSchema"]["required"].size() >= 1);
    CHECK(!t["description"].as_string().empty());
  }
}

YUI_TEST(daemon, mcp_os_tools_fail_closed_without_token) {
  auto d = make_daemon(true);
  const size_t audit_before = d.enforcer().audit().size();

  service::RpcError err;
  // sys_power without token
  auto r1 = d.mcp_tool_call(json::parse(R"({"name":"sys_power","arguments":{"mode":"powersave","confirm":true}})").value, err);
  CHECK(r1["isError"].as_bool());
  CHECK(r1["content"].arr()[0]["text"].as_string().find("thiếu thẻ năng lực") != std::string::npos);

  // sys_scheduler without token
  auto r2 = d.mcp_tool_call(json::parse(R"({"name":"sys_scheduler","arguments":{"profile":"batch","confirm":true}})").value, err);
  CHECK(r2["isError"].as_bool());
  CHECK(r2["content"].arr()[0]["text"].as_string().find("thiếu thẻ năng lực") != std::string::npos);

  // sys_app_stripper without token
  auto r3 = d.mcp_tool_call(json::parse(R"({"name":"sys_app_stripper","arguments":{"app_path":"/tmp/app"}})").value, err);
  CHECK(r3["isError"].as_bool());
  CHECK(r3["content"].arr()[0]["text"].as_string().find("thiếu thẻ năng lực") != std::string::npos);

  // sys_proc without token
  auto r4 = d.mcp_tool_call(json::parse(R"({"name":"sys_proc","arguments":{"action":"list"}})").value, err);
  CHECK(r4["isError"].as_bool());
  CHECK(r4["content"].arr()[0]["text"].as_string().find("thiếu thẻ năng lực") != std::string::npos);

  // sys_search_vault without token
  auto r5 = d.mcp_tool_call(json::parse(R"({"name":"sys_search_vault","arguments":{"q":"secret","confirm":true}})").value, err);
  CHECK(r5["isError"].as_bool());
  CHECK(r5["content"].arr()[0]["text"].as_string().find("thiếu thẻ năng lực") != std::string::npos);

  // Verify all 5 unauthenticated calls were audited as DENIED
  const auto& audit = d.enforcer().audit();
  CHECK_EQ(audit.size() - audit_before, size_t(5));
  for (size_t i = audit_before; i < audit.size(); ++i) {
    CHECK(!audit[i].decision.allowed);
  }
}

YUI_TEST(daemon, mcp_os_tools_fail_closed_invalid_or_unauthorized_token) {
  auto d = make_daemon(true);
  const int64_t now = std::time(nullptr);

  // 1. Corrupted bearer token
  service::RpcError err;
  auto r_corrupt = d.mcp_tool_call(json::parse(R"({"name":"sys_power","arguments":{"mode":"powersave","confirm":true,"token":"invalid.bearer.data"}})") .value, err);
  CHECK(r_corrupt["isError"].as_bool());

  // 2. Expired token
  sandbox::Token tok_expired = d.authority().issue("app:test", sandbox::kRightPower, {}, now - 7200, 60);
  std::string bearer_expired = d.authority().bearer(tok_expired);
  auto r_exp = d.mcp_tool_call(json::parse(R"({"name":"sys_power","arguments":{"mode":"powersave","confirm":true,"token":")" + bearer_expired + R"("}})") .value, err);
  CHECK(r_exp["isError"].as_bool());
  CHECK(r_exp["content"].arr()[0]["text"].as_string().find("hết hạn") != std::string::npos ||
        r_exp["content"].arr()[0]["text"].as_string().find("expired") != std::string::npos);

  // 3. Wrong capability: token has only kRightFsRead, calling sys_power
  sandbox::Token tok_fs = d.authority().issue("app:test", sandbox::kRightFsRead, {"/home"}, now, 3600);
  std::string bearer_fs = d.authority().bearer(tok_fs);
  auto r_wrong_cap = d.mcp_tool_call(json::parse(R"({"name":"sys_power","arguments":{"mode":"powersave","confirm":true,"token":")" + bearer_fs + R"("}})") .value, err);
  CHECK(r_wrong_cap["isError"].as_bool());
  CHECK(r_wrong_cap["content"].arr()[0]["text"].as_string().find("không có quyền 'power'") != std::string::npos);

  // Token has power, calling sys_proc (needs admin)
  sandbox::Token tok_power = d.authority().issue("app:test", sandbox::kRightPower, {}, now, 3600);
  std::string bearer_power = d.authority().bearer(tok_power);
  auto r_need_admin = d.mcp_tool_call(json::parse(R"({"name":"sys_proc","arguments":{"action":"list","token":")" + bearer_power + R"("}})") .value, err);
  CHECK(r_need_admin["isError"].as_bool());
  CHECK(r_need_admin["content"].arr()[0]["text"].as_string().find("không có quyền 'admin'") != std::string::npos);

  // Token has power, calling sys_search_vault (needs vault)
  auto r_need_vault = d.mcp_tool_call(json::parse(R"({"name":"sys_search_vault","arguments":{"q":"test","confirm":true,"token":")" + bearer_power + R"("}})") .value, err);
  CHECK(r_need_vault["isError"].as_bool());
  CHECK(r_need_vault["content"].arr()[0]["text"].as_string().find("không có quyền 'vault'") != std::string::npos);

  // 4. Scope mismatch for sys_app_stripper
  sandbox::Token tok_scoped = d.authority().issue("app:test", sandbox::kRightFsRead | sandbox::kRightFsWrite, {"/home/user/AppA"}, now, 3600);
  std::string bearer_scoped = d.authority().bearer(tok_scoped);
  auto r_bad_scope = d.mcp_tool_call(json::parse(R"({"name":"sys_app_stripper","arguments":{"app_path":"/etc/shadow","action":"preview","token":")" + bearer_scoped + R"("}})") .value, err);
  CHECK(r_bad_scope["isError"].as_bool());
  CHECK(r_bad_scope["content"].arr()[0]["text"].as_string().find("không bao phủ") != std::string::npos);
}

YUI_TEST(daemon, mcp_os_tools_user_confirmation_gate) {
  auto d = make_daemon(true);
  const int64_t now = std::time(nullptr);
  sandbox::Token tok_full = d.authority().issue(
      "agent:assistant",
      sandbox::kRightPower | sandbox::kRightAdmin | sandbox::kRightFsRead |
          sandbox::kRightFsWrite | sandbox::kRightVault,
      {"*"}, now, 3600);
  std::string bearer = d.authority().bearer(tok_full);

  service::RpcError err;

  // 1. sys_power without confirm
  auto r1 = d.mcp_tool_call(json::parse(R"({"name":"sys_power","arguments":{"mode":"powersave","charge_limit":80,"token":")" + bearer + R"("}})") .value, err);
  CHECK(!r1["isError"].as_bool());
  auto p1 = json::parse(r1["content"].arr()[0]["text"].as_string());
  CHECK(p1.ok);
  CHECK(p1.value["needs_confirmation"].as_bool());
  CHECK(!p1.value["message"].as_string().empty());
  // Side effect check: power_mode must remain default
  CHECK_EQ(d.power_mode(), "can_bang");
  CHECK_EQ(d.charge_limit(), 100);

  // 2. sys_scheduler without confirm
  auto r2 = d.mcp_tool_call(json::parse(R"({"name":"sys_scheduler","arguments":{"profile":"realtime","slice_us":2000,"token":")" + bearer + R"("}})") .value, err);
  CHECK(!r2["isError"].as_bool());
  auto p2 = json::parse(r2["content"].arr()[0]["text"].as_string());
  CHECK(p2.ok);
  CHECK(p2.value["needs_confirmation"].as_bool());
  CHECK_EQ(d.sched_profile(), "interactive");
  CHECK_EQ(d.sched_slice_us(), 5000);

  // 3. sys_app_stripper apply without confirm
  auto r3 = d.mcp_tool_call(json::parse(R"({"name":"sys_app_stripper","arguments":{"app_path":"/tmp/test_app","action":"apply","token":")" + bearer + R"("}})") .value, err);
  CHECK(!r3["isError"].as_bool());
  auto p3 = json::parse(r3["content"].arr()[0]["text"].as_string());
  CHECK(p3.ok);
  CHECK(p3.value["needs_confirmation"].as_bool());

  // 4. sys_proc kill without confirm
  auto r4 = d.mcp_tool_call(json::parse(R"({"name":"sys_proc","arguments":{"action":"kill","pid":999,"token":")" + bearer + R"("}})") .value, err);
  CHECK(!r4["isError"].as_bool());
  auto p4 = json::parse(r4["content"].arr()[0]["text"].as_string());
  CHECK(p4.ok);
  CHECK(p4.value["needs_confirmation"].as_bool());

  // 5. sys_search_vault without confirm
  auto r5 = d.mcp_tool_call(json::parse(R"({"name":"sys_search_vault","arguments":{"q":"bi-mat","token":")" + bearer + R"("}})") .value, err);
  CHECK(!r5["isError"].as_bool());
  auto p5 = json::parse(r5["content"].arr()[0]["text"].as_string());
  CHECK(p5.ok);
  CHECK(p5.value["needs_confirmation"].as_bool());
  // No sensitive plaintext matches exposed without confirmation!
  CHECK(!p5.value.has("matches"));
}

YUI_TEST(daemon, mcp_os_tools_confirmed_execution_and_audit) {
  auto d = make_daemon(true);
  const int64_t now = std::time(nullptr);
  sandbox::Token tok_full = d.authority().issue(
      "agent:assistant",
      sandbox::kRightPower | sandbox::kRightAdmin | sandbox::kRightFsRead |
          sandbox::kRightFsWrite | sandbox::kRightVault,
      {"*"}, now, 3600);
  std::string bearer = d.authority().bearer(tok_full);

  service::RpcError err;

  // 1. sys_power with confirm = true
  auto r1 = d.mcp_tool_call(json::parse(R"({"name":"sys_power","arguments":{"mode":"powersave","charge_limit":80,"confirm":true,"token":")" + bearer + R"("}})") .value, err);
  CHECK(!r1["isError"].as_bool());
  auto p1 = json::parse(r1["content"].arr()[0]["text"].as_string());
  CHECK(p1.ok);
  CHECK(p1.value["applied"].as_bool());
  CHECK_EQ(d.power_mode(), "powersave");
  CHECK_EQ(d.charge_limit(), 80);

  // 2. sys_scheduler with confirm = true
  auto r2 = d.mcp_tool_call(json::parse(R"({"name":"sys_scheduler","arguments":{"profile":"batch","slice_us":10000,"confirm":true,"token":")" + bearer + R"("}})") .value, err);
  CHECK(!r2["isError"].as_bool());
  auto p2 = json::parse(r2["content"].arr()[0]["text"].as_string());
  CHECK(p2.ok);
  CHECK(p2.value["applied"].as_bool());
  CHECK_EQ(d.sched_profile(), "batch");
  CHECK_EQ(d.sched_slice_us(), 10000);

  // 3. sys_app_stripper preview (read-only, does not require confirm)
  auto r3_prev = d.mcp_tool_call(json::parse(R"({"name":"sys_app_stripper","arguments":{"app_path":"/tmp/app","action":"preview","token":")" + bearer + R"("}})") .value, err);
  CHECK(!r3_prev["isError"].as_bool());
  auto p3_prev = json::parse(r3_prev["content"].arr()[0]["text"].as_string());
  CHECK(p3_prev.ok);
  CHECK(p3_prev.value["preview"].as_bool());

  // sys_app_stripper apply with confirm = true
  auto r3 = d.mcp_tool_call(json::parse(R"({"name":"sys_app_stripper","arguments":{"app_path":"/tmp/app","action":"apply","confirm":true,"token":")" + bearer + R"("}})") .value, err);
  CHECK(!r3["isError"].as_bool());
  auto p3 = json::parse(r3["content"].arr()[0]["text"].as_string());
  CHECK(p3.ok);
  CHECK(p3.value["applied"].as_bool());

  // 4. sys_proc list (read-only, does not require confirm)
  auto r4_list = d.mcp_tool_call(json::parse(R"({"name":"sys_proc","arguments":{"action":"list","token":")" + bearer + R"("}})") .value, err);
  CHECK(!r4_list["isError"].as_bool());
  auto p4_list = json::parse(r4_list["content"].arr()[0]["text"].as_string());
  CHECK(p4_list.ok);
  CHECK(p4_list.value.has("processes"));

  // sys_proc kill with confirm = true
  auto r4 = d.mcp_tool_call(json::parse(R"({"name":"sys_proc","arguments":{"action":"kill","pid":555,"confirm":true,"token":")" + bearer + R"("}})") .value, err);
  CHECK(!r4["isError"].as_bool());
  auto p4 = json::parse(r4["content"].arr()[0]["text"].as_string());
  CHECK(p4.ok);
  CHECK(p4.value["applied"].as_bool());

  // 5. sys_search_vault with confirm = true
  auto r5 = d.mcp_tool_call(json::parse(R"({"name":"sys_search_vault","arguments":{"q":"ssh-key","confirm":true,"token":")" + bearer + R"("}})") .value, err);
  CHECK(!r5["isError"].as_bool());
  auto p5 = json::parse(r5["content"].arr()[0]["text"].as_string());
  CHECK(p5.ok);
  CHECK_EQ(p5.value["vault_status"].as_string(), "unlocked");
  CHECK_EQ(p5.value["query"].as_string(), "ssh-key");

  // Verify that successful actions were recorded in enforcer audit log
  bool saw_power_allow = false, saw_sched_allow = false, saw_vault_allow = false;
  for (const auto& a : d.enforcer().audit()) {
    if (a.action.kind == "sys_power" && a.decision.allowed) saw_power_allow = true;
    if (a.action.kind == "sys_scheduler" && a.decision.allowed) saw_sched_allow = true;
    if (a.action.kind == "sys_search_vault" && a.decision.allowed) saw_vault_allow = true;
  }
  CHECK(saw_power_allow && saw_sched_allow && saw_vault_allow);
}

YUI_TEST(daemon, mcp_os_tools_audit_log_file_no_secrets_leaked) {
  service::Options opts;
  opts.state_dir = "/tmp/yui-audit-test-" + std::to_string(std::time(nullptr));
  opts.vault_password = "super-secret-vault-master-password-12345";
  service::Daemon d(opts);

  const int64_t now = std::time(nullptr);
  sandbox::Token tok = d.authority().issue(
      "agent:assistant",
      sandbox::kRightPower | sandbox::kRightAdmin | sandbox::kRightVault,
      {"*"}, now, 3600);
  std::string bearer = d.authority().bearer(tok);

  service::RpcError err;
  d.mcp_tool_call(json::parse(R"({"name":"sys_power","arguments":{"mode":"powersave","confirm":true,"token":")" + bearer + R"("}})") .value, err);
  d.mcp_tool_call(json::parse(R"({"name":"sys_search_vault","arguments":{"q":"my-sensitive-query","confirm":true,"token":")" + bearer + R"("}})") .value, err);

  std::ifstream f(opts.state_dir + "/audit.log");
  CHECK(f.is_open());
  std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  f.close();

  // Audit log must have content
  CHECK(!content.empty());
  CHECK(content.find("sys_power") != std::string::npos);
  CHECK(content.find("sys_search_vault") != std::string::npos);
  CHECK(content.find("ALLOWED") != std::string::npos);

  // Critical security requirement (P4.3): Audit log must NOT contain secrets, passwords or full bearer tokens
  CHECK(content.find("super-secret-vault-master-password-12345") == std::string::npos);
  CHECK(content.find(bearer) == std::string::npos);
}

YUI_TEST(daemon, mcp_os_tools_stress_and_latency) {
  auto d = make_daemon(true);
  const int64_t now = std::time(nullptr);
  sandbox::Token tok = d.authority().issue(
      "stress:runner",
      sandbox::kRightPower | sandbox::kRightAdmin | sandbox::kRightFsRead |
          sandbox::kRightFsWrite | sandbox::kRightVault,
      {"*"}, now, 7200);
  std::string bearer = d.authority().bearer(tok);

  const int kIterations = 1000;
  std::vector<double> latencies_us;
  latencies_us.reserve(kIterations);

  const auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < kIterations; ++i) {
    service::RpcError err;
    const auto t0 = std::chrono::steady_clock::now();
    const auto r = d.mcp_tool_call(
        json::parse(R"({"name":"sys_power","arguments":{"mode":"powersave","confirm":true,"token":")" + bearer + R"("}})") .value, err);
    const auto t1 = std::chrono::steady_clock::now();
    latencies_us.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
    CHECK(!r["isError"].as_bool());
  }
  const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();

  // Sort to find p95 latency
  std::sort(latencies_us.begin(), latencies_us.end());
  const double p95_us = latencies_us[size_t(double(kIterations) * 0.95)];

  // Clean-room & performance verification: 1,000 calls must run in under 200 ms total; p95 latency < 500 µs (0.5 ms)
  CHECK_MSG(elapsed < 200.0, "1000 lời gọi phải hoàn tất < 200 ms");
  CHECK_MSG(p95_us < 500.0, "p95 latency phải < 500 µs");
}

YUI_TEST(daemon, audit_log_readable_vietnamese_and_no_secrets) {
  service::Options opts;
  opts.state_dir = "/tmp/yui-p1-9-audit-" + std::to_string(std::time(nullptr));
  opts.vault_password = "super-secret-vault-master-password-12345";
  service::Daemon d(opts);

  const int64_t now = std::time(nullptr);
  sandbox::Token tok = d.authority().issue(
      "agent:assistant",
      sandbox::kRightPower | sandbox::kRightAdmin,
      {"*"}, now, 3600);
  std::string bearer = d.authority().bearer(tok);

  service::RpcError err;
  d.mcp_tool_call(json::parse(R"({"name":"sys_power","arguments":{"mode":"performance","confirm":false,"token":")" + bearer + R"("}})") .value, err);
  d.mcp_tool_call(json::parse(R"({"name":"sys_power","arguments":{"mode":"performance","confirm":true,"token":")" + bearer + R"("}})") .value, err);
  d.mcp_tool_call(json::parse(R"({"name":"sys_scheduler","arguments":{"profile":"tiet-kiem-pin","confirm":true,"token":")" + bearer + R"("}})") .value, err);
  d.mcp_tool_call(json::parse(R"({"name":"sys_proc","arguments":{"action":"list","token":")" + bearer + R"("}})") .value, err);

  std::ifstream f(opts.state_dir + "/audit.log");
  CHECK(f.is_open());
  std::vector<std::string> lines;
  std::string line;
  while (std::getline(f, line)) {
    if (!line.empty()) lines.push_back(line);
  }
  f.close();

  // DoD P1.9: audit.log có ≥ 3 mục thật
  CHECK(lines.size() >= 3);
  // Mỗi mục có lý do tiếng Việt và không chứa bí mật
  for (const auto& l : lines) {
    CHECK(l.find("reason=\"") != std::string::npos);
    bool has_vietnamese = (l.find("xác nhận") != std::string::npos ||
                           l.find("thành công") != std::string::npos ||
                           l.find("liệt kê") != std::string::npos ||
                           l.find("cấu hình") != std::string::npos);
    CHECK(has_vietnamese);
    CHECK(l.find("super-secret-vault-master-password-12345") == std::string::npos);
    CHECK(l.find(bearer) == std::string::npos);
    CHECK(l.find(opts.cap_master_key) == std::string::npos);
  }
}

YUI_TEST(daemon, audit_log_rotation_and_clear_and_domain_hash) {
  service::Options opts;
  opts.state_dir = "/tmp/yui-p7-5-audit-" + std::to_string(std::time(nullptr));
  opts.audit_max_bytes = 200;
  opts.audit_hash_domains = true;
  service::Daemon d(opts);

  d.audit_log("sandbox:browser", "net.connect", "https://analytics.evil-tracker.example.com/telemetry", true, "kết nối được cho phép");
  d.audit_log("sandbox:crawler", "dns.resolve", "api.internal.service.example.org", true, "phân giải DNS thành công");

  std::ifstream f1(opts.state_dir + "/audit.log");
  CHECK(f1.is_open());
  std::string c1((std::istreambuf_iterator<char>(f1)), std::istreambuf_iterator<char>());
  f1.close();

  CHECK(c1.find("analytics.evil-tracker.example.com") == std::string::npos);
  CHECK(c1.find("api.internal.service.example.org") == std::string::npos);
  CHECK(c1.find("target=sha256:") != std::string::npos);

  d.audit_log("app:worker", "fs.read", "/var/data/job1.txt", true, "đọc dữ liệu công việc");
  d.audit_log("app:worker", "fs.write", "/var/data/job2.txt", true, "ghi kết quả công việc");

  struct stat st_rot;
  CHECK(::stat((opts.state_dir + "/audit.log.1").c_str(), &st_rot) == 0);

  service::RpcError err;
  const auto clear_res = d.handle("audit.clear", json::Value(json::Object{}), err);
  CHECK(!err.code);
  CHECK(clear_res["cleared"].as_bool());

  struct stat st_main;
  CHECK(::stat((opts.state_dir + "/audit.log").c_str(), &st_main) == 0);
  CHECK(st_main.st_size == 0);
  CHECK(::stat((opts.state_dir + "/audit.log.1").c_str(), &st_rot) != 0);
  CHECK(d.enforcer().audit().empty());

  ui::Layout layout;
  shell::Shell sh(d, layout);
  d.audit_log("test", "test", "target", true, "thử nghiệm");
  CHECK(!d.enforcer().audit().empty());
  std::string sh_out = sh.run("nhậtký xoá");
  CHECK(sh_out.find("Đã xoá sạch") != std::string::npos);
  CHECK(d.enforcer().audit().empty());
}

YUI_TEST_MAIN("test_daemon")
