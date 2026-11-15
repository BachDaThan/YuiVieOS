// yui_text — UTF-8 helpers with first-class Vietnamese support.
//
// Used by the graph search, the bilingual shell and the Telex/VNI input engine.
// The Vietnamese case-folding table is generated from Unicode's own case
// mappings (74 uppercase → lowercase pairs covering every Vietnamese letter and
// tone mark), so "TRÌNH" folds to "trình" as a Vietnamese reader expects.
#ifndef YUI_TEXT_HPP
#define YUI_TEXT_HPP

#include <cstdint>
#include <string>
#include <vector>

namespace yui::text {

// Decode one UTF-8 code point at `pos`, advancing past it.  Invalid bytes are
// returned as U+FFFD and advance by one byte.
uint32_t next_codepoint(const std::string& s, size_t& pos);
void append_codepoint(std::string& out, uint32_t cp);
size_t length(const std::string& s);          // in code points
size_t display_width(const std::string& s);   // rough terminal width (combining marks = 0)

bool is_vietnamese_letter(uint32_t cp);
// Lowercase ASCII + Vietnamese.  Other scripts pass through unchanged.
std::string fold(const std::string& s);
bool contains_ci(const std::string& haystack, const std::string& needle);
bool equals_ci(const std::string& a, const std::string& b);
std::vector<std::string> split(const std::string& s, char delim);
std::string join(const std::vector<std::string>& parts, const std::string& sep);
std::string trim(const std::string& s);
bool starts_with(const std::string& s, const std::string& prefix);
bool ends_with(const std::string& s, const std::string& suffix);

}  // namespace yui::text

#endif  // YUI_TEXT_HPP
