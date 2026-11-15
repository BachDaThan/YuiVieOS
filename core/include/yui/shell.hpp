// Shell lệnh song ngữ Việt – Anh (Tầng 5).
//
// The shell is deliberately a thin, testable layer over the same Daemon the
// socket serves: every command is a straight translation into an yuid method
// call, so the GUI, scripts and the terminal can never drift apart.
#ifndef YUI_SHELL_HPP
#define YUI_SHELL_HPP

#include <string>
#include <vector>

#include "yui/service.hpp"
#include "yui/ui.hpp"

namespace yui::shell {

struct Command {
  std::string name_vi;      // primary Vietnamese name
  std::string name_en;      // English alias
  std::string usage_vi;
  std::string usage_en;
  std::string help_vi;
  std::string help_en;
};

class Shell {
 public:
  Shell(service::Daemon& daemon, const ui::Layout& layout);

  // Executes one input line and returns the text to print (no trailing newline).
  std::string run(const std::string& line);

  std::string banner() const;
  std::string help_text() const;
  const std::vector<Command>& commands() const { return commands_; }
  bool wants_exit() const { return exit_; }

 private:
  std::string dispatch(const std::string& cmd, const std::vector<std::string>& args);
  service::Daemon& daemon_;
  ui::Layout layout_;
  std::vector<Command> commands_;
  bool exit_ = false;
  int history_ = 0;
};

}  // namespace yui::shell

#endif  // YUI_SHELL_HPP
