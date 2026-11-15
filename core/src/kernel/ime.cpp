// Vietnamese Telex/VNI input engine (Tầng 5, "Bộ gõ Telex/VNI tầng dịch vụ").
//
// Pipeline per word: keystrokes -> units (letters with vowel modifiers, no tone)
// -> tone application -> composed NFC text.  Everything is recomputed for the
// whole word on each keystroke, which keeps the engine stateless in practice and
// makes the tone mark "move" correctly as the user keeps typing.
#include "yui/ime.hpp"

#include <cctype>
#include <vector>

#include "yui/text.hpp"

namespace yui::ime {
namespace {

#include "vi_vowels.inc"

// Form indexes into kVowels[].
enum Form {
  A = 0, ABREVE = 1, ACIRC = 2, E = 3, ECIRC = 4, I = 5,
  O = 6, OCIRC = 7, OHORN = 8, U = 9, UHORN = 10, Y = 11
};

constexpr int kVowelCount = int(sizeof(kVowels) / sizeof(kVowels[0]));
static_assert(kVowelCount == 12, "vowel table must cover 12 Vietnamese vowel forms");

bool is_modified(int form) {
  return form == ABREVE || form == ACIRC || form == ECIRC || form == OCIRC || form == OHORN ||
         form == UHORN;
}

int plain_form(char c) {
  switch (c) {
    case 'a': return A;
    case 'e': return E;
    case 'i': return I;
    case 'o': return O;
    case 'u': return U;
    case 'y': return Y;
    default: return -1;
  }
}

bool is_vowel_char(char c) { return plain_form(c) >= 0; }

struct Unit {
  bool vowel = false;
  int form = 0;     // vowel form index (valid when vowel == true)
  uint32_t ch = 0;  // consonant code point ('d' == 0x64, đ == 0x111)
  bool upper = false;
};

// Orthographic tone placement.  Returns the unit index that carries the mark.
size_t pick_tone_index(const std::vector<Unit>& units) {
  std::vector<size_t> vowels;
  for (size_t i = 0; i < units.size(); ++i)
    if (units[i].vowel) vowels.push_back(i);
  if (vowels.empty()) return 0;

  size_t from = 0;
  // "qu" and "gi": the glide does not take the mark when another vowel follows.
  if (units.size() >= 2 && units[0].ch == 'q' && units[1].vowel && units[1].form == U &&
      vowels.size() >= 2 && vowels[0] == 1)
    from = 1;
  else if (units.size() >= 2 && units[0].ch == 'g' && units[1].vowel && units[1].form == I &&
           vowels.size() >= 2 && vowels[0] == 1)
    from = 1;

  std::vector<size_t> cluster(vowels.begin() + long(from), vowels.end());

  // 1. A modified vowel always wins (the last one, for digraphs like ươ, uyê).
  for (auto it = cluster.rbegin(); it != cluster.rend(); ++it)
    if (is_modified(units[*it].form)) return *it;

  // 2. Otherwise use the shape of the syllable.
  bool closed = false;
  for (size_t i = cluster.back() + 1; i < units.size(); ++i)
    if (!units[i].vowel) closed = true;
  if (cluster.size() >= 3) return cluster[1];
  if (cluster.size() == 2) return closed ? cluster[1] : cluster[0];
  return cluster[0];
}

std::string render(const std::vector<Unit>& units, int tone) {
  if (units.empty()) return "";
  bool has_vowel = false;
  for (const auto& u : units)
    if (u.vowel) has_vowel = true;
  const size_t tone_idx = has_vowel ? pick_tone_index(units) : 0;

  std::string out;
  for (size_t i = 0; i < units.size(); ++i) {
    const Unit& u = units[i];
    if (u.vowel) {
      const int t = (i == tone_idx) ? tone : int(kNgang);
      out += u.upper ? kVowels[u.form].upper[t] : kVowels[u.form].lower[t];
    } else if (u.ch == 0x111) {
      out += u.upper ? "\xC4\x90" : "\xC4\x91";  // Đ / đ
    } else {
      out += char(u.upper ? std::toupper(int(u.ch)) : int(u.ch));
    }
  }
  return out;
}

// Appends (or transforms) one already-lowercased letter.  `modifier_ok` letters
// are the ones the input method treats as text, not as commands.
void push_letter(std::vector<Unit>& units, char c, bool upper) {
  Unit u;
  u.ch = uint32_t(c);
  u.upper = upper;
  units.push_back(u);
}

void push_vowel(std::vector<Unit>& units, int form, bool upper) {
  Unit u;
  u.vowel = true;
  u.form = form;
  u.upper = upper;
  units.push_back(u);
}

int telex_tone_key(char c) {
  switch (c) {
    case 's': return kSac;
    case 'f': return kHuyen;
    case 'r': return kHoi;
    case 'x': return kNga;
    case 'j': return kNang;
    default: return -1;
  }
}

int vni_tone_key(char c) {
  switch (c) {
    case '1': return kSac;
    case '2': return kHuyen;
    case '3': return kHoi;
    case '4': return kNga;
    case '5': return kNang;
    default: return -1;
  }
}

// Telex text transformation: dd, aa, ee, oo, aw, ow, uw, and bare w -> ư.
void telex_text(std::vector<Unit>& units, char c, bool upper) {
  if (c == 'w') {
    if (!units.empty() && units.back().vowel) {
      Unit& last = units.back();
      if (last.form == A) { last.form = ABREVE; return; }
      if (last.form == O) { last.form = OHORN; return; }
      if (last.form == U) { last.form = UHORN; return; }
    }
    push_vowel(units, UHORN, upper);
    return;
  }
  if (is_vowel_char(c)) {
    const int base = plain_form(c);
    if (!units.empty() && units.back().vowel && units.back().form == base) {
      Unit& last = units.back();
      if (base == A) { last.form = ACIRC; return; }
      if (base == E) { last.form = ECIRC; return; }
      if (base == O) { last.form = OCIRC; return; }
      // "ii", "uu", "yy" are not Vietnamese: emit the second letter literally.
    }
    push_vowel(units, base, upper);
    return;
  }
  if (c == 'd' && !units.empty() && !units.back().vowel && units.back().ch == 'd') {
    units.back().ch = 0x111;
    return;
  }
  push_letter(units, c, upper);
}

// VNI modifiers.  Returns true when the character was consumed as a modifier.
bool vni_modifier(std::vector<Unit>& units, int& tone, char c) {
  if (c == '0') {
    tone = kNgang;
    return true;
  }
  const int t = vni_tone_key(c);
  if (t >= 0) {
    tone = t;
    return true;
  }
  if (units.empty()) return false;
  Unit& last = units.back();
  if (c == '6') {  // dấu mũ
    if (!last.vowel) return false;
    if (last.form == A) last.form = ACIRC;
    else if (last.form == E) last.form = ECIRC;
    else if (last.form == O) last.form = OCIRC;
    else return false;
    return true;
  }
  if (c == '7') {  // dấu móc
    if (!last.vowel) return false;
    if (last.form == O) last.form = OHORN;
    else if (last.form == U) last.form = UHORN;
    else return false;
    return true;
  }
  if (c == '8') {  // dấu trăng
    if (!last.vowel || last.form != A) return false;
    last.form = ABREVE;
    return true;
  }
  if (c == '9') {  // đ
    if (last.vowel || last.ch != 'd') return false;
    last.ch = 0x111;
    return true;
  }
  return false;
}

}  // namespace

std::string convert_word(const std::string& keystrokes, Method method) {
  std::vector<Unit> units;
  int tone = kNgang;
  std::string out;

  auto flush = [&]() {
    out += render(units, tone);
    units.clear();
    tone = kNgang;
  };
  auto has_vowel = [&]() {
    for (const auto& u : units)
      if (u.vowel) return true;
    return false;
  };

  for (size_t pos = 0; pos < keystrokes.size();) {
    const unsigned char c = static_cast<unsigned char>(keystrokes[pos]);
    if (c >= 0x80) {  // pre-composed input: flush what we have and copy verbatim
      flush();
      const size_t start = pos;
      text::next_codepoint(keystrokes, pos);
      out += keystrokes.substr(start, pos - start);
      continue;
    }
    ++pos;
    const char ch = char(c);
    const bool upper = ch >= 'A' && ch <= 'Z';
    const char lc = char(std::tolower(int(ch)));
    const bool vowel_present = has_vowel();

    if (method == Method::Telex) {
      // A tone key only acts as a command once the word has a vowel to mark;
      // otherwise "xin" would turn into "xĩn".
      const int t = telex_tone_key(lc);
      if (t >= 0 && vowel_present) {
        tone = t;
        continue;
      }
      if (lc == 'z' && vowel_present) {  // xoá thanh
        tone = kNgang;
        continue;
      }
      telex_text(units, lc, upper);
      continue;
    }

    // VNI digits are modifiers inside a word; "9" also applies to a bare "d".
    if (std::isdigit(c) && (vowel_present || lc == '9') && vni_modifier(units, tone, lc)) continue;
    telex_text(units, lc, upper);
  }
  flush();
  return out;
}

std::string convert(const std::string& keystrokes, Method method) {
  std::string out;
  std::string word;
  auto flush = [&]() {
    if (!word.empty()) {
      out += convert_word(word, method);
      word.clear();
    }
  };
  for (size_t pos = 0; pos < keystrokes.size();) {
    const unsigned char c = static_cast<unsigned char>(keystrokes[pos]);
    if (c >= 0x80) {  // pass UTF-8 through untouched
      flush();
      const size_t start = pos;
      text::next_codepoint(keystrokes, pos);
      out += keystrokes.substr(start, pos - start);
      continue;
    }
    ++pos;
    if (std::isalnum(c)) {
      word += char(c);
    } else {
      flush();
      out += char(c);
    }
  }
  flush();
  return out;
}

std::string WordEngine::feed(uint32_t codepoint) {
  const bool boundary = codepoint == ' ' || codepoint == '\t' || codepoint == '\n' ||
                        codepoint == '\r' || codepoint == ',' || codepoint == '.' ||
                        codepoint == '!' || codepoint == '?' || codepoint == ';' ||
                        codepoint == ':' || codepoint == '"' || codepoint == '(' ||
                        codepoint == ')' || codepoint == '-' || codepoint == '/';
  if (boundary) {
    keys_.clear();
    return "";
  }
  if (codepoint < 0x80 && std::isalnum(int(codepoint))) {
    keys_ += char(codepoint);
    return current();
  }
  // Anything else is passed straight through; it also terminates the word.
  keys_.clear();
  std::string s;
  text::append_codepoint(s, codepoint);
  return s;
}

int WordEngine::tone_position(const std::string& base_letters) {
  std::vector<Unit> units;
  for (char c : base_letters) {
    const char lc = char(std::tolower(int(c)));
    if (is_vowel_char(lc))
      push_vowel(units, plain_form(lc), false);
    else
      push_letter(units, lc, false);
  }
  bool any_vowel = false;
  for (const auto& u : units)
    if (u.vowel) any_vowel = true;
  if (!any_vowel) return -1;
  return int(pick_tone_index(units));
}

}  // namespace yui::ime
