// YUI-LZ encoder/decoder + YUIP container.
#include "yui/codec.hpp"

#include <cstring>

#include "yui/crypto.hpp"

namespace yui::codec {
namespace {

constexpr size_t kMinMatch = 4;
constexpr size_t kMaxDistance = 65535;
constexpr size_t kMaxLitNibble = 15;
constexpr size_t kHashBits = 16;
constexpr size_t kHashSize = size_t(1) << kHashBits;

inline uint32_t hash4(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  v *= 2654435761u;  // Knuth multiplicative hash
  return v >> (32 - kHashBits);
}

inline void put_len(Bytes& out, size_t extra) {
  while (extra >= 255) {
    out.push_back(255);
    extra -= 255;
  }
  out.push_back(uint8_t(extra));
}

inline void put_u16(Bytes& out, uint16_t v) {
  out.push_back(uint8_t(v & 0xff));
  out.push_back(uint8_t(v >> 8));
}
inline void put_u64(Bytes& out, uint64_t v) {
  for (int i = 0; i < 8; ++i) out.push_back(uint8_t((v >> (8 * i)) & 0xff));
}
inline uint16_t get_u16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
inline uint64_t get_u64(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
  return v;
}

}  // namespace

size_t bound(size_t n) { return n + n / 255 + 16; }

uint32_t crc32(const uint8_t* data, size_t len) {
  static uint32_t table[256];
  static bool init = false;
  if (!init) {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
      table[i] = c;
    }
    init = true;
  }
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < len; ++i) c = table[(c ^ data[i]) & 0xff] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}

double ratio(size_t original, size_t compressed) {
  if (original == 0) return 0.0;
  return 1.0 - double(compressed) / double(original);
}

bool compress(const uint8_t* in, size_t in_len, Bytes& out, int level) {
  out.clear();
  out.reserve(bound(in_len));
  if (in_len == 0) {
    out.push_back(0);  // zero literals, no match
    return true;
  }
  // `level` 0..9 controls how many hash-chain candidates we probe.  Level 6 is
  // the ZRAM default; level 0 degenerates to a pure literal stream (store mode).
  const int search_depth = level <= 0 ? 0 : (level <= 3 ? 1 : (level <= 6 ? 4 : 16));

  std::vector<int32_t> head(kHashSize, -1);
  std::vector<int32_t> prev(in_len, -1);

  size_t anchor = 0;  // start of the pending literal run
  size_t pos = 0;

  while (pos + kMinMatch <= in_len) {
    const uint32_t h = hash4(in + pos);
    const int32_t candidate_start = head[h];
    head[h] = int32_t(pos);
    prev[pos] = candidate_start;

    size_t best_len = 0, best_dist = 0;
    int32_t candidate = candidate_start;
    int probes = search_depth;
    const size_t limit = in_len - pos;
    while (candidate >= 0 && probes-- > 0) {
      const size_t dist = pos - size_t(candidate);
      if (dist == 0 || dist > kMaxDistance) break;
      if (best_len < limit) {
        size_t len = 0;
        while (len < limit && in[size_t(candidate) + len] == in[pos + len]) ++len;
        if (len > best_len) {
          best_len = len;
          best_dist = dist;
          if (len >= 128) break;  // good enough, stop probing
        }
      }
      candidate = prev[size_t(candidate)];
    }

    if (best_len >= kMinMatch) {
      const size_t lit_len = pos - anchor;
      const size_t match_extra = best_len - kMinMatch;
      out.push_back(uint8_t((std::min<size_t>(lit_len, kMaxLitNibble) << 4) |
                            std::min<size_t>(match_extra, kMaxLitNibble)));
      if (lit_len >= kMaxLitNibble) put_len(out, lit_len - kMaxLitNibble);
      out.insert(out.end(), in + anchor, in + pos);
      put_u16(out, uint16_t(best_dist));
      if (match_extra >= kMaxLitNibble) put_len(out, match_extra - kMaxLitNibble);
      // Register the skipped positions so later matches can still find them.
      for (size_t p = pos + 1; p < pos + best_len && p + kMinMatch <= in_len; ++p) {
        const uint32_t hh = hash4(in + p);
        prev[p] = head[hh];
        head[hh] = int32_t(p);
      }
      pos += best_len;
      anchor = pos;
    } else {
      ++pos;
    }
  }

  // Final literals-only sequence: no distance field follows.
  const size_t lit_len = in_len - anchor;
  out.push_back(uint8_t(std::min<size_t>(lit_len, kMaxLitNibble) << 4));
  if (lit_len >= kMaxLitNibble) put_len(out, lit_len - kMaxLitNibble);
  out.insert(out.end(), in + anchor, in + in_len);
  return true;
}

bool decompress(const uint8_t* in, size_t in_len, uint8_t* out, size_t out_len) {
  size_t ip = 0, op = 0;
  while (ip < in_len) {
    const uint8_t token = in[ip++];
    size_t lit_len = token >> 4;
    if (lit_len == kMaxLitNibble) {
      while (true) {
        if (ip >= in_len) return false;
        const uint8_t b = in[ip++];
        lit_len += b;
        if (b != 255) break;
      }
    }
    if (ip + lit_len > in_len || op + lit_len > out_len) return false;
    std::memcpy(out + op, in + ip, lit_len);
    ip += lit_len;
    op += lit_len;
    if (ip == in_len) break;  // final sequence: no match follows
    if (ip + 2 > in_len) return false;
    const uint16_t dist = get_u16(in + ip);
    ip += 2;
    size_t match_len = size_t(token & 0x0f) + kMinMatch;
    if ((token & 0x0f) == kMaxLitNibble) {
      while (true) {
        if (ip >= in_len) return false;
        const uint8_t b = in[ip++];
        match_len += b;
        if (b != 255) break;
      }
    }
    if (dist == 0 || dist > op) return false;
    if (op + match_len > out_len) return false;
    for (size_t i = 0; i < match_len; ++i, ++op) out[op] = out[op - dist];
  }
  return op == out_len;
}

Bytes compress(const Bytes& in, int level) {
  Bytes out;
  compress(in.data(), in.size(), out, level);
  return out;
}

bool decompress(const Bytes& in, size_t original_len, Bytes& out) {
  out.assign(original_len, 0);
  if (original_len == 0) return true;
  return decompress(in.data(), in.size(), out.data(), original_len);
}

Bytes pack(const Bytes& input, int level) {
  Bytes compressed;
  compress(input.data(), input.size(), compressed, level);
  // If the codec cannot beat the raw size, store the original verbatim.
  uint8_t codec_id = kCodecYuiLz;
  Bytes payload = compressed;
  if (payload.size() >= input.size()) {
    codec_id = kCodecStored;
    payload = input;
  }
  const auto digest = crypto::sha256(input);

  Bytes out;
  out.reserve(kPackHeaderSize + payload.size());
  out.push_back('Y');
  out.push_back('U');
  out.push_back('I');
  out.push_back('P');
  out.push_back(kPackVersion);
  out.push_back(codec_id);
  put_u16(out, 0);  // flags
  put_u64(out, input.size());
  put_u64(out, payload.size());
  out.insert(out.end(), digest.begin(), digest.end());
  const uint32_t crc = crc32(payload.data(), payload.size());
  for (int i = 0; i < 4; ++i) out.push_back(uint8_t((crc >> (8 * i)) & 0xff));
  out.insert(out.end(), payload.begin(), payload.end());
  return out;
}

bool unpack(const Bytes& packed, Bytes& out, PackInfo* info, std::string* error) {
  auto err = [&](const char* msg) {
    if (error) *error = msg;
    out.clear();
    return false;
  };
  if (packed.size() < kPackHeaderSize) return err("payload shorter than YUIP header");
  if (packed[0] != 'Y' || packed[1] != 'U' || packed[2] != 'I' || packed[3] != 'P')
    return err("bad magic (not a YUIP container)");
  if (packed[4] != kPackVersion) return err("unsupported container version");
  const uint8_t codec_id = packed[5];
  const uint16_t flags = get_u16(&packed[6]);
  const uint64_t original = get_u64(&packed[8]);
  const uint64_t psize = get_u64(&packed[16]);
  if (packed.size() != kPackHeaderSize + psize) return err("payload length mismatch");
  if (original > (1ull << 34)) return err("implausible original size");

  const uint8_t* payload = packed.data() + kPackHeaderSize;
  uint32_t stored_crc = 0;
  for (int i = 3; i >= 0; --i) stored_crc = (stored_crc << 8) | packed[56 + size_t(i)];
  if (crc32(payload, size_t(psize)) != stored_crc) return err("payload CRC32 mismatch (corrupt container)");
  Bytes plain;
  switch (codec_id) {
    case kCodecStored:
      if (psize != original) return err("stored payload size mismatch");
      plain.assign(payload, payload + psize);
      break;
    case kCodecYuiLz:
      plain.assign(size_t(original), 0);
      if (original && !decompress(payload, size_t(psize), plain.data(), size_t(original)))
        return err("YUI-LZ stream is corrupt");
      break;
    default: return err("unknown codec id");
  }

  const auto digest = crypto::sha256(plain);
  if (!crypto::ct_equal(digest.data(), packed.data() + 24, 32)) return err("SHA-256 mismatch (corrupt or tampered)");
  if (info) {
    info->version = packed[4];
    info->codec = codec_id;
    info->flags = flags;
    info->original_size = original;
    info->payload_size = psize;
    info->digest_hex = crypto::hex(digest);
  }
  out = std::move(plain);
  return true;
}

}  // namespace yui::codec
