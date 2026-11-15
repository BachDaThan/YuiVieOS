// yui_text implementation.
#include "yui/text.hpp"

#include <algorithm>

namespace yui::text {
namespace {

struct ViFold {
  const char* upper;
  const char* lower;
};

static const ViFold kViFold[] = {
    {"\x41", "\x61"},
    {"\xc3\x80", "\xc3\xa0"},
    {"\xe1\xba\xa2", "\xe1\xba\xa3"},
    {"\xc3\x83", "\xc3\xa3"},
    {"\xc3\x81", "\xc3\xa1"},
    {"\xe1\xba\xa0", "\xe1\xba\xa1"},
    {"\xc4\x82", "\xc4\x83"},
    {"\xe1\xba\xb0", "\xe1\xba\xb1"},
    {"\xe1\xba\xb2", "\xe1\xba\xb3"},
    {"\xe1\xba\xb4", "\xe1\xba\xb5"},
    {"\xe1\xba\xae", "\xe1\xba\xaf"},
    {"\xe1\xba\xb6", "\xe1\xba\xb7"},
    {"\xc3\x82", "\xc3\xa2"},
    {"\xe1\xba\xa6", "\xe1\xba\xa7"},
    {"\xe1\xba\xa8", "\xe1\xba\xa9"},
    {"\xe1\xba\xaa", "\xe1\xba\xab"},
    {"\xe1\xba\xa4", "\xe1\xba\xa5"},
    {"\xe1\xba\xac", "\xe1\xba\xad"},
    {"\x45", "\x65"},
    {"\xc3\x88", "\xc3\xa8"},
    {"\xe1\xba\xba", "\xe1\xba\xbb"},
    {"\xe1\xba\xbc", "\xe1\xba\xbd"},
    {"\xc3\x89", "\xc3\xa9"},
    {"\xe1\xba\xb8", "\xe1\xba\xb9"},
    {"\xc3\x8a", "\xc3\xaa"},
    {"\xe1\xbb\x80", "\xe1\xbb\x81"},
    {"\xe1\xbb\x82", "\xe1\xbb\x83"},
    {"\xe1\xbb\x84", "\xe1\xbb\x85"},
    {"\xe1\xba\xbe", "\xe1\xba\xbf"},
    {"\xe1\xbb\x86", "\xe1\xbb\x87"},
    {"\x49", "\x69"},
    {"\xc3\x8c", "\xc3\xac"},
    {"\xe1\xbb\x88", "\xe1\xbb\x89"},
    {"\xc4\xa8", "\xc4\xa9"},
    {"\xc3\x8d", "\xc3\xad"},
    {"\xe1\xbb\x8a", "\xe1\xbb\x8b"},
    {"\x4f", "\x6f"},
    {"\xc3\x92", "\xc3\xb2"},
    {"\xe1\xbb\x8e", "\xe1\xbb\x8f"},
    {"\xc3\x95", "\xc3\xb5"},
    {"\xc3\x93", "\xc3\xb3"},
    {"\xe1\xbb\x8c", "\xe1\xbb\x8d"},
    {"\xc3\x94", "\xc3\xb4"},
    {"\xe1\xbb\x92", "\xe1\xbb\x93"},
    {"\xe1\xbb\x94", "\xe1\xbb\x95"},
    {"\xe1\xbb\x96", "\xe1\xbb\x97"},
    {"\xe1\xbb\x90", "\xe1\xbb\x91"},
    {"\xe1\xbb\x98", "\xe1\xbb\x99"},
    {"\xc6\xa0", "\xc6\xa1"},
    {"\xe1\xbb\x9c", "\xe1\xbb\x9d"},
    {"\xe1\xbb\x9e", "\xe1\xbb\x9f"},
    {"\xe1\xbb\xa0", "\xe1\xbb\xa1"},
    {"\xe1\xbb\x9a", "\xe1\xbb\x9b"},
    {"\xe1\xbb\xa2", "\xe1\xbb\xa3"},
    {"\x55", "\x75"},
    {"\xc3\x99", "\xc3\xb9"},
    {"\xe1\xbb\xa6", "\xe1\xbb\xa7"},
    {"\xc5\xa8", "\xc5\xa9"},
    {"\xc3\x9a", "\xc3\xba"},
    {"\xe1\xbb\xa4", "\xe1\xbb\xa5"},
    {"\xc6\xaf", "\xc6\xb0"},
    {"\xe1\xbb\xaa", "\xe1\xbb\xab"},
    {"\xe1\xbb\xac", "\xe1\xbb\xad"},
    {"\xe1\xbb\xae", "\xe1\xbb\xaf"},
    {"\xe1\xbb\xa8", "\xe1\xbb\xa9"},
    {"\xe1\xbb\xb0", "\xe1\xbb\xb1"},
    {"\x59", "\x79"},
    {"\xe1\xbb\xb2", "\xe1\xbb\xb3"},
    {"\xe1\xbb\xb6", "\xe1\xbb\xb7"},
    {"\xe1\xbb\xb8", "\xe1\xbb\xb9"},
    {"\xc3\x9d", "\xc3\xbd"},
    {"\xe1\xbb\xb4", "\xe1\xbb\xb5"},
    {"\x44", "\x64"},
    {"\xc4\x90", "\xc4\x91"},
};


constexpr size_t kViFoldCount = sizeof(kViFold) / sizeof(kViFold[0]);

bool valid_utf8(const std::string& s, size_t pos, size_t len) {
  if (pos + len > s.size()) return false;
  for (size_t i = 1; i < len; ++i)
    if ((static_cast<unsigned char>(s[pos + i]) & 0xC0) != 0x80) return false;
  return true;
}

}  // namespace

uint32_t next_codepoint(const std::string& s, size_t& pos) {
  if (pos >= s.size()) return 0;
  const unsigned char c = static_cast<unsigned char>(s[pos]);
  if (c < 0x80) {
    ++pos;
    return c;
  }
  size_t len = 0;
  uint32_t cp = 0;
  if ((c & 0xE0) == 0xC0) {
    len = 2;
    cp = c & 0x1F;
  } else if ((c & 0xF0) == 0xE0) {
    len = 3;
    cp = c & 0x0F;
  } else if ((c & 0xF8) == 0xF0) {
    len = 4;
    cp = c & 0x07;
  } else {
    ++pos;
    return 0xFFFD;
  }
  if (!valid_utf8(s, pos, len)) {
    ++pos;
    return 0xFFFD;
  }
  for (size_t i = 1; i < len; ++i) cp = (cp << 6) | (static_cast<unsigned char>(s[pos + i]) & 0x3F);
  pos += len;
  return cp;
}

void append_codepoint(std::string& out, uint32_t cp) {
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

size_t length(const std::string& s) {
  size_t pos = 0, n = 0;
  while (pos < s.size()) {
    next_codepoint(s, pos);
    ++n;
  }
  return n;
}

bool is_vietnamese_letter(uint32_t cp) {
  if ((cp >= 0x0041 && cp <= 0x005A) || (cp >= 0x0061 && cp <= 0x007A)) return true;  // ASCII
  return (cp >= 0x00C0 && cp <= 0x01B0) || (cp >= 0x1EA0 && cp <= 0x1EF9);
}

size_t display_width(const std::string& s) {
  size_t pos = 0, w = 0;
  while (pos < s.size()) {
    const uint32_t cp = next_codepoint(s, pos);
    if (cp == 0x0300 || (cp >= 0x0301 && cp <= 0x036F)) continue;  // combining marks
    if (cp == 0x200B || cp == 0xFEFF) continue;                    // zero-width
    w += (cp >= 0x1100 && (cp <= 0x115F || cp == 0x2329 || cp == 0x232A ||
                           (cp >= 0x2E80 && cp <= 0xA4CF) || (cp >= 0xAC00 && cp <= 0xD7A3) ||
                           (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0xFE30 && cp <= 0xFE6F) ||
                           (cp >= 0xFF00 && cp <= 0xFF60) || (cp >= 0xFFE0 && cp <= 0xFFE6)))
             ? 2
             : 1;
  }
  return w;
}

std::string fold(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  size_t pos = 0;
  while (pos < s.size()) {
    const unsigned char c = static_cast<unsigned char>(s[pos]);
    if (c < 0x80) {
      out += char((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c);
      ++pos;
      continue;
    }
    bool matched = false;
    for (size_t i = 0; i < kViFoldCount; ++i) {
      const char* up = kViFold[i].upper;
      const size_t ulen = std::char_traits<char>::length(up);
      if (s.compare(pos, ulen, up) == 0) {
        out += kViFold[i].lower;
        pos += ulen;
        matched = true;
        break;
      }
    }
    if (!matched) {  // copy the code point unchanged
      const size_t before = pos;
      const uint32_t cp = next_codepoint(s, pos);
      if (pos == before) ++pos;
      append_codepoint(out, cp);
    }
  }
  return out;
}

bool contains_ci(const std::string& haystack, const std::string& needle) {
  if (needle.empty()) return true;
  return fold(haystack).find(fold(needle)) != std::string::npos;
}

bool equals_ci(const std::string& a, const std::string& b) { return fold(a) == fold(b); }

std::vector<std::string> split(const std::string& s, char delim) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) {
    if (c == delim) {
      out.push_back(cur);
      cur.clear();
    } else {
      cur += c;
    }
  }
  out.push_back(cur);
  return out;
}

std::string join(const std::vector<std::string>& parts, const std::string& sep) {
  std::string out;
  for (size_t i = 0; i < parts.size(); ++i) {
    if (i) out += sep;
    out += parts[i];
  }
  return out;
}

std::string trim(const std::string& s) {
  size_t b = 0, e = s.size();
  while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\n' || s[b] == '\r')) ++b;
  while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\n' || s[e - 1] == '\r')) --e;
  return s.substr(b, e - b);
}

bool starts_with(const std::string& s, const std::string& prefix) {
  return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

bool ends_with(const std::string& s, const std::string& suffix) {
  return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

}  // namespace yui::text
