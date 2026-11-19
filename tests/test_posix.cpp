// Chế độ POSIX của yui-shell (P4.12).
//
// DoD: 20 kịch bản — ống dẫn, `>`, `>>`, `<`, `&`, `jobs`, `kill`, biến môi
// trường, trích dẫn; dòng không nhận ra không tự chạy; mọi lệnh đi qua cùng
// Daemon (shell.posix_check + audit); bị từ chối ⇒ không tiến trình nào.
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "testutil.hpp"
#include "yui/json.hpp"
#include "yui/posix.hpp"
#include "yui/service.hpp"
#include "yui/shell.hpp"
#include "yui/ui.hpp"

using namespace yui;

namespace {

std::string mkdtemp_dir(const std::string& tag) {
  std::string tmpl = "/tmp/yuitest-" + tag + "-XXXXXX";
  std::vector<char> buf(tmpl.begin(), tmpl.end());
  buf.push_back('\0');
  const char* dir = ::mkdtemp(buf.data());
  return dir ? std::string(dir) : std::string();
}

bool exists(const std::string& p) {
  struct stat st{};
  return ::lstat(p.c_str(), &st) == 0;
}

std::string slurp(const std::string& p) {
  std::ifstream f(p, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

void spit(const std::string& p, const std::string& s) {
  std::ofstream f(p, std::ios::binary);
  f << s;
}

std::string helper_path() { return std::string(YUI_SOURCE_DIR) + "/build/bin/yui-sandbox-exec"; }

bool landlock_available() {
  const std::string cmd = helper_path() + " --probe >/dev/null 2>&1";
  return std::system(cmd.c_str()) == 0;
}

// Môi trường thử: thư mục tạm là vùng đọc-ghi duy nhất; `secret/` và
// state_dir bị cấm; stdout/stderr của lệnh con ghi vào tệp để đọc lại.
struct Env {
  std::string root, work, secret, state, out, err;
  service::Options so;
  std::unique_ptr<service::Daemon> daemon;
  std::unique_ptr<posix::Runner> runner;
  int out_fd = -1, err_fd = -1;
  bool sandboxed = false;

  Env() {
    root = mkdtemp_dir("posix");
    work = root + "/work";
    secret = root + "/secret";
    state = root + "/state";
    ::mkdir(work.c_str(), 0700);
    ::mkdir(secret.c_str(), 0700);
    ::mkdir(state.c_str(), 0700);
    spit(secret + "/key", "bí-mật\n");
    out = root + "/stdout.txt";
    err = root + "/stderr.txt";
    so.state_dir = state;
    so.posix_rw_dirs = {work};
    so.posix_ro_dirs = {"/"};
    so.posix_deny = {secret, state};
    daemon = std::make_unique<service::Daemon>(so);
    sandboxed = landlock_available();
    posix::Options po;
    po.sandbox_exec = helper_path();
    po.allow_unsandboxed = !sandboxed;  // nhân CI không có Landlock: vẫn thử lớp yuid
    out_fd = ::open(out.c_str(), O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0600);
    err_fd = ::open(err.c_str(), O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0600);
    po.stdin_fd = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
    po.stdout_fd = out_fd;
    po.stderr_fd = err_fd;
    runner = std::make_unique<posix::Runner>(*daemon, po);
    runner->run("cd " + work);
  }
  ~Env() {
    runner.reset();
    daemon.reset();
    if (out_fd >= 0) ::close(out_fd);
    if (err_fd >= 0) ::close(err_fd);
    const std::string cmd = "rm -rf '" + root + "'";
    if (std::system(cmd.c_str()) != 0) std::fprintf(stderr, "không xoá được %s\n", root.c_str());
  }
  // Chạy một dòng; trả về stdout mới của lệnh con (đã cắt bỏ phần cũ).
  std::string run(const std::string& line, std::string* msg = nullptr) {
    ::ftruncate(out_fd, 0);
    ::lseek(out_fd, 0, SEEK_SET);
    ::ftruncate(err_fd, 0);
    ::lseek(err_fd, 0, SEEK_SET);
    const std::string m = runner->run(line);
    if (msg) *msg = m;
    return slurp(out);
  }
  std::string stderr_text() const { return slurp(err); }
  std::string audit() const { return slurp(state + "/audit.log"); }
};

}  // namespace

// ------------------------------------------------------------- bộ phân tích --

YUI_TEST(posix, parse_pipeline_redirects_background) {
  posix::Vars v{{"HOME", "/home/u"}};
  const auto r = posix::parse("a x | b -y > o.txt 2>&1 &", v, 0, "/w");
  CHECK(r.ok);
  CHECK_EQ(r.pipeline.cmds.size(), size_t(2));
  CHECK(r.pipeline.background);
  CHECK_EQ(std::string(r.pipeline.cmds[0].argv[0]), std::string("a"));
  CHECK_EQ(std::string(r.pipeline.cmds[1].argv[1]), std::string("-y"));
  CHECK_EQ(r.pipeline.cmds[1].redirs.size(), size_t(2));
  CHECK_EQ(std::string(r.pipeline.cmds[1].redirs[0].path), std::string("/w/o.txt"));
  CHECK(r.pipeline.cmds[1].redirs[1].kind == posix::Redirect::DupOut);
  CHECK_EQ(r.pipeline.cmds[1].redirs[1].fd, 2);
  CHECK_EQ(r.pipeline.cmds[1].redirs[1].dup_to, 1);
}

YUI_TEST(posix, parse_quotes_escapes_vars_no_word_split) {
  posix::Vars v{{"X", "a b  *"}, {"HOME", "/home/u"}};
  const auto r = posix::parse("echo \"$X\" '$X' $X ${X}! \\$X a\\ b ~/f", v, 7, "/");
  CHECK(r.ok);
  const auto& a = r.pipeline.cmds[0].argv;
  CHECK_EQ(a.size(), size_t(8));
  CHECK_EQ(std::string(a[1]), std::string("a b  *"));
  CHECK_EQ(std::string(a[2]), std::string("$X"));
  CHECK_EQ(std::string(a[3]), std::string("a b  *"));  // không tách từ, không glob
  CHECK_EQ(std::string(a[4]), std::string("a b  *!"));
  CHECK_EQ(std::string(a[5]), std::string("$X"));
  CHECK_EQ(std::string(a[6]), std::string("a b"));
  CHECK_EQ(std::string(a[7]), std::string("/home/u/f"));
  const auto q = posix::parse("echo $?", v, 7, "/");
  CHECK(q.ok);
  CHECK_EQ(std::string(q.pipeline.cmds[0].argv[1]), std::string("7"));
}

YUI_TEST(posix, parse_assignments_before_command) {
  posix::Vars v{{"HOME", "/home/u"}};
  const auto r = posix::parse("A=1 B='x y' C=~/d env", v, 0, "/");
  CHECK(r.ok);
  const auto& c = r.pipeline.cmds[0];
  CHECK_EQ(c.assigns.size(), size_t(3));
  CHECK_EQ(std::string(c.assigns[1].second), std::string("x y"));
  CHECK_EQ(std::string(c.assigns[2].first), std::string("C"));
  CHECK_EQ(std::string(c.assigns[2].second), std::string("/home/u/d"));
  CHECK_EQ(std::string(c.argv[0]), std::string("env"));
}

YUI_TEST(posix, parse_rejects_unsupported_syntax_clearly) {
  posix::Vars v;
  for (const char* bad : {"a ; b", "a && b", "a || b", "echo $(id)", "echo `id`", "cat << EOF", "(a)",
                          "a |", "| a", "a >", "echo 'mở", "a & b"}) {
    const auto r = posix::parse(bad, v, 0, "/");
    CHECK_MSG(!r.ok, bad);
    CHECK_MSG(!r.error_vi.empty(), bad);
  }
}

YUI_TEST(posix, find_program_like_execvp) {
  CHECK(!posix::find_program("sh", "/usr/bin:/bin", "/").empty());
  CHECK(posix::find_program("không-có-lệnh-này", "/usr/bin:/bin", "/").empty());
  CHECK_EQ(posix::find_program("/bin/sh", "", "/"), std::string("/bin/sh"));
}

// ---------------------------------------------------- 20+ kịch bản chạy thật --

YUI_TEST(posix, s01_simple_command) {
  Env e;
  CHECK_EQ(e.run("echo xin chào"), std::string("xin chào\n"));
  CHECK_EQ(e.runner->last_status(), 0);
}

YUI_TEST(posix, s02_pipe_two_and_three_stages) {
  Env e;
  CHECK_EQ(e.run("echo abc | tr a-z A-Z"), std::string("ABC\n"));
  CHECK_EQ(e.run("printf 'b\\na\\nc\\n' | sort | head -n 2"), std::string("a\nb\n"));
}

YUI_TEST(posix, s03_redirect_out_truncates) {
  Env e;
  e.run("echo một > f.txt");
  e.run("echo hai > f.txt");
  CHECK_EQ(slurp(e.work + "/f.txt"), std::string("hai\n"));
}

YUI_TEST(posix, s04_redirect_append) {
  Env e;
  e.run("echo một > f.txt");
  e.run("echo hai >> f.txt");
  CHECK_EQ(slurp(e.work + "/f.txt"), std::string("một\nhai\n"));
}

YUI_TEST(posix, s05_redirect_in) {
  Env e;
  spit(e.work + "/in.txt", "z\ny\nx\n");
  CHECK_EQ(e.run("sort < in.txt"), std::string("x\ny\nz\n"));
  CHECK_EQ(e.run("wc -l < in.txt | tr -d ' '"), std::string("3\n"));
}

YUI_TEST(posix, s06_stderr_redirects) {
  Env e;
  e.run("sh -c 'echo lỗi >&2' 2> e.txt");
  CHECK_EQ(slurp(e.work + "/e.txt"), std::string("lỗi\n"));
  CHECK_EQ(e.run("sh -c 'echo ra; echo lỗi >&2' 2>&1 | sort"), std::string("lỗi\nra\n"));
  e.run("sh -c 'echo x >&2' 2>> e.txt");
  CHECK_EQ(slurp(e.work + "/e.txt"), std::string("lỗi\nx\n"));
  e.run("echo tới-stderr >&2");
  CHECK_EQ(e.stderr_text(), std::string("tới-stderr\n"));
}

YUI_TEST(posix, s07_exit_status_variable) {
  Env e;
  e.run("false");
  CHECK_EQ(e.runner->last_status(), 1);
  CHECK_EQ(e.run("echo $?"), std::string("1\n"));
  e.run("sh -c 'exit 42'");
  CHECK_EQ(e.run("echo $?"), std::string("42\n"));
  e.run("true | false");  // trạng thái của lệnh cuối như sh
  CHECK_EQ(e.runner->last_status(), 1);
}

YUI_TEST(posix, s08_export_unset_and_env) {
  Env e;
  e.run("export TEN=Yui");
  CHECK_EQ(e.run("sh -c 'echo $TEN'"), std::string("Yui\n"));
  CHECK_EQ(e.run("echo ${TEN}Vie"), std::string("YuiVie\n"));
  e.run("unset TEN");
  CHECK_EQ(e.run("sh -c 'echo \"[$TEN]\"'"), std::string("[]\n"));
}

YUI_TEST(posix, s09_assignment_prefix_only_for_that_command) {
  Env e;
  CHECK_EQ(e.run("X=5 sh -c 'echo $X'"), std::string("5\n"));
  CHECK_EQ(e.run("sh -c 'echo \"[$X]\"'"), std::string("[]\n"));
  e.run("Y=7");  // gán trần = biến shell
  CHECK_EQ(e.run("echo $Y"), std::string("7\n"));
}

YUI_TEST(posix, s10_quoting_reaches_program_intact) {
  Env e;
  CHECK_EQ(e.run("printf '%s|' \"a  b\" 'c d' e\\ f"), std::string("a  b|c d|e f|"));
  CHECK_EQ(e.run("echo '*' \"|\" '>'"), std::string("* | >\n"));
  CHECK(!exists(e.work + "/>"));
}

YUI_TEST(posix, s11_glob_and_no_match) {
  Env e;
  spit(e.work + "/a.txt", "");
  spit(e.work + "/b.txt", "");
  spit(e.work + "/c.md", "");
  CHECK_EQ(e.run("echo *.txt"), std::string("a.txt b.txt\n"));
  CHECK_EQ(e.run("echo ?.md"), std::string("c.md\n"));
  CHECK_EQ(e.run("echo *.không"), std::string("*.không\n"));  // không khớp: giữ nguyên như sh
}

YUI_TEST(posix, s12_cd_changes_cwd_for_children) {
  Env e;
  ::mkdir((e.work + "/con").c_str(), 0700);
  e.run("cd con");
  CHECK_EQ(e.runner->cwd(), e.work + "/con");
  CHECK_EQ(e.run("pwd"), e.work + "/con\n");
  std::string msg;
  e.run("cd /không/có", &msg);
  CHECK(!msg.empty());
  CHECK_EQ(e.runner->cwd(), e.work + "/con");
}

YUI_TEST(posix, s13_background_jobs_and_wait) {
  Env e;
  std::string msg;
  e.run("sh -c 'echo nền > bg.txt' &", &msg);
  CHECK(msg.find("[1]") != std::string::npos);
  CHECK_EQ(e.runner->jobs().size(), size_t(1));
  e.runner->wait_all();
  CHECK_EQ(slurp(e.work + "/bg.txt"), std::string("nền\n"));
}

YUI_TEST(posix, s14_jobs_lists_running) {
  Env e;
  e.run("sleep 30 &");
  std::string msg;
  e.run("jobs", &msg);
  CHECK(msg.find("[1]") != std::string::npos);
  CHECK(msg.find("sleep 30") != std::string::npos);
  CHECK(e.runner->jobs()[0].running());
  e.run("kill %1");
}

YUI_TEST(posix, s15_kill_job_by_spec_and_signal) {
  Env e;
  e.run("sleep 30 &");
  e.run("sleep 30 &");
  std::string msg;
  e.run("kill %1", &msg);
  e.run("wait %1", &msg);  // wait báo việc đã xong rồi bỏ khỏi danh sách (như sh)
  CHECK(msg.find("[1] Bị SIGTERM") != std::string::npos);
  CHECK_EQ(e.runner->last_status(), 128 + SIGTERM);
  CHECK_EQ(e.runner->jobs().size(), size_t(1));
  CHECK_EQ(e.runner->jobs()[0].id, 2);
  CHECK(e.runner->jobs()[0].running());
  e.run("kill -KILL %2", &msg);
  e.run("wait", &msg);
  CHECK(msg.find("[2] Bị SIGKILL") != std::string::npos);
  CHECK(e.runner->jobs().empty());
  CHECK(e.audit().find("shell.kill") != std::string::npos);
}

YUI_TEST(posix, s16_kill_refuses_foreign_processes) {
  Env e;
  std::string msg;
  e.run("kill 1", &msg);
  CHECK(!msg.empty());
  CHECK(e.runner->last_status() != 0);
  e.run("kill %9", &msg);
  CHECK(e.runner->last_status() != 0);
}

YUI_TEST(posix, s17_unsupported_syntax_runs_nothing) {
  Env e;
  std::string msg;
  CHECK_EQ(e.run("touch a.txt ; touch b.txt", &msg), std::string(""));
  CHECK(!msg.empty());
  e.run("touch a.txt && touch b.txt", &msg);
  e.run("echo $(touch c.txt)", &msg);
  CHECK(!exists(e.work + "/a.txt"));
  CHECK(!exists(e.work + "/b.txt"));
  CHECK(!exists(e.work + "/c.txt"));
}

YUI_TEST(posix, s18_policy_denial_spawns_nothing_in_pipeline) {
  Env e;
  std::string msg;
  // Lệnh đầu hợp lệ nhưng lệnh sau ghi vào /etc ⇒ cả ống dẫn không chạy.
  e.run("touch tạo-ra.txt | cat > /etc/yui-không-được", &msg);
  CHECK(msg.find("không tiến trình nào") != std::string::npos);
  CHECK(!exists(e.work + "/tạo-ra.txt"));
  CHECK(!exists("/etc/yui-không-được"));
  CHECK(e.runner->last_status() != 0);
  // Đọc tệp cấm (tham số hoặc `<`) và ghi vào state_dir của yuid đều bị chặn.
  e.run("cat " + e.secret + "/key", &msg);
  CHECK(msg.find("Từ chối") != std::string::npos);
  e.run("cat < " + e.secret + "/key", &msg);
  CHECK(msg.find("Từ chối") != std::string::npos);
  e.run("echo x > " + e.state + "/hack", &msg);
  CHECK(!exists(e.state + "/hack"));
  // Đường vòng qua liên kết mềm vẫn bị so với đích thật.
  CHECK_EQ(::symlink(e.secret.c_str(), (e.work + "/lối-tắt").c_str()), 0);
  e.run("cat lối-tắt/key", &msg);
  CHECK(msg.find("Từ chối") != std::string::npos);
  e.run("echo x > lối-tắt/mới", &msg);
  CHECK(!exists(e.secret + "/mới"));
}

YUI_TEST(posix, s19_every_command_audited_by_daemon) {
  Env e;
  e.run("echo a | cat");
  e.run("echo x > /etc/yui-không-được");
  const std::string log = e.audit();
  size_t n = 0;
  for (size_t at = log.find("shell.exec"); at != std::string::npos; at = log.find("shell.exec", at + 1)) ++n;
  CHECK(n >= 3);  // echo, cat, echo bị chặn
  CHECK(log.find("/etc/yui-không-được") != std::string::npos);
  // Cùng Daemon trả lời trực tiếp qua RPC (như GUI/MCP gọi).
  service::RpcError err;
  json::Value q(json::Object{});
  q.set("program", "/bin/sh");
  q.set("argc", 1LL);
  q.set("cwd", e.work);
  q.set("background", false);
  json::Value w(json::Array{});
  w.push("/usr/lib/x");
  q.set("reads", json::Value(json::Array{}));
  q.set("writes", w);
  const auto r = e.daemon->handle("shell.posix_check", q, err);
  CHECK_EQ(err.code, 0);
  CHECK(!r["ok"].as_bool());
  q.set("writes", json::Value(json::Array{}));
  const auto ok = e.daemon->handle("shell.posix_check", q, err);
  CHECK(ok["ok"].as_bool());
  CHECK_EQ(ok["rw"].arr().size(), size_t(1));
  CHECK_EQ(std::string(ok["rw"].arr()[0].as_string()), e.work);
  json::Value rel(json::Object{});
  rel.set("program", "sh");  // phải là đường tuyệt đối đã phân giải
  e.daemon->handle("shell.posix_check", rel, err);
  CHECK(err.code != 0);
}

YUI_TEST(posix, s20_kernel_sandbox_blocks_program_writes) {
  Env e;
  if (!e.sandboxed) {
    std::printf("  (bỏ qua: nhân không có Landlock)\n");
    return;
  }
  // Chương trình tự mở tệp (không qua chuyển hướng) — chỉ Landlock chặn được.
  std::string msg;
  e.run("sh -c 'echo x > " + e.root + "/ngoài.txt'", &msg);
  CHECK(!exists(e.root + "/ngoài.txt"));
  CHECK(e.runner->last_status() != 0);
  CHECK(e.stderr_text().find("Permission denied") != std::string::npos);
  e.run("sh -c 'echo x > trong.txt'");
  CHECK_EQ(slurp(e.work + "/trong.txt"), std::string("x\n"));
}

YUI_TEST(posix, s21_no_privilege_escalation) {
  Env e;
  if (!e.sandboxed) return;
  // no_new_privs: setuid không có tác dụng (sudo/su không lên root được).
  CHECK_EQ(e.run("sh -c 'grep NoNewPrivs /proc/self/status | tr -d \"\\t \"'"), std::string("NoNewPrivs:1\n"));
}

YUI_TEST(posix, s22_unknown_program_and_secret_env_stripped) {
  Env e;
  std::string msg;
  e.run("không-có-lệnh-này", &msg);
  CHECK(msg.find("không tìm thấy") != std::string::npos);
  CHECK_EQ(e.runner->last_status(), 127);
  ::setenv("YUI_VAULT_KEY", "lộ", 1);
  Env e2;  // biến được chụp lúc tạo Runner
  CHECK_EQ(e2.run("sh -c 'echo \"[$YUI_VAULT_KEY]\"'"), std::string("[]\n"));
  ::unsetenv("YUI_VAULT_KEY");
}

// --------------------------------------------------- tích hợp vào yui-shell --

YUI_TEST(posix, shell_unknown_line_never_executes) {
  Env e;
  shell::Shell sh(*e.daemon, ui::Layout{});
  const std::string marker = e.work + "/không-được-tạo";
  const std::string out = sh.run("touch " + marker);
  CHECK(!exists(marker));
  CHECK(out.find("Không hiểu lệnh") != std::string::npos);
  CHECK(out.find("!touch") != std::string::npos);  // chỉ gợi ý cách chạy rõ ràng
}

YUI_TEST(posix, shell_bang_and_chay_forward_to_runner) {
  Env e;
  shell::Shell sh(*e.daemon, ui::Layout{});
  posix::Options po;
  po.sandbox_exec = helper_path();
  po.allow_unsandboxed = !e.sandboxed;
  po.stdout_fd = e.out_fd;
  po.stderr_fd = e.err_fd;
  sh.set_posix_options(po);
  sh.run("!cd " + e.work);
  sh.run("!echo một > s.txt");
  sh.run("chạy echo hai >> s.txt");
  sh.run("run echo ba >> s.txt");
  CHECK_EQ(slurp(e.work + "/s.txt"), std::string("một\nhai\nba\n"));
  CHECK(sh.run("!").find("Cách dùng") != std::string::npos);
  CHECK(sh.run("chạy").find("Cách dùng") != std::string::npos);
  CHECK(sh.help_text().find("chạy") != std::string::npos);
}

YUI_TEST_MAIN("test_posix")
