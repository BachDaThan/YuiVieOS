// YUI-LZ — the built-in block codec behind ZRAM (Tầng 2) and Trình Gọt Tỉa &
// Nén App (Tầng 5).  A byte-oriented LZ77 with a 64 KiB window, tuned for the
// "compress in a few hundred microseconds, decompress at memcpy speed" budget
// that an in-RAM swap device needs.
//
// Wire format (little-endian, see docs/FORMAT-YUIP.md):
//   token = (lit_len << 4) | (match_len - 4)      one byte, either nibble 15
//                                                  means "read extension bytes"
//   [lit extension: 0xFF* then remainder]
//   literal bytes
//   [if not the final sequence] 2-byte match distance
//   [match extension: 0xFF* then remainder]
//
// Container ("YUIP" pack, used for trimmed apps):
//   magic 'Y','U','I','P' | version | codec id | flags | orig size (u64) |
//   payload size (u64) | sha-256 of the original bytes (32) | crc32 (u32) | payload
//
// The SHA-256 guards the *restored* content; the CRC32 additionally guards the
// compressed payload byte-for-byte, so even a mutation that happens to decode
// to the same bytes (e.g. an unused trailing token nibble) is rejected.
#ifndef YUI_CODEC_HPP
#define YUI_CODEC_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace yui::codec {

using Bytes = std::vector<uint8_t>;

// Upper bound on the compressed size of `n` bytes (useful for buffer sizing).
size_t bound(size_t n);

// Returns false when the input is incompressible enough that the encoded form
// would be larger than `bound`; `out` then holds the raw LZ stream anyway.
bool compress(const uint8_t* in, size_t in_len, Bytes& out, int level = 6);
bool decompress(const uint8_t* in, size_t in_len, uint8_t* out, size_t out_len);
// Convenience wrappers that carry the original length in the payload header.
Bytes compress(const Bytes& in, int level = 6);
bool decompress(const Bytes& in, size_t original_len, Bytes& out);

// ---- container ("YUIP") ----
constexpr uint32_t kPackMagic = 0x50495559;  // 'Y','U','I','P'
constexpr uint8_t kPackVersion = 1;
constexpr uint8_t kCodecYuiLz = 1;
constexpr uint8_t kCodecStored = 0;  // pass-through (already compressed input)
constexpr size_t kPackHeaderSize = 4 + 1 + 1 + 2 + 8 + 8 + 32 + 4;

struct PackInfo {
  uint8_t version = 0;
  uint8_t codec = 0;
  uint16_t flags = 0;
  uint64_t original_size = 0;
  uint64_t payload_size = 0;
  std::string digest_hex;  // sha-256 of the original content
};

Bytes pack(const Bytes& input, int level = 6);
// Verifies magic, length and the SHA-256 of the restored content.
bool unpack(const Bytes& packed, Bytes& out, PackInfo* info = nullptr, std::string* error = nullptr);

// Ratio helper for reporting: 1 - compressed/original.
double ratio(size_t original, size_t compressed);
uint32_t crc32(const uint8_t* data, size_t len);

}  // namespace yui::codec

#endif  // YUI_CODEC_HPP
