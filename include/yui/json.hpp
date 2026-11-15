// Minimal JSON (RFC 8259) reader/writer used by the yuid socket protocol, the
// MCP bridge, sandbox policies and the knowledge-graph export format.
//
// Design notes:
//   * header-only, no exceptions thrown out of the parser (error returned)
//   * objects preserve insertion order so serialisation is deterministic
//   * integers are kept as int64 when the literal has no fraction/exponent
//   * UTF-8 is passed through; \uXXXX escapes (incl. surrogate pairs) decode
#ifndef YUI_JSON_HPP
#define YUI_JSON_HPP

#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace yui::json {

class Value;
using Array = std::vector<Value>;
using Member = std::pair<std::string, Value>;
using Object = std::vector<Member>;

enum class Type { Null, Bool, Int, Double, String, Array, Object };

class Value {
 public:
  Value() : type_(Type::Null) {}
  Value(std::nullptr_t) : type_(Type::Null) {}
  Value(bool b) : type_(Type::Bool), b_(b) {}
  Value(int v) : type_(Type::Int), i_(v) {}
  Value(long v) : type_(Type::Int), i_(v) {}
  Value(long long v) : type_(Type::Int), i_(v) {}
  Value(unsigned v) : type_(Type::Int), i_(v) {}
  Value(size_t v) : type_(Type::Int), i_(int64_t(v)) {}
  Value(double d) : type_(Type::Double), d_(d) {}
  Value(const char* s) : type_(Type::String), s_(s ? s : "") {}
  Value(std::string s) : type_(Type::String), s_(std::move(s)) {}
  Value(Array a) : type_(Type::Array), a_(std::move(a)) {}
  Value(Object o) : type_(Type::Object), o_(std::move(o)) {}

  Type type() const { return type_; }
  bool is_null() const { return type_ == Type::Null; }
  bool is_bool() const { return type_ == Type::Bool; }
  bool is_number() const { return type_ == Type::Int || type_ == Type::Double; }
  bool is_string() const { return type_ == Type::String; }
  bool is_array() const { return type_ == Type::Array; }
  bool is_object() const { return type_ == Type::Object; }

  bool as_bool(bool def = false) const { return type_ == Type::Bool ? b_ : def; }
  int64_t as_int(int64_t def = 0) const {
    if (type_ == Type::Int) return i_;
    if (type_ == Type::Double) return int64_t(d_);
    if (type_ == Type::Bool) return b_ ? 1 : 0;
    return def;
  }
  double as_double(double def = 0.0) const {
    if (type_ == Type::Double) return d_;
    if (type_ == Type::Int) return double(i_);
    return def;
  }
  std::string as_string(const std::string& def = "") const { return type_ == Type::String ? s_ : def; }

  const Array& arr() const { return a_; }
  Array& arr() { return a_; }
  const Object& obj() const { return o_; }
  Object& obj() { return o_; }

  // Object helpers.  Missing keys return a shared null value.
  const Value& operator[](const std::string& key) const {
    static const Value kNull;
    if (type_ != Type::Object) return kNull;
    for (const auto& m : o_)
      if (m.first == key) return m.second;
    return kNull;
  }
  Value& set(const std::string& key, Value v) {
    if (type_ != Type::Object) {
      type_ = Type::Object;
      o_.clear();
    }
    for (auto& m : o_)
      if (m.first == key) {
        m.second = std::move(v);
        return m.second;
      }
    o_.emplace_back(key, std::move(v));
    return o_.back().second;
  }
  bool has(const std::string& key) const {
    if (type_ != Type::Object) return false;
    for (const auto& m : o_)
      if (m.first == key) return true;
    return false;
  }
  size_t size() const {
    if (type_ == Type::Array) return a_.size();
    if (type_ == Type::Object) return o_.size();
    return 0;
  }
  void push(Value v) {
    if (type_ != Type::Array) {
      type_ = Type::Array;
      a_.clear();
    }
    a_.push_back(std::move(v));
  }

  // Serialise.  `pretty` emits 2-space indentation.
  std::string dump(bool pretty = false) const;

 private:
  void dump_to(std::string& out, bool pretty, int indent) const;

  Type type_ = Type::Null;
  bool b_ = false;
  int64_t i_ = 0;
  double d_ = 0.0;
  std::string s_;
  Array a_;
  Object o_;
};

struct ParseResult {
  bool ok = false;
  Value value;
  std::string error;
  size_t offset = 0;  // byte offset of the error, for diagnostics
};

ParseResult parse(const std::string& text);
// Convenience: parse and return an object value; on failure returns null value.
Value parse_or_null(const std::string& text);

// Escape a string as a JSON string literal (with quotes).
std::string quote(const std::string& s);

}  // namespace yui::json

// ------------------------------------------------------------- inline impl --
namespace yui::json {
namespace detail {

inline void append_utf8(std::string& out, uint32_t cp) {
  if (cp < 0x80) {
    out += char(cp);
  } else if (cp < 0x800) {
    out += char(0xC0 | (cp >> 6));
    out += char(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += char(0xE0 | (cp >> 12));
    out += char(0x80 | ((cp >> 6) & 0x3F));
    out += char(0x80 | (cp & 0x3F));
  } else {
    out += char(0xF0 | (cp >> 18));
    out += char(0x80 | ((cp >> 12) & 0x3F));
    out += char(0x80 | ((cp >> 6) & 0x3F));
    out += char(0x80 | (cp & 0x3F));
  }
}

class Parser {
 public:
  explicit Parser(const std::string& t) : t_(t) {}

  ParseResult run() {
    ParseResult r;
    skip_ws();
    if (!parse_value(r.value)) {
      r.error = err_;
      r.offset = pos_;
      return r;
    }
    skip_ws();
    if (pos_ != t_.size()) {
      r.error = "trailing characters after JSON value";
      r.offset = pos_;
      return r;
    }
    r.ok = true;
    return r;
  }

 private:
  void skip_ws() {
    while (pos_ < t_.size()) {
      const char c = t_[pos_];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
        ++pos_;
      else
        break;
    }
  }
  bool fail(const char* what) {
    if (err_.empty()) err_ = std::string(what) + " at offset " + std::to_string(pos_);
    return false;
  }
  bool literal(const char* lit) {
    const size_t n = std::char_traits<char>::length(lit);
    if (t_.compare(pos_, n, lit) != 0) return fail("invalid literal");
    pos_ += n;
    return true;
  }

  bool parse_value(Value& out) {
    if (pos_ >= t_.size()) return fail("unexpected end of input");
    switch (t_[pos_]) {
      case '{': return parse_object(out);
      case '[': return parse_array(out);
      case '"': {
        std::string s;
        if (!parse_string(s)) return false;
        out = Value(std::move(s));
        return true;
      }
      case 't':
        if (!literal("true")) return false;
        out = Value(true);
        return true;
      case 'f':
        if (!literal("false")) return false;
        out = Value(false);
        return true;
      case 'n':
        if (!literal("null")) return false;
        out = Value(nullptr);
        return true;
      default: return parse_number(out);
    }
  }

  bool parse_object(Value& out) {
    ++pos_;  // '{'
    Object obj;
    skip_ws();
    if (pos_ < t_.size() && t_[pos_] == '}') {
      ++pos_;
      out = Value(std::move(obj));
      return true;
    }
    while (true) {
      skip_ws();
      if (pos_ >= t_.size() || t_[pos_] != '"') return fail("expected object key");
      std::string key;
      if (!parse_string(key)) return false;
      skip_ws();
      if (pos_ >= t_.size() || t_[pos_] != ':') return fail("expected ':'");
      ++pos_;
      skip_ws();
      Value v;
      if (!parse_value(v)) return false;
      obj.emplace_back(std::move(key), std::move(v));
      skip_ws();
      if (pos_ >= t_.size()) return fail("unterminated object");
      if (t_[pos_] == ',') {
        ++pos_;
        continue;
      }
      if (t_[pos_] == '}') {
        ++pos_;
        out = Value(std::move(obj));
        return true;
      }
      return fail("expected ',' or '}'");
    }
  }

  bool parse_array(Value& out) {
    ++pos_;  // '['
    Array arr;
    skip_ws();
    if (pos_ < t_.size() && t_[pos_] == ']') {
      ++pos_;
      out = Value(std::move(arr));
      return true;
    }
    while (true) {
      skip_ws();
      Value v;
      if (!parse_value(v)) return false;
      arr.push_back(std::move(v));
      skip_ws();
      if (pos_ >= t_.size()) return fail("unterminated array");
      if (t_[pos_] == ',') {
        ++pos_;
        continue;
      }
      if (t_[pos_] == ']') {
        ++pos_;
        out = Value(std::move(arr));
        return true;
      }
      return fail("expected ',' or ']'");
    }
  }

  bool parse_hex4(uint32_t& out) {
    if (pos_ + 4 > t_.size()) return fail("truncated \\u escape");
    out = 0;
    for (int i = 0; i < 4; ++i) {
      const char c = t_[pos_++];
      out <<= 4;
      if (c >= '0' && c <= '9')
        out |= uint32_t(c - '0');
      else if (c >= 'a' && c <= 'f')
        out |= uint32_t(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F')
        out |= uint32_t(c - 'A' + 10);
      else
        return fail("invalid hex digit in \\u escape");
    }
    return true;
  }

  bool parse_string(std::string& out) {
    ++pos_;  // opening quote
    out.clear();
    while (pos_ < t_.size()) {
      const unsigned char c = static_cast<unsigned char>(t_[pos_++]);
      if (c == '"') return true;
      if (c == '\\') {
        if (pos_ >= t_.size()) return fail("truncated escape");
        const char e = t_[pos_++];
        switch (e) {
          case '"': out += '"'; break;
          case '\\': out += '\\'; break;
          case '/': out += '/'; break;
          case 'b': out += '\b'; break;
          case 'f': out += '\f'; break;
          case 'n': out += '\n'; break;
          case 'r': out += '\r'; break;
          case 't': out += '\t'; break;
          case 'u': {
            uint32_t cp = 0;
            if (!parse_hex4(cp)) return false;
            if (cp >= 0xD800 && cp <= 0xDBFF) {  // high surrogate
              if (pos_ + 1 < t_.size() && t_[pos_] == '\\' && t_[pos_ + 1] == 'u') {
                pos_ += 2;
                uint32_t lo = 0;
                if (!parse_hex4(lo)) return false;
                if (lo < 0xDC00 || lo > 0xDFFF) return fail("invalid low surrogate");
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
              } else {
                return fail("unpaired high surrogate");
              }
            }
            append_utf8(out, cp);
            break;
          }
          default: return fail("unknown escape sequence");
        }
      } else if (c < 0x20) {
        return fail("raw control character in string");
      } else {
        out += char(c);
      }
    }
    return fail("unterminated string");
  }

  bool parse_number(Value& out) {
    const size_t start = pos_;
    bool is_int = true;
    if (pos_ < t_.size() && (t_[pos_] == '-' || t_[pos_] == '+')) ++pos_;
    bool any = false;
    while (pos_ < t_.size() && t_[pos_] >= '0' && t_[pos_] <= '9') {
      ++pos_;
      any = true;
    }
    if (pos_ < t_.size() && t_[pos_] == '.') {
      is_int = false;
      ++pos_;
      while (pos_ < t_.size() && t_[pos_] >= '0' && t_[pos_] <= '9') {
        ++pos_;
        any = true;
      }
    }
    if (pos_ < t_.size() && (t_[pos_] == 'e' || t_[pos_] == 'E')) {
      is_int = false;
      ++pos_;
      if (pos_ < t_.size() && (t_[pos_] == '-' || t_[pos_] == '+')) ++pos_;
      while (pos_ < t_.size() && t_[pos_] >= '0' && t_[pos_] <= '9') ++pos_;
    }
    if (!any) return fail("invalid number");
    const std::string lit = t_.substr(start, pos_ - start);
    if (is_int) {
      errno = 0;
      const long long v = std::strtoll(lit.c_str(), nullptr, 10);
      if (errno == ERANGE) {
        out = Value(std::strtod(lit.c_str(), nullptr));
      } else {
        out = Value(int64_t(v));
      }
    } else {
      out = Value(std::strtod(lit.c_str(), nullptr));
    }
    return true;
  }

  const std::string& t_;
  size_t pos_ = 0;
  std::string err_;
};

}  // namespace detail

inline std::string quote(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 2);
  out += '"';
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out += char(c);
        }
    }
  }
  out += '"';
  return out;
}

inline void Value::dump_to(std::string& out, bool pretty, int indent) const {
  auto nl = [&](int depth) {
    if (!pretty) return;
    out += '\n';
    out.append(size_t(depth) * 2, ' ');
  };
  switch (type_) {
    case Type::Null: out += "null"; break;
    case Type::Bool: out += b_ ? "true" : "false"; break;
    case Type::Int: out += std::to_string(i_); break;
    case Type::Double: {
      char buf[40];
      const double d = d_;
      if (d == double(int64_t(d)) && std::abs(d) < 1e15)
        std::snprintf(buf, sizeof(buf), "%.1f", d);  // keep it a JSON double
      else
        std::snprintf(buf, sizeof(buf), "%.17g", d);
      out += buf;
      break;
    }
    case Type::String: out += quote(s_); break;
    case Type::Array: {
      if (a_.empty()) {
        out += "[]";
        break;
      }
      out += '[';
      for (size_t i = 0; i < a_.size(); ++i) {
        if (i) out += ',';
        nl(indent + 1);
        a_[i].dump_to(out, pretty, indent + 1);
      }
      nl(indent);
      out += ']';
      break;
    }
    case Type::Object: {
      if (o_.empty()) {
        out += "{}";
        break;
      }
      out += '{';
      for (size_t i = 0; i < o_.size(); ++i) {
        if (i) out += ',';
        nl(indent + 1);
        out += quote(o_[i].first);
        out += pretty ? ": " : ":";
        o_[i].second.dump_to(out, pretty, indent + 1);
      }
      nl(indent);
      out += '}';
      break;
    }
  }
}

inline std::string Value::dump(bool pretty) const {
  std::string out;
  out.reserve(64);
  dump_to(out, pretty, 0);
  return out;
}

inline ParseResult parse(const std::string& text) { return detail::Parser(text).run(); }

inline Value parse_or_null(const std::string& text) {
  ParseResult r = parse(text);
  return r.ok ? r.value : Value(nullptr);
}

}  // namespace yui::json

#endif  // YUI_JSON_HPP
