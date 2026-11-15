// Trình Gọt Tỉa & Nén App (Tầng 5).
//
// What it actually does, in order:
//   1. walk the tree and classify every file;
//   2. drop what the OS never executes: VCS metadata, build caches, *.debug
//      symbols, locales outside the keep-list, optionally docs;
//   3. zero-fill ELF debug/comment/note sections (keeps the binary runnable and
//      shrinks beautifully once compressed);
//   4. content-address every remaining file and turn byte-identical copies into
//      dedupe references instead of storing them twice;
//   5. archive the result and compress it with YUI-LZ inside a YUIP container.
//
// Nothing here guesses: every dropped byte is counted in the report so the shell
// can show a Vietnamese summary of exactly what was removed.
#ifndef YUI_APPPACK_HPP
#define YUI_APPPACK_HPP

#include <cstdint>
#include <string>
#include <vector>

namespace yui::apppack {

using Bytes = std::vector<uint8_t>;

struct TrimOptions {
  std::vector<std::string> keep_locales{"vi", "en"};
  bool strip_docs = false;         // share/doc, share/man, *.md
  bool strip_debug_elf = true;     // zero-fill .debug_*/.comment/.note* sections
  bool dedupe = true;              // content-addressed duplicate removal
  int level = 7;                   // YUI-LZ effort
};

struct Entry {
  std::string path;                // relative to the tree root, '/'-separated
  uint32_t mode = 0644;
  uint64_t size = 0;               // original size
  Bytes data;                      // file content, or the target path for 'd'
  uint8_t kind = 'f';              // 'f' file, 'd' dedupe link, 'x' executable
};

struct TrimReport {
  size_t files_in = 0;        // regular files visited (skipped subtrees are not walked)
  size_t files_kept = 0;
  size_t files_dropped = 0;
  size_t duplicates = 0;
  uint64_t bytes_in = 0;
  uint64_t bytes_kept = 0;
  uint64_t bytes_after_dedupe = 0;
  uint64_t bytes_debug_zeroed = 0;
  uint64_t bytes_packed = 0;
  std::vector<std::string> dropped_samples;
  std::vector<std::string> warnings;
  std::string summary_vi() const;
};

// Collect + trim without packing (useful for previews and tests).
bool collect(const std::string& root, const TrimOptions& opts, std::vector<Entry>& out, TrimReport& rep,
             std::string* error = nullptr);

// Archive (de)serialisation: "YUIA" v1 container.
Bytes build_archive(const std::vector<Entry>& entries);
bool parse_archive(const Bytes& archive, std::vector<Entry>& out, std::string* error = nullptr);
bool extract(const std::vector<Entry>& entries, const std::string& dest, std::string* error = nullptr);

// End-to-end: tree -> .yuip bytes -> tree.
Bytes pack_tree(const std::string& root, const TrimOptions& opts, TrimReport& rep, std::string* error = nullptr);
bool unpack_to(const Bytes& packed, const std::string& dest, std::string* error = nullptr);

// Zero-fills debug/comment/note section bodies in an ELF64 image.  Returns the
// number of bytes blanked (0 when the input is not ELF64 or has no such
// sections).  The image stays loadable because section bodies are only touched
// outside of program-header coverage.
uint64_t strip_elf_debug(Bytes& image);

}  // namespace yui::apppack

#endif  // YUI_APPPACK_HPP
