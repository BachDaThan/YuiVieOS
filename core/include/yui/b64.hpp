// Small, allocation-light base64 (RFC 4648) used by the socket protocol and the
// C-ABI to move binary payloads through JSON.
#ifndef YUI_B64_HPP
#define YUI_B64_HPP

#include <cstdint>
#include <string>
#include <vector>

namespace yui::b64 {

inline std::string encode(const uint8_t* data, size_t len) {
  static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve((len + 2) / 3 * 4);
  size_t i = 0;
  for (; i + 3 <= len; i += 3) {
    const uint32_t v = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8) | data[i + 2];
    out += tbl[(v >> 18) & 63];
    out += tbl[(v >> 12) & 63];
    out += tbl[(v >> 6) & 63];
    out += tbl[v & 63];
  }
  if (i + 1 == len) {
    const uint32_t v = uint32_t(data[i]) << 16;
    out += tbl[(v >> 18) & 63];
    out += tbl[(v >> 12) & 63];
    out += "==";
  } else if (i + 2 == len) {
    const uint32_t v = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8);
    out += tbl[(v >> 18) & 63];
    out += tbl[(v >> 12) & 63];
    out += tbl[(v >> 6) & 63];
    out += '=';
  }
  return out;
}

inline std::string encode(const std::string& s) {
  return encode(reinterpret_cast<const uint8_t*>(s.data()), s.size());
}
inline std::string encode(const std::vector<uint8_t>& v) { return encode(v.data(), v.size()); }

inline bool decode(const std::string& in, std::vector<uint8_t>& out) {
  auto val = [](char c) -> int {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
  };
  out.clear();
  uint32_t buf = 0;
  int bits = 0;
  for (char c : in) {
    if (c == '=' || c == '\n' || c == '\r' || c == ' ') continue;
    const int v = val(c);
    if (v < 0) return false;
    buf = (buf << 6) | uint32_t(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(uint8_t((buf >> bits) & 0xff));
    }
  }
  return true;
}

inline bool decode_string(const std::string& in, std::string& out) {
  std::vector<uint8_t> v;
  if (!decode(in, v)) return false;
  out.assign(v.begin(), v.end());
  return true;
}

}  // namespace yui::b64

#endif  // YUI_B64_HPP
