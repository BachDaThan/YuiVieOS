// yui_ime — the Vietnamese input engine used by the Tầng 5 shell and by any
// app that links libyui.  Pure logic: it takes a keystroke string and returns
// composed Vietnamese text, implementing the two national input methods.
//
//   Telex:  aa→â  aw→ă  ee→ê  oo→ô  ow→ơ  uw→ư  dd→đ
//           s→sắc  f→huyền  r→hỏi  x→ngã  j→nặng  z→xoá thanh
//   VNI:    6→dấu mũ (a6→â, e6→ê, o6→ô), 7→dấu móc (o7→ơ, u7→ư),
//           8→trăng (a8→ă), 9→đ, 1..5→thanh điệu, 0→xoá thanh
//
// Tone placement follows the standard orthographic rule (see ime.cpp):
// modified vowels win, then a closed syllable puts the mark on the last vowel,
// an open two-vowel cluster on the first, and a three-vowel cluster in the middle.
#ifndef YUI_IME_HPP
#define YUI_IME_HPP

#include <cstdint>
#include <string>
#include <vector>

namespace yui::ime {

enum class Method { Telex, Vni };
enum Tone { kNgang = 0, kSac = 1, kHuyen = 2, kHoi = 3, kNga = 4, kNang = 5 };

// Convert a whole keystroke string.  Spaces and punctuation act as word
// boundaries and are passed through unchanged.
std::string convert(const std::string& keystrokes, Method method = Method::Telex);
inline std::string telex(const std::string& keys) { return convert(keys, Method::Telex); }
inline std::string vni(const std::string& keys) { return convert(keys, Method::Vni); }

// Convert a single word (no separators).  Exposed because a keyboard hook needs
// to re-render just the word being typed.
std::string convert_word(const std::string& keystrokes, Method method = Method::Telex);

// Stateful word-level processor, for a real keyboard hook where keys arrive one
// at a time and the app needs to know what to replace.
class WordEngine {
 public:
  explicit WordEngine(Method m = Method::Telex) : method_(m) {}
  // Feed one keystroke.  Returns the composed text for the whole current word,
  // or an empty string when the keystroke ended the word.
  std::string feed(uint32_t codepoint);
  std::string current() const { return convert_word(keys_, method_); }
  void reset() { keys_.clear(); }
  bool empty() const { return keys_.empty(); }
  Method method() const { return method_; }
  void set_method(Method m) {
    method_ = m;
    reset();
  }
  // Index (into the letters of `base_letters`) that would receive the tone mark.
  static int tone_position(const std::string& base_letters);

 private:
  Method method_;
  std::string keys_;
};

// Editable composition buffer for desktop input-method adapters.  It retains
// the original ASCII keystrokes so backspace can recompute Telex/VNI correctly
// instead of trying to reverse Vietnamese Unicode transformations.
class Composition {
 public:
  explicit Composition(Method m = Method::Telex) : method_(m) {}

  // Append an ASCII letter or digit. Punctuation/non-ASCII input is left for
  // the adapter to commit the current composition and forward to the client.
  bool feed(uint32_t codepoint);
  bool backspace();
  std::string preedit() const { return convert_word(keys_, method_); }
  std::string commit();
  void reset() { keys_.clear(); }
  bool empty() const { return keys_.empty(); }
  Method method() const { return method_; }
  void set_method(Method m) {
    method_ = m;
    reset();
  }

 private:
  Method method_;
  std::string keys_;
};

}  // namespace yui::ime

#endif  // YUI_IME_HPP
