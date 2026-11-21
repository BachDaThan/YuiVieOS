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
//
// Protected apps (P1.6): before touching anything, the tree is scanned for
// copy-protection / integrity markers (Widevine CDM, VMP/integrity `.sig` next
// to a binary, VMProtect/Themida/WinLicense/Enigma/SteamStub sections in ELF or
// PE).  If any is found the trimmer switches to verbatim mode: nothing is
// dropped, zeroed or deduped, the report says `stripped == false` with the
// reasons, and the package only holds a lossless copy (0 bytes modified).
// Modifying such files breaks the app at best and may breach its licence.
#ifndef YUI_APPPACK_HPP
#define YUI_APPPACK_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "yui/json.hpp"

namespace yui::sandbox {
struct Policy;
}

namespace yui::apppack {

using Bytes = std::vector<uint8_t>;

struct TrimOptions {
  std::vector<std::string> keep_locales{"vi", "en"};
  bool strip_docs = false;         // share/doc, share/man, *.md
  bool strip_debug_elf = true;     // zero-fill .debug_*/.comment/.note* sections
  bool dedupe = true;              // content-addressed duplicate removal
  // node_modules/, .venv/, target/, build/: build products of a *source* tree,
  // but runtime code inside a shipped app (Electron `resources/app/node_modules`,
  // `app.asar.unpacked/node_modules`).  Off by default since P1.6.
  bool strip_build_dirs = false;
  bool verbatim = false;           // keep every byte (forced on for protected apps)
  bool detect_protection = true;   // scan for DRM/integrity markers first
  int level = 7;                   // YUI-LZ effort
};

// One copy-protection / integrity marker found in a tree.
struct ProtectionMarker {
  std::string path;     // relative path of the file that carries the marker
  std::string reason;   // short Vietnamese reason, e.g. "Widevine CDM (DRM)"
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
  bool stripped = true;       // false ⇒ verbatim copy, 0 bytes modified (protected app)
  std::vector<ProtectionMarker> protection;
  size_t specials_skipped = 0;  // symlinks, sockets, FIFOs (never archived)
  std::vector<std::string> dropped_samples;
  std::vector<std::string> warnings;
  std::string summary_vi() const;
};

// Scans a tree for copy-protection / integrity markers (see header comment).
// VCS metadata directories are not scanned.  Results are sorted by path.
std::vector<ProtectionMarker> detect_protection(const std::string& root);

// Section names of an ELF64 or PE/COFF image ({} when neither / malformed).
std::vector<std::string> binary_section_names(const Bytes& image);

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

// --------------------------------- Proposed Policy Generator (P3.15) --
enum class ProposedScopeSource {
  Manifest,       // (a) Khai báo trong manifest
  ScannedString,  // (b) Quét từ chuỗi tĩnh trong bộ cài (độ tin cậy thấp)
  SandboxTrial    // (c) Ghi nhận khi chạy thử trong hộp cát ("chặn và ghi")
};

const char* proposed_scope_source_name(ProposedScopeSource s);
const char* proposed_scope_source_name_vi(ProposedScopeSource s);

struct ProposedTarget {
  std::string target;
  ProposedScopeSource source = ProposedScopeSource::Manifest;
  std::string confidence;  // "high", "low", "observed"
  std::string reason_vi;
};

struct ProposedPolicy {
  int version = 1;
  std::string app_id;
  std::string tier = "microvm";
  bool enabled = false;  // Không bao giờ tự bật; người dùng duyệt từng đích
  std::vector<std::string> capabilities;
  std::vector<ProposedTarget> targets;
  std::vector<std::string> deny;

  json::Value to_json() const;
  std::string to_json_string(bool pretty = true) const;
  static ProposedPolicy from_json(const json::Value& v, std::string* error = nullptr);

  // Chuyển thành Policy của sandbox. Deny luôn thắng allow.
  sandbox::Policy to_sandbox_policy(bool approve_all = false,
                                    const std::vector<std::string>& approved_targets = {}) const;
};

// Trích xuất các tên miền tiềm năng từ dữ liệu tệp
std::vector<std::string> extract_domains_from_data(const Bytes& data);

// Sinh chính sách đề xuất từ 3 nguồn: manifest, quét chuỗi tĩnh, và chạy thử hộp cát
ProposedPolicy generate_proposed_policy(
    const std::string& app_id,
    const std::string& manifest_json,
    const std::vector<Entry>& entries,
    const std::vector<std::string>& sandbox_trial_blocked_domains,
    const std::vector<std::string>& initial_deny = {});

}  // namespace yui::apppack

#endif  // YUI_APPPACK_HPP
