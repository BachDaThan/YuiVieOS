// Minimal, dependency-free test harness shared by every YuiVieOS C++ test.
// Tests self-register; main() (provided by YUI_TEST_MAIN) runs them in order.
#ifndef YUI_TESTUTIL_HPP
#define YUI_TESTUTIL_HPP

#include <cstdio>
#include <string>
#include <vector>

namespace yuitest {

struct TestCase {
  const char* suite;
  const char* name;
  void (*fn)();
};

inline std::vector<TestCase>& registry() {
  static std::vector<TestCase> r;
  return r;
}
inline const char*& current() {
  static const char* c = "";
  return c;
}
inline int& failures() {
  static int f = 0;
  return f;
}
inline int& checks() {
  static int c = 0;
  return c;
}
inline bool& verbose() {
  static bool v = true;
  return v;
}

struct Registrar {
  Registrar(const char* suite, const char* name, void (*fn)()) { registry().push_back({suite, name, fn}); }
};

inline void pass(const char* what) {
  ++checks();
  if (verbose()) std::printf("  \033[32mok\033[0m     %s\n", what);
}

inline void fail(const char* what, const char* file, int line, const std::string& msg) {
  ++checks();
  ++failures();
  std::printf("  \033[31mFAIL\033[0m   %s\n           %s:%d  %s\n", what, file, line, msg.c_str());
}

inline int run_all(const char* binary) {
  const char* suite = "";
  for (const auto& t : registry()) {
    if (std::string(t.suite) != suite) {
      suite = t.suite;
      std::printf("\n\033[1m%s\033[0m\n", suite);
    }
    current() = t.name;
    t.fn();
  }
  std::printf("\n");
  if (failures() == 0) {
    std::printf("\033[32m✓ %s: %d checks passed, 0 failed\033[0m\n", binary, checks());
    return 0;
  }
  std::printf("\033[31m✗ %s: %d of %d checks FAILED\033[0m\n", binary, failures(), checks());
  return 1;
}

}  // namespace yuitest

#define YUI_CONCAT_IMPL(a, b) a##b
#define YUI_CONCAT(a, b) YUI_CONCAT_IMPL(a, b)

#define YUI_TEST(suite_name, test_name)                                              \
  static void YUI_CONCAT(yui_test_, YUI_CONCAT(suite_name, YUI_CONCAT(_, test_name)))(); \
  static ::yuitest::Registrar YUI_CONCAT(yui_reg_, YUI_CONCAT(suite_name, YUI_CONCAT(_, test_name)))( \
      #suite_name, #test_name,                                                       \
      &YUI_CONCAT(yui_test_, YUI_CONCAT(suite_name, YUI_CONCAT(_, test_name))));      \
  static void YUI_CONCAT(yui_test_, YUI_CONCAT(suite_name, YUI_CONCAT(_, test_name)))()

#define YUI_TEST_MAIN(binary_name)                     \
  int main(int argc, char** argv) {                    \
    yuitest::verbose() = !(argc > 1 && std::string(argv[1]) == "-q"); \
    return ::yuitest::run_all(binary_name);            \
  }

#define CHECK(cond)                                                       \
  do {                                                                    \
    if (cond) {                                                           \
      ::yuitest::pass(#cond);                                             \
    } else {                                                              \
      ::yuitest::fail(#cond, __FILE__, __LINE__, "condition was false");   \
    }                                                                     \
  } while (0)

#define CHECK_MSG(cond, msg)                                              \
  do {                                                                    \
    if (cond) {                                                           \
      ::yuitest::pass(#cond);                                             \
    } else {                                                              \
      ::yuitest::fail(#cond, __FILE__, __LINE__, (msg));                   \
    }                                                                     \
  } while (0)

#define CHECK_EQ(a, b)                                                                 \
  do {                                                                                 \
    const auto& _va = (a);                                                             \
    const auto& _vb = (b);                                                             \
    if (_va == _vb) {                                                                  \
      ::yuitest::pass(#a " == " #b);                                                    \
    } else {                                                                           \
      ::yuitest::fail(#a " == " #b, __FILE__, __LINE__, std::string("values differ"));  \
    }                                                                                  \
  } while (0)

#define CHECK_HEX(got, expected)                                                             \
  do {                                                                                       \
    const std::string _g = (got);                                                            \
    const std::string _e = (expected);                                                       \
    if (_g == _e) {                                                                          \
      ::yuitest::pass(#got);                                                                  \
    } else {                                                                                 \
      ::yuitest::fail(#got, __FILE__, __LINE__, "hex mismatch\n           got: " + _g +      \
                                                    "\n           exp: " + _e);              \
    }                                                                                        \
  } while (0)

#endif  // YUI_TESTUTIL_HPP
