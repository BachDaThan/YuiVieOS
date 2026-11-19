// Chế độ POSIX của yui-shell (Tầng 5, P4.12).
//
// Hai chế độ trong cùng một shell:
//   * lệnh Việt–Anh nội bộ (`tìm`, `cài`, `xoá`…) — như trước;
//   * lệnh POSIX, CHỈ khi bạn nói rõ: dòng bắt đầu bằng `!` hoặc lệnh `chạy …`.
//     Dòng không nhận ra KHÔNG BAO GIỜ tự được chạy như lệnh hệ thống.
//
// Hỗ trợ: ống dẫn `|`, chuyển hướng `<` `>` `>>` `2>` `2>>` `2>&1` `>&2`, chạy
// nền `&`, `jobs`, `kill [-TÍN_HIỆU] %n|pid`, `wait`, `cd`, `export`, `unset`,
// biến `$X` `${X}` `$?`, `~`, trích dẫn '…' "…" và `\`, glob `* ? [..]`.
// Cố ý KHÔNG hỗ trợ (báo lỗi rõ, không đoán): `;` `&&` `||` `$(…)` `` `…` ``
// `<<` `( )`; biến không được tách từ (an toàn hơn sh).
//
// Mỗi lệnh đi qua CÙNG `Daemon` như GUI/MCP: `shell.posix_check` quyết định
// theo chính sách hộp cát (chạy chương trình nào, đọc/ghi tệp chuyển hướng
// nào) và ghi audit; Daemon trả về các nhánh đọc/ghi để `yui-sandbox-exec` áp
// Landlock + no_new_privs trước khi exec. Bị từ chối ⇒ không tiến trình nào
// được tạo, kể cả các lệnh khác trong cùng ống dẫn.
#ifndef YUI_POSIX_HPP
#define YUI_POSIX_HPP

#include <sys/types.h>

#include <map>
#include <string>
#include <vector>

namespace yui::service {
class Daemon;
}

namespace yui::posix {

using Vars = std::map<std::string, std::string>;

struct Redirect {
  enum Kind { In, Out, Append, DupOut };  // DupOut: fd ← dup_to (2>&1, >&2)
  int fd = 1;
  Kind kind = Out;
  std::string path;  // tuyệt đối sau khi phân tích
  int dup_to = -1;
};

struct Command {
  std::vector<std::string> argv;
  std::vector<std::pair<std::string, std::string>> assigns;  // X=1 cmd
  std::vector<Redirect> redirs;
};

struct Pipeline {
  std::vector<Command> cmds;
  bool background = false;
  std::string text;  // dòng gốc (để hiện trong `jobs`)
};

struct ParseResult {
  bool ok = false;
  std::string error_vi;
  Pipeline pipeline;
};

// `cwd` dùng cho glob và đường dẫn tương đối của chuyển hướng.
ParseResult parse(const std::string& line, const Vars& vars, int last_status, const std::string& cwd);

// Tìm chương trình như execvp: có '/' ⇒ theo cwd; không ⇒ trong PATH.
std::string find_program(const std::string& name, const std::string& path_var, const std::string& cwd);

struct Options {
  std::string sandbox_exec;  // rỗng ⇒ $YUI_SANDBOX_EXEC, rồi cạnh /proc/self/exe, rồi PATH
  bool interactive = false;  // có TTY: chuyển quyền điều khiển terminal cho việc tiền cảnh
  bool allow_unsandboxed = false;  // nhân không có Landlock vẫn chạy (chỉ còn kiểm tra của yuid)
  int stdin_fd = 0, stdout_fd = 1, stderr_fd = 2;  // fd của shell trao cho lệnh con
};

struct Job {
  int id = 0;
  pid_t pgid = 0;
  std::vector<pid_t> pids;
  std::vector<int> status;  // -1 = đang chạy; ≥ 0 = mã thoát; 128+n = chết vì tín hiệu n
  std::string text;
  bool reported_done = false;
  bool running() const;
};

class Runner {
 public:
  Runner(service::Daemon& daemon, Options opts);
  ~Runner();  // việc nền còn chạy nhận SIGHUP (như sh khi đóng phiên)
  Runner(const Runner&) = delete;
  Runner& operator=(const Runner&) = delete;

  // Chạy một dòng POSIX. Đầu ra của chương trình đi thẳng ra fd đã cấu hình;
  // giá trị trả về là thông báo của shell (lỗi, `jobs`, `[1] 1234`…).
  std::string run(const std::string& line);

  int last_status() const { return last_status_; }
  const std::string& cwd() const { return cwd_; }
  Vars& vars() { return vars_; }
  const std::vector<Job>& jobs() const { return jobs_; }
  // Đợi mọi việc nền (test dùng); trả số việc đã xong.
  size_t wait_all();
  std::string sandbox_exec_path() const { return helper_; }

 private:
  std::string builtin(const Command& c, bool& handled);
  std::string run_pipeline(const Pipeline& p);
  std::string jobs_text(bool only_changes);
  void reap(bool block);
  Job* find_job(const std::string& spec);

  service::Daemon& daemon_;
  Options opts_;
  std::string helper_;
  std::string cwd_;
  Vars vars_;
  int last_status_ = 0;
  int next_job_ = 1;
  std::vector<Job> jobs_;
};

}  // namespace yui::posix

#endif  // YUI_POSIX_HPP
