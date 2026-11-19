// Chế độ POSIX của yui-shell (P4.12). Xem include/yui/posix.hpp.
#include "yui/posix.hpp"

#include <fcntl.h>
#include <glob.h>
#include <limits.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <sstream>

#include "yui/json.hpp"
#include "yui/service.hpp"

extern char** environ;

namespace yui::posix {
namespace {

bool is_name_start(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_'; }
bool is_name_char(char c) { return is_name_start(c) || (c >= '0' && c <= '9'); }

bool valid_name(const std::string& n) {
  if (n.empty() || !is_name_start(n[0])) return false;
  for (char c : n)
    if (!is_name_char(c)) return false;
  return true;
}

// Chuẩn hoá từ vựng: bỏ "." và xử lý ".." (không đi theo liên kết).
std::string lexical_normalize(const std::string& abs) {
  std::vector<std::string> parts;
  std::string cur;
  std::istringstream in(abs);
  while (std::getline(in, cur, '/')) {
    if (cur.empty() || cur == ".") continue;
    if (cur == "..") {
      if (!parts.empty()) parts.pop_back();
      continue;
    }
    parts.push_back(cur);
  }
  std::string out;
  for (const auto& p : parts) out += "/" + p;
  return out.empty() ? "/" : out;
}

// Đường dẫn tuyệt đối để KIỂM TRA chính sách: thư mục cha được realpath (đi
// theo liên kết) nên `> ~/lối-tắt-tới-etc/x` bị so với /etc/x thật.
std::string absolute_for_check(const std::string& p, const std::string& cwd) {
  std::string a = p.empty() || p[0] != '/' ? cwd + "/" + p : p;
  a = lexical_normalize(a);
  if (a == "/") return a;
  const size_t k = a.find_last_of('/');
  const std::string parent = k == 0 ? "/" : a.substr(0, k);
  char buf[PATH_MAX];
  if (::realpath(parent.c_str(), buf)) {
    const std::string rp = buf;
    return (rp == "/" ? "" : rp) + a.substr(k);
  }
  return a;
}

// Tham số trông như đường dẫn (có '/', hoặc tồn tại trong cwd; cả phần sau
// '=' của `--file=…`) → khai báo là "đọc" để yuid so với danh sách cấm.
// Đây là lớp kiểm tra chính sách; chương trình vẫn có thể tự mở đường khác —
// ranh giới nhân là Landlock trong yui-sandbox-exec.
std::vector<std::string> path_like_args(const std::vector<std::string>& argv, const std::string& cwd) {
  std::vector<std::string> out;
  for (size_t i = 1; i < argv.size(); ++i) {
    std::string a = argv[i];
    const size_t eq = a.find('=');
    if (a.size() > 1 && a[0] == '-' && eq != std::string::npos) a = a.substr(eq + 1);
    if (a.empty() || (a[0] == '-' && a.find('/') == std::string::npos)) continue;
    struct stat st;
    const std::string abs = a[0] == '/' ? a : cwd + "/" + a;
    const bool exists = ::lstat(abs.c_str(), &st) == 0;
    if (a.find('/') == std::string::npos && !exists) continue;
    char buf[PATH_MAX];
    out.push_back(exists && ::realpath(abs.c_str(), buf) ? std::string(buf) : absolute_for_check(a, cwd));
  }
  return out;
}

struct Tok {
  enum Type { Word, Pipe, Amp, Redir } type = Word;
  std::string text;      // từ đã mở trích dẫn + mở biến
  std::string pattern;   // dạng glob (ký tự trong trích dẫn được thoát)
  bool glob = false;     // có ký tự glob không trích dẫn
  size_t first_quoted = std::string::npos;  // vị trí ký tự trích dẫn đầu tiên trong text
  Redirect redir;
};

bool lex(const std::string& line, const Vars& vars, int last_status, std::vector<Tok>& out, std::string& err) {
  auto var = [&](const std::string& n) -> std::string {
    auto it = vars.find(n);
    return it == vars.end() ? std::string() : it->second;
  };
  Tok cur;
  bool in_word = false;
  auto push_char = [&](char c, bool quoted) {
    if (quoted && cur.first_quoted == std::string::npos) cur.first_quoted = cur.text.size();
    cur.text += c;
    const bool special = c == '*' || c == '?' || c == '[' || c == ']' || c == '\\';
    if (special && quoted) cur.pattern += '\\';
    cur.pattern += c;
    if (!quoted && (c == '*' || c == '?' || c == '[')) cur.glob = true;
    in_word = true;
  };
  // Giá trị biến/`~`: coi như trích dẫn — không glob, không tách từ.
  auto push_str = [&](const std::string& s, bool /*quoted*/) {
    for (char c : s) push_char(c, true);
    in_word = true;
  };
  auto finish = [&]() {
    if (in_word) out.push_back(cur);
    cur = Tok{};
    in_word = false;
  };
  // Mở `$…` tại vị trí i (line[i] == '$'); trả false nếu lỗi.
  auto dollar = [&](size_t& i, bool quoted) -> bool {
    if (i + 1 >= line.size()) {
      push_char('$', quoted);
      return true;
    }
    const char n = line[i + 1];
    if (n == '(') {
      err = "không hỗ trợ thay thế lệnh $(…)";
      return false;
    }
    if (n == '?') {
      push_str(std::to_string(last_status), quoted);
      i += 1;
      return true;
    }
    if (n == '{') {
      const size_t close = line.find('}', i + 2);
      if (close == std::string::npos) {
        err = "thiếu '}' sau ${";
        return false;
      }
      const std::string name = line.substr(i + 2, close - i - 2);
      if (name == "?") push_str(std::to_string(last_status), quoted);
      else if (!valid_name(name)) {
        err = "tên biến không hợp lệ: ${" + name + "}";
        return false;
      } else push_str(var(name), quoted);
      i = close;
      return true;
    }
    if (is_name_start(n)) {
      size_t j = i + 1;
      while (j < line.size() && is_name_char(line[j])) ++j;
      push_str(var(line.substr(i + 1, j - i - 1)), quoted);
      i = j - 1;
      return true;
    }
    push_char('$', quoted);
    return true;
  };

  for (size_t i = 0; i < line.size(); ++i) {
    const char c = line[i];
    if (c == ' ' || c == '\t' || c == '\n') {
      finish();
      continue;
    }
    if (c == '#' && !in_word) break;  // chú thích
    if (c == '\'') {
      const size_t close = line.find('\'', i + 1);
      if (close == std::string::npos) {
        err = "thiếu dấu ' đóng";
        return false;
      }
      if (close == i + 1 && cur.first_quoted == std::string::npos) cur.first_quoted = cur.text.size();
      for (size_t k = i + 1; k < close; ++k) push_char(line[k], true);
      in_word = true;
      i = close;
      continue;
    }
    if (c == '"') {
      size_t k = i + 1;
      if (cur.first_quoted == std::string::npos) cur.first_quoted = cur.text.size();
      in_word = true;
      bool closed = false;
      for (; k < line.size(); ++k) {
        const char d = line[k];
        if (d == '"') {
          closed = true;
          break;
        }
        if (d == '\\' && k + 1 < line.size() &&
            (line[k + 1] == '$' || line[k + 1] == '`' || line[k + 1] == '"' || line[k + 1] == '\\')) {
          push_char(line[++k], true);
          continue;
        }
        if (d == '`') {
          err = "không hỗ trợ thay thế lệnh `…`";
          return false;
        }
        if (d == '$') {
          if (!dollar(k, true)) return false;
          continue;
        }
        push_char(d, true);
      }
      if (!closed) {
        err = "thiếu dấu \" đóng";
        return false;
      }
      i = k;
      continue;
    }
    if (c == '\\') {
      if (i + 1 < line.size()) push_char(line[++i], true);
      continue;
    }
    if (c == '`') {
      err = "không hỗ trợ thay thế lệnh `…`";
      return false;
    }
    if (c == '$') {
      if (!dollar(i, false)) return false;
      continue;
    }
    // `~` đầu từ, hoặc ngay sau `TÊN=` của phép gán (như sh: X=~/a).
    const bool after_assign = in_word && cur.first_quoted == std::string::npos && !cur.text.empty() &&
                              cur.text.back() == '=' && valid_name(cur.text.substr(0, cur.text.size() - 1));
    if (c == '~' && (!in_word || after_assign) &&
        (i + 1 == line.size() || line[i + 1] == '/' || line[i + 1] == ' ' || line[i + 1] == '\t')) {
      push_str(var("HOME"), false);
      continue;
    }
    if (c == ';') {
      err = "không hỗ trợ ';' — mỗi dòng một lệnh/ống dẫn";
      return false;
    }
    if (c == '(' || c == ')') {
      err = "không hỗ trợ nhóm lệnh ( )";
      return false;
    }
    if (c == '|') {
      if (i + 1 < line.size() && line[i + 1] == '|') {
        err = "không hỗ trợ '||'";
        return false;
      }
      finish();
      Tok t;
      t.type = Tok::Pipe;
      out.push_back(t);
      continue;
    }
    if (c == '&') {
      if (i + 1 < line.size() && line[i + 1] == '&') {
        err = "không hỗ trợ '&&'";
        return false;
      }
      if (i + 1 < line.size() && line[i + 1] == '>') {
        err = "không hỗ trợ '&>' — dùng '> tệp 2>&1'";
        return false;
      }
      finish();
      Tok t;
      t.type = Tok::Amp;
      out.push_back(t);
      continue;
    }
    if (c == '<' || c == '>') {
      int fd = c == '<' ? 0 : 1;
      // "2>" : một chữ số đứng một mình ngay trước là fd.
      if (in_word && cur.first_quoted == std::string::npos && cur.text.size() == 1 && cur.text[0] >= '0' &&
          cur.text[0] <= '2') {
        fd = cur.text[0] - '0';
        cur = Tok{};
        in_word = false;
      } else {
        finish();
      }
      Tok t;
      t.type = Tok::Redir;
      t.redir.fd = fd;
      if (c == '<') {
        if (i + 1 < line.size() && (line[i + 1] == '<' || line[i + 1] == '(')) {
          err = "không hỗ trợ '<<' / '<(…)'";
          return false;
        }
        t.redir.kind = Redirect::In;
      } else if (i + 1 < line.size() && line[i + 1] == '>') {
        t.redir.kind = Redirect::Append;
        ++i;
      } else if (i + 1 < line.size() && line[i + 1] == '&') {
        if (i + 2 < line.size() && (line[i + 2] == '1' || line[i + 2] == '2')) {
          t.redir.kind = Redirect::DupOut;
          t.redir.dup_to = line[i + 2] - '0';
          i += 2;
        } else {
          err = "chỉ hỗ trợ >&1 / >&2";
          return false;
        }
      } else {
        t.redir.kind = Redirect::Out;
      }
      out.push_back(t);
      continue;
    }
    push_char(c, false);
  }
  finish();
  return true;
}

std::vector<std::string> expand_glob(const Tok& t, const std::string& cwd) {
  if (!t.glob) return {t.text};
  const bool rel = t.pattern.empty() || t.pattern[0] != '/';
  const std::string prefix = rel ? cwd + "/" : "";
  glob_t g{};
  std::vector<std::string> out;
  if (::glob((prefix + t.pattern).c_str(), 0, nullptr, &g) == 0) {
    for (size_t k = 0; k < g.gl_pathc; ++k) {
      std::string s = g.gl_pathv[k];
      if (rel && s.compare(0, prefix.size(), prefix) == 0) s = s.substr(prefix.size());
      out.push_back(s);
    }
  }
  ::globfree(&g);
  if (out.empty()) out.push_back(t.text);  // không khớp ⇒ giữ nguyên như sh
  return out;
}

bool executable_file(const std::string& p) {
  struct stat st;
  return ::stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode) && ::access(p.c_str(), X_OK) == 0;
}

const char* kBuiltins[] = {"cd", "export", "unset", "jobs", "kill", "wait"};

bool is_builtin(const std::string& n) {
  for (const char* b : kBuiltins)
    if (n == b) return true;
  return false;
}

std::string signal_name(int sig) {
  switch (sig) {
    case SIGHUP: return "SIGHUP";
    case SIGINT: return "SIGINT";
    case SIGKILL: return "SIGKILL";
    case SIGTERM: return "SIGTERM";
    case SIGSTOP: return "SIGSTOP";
    case SIGCONT: return "SIGCONT";
    case SIGPIPE: return "SIGPIPE";
    case SIGQUIT: return "SIGQUIT";
    case SIGUSR1: return "SIGUSR1";
    case SIGUSR2: return "SIGUSR2";
    case SIGTSTP: return "SIGTSTP";
    default: return "tín hiệu " + std::to_string(sig);
  }
}

int parse_signal(std::string s) {
  if (!s.empty() && s[0] == '-') s = s.substr(1);
  if (s.compare(0, 3, "SIG") == 0) s = s.substr(3);
  if (!s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; })) {
    const int n = std::atoi(s.c_str());
    return n > 0 && n < 65 ? n : -1;
  }
  static const std::pair<const char*, int> kNames[] = {
      {"HUP", SIGHUP},   {"INT", SIGINT},   {"QUIT", SIGQUIT}, {"KILL", SIGKILL}, {"TERM", SIGTERM},
      {"STOP", SIGSTOP}, {"CONT", SIGCONT}, {"USR1", SIGUSR1}, {"USR2", SIGUSR2}, {"TSTP", SIGTSTP}};
  for (const auto& kv : kNames)
    if (s == kv.first) return kv.second;
  return -1;
}

int decode_status(int st) {
  if (WIFEXITED(st)) return WEXITSTATUS(st);
  if (WIFSIGNALED(st)) return 128 + WTERMSIG(st);
  return -1;
}

constexpr int kStopped = -2;

}  // namespace

// ================================================================ parse ====
ParseResult parse(const std::string& line, const Vars& vars, int last_status, const std::string& cwd) {
  ParseResult r;
  r.pipeline.text = line;
  std::vector<Tok> toks;
  if (!lex(line, vars, last_status, toks, r.error_vi)) return r;
  if (toks.empty()) {
    r.error_vi = "dòng trống";
    return r;
  }
  if (toks.back().type == Tok::Amp) {
    r.pipeline.background = true;
    toks.pop_back();
  }
  Command cmd;
  bool assigning = true;
  auto flush = [&]() -> bool {
    if (cmd.argv.empty()) {
      r.error_vi = cmd.redirs.empty() && cmd.assigns.empty() ? "ống dẫn thiếu lệnh" : "thiếu tên lệnh";
      if (!cmd.assigns.empty() && cmd.redirs.empty() && r.pipeline.cmds.empty()) {
        r.pipeline.cmds.push_back(cmd);  // chỉ gán biến: X=1
        cmd = Command{};
        return true;
      }
      return false;
    }
    r.pipeline.cmds.push_back(cmd);
    cmd = Command{};
    assigning = true;
    return true;
  };
  for (size_t i = 0; i < toks.size(); ++i) {
    const Tok& t = toks[i];
    if (t.type == Tok::Amp) {
      r.error_vi = "'&' chỉ được đặt ở cuối dòng";
      return r;
    }
    if (t.type == Tok::Pipe) {
      if (!flush() || r.pipeline.cmds.back().argv.empty()) {
        if (r.error_vi.empty()) r.error_vi = "ống dẫn thiếu lệnh";
        return r;
      }
      if (i + 1 == toks.size()) {
        r.error_vi = "ống dẫn thiếu lệnh sau '|'";
        return r;
      }
      continue;
    }
    if (t.type == Tok::Redir) {
      Redirect rd = t.redir;
      if (rd.kind != Redirect::DupOut) {
        if (i + 1 >= toks.size() || toks[i + 1].type != Tok::Word) {
          r.error_vi = "chuyển hướng thiếu tên tệp";
          return r;
        }
        rd.path = absolute_for_check(toks[++i].text, cwd);
      }
      cmd.redirs.push_back(rd);
      continue;
    }
    // Từ thường.
    const size_t eq = t.text.find('=');
    if (assigning && cmd.argv.empty() && eq != std::string::npos && eq > 0 && eq < t.first_quoted &&
        valid_name(t.text.substr(0, eq))) {
      cmd.assigns.emplace_back(t.text.substr(0, eq), t.text.substr(eq + 1));
      continue;
    }
    assigning = false;
    for (auto& w : expand_glob(t, cwd)) cmd.argv.push_back(w);
  }
  if (!flush()) return r;
  if (r.pipeline.cmds.size() > 1)
    for (const auto& c : r.pipeline.cmds)
      if (c.argv.empty()) {
        r.error_vi = "ống dẫn thiếu lệnh";
        return r;
      }
  if (r.pipeline.background && r.pipeline.cmds.size() == 1 && r.pipeline.cmds[0].argv.empty()) {
    r.error_vi = "không có lệnh để chạy nền";
    return r;
  }
  r.error_vi.clear();
  r.ok = true;
  return r;
}

std::string find_program(const std::string& name, const std::string& path_var, const std::string& cwd) {
  if (name.empty()) return "";
  if (name.find('/') != std::string::npos) {
    const std::string p = name[0] == '/' ? name : lexical_normalize(cwd + "/" + name);
    return executable_file(p) ? p : "";
  }
  std::istringstream in(path_var);
  std::string dir;
  while (std::getline(in, dir, ':')) {
    if (dir.empty()) continue;  // bỏ mục rỗng (sh coi là ".", nguy hiểm)
    const std::string p = dir + "/" + name;
    if (executable_file(p)) return p;
  }
  return "";
}

bool Job::running() const {
  for (int s : status)
    if (s == -1 || s == kStopped) return true;
  return false;
}

// =============================================================== Runner ====
Runner::Runner(service::Daemon& daemon, Options opts) : daemon_(daemon), opts_(std::move(opts)) {
  char buf[PATH_MAX];
  cwd_ = ::getcwd(buf, sizeof buf) ? buf : "/";
  for (char** e = environ; e && *e; ++e) {
    const std::string kv = *e;
    const size_t eq = kv.find('=');
    if (eq == std::string::npos) continue;
    const std::string k = kv.substr(0, eq);
    // Bí mật của chính yui-shell/yuid không đi sang lệnh con.
    if (k.compare(0, 9, "YUI_VAULT") == 0 || k.compare(0, 7, "YUI_CAP") == 0) continue;
    vars_[k] = kv.substr(eq + 1);
  }
  if (vars_.find("PATH") == vars_.end()) vars_["PATH"] = "/usr/local/bin:/usr/bin:/bin";
  vars_["PWD"] = cwd_;
  if (const char* a = std::getenv("YUI_POSIX_ALLOW_UNSANDBOXED"))
    if (std::string(a) == "1") opts_.allow_unsandboxed = true;

  helper_ = opts_.sandbox_exec;
  if (helper_.empty())
    if (const char* h = std::getenv("YUI_SANDBOX_EXEC")) helper_ = h;
  if (helper_.empty()) {
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n > 0) {
      buf[n] = '\0';
      std::string self = buf;
      const std::string cand = self.substr(0, self.find_last_of('/')) + "/yui-sandbox-exec";
      if (executable_file(cand)) helper_ = cand;
    }
  }
  if (helper_.empty()) helper_ = find_program("yui-sandbox-exec", vars_["PATH"], cwd_);
}

Runner::~Runner() {
  bool any = false;
  for (const auto& j : jobs_)
    if (j.running()) {
      ::killpg(j.pgid, SIGHUP);
      ::killpg(j.pgid, SIGCONT);
      any = true;
    }
  if (!any) return;
  for (int round = 0; round < 40; ++round) {
    reap(false);
    bool left = false;
    for (const auto& j : jobs_) left = left || j.running();
    if (!left) return;
    ::usleep(5000);
  }
  for (const auto& j : jobs_)
    if (j.running()) ::killpg(j.pgid, SIGKILL);
  reap(true);
}

void Runner::reap(bool block) {
  for (auto& j : jobs_) {
    for (size_t k = 0; k < j.pids.size(); ++k) {
      if (j.status[k] >= 0) continue;
      int st = 0;
      pid_t r;
      do {
        r = ::waitpid(j.pids[k], &st, (block ? 0 : WNOHANG) | WUNTRACED | WCONTINUED);
      } while (r < 0 && errno == EINTR);
      if (r == j.pids[k]) {
        if (WIFSTOPPED(st)) j.status[k] = kStopped;
        else if (WIFCONTINUED(st)) j.status[k] = -1;
        else j.status[k] = decode_status(st);
      } else if (r < 0 && errno == ECHILD) {
        j.status[k] = 0;  // đã được ai đó thu dọn
      }
    }
  }
}

size_t Runner::wait_all() {
  size_t n = 0;
  for (auto& j : jobs_) {
    for (size_t k = 0; k < j.pids.size(); ++k) {
      while (j.status[k] < 0) {
        int st = 0;
        const pid_t r = ::waitpid(j.pids[k], &st, 0);
        if (r < 0 && errno == EINTR) continue;
        j.status[k] = r == j.pids[k] ? decode_status(st) : 0;
        if (j.status[k] < 0) j.status[k] = 0;
      }
    }
    ++n;
  }
  return n;
}

Job* Runner::find_job(const std::string& spec) {
  if (spec.size() > 1 && spec[0] == '%') {
    const int id = std::atoi(spec.c_str() + 1);
    for (auto& j : jobs_)
      if (j.id == id) return &j;
    return nullptr;
  }
  const pid_t pid = pid_t(std::atoi(spec.c_str()));
  if (pid <= 0) return nullptr;
  for (auto& j : jobs_)
    if (j.pgid == pid || std::find(j.pids.begin(), j.pids.end(), pid) != j.pids.end()) return &j;
  return nullptr;
}

std::string Runner::jobs_text(bool only_changes) {
  reap(false);
  std::ostringstream os;
  bool first = true;
  for (auto& j : jobs_) {
    const bool done = !j.running();
    bool stopped = false;
    for (int s : j.status) stopped = stopped || s == kStopped;
    if (only_changes && !done) continue;
    if (done && j.reported_done) continue;
    std::string state;
    if (!done) {
      state = stopped ? "Đã dừng" : "Đang chạy";
    } else {
      const int s = j.status.back();
      if (s == 0) state = "Xong";
      else if (s > 128) state = "Bị " + signal_name(s - 128);
      else state = "Thoát (mã " + std::to_string(s) + ")";
      j.reported_done = true;
    }
    if (!first) os << "\n";
    first = false;
    os << "[" << j.id << "] " << state << "  " << j.text;
  }
  jobs_.erase(std::remove_if(jobs_.begin(), jobs_.end(), [](const Job& j) { return j.reported_done; }),
              jobs_.end());
  return os.str();
}

std::string Runner::builtin(const Command& c, bool& handled) {
  handled = true;
  const std::string& n = c.argv[0];
  if (n == "cd") {
    std::string t = c.argv.size() > 1 ? c.argv[1] : vars_["HOME"];
    if (t == "-") t = vars_["OLDPWD"];
    if (t.empty()) {
      last_status_ = 1;
      return "cd: không có thư mục đích";
    }
    const std::string abs = t[0] == '/' ? t : cwd_ + "/" + t;
    char buf[PATH_MAX];
    struct stat st;
    if (!::realpath(abs.c_str(), buf) || ::stat(buf, &st) != 0 || !S_ISDIR(st.st_mode)) {
      last_status_ = 1;
      return "cd: không vào được " + t;
    }
    vars_["OLDPWD"] = cwd_;
    cwd_ = buf;
    vars_["PWD"] = cwd_;
    last_status_ = 0;
    return "";
  }
  if (n == "export") {
    if (c.argv.size() == 1) {
      std::ostringstream os;
      bool first = true;
      for (const auto& kv : vars_) {
        if (!first) os << "\n";
        first = false;
        os << kv.first << "=" << kv.second;
      }
      last_status_ = 0;
      return os.str();
    }
    for (size_t i = 1; i < c.argv.size(); ++i) {
      const std::string& a = c.argv[i];
      const size_t eq = a.find('=');
      const std::string name = a.substr(0, eq);
      if (!valid_name(name)) {
        last_status_ = 1;
        return "export: tên biến không hợp lệ: " + name;
      }
      if (eq != std::string::npos) vars_[name] = a.substr(eq + 1);
      else vars_.emplace(name, "");
    }
    last_status_ = 0;
    return "";
  }
  if (n == "unset") {
    for (size_t i = 1; i < c.argv.size(); ++i) vars_.erase(c.argv[i]);
    last_status_ = 0;
    return "";
  }
  if (n == "jobs") {
    last_status_ = 0;
    const std::string t = jobs_text(false);
    return t.empty() ? "(không có việc nền)" : t;
  }
  if (n == "kill") {
    int sig = SIGTERM;
    size_t i = 1;
    if (i < c.argv.size() && c.argv[i] == "-l") {
      last_status_ = 0;
      return "HUP INT QUIT KILL TERM STOP CONT USR1 USR2 TSTP";
    }
    if (i < c.argv.size() && c.argv[i].size() > 1 && c.argv[i][0] == '-') {
      sig = parse_signal(c.argv[i]);
      if (sig < 0) {
        last_status_ = 1;
        return "kill: tín hiệu lạ " + c.argv[i];
      }
      ++i;
    }
    if (i >= c.argv.size()) {
      last_status_ = 1;
      return "Cách dùng: kill [-TÍN_HIỆU] %số_việc|pid";
    }
    std::ostringstream os;
    last_status_ = 0;
    for (; i < c.argv.size(); ++i) {
      const std::string& spec = c.argv[i];
      reap(false);
      Job* j = find_job(spec);
      if (!j || !j->running()) {
        last_status_ = 1;
        os << (os.tellp() > 0 ? "\n" : "") << "kill: " << spec
           << " không phải việc đang chạy do shell này tạo (tiến trình khác: dùng công cụ quản lý tiến trình sys_proc, có xác nhận)";
        daemon_.audit_log("shell", "shell.kill", spec, false, "không thuộc shell này");
        continue;
      }
      const bool group = spec[0] == '%' || pid_t(std::atoi(spec.c_str())) == j->pgid;
      const int rc = group ? ::killpg(j->pgid, sig) : ::kill(pid_t(std::atoi(spec.c_str())), sig);
      daemon_.audit_log("shell", "shell.kill", "[" + std::to_string(j->id) + "] " + signal_name(sig), rc == 0,
                        rc == 0 ? "đã gửi" : std::strerror(errno));
      if (rc != 0) {
        last_status_ = 1;
        os << (os.tellp() > 0 ? "\n" : "") << "kill: " << spec << ": " << std::strerror(errno);
      }
    }
    return os.str();
  }
  if (n == "wait") {
    reap(false);
    if (c.argv.size() > 1 && !find_job(c.argv[1])) {
      last_status_ = 127;
      return "wait: không có việc " + c.argv[1] + " do shell này tạo";
    }
    last_status_ = 0;
    for (auto& j : jobs_) {
      if (c.argv.size() > 1 && find_job(c.argv[1]) != &j) continue;
      for (size_t k = 0; k < j.pids.size(); ++k) {
        while (j.status[k] < 0) {
          int st = 0;
          const pid_t r = ::waitpid(j.pids[k], &st, 0);
          if (r < 0 && errno == EINTR) continue;
          j.status[k] = r == j.pids[k] ? decode_status(st) : 0;
          if (j.status[k] < 0) j.status[k] = 0;
        }
      }
      last_status_ = j.status.back();
    }
    return jobs_text(true);
  }
  handled = false;
  return "";
}

std::string Runner::run(const std::string& raw) {
  std::string line = raw;
  while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\n')) line.pop_back();
  size_t s = 0;
  while (s < line.size() && (line[s] == ' ' || line[s] == '\t')) ++s;
  line = line.substr(s);
  if (line.empty()) return "";

  const auto pr = parse(line, vars_, last_status_, cwd_);
  // Báo việc nền đã xong trước kết quả lệnh (như sh trước dấu nhắc) — trừ
  // `jobs`/`wait`, vốn tự báo (và `wait %n` cần thấy việc chưa bị bỏ đi).
  const bool self_reporting = pr.ok && pr.pipeline.cmds.size() == 1 && !pr.pipeline.cmds[0].argv.empty() &&
                              (pr.pipeline.cmds[0].argv[0] == "jobs" || pr.pipeline.cmds[0].argv[0] == "wait");
  const std::string notices = self_reporting ? std::string() : jobs_text(true);
  auto with_notices = [&](const std::string& msg) {
    if (notices.empty()) return msg;
    return msg.empty() ? notices : notices + "\n" + msg;
  };
  if (!pr.ok) {
    last_status_ = 2;
    return with_notices("Lỗi cú pháp: " + pr.error_vi);
  }
  const Pipeline& p = pr.pipeline;
  if (p.cmds.size() == 1 && p.cmds[0].argv.empty()) {  // X=1 Y=2
    for (const auto& kv : p.cmds[0].assigns) vars_[kv.first] = kv.second;
    last_status_ = 0;
    return with_notices("");
  }
  if (p.cmds.size() == 1 && !p.background && p.cmds[0].redirs.empty() && is_builtin(p.cmds[0].argv[0])) {
    bool handled = false;
    const std::string out = builtin(p.cmds[0], handled);
    if (handled) return with_notices(out);
  }
  return with_notices(run_pipeline(p));
}

std::string Runner::run_pipeline(const Pipeline& p) {
  // 1. Phân giải chương trình + hỏi Daemon cho TỪNG lệnh trước khi tạo tiến trình nào.
  std::vector<std::string> programs;
  json::Value rw(json::Array{}), ro(json::Array{});
  bool have_dirs = false;
  for (const auto& c : p.cmds) {
    if (is_builtin(c.argv[0])) {
      last_status_ = 2;
      return c.argv[0] + ": lệnh nội bộ không dùng được trong ống dẫn, khi chạy nền hay khi có chuyển hướng";
    }
    const std::string prog = find_program(c.argv[0], vars_["PATH"], cwd_);
    if (prog.empty()) {
      last_status_ = 127;
      return "không tìm thấy lệnh: " + c.argv[0];
    }
    programs.push_back(prog);
    json::Value q(json::Object{});
    q.set("program", prog);
    q.set("argc", static_cast<long long>(c.argv.size()));
    q.set("cwd", cwd_);
    q.set("background", p.background);
    json::Value reads(json::Array{}), writes(json::Array{});
    for (const auto& a : path_like_args(c.argv, cwd_)) reads.push(a);
    for (const auto& r : c.redirs) {
      if (r.kind == Redirect::In) reads.push(r.path);
      else if (r.kind == Redirect::Out || r.kind == Redirect::Append) writes.push(r.path);
    }
    q.set("reads", reads);
    q.set("writes", writes);
    service::RpcError err;
    const auto res = daemon_.handle("shell.posix_check", q, err);
    if (err.code != 0) {
      last_status_ = 126;
      return "yuid từ chối kiểm tra: " + err.message;
    }
    if (!res["ok"].as_bool()) {
      last_status_ = 126;
      return "Từ chối (hộp cát): " + res["reason_vi"].as_string() + " — không tiến trình nào được tạo";
    }
    if (!have_dirs) {
      rw = res["rw"];
      ro = res["ro"];
      have_dirs = true;
    }
  }
  if (helper_.empty() || !executable_file(helper_)) {
    last_status_ = 126;
    return "thiếu yui-sandbox-exec (bệ phóng hộp cát): không chạy lệnh ngoài hộp cát";
  }

  // 2. Môi trường cho lệnh con.
  auto make_env = [&](const Command& c) {
    Vars v = vars_;
    for (const auto& kv : c.assigns) v[kv.first] = kv.second;
    v["PWD"] = cwd_;
    std::vector<std::string> out;
    for (const auto& kv : v) out.push_back(kv.first + "=" + kv.second);
    return out;
  };

  // 3. Ống dẫn + spawn qua bệ phóng.
  const size_t n = p.cmds.size();
  std::vector<int> pipes(2 * (n > 0 ? n - 1 : 0), -1);
  for (size_t i = 0; i + 1 < n; ++i) {
    if (::pipe2(&pipes[2 * i], O_CLOEXEC) != 0) {
      for (int fd : pipes)
        if (fd >= 0) ::close(fd);
      last_status_ = 126;
      return std::string("không tạo được ống dẫn: ") + std::strerror(errno);
    }
  }
  const int devnull = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
  std::vector<pid_t> pids;
  pid_t pgid = 0;
  std::string spawn_error;
  for (size_t i = 0; i < n; ++i) {
    const Command& c = p.cmds[i];
    std::vector<std::string> args = {helper_, "--chdir", cwd_};
    for (const auto& d : ro.arr()) args.insert(args.end(), {"--ro", d.as_string()});
    for (const auto& d : rw.arr()) args.insert(args.end(), {"--rw", d.as_string()});
    for (const auto& r : c.redirs) {
      switch (r.kind) {
        case Redirect::In: args.insert(args.end(), {"--stdin", r.path}); break;
        case Redirect::Out:
          args.insert(args.end(), {r.fd == 2 ? "--stderr" : "--stdout", r.path});
          break;
        case Redirect::Append:
          args.insert(args.end(), {r.fd == 2 ? "--stderr-append" : "--stdout-append", r.path});
          break;
        case Redirect::DupOut:
          if (r.fd == 2 && r.dup_to == 1) args.push_back("--stderr-to-stdout");
          else if (r.fd == 1 && r.dup_to == 2) args.push_back("--stdout-to-stderr");
          break;
      }
    }
    if (opts_.allow_unsandboxed) args.push_back("--allow-unsandboxed");
    args.push_back("--");
    args.push_back(programs[i]);
    args.insert(args.end(), c.argv.begin() + 1, c.argv.end());

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    const int in_fd = i > 0 ? pipes[2 * (i - 1)] : (p.background ? devnull : opts_.stdin_fd);
    const int out_fd = i + 1 < n ? pipes[2 * i + 1] : opts_.stdout_fd;
    posix_spawn_file_actions_adddup2(&fa, in_fd, 0);
    posix_spawn_file_actions_adddup2(&fa, out_fd, 1);
    posix_spawn_file_actions_adddup2(&fa, opts_.stderr_fd, 2);

    posix_spawnattr_t at;
    posix_spawnattr_init(&at);
    sigset_t defs, empty;
    sigemptyset(&defs);
    for (int sg : {SIGINT, SIGQUIT, SIGTSTP, SIGTTIN, SIGTTOU, SIGPIPE, SIGCHLD, SIGHUP, SIGTERM}) sigaddset(&defs, sg);
    sigemptyset(&empty);
    posix_spawnattr_setsigdefault(&at, &defs);
    posix_spawnattr_setsigmask(&at, &empty);
    posix_spawnattr_setpgroup(&at, pgid);
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK);

    std::vector<char*> av;
    for (auto& a : args) av.push_back(&a[0]);
    av.push_back(nullptr);
    auto env = make_env(c);
    std::vector<char*> ev;
    for (auto& e : env) ev.push_back(&e[0]);
    ev.push_back(nullptr);

    pid_t pid = 0;
    const int rc = ::posix_spawn(&pid, helper_.c_str(), &fa, &at, av.data(), ev.data());
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&at);
    if (rc != 0) {
      spawn_error = std::string("không tạo được tiến trình: ") + std::strerror(rc);
      break;
    }
    if (pgid == 0) pgid = pid;
    pids.push_back(pid);
  }
  for (int fd : pipes)
    if (fd >= 0) ::close(fd);
  if (devnull >= 0) ::close(devnull);
  if (!spawn_error.empty()) {
    if (pgid) ::killpg(pgid, SIGKILL);
    for (pid_t pid : pids) ::waitpid(pid, nullptr, 0);
    last_status_ = 126;
    return spawn_error;
  }

  Job job;
  job.id = next_job_;
  job.pgid = pgid;
  job.pids = pids;
  job.status.assign(pids.size(), -1);
  job.text = p.text;

  if (p.background) {
    ++next_job_;
    jobs_.push_back(job);
    last_status_ = 0;
    return "[" + std::to_string(job.id) + "] " + std::to_string(pgid);
  }

  // 4. Tiền cảnh: trao terminal (nếu có) cho nhóm tiến trình, đợi, lấy lại.
  const bool tty = opts_.interactive && ::isatty(opts_.stdin_fd);
  if (tty) ::tcsetpgrp(opts_.stdin_fd, pgid);
  bool stopped = false;
  for (size_t k = 0; k < pids.size(); ++k) {
    int st = 0;
    pid_t r;
    do {
      r = ::waitpid(pids[k], &st, WUNTRACED);
    } while (r < 0 && errno == EINTR);
    if (r == pids[k] && WIFSTOPPED(st)) {
      job.status[k] = kStopped;
      stopped = true;
    } else {
      job.status[k] = r == pids[k] ? decode_status(st) : 0;
    }
  }
  if (tty) ::tcsetpgrp(opts_.stdin_fd, ::getpgrp());
  if (stopped) {
    ++next_job_;
    jobs_.push_back(job);
    last_status_ = 148;
    return "[" + std::to_string(job.id) + "] Đã dừng  " + job.text + "  (tiếp tục: kill -CONT %" +
           std::to_string(job.id) + ")";
  }
  last_status_ = job.status.back();
  if (last_status_ > 128) return "Bị " + signal_name(last_status_ - 128);
  return "";
}

}  // namespace yui::posix
