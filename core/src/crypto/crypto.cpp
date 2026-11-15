// SHA-256, HMAC-SHA-256, Keccak/SHAKE-256, scrypt + small helpers.
// Clean-room, dependency-free.  See crypto.hpp for the contract.
#include "yui/crypto.hpp"

#include <cstdio>
#include <cstring>
#include <stdexcept>

#if defined(__linux__)
#include <sys/random.h>
#endif

namespace yui::crypto {

// ---------------------------------------------------------------- SHA-256 --
namespace {
constexpr uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

inline uint32_t rotr32(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
inline uint32_t rotl32(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }
inline uint32_t load_be32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}
inline void store_be32(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v);
}
inline void store_be64(uint8_t* p, uint64_t v) {
  for (int i = 7; i >= 0; --i) { p[i] = uint8_t(v); v >>= 8; }
}
}  // namespace

Sha256::Sha256() { reset(); }

void Sha256::reset() {
  h_[0] = 0x6a09e667; h_[1] = 0xbb67ae85; h_[2] = 0x3c6ef372; h_[3] = 0xa54ff53a;
  h_[4] = 0x510e527f; h_[5] = 0x9b05688c; h_[6] = 0x1f83d9ab; h_[7] = 0x5be0cd19;
  buf_len_ = 0;
  total_len_ = 0;
}

void Sha256::compress(const uint8_t block[64]) {
  uint32_t w[64];
  for (int i = 0; i < 16; ++i) w[i] = load_be32(block + 4 * i);
  for (int i = 16; i < 64; ++i) {
    const uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3];
  uint32_t e = h_[4], f = h_[5], g = h_[6], h = h_[7];
  for (int i = 0; i < 64; ++i) {
    const uint32_t S1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
    const uint32_t ch = (e & f) ^ (~e & g);
    const uint32_t t1 = h + S1 + ch + K256[i] + w[i];
    const uint32_t S0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
    const uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
    const uint32_t t2 = S0 + mj;
    h = g; g = f; f = e; e = d + t1;
    d = c; c = b; b = a; a = t1 + t2;
  }
  h_[0] += a; h_[1] += b; h_[2] += c; h_[3] += d;
  h_[4] += e; h_[5] += f; h_[6] += g; h_[7] += h;
}

void Sha256::update(const void* data, size_t len) {
  const uint8_t* p = static_cast<const uint8_t*>(data);
  total_len_ += len;
  while (len > 0) {
    const size_t take = (len < 64 - buf_len_) ? len : (64 - buf_len_);
    std::memcpy(buf_ + buf_len_, p, take);
    buf_len_ += take;
    p += take;
    len -= take;
    if (buf_len_ == 64) {
      compress(buf_);
      buf_len_ = 0;
    }
  }
}

Digest32 Sha256::final() {
  const uint64_t bits = total_len_ * 8;
  buf_[buf_len_++] = 0x80;
  if (buf_len_ > 56) {
    std::memset(buf_ + buf_len_, 0, 64 - buf_len_);
    compress(buf_);
    buf_len_ = 0;
  }
  std::memset(buf_ + buf_len_, 0, 56 - buf_len_);
  store_be64(buf_ + 56, bits);
  compress(buf_);
  Digest32 out{};
  for (int i = 0; i < 8; ++i) store_be32(out.data() + 4 * i, h_[i]);
  reset();
  return out;
}

Digest32 sha256(const void* data, size_t len) {
  Sha256 s;
  s.update(data, len);
  return s.final();
}

HmacSha256::HmacSha256(const void* key, size_t key_len) {
  uint8_t k[64] = {0};
  if (key_len > 64) {
    const Digest32 d = sha256(key, key_len);
    std::memcpy(k, d.data(), 32);
  } else if (key_len > 0) {
    std::memcpy(k, key, key_len);
  }
  uint8_t ipad[64], opad[64];
  for (int i = 0; i < 64; ++i) {
    ipad[i] = uint8_t(k[i] ^ 0x36);
    opad[i] = uint8_t(k[i] ^ 0x5c);
  }
  inner_.update(ipad, 64);
  std::memcpy(okey_.data(), opad, 64);
  wipe(k, sizeof(k));
  wipe(ipad, sizeof(ipad));
  wipe(opad, sizeof(opad));
}

Digest32 HmacSha256::final() {
  const Digest32 inner = inner_.final();
  Sha256 outer;
  outer.update(okey_.data(), okey_.size());
  outer.update(inner.data(), inner.size());
  Digest32 out = outer.final();
  wipe(okey_.data(), okey_.size());
  return out;
}

Digest32 hmac_sha256(const Bytes& key, const Bytes& msg) {
  HmacSha256 h(key);
  h.update(msg);
  return h.final();
}

bool ct_equal(const uint8_t* a, const uint8_t* b, size_t len) {
  uint8_t diff = 0;
  for (size_t i = 0; i < len; ++i) diff = uint8_t(diff | (a[i] ^ b[i]));
  return diff == 0;
}

// ------------------------------------------------------- Keccak / SHAKE  --
namespace {
constexpr uint64_t KECCAK_RC[24] = {
    0x0000000000000001ULL, 0x0000000000008082ULL, 0x800000000000808aULL, 0x8000000080008000ULL,
    0x000000000000808bULL, 0x0000000080000001ULL, 0x8000000080008081ULL, 0x8000000000008009ULL,
    0x000000000000008aULL, 0x0000000000000088ULL, 0x0000000080008009ULL, 0x000000008000000aULL,
    0x000000008000808bULL, 0x800000000000008bULL, 0x8000000000008089ULL, 0x8000000000008003ULL,
    0x8000000000008002ULL, 0x8000000000000080ULL, 0x000000000000800aULL, 0x800000008000000aULL,
    0x8000000080008081ULL, 0x8000000000008080ULL, 0x0000000080000001ULL, 0x8000000080008008ULL};
// Rho rotation amounts, in the order the Pi walk visits lanes.
constexpr int KECCAK_ROT[24] = {1, 3, 6, 10, 15, 21, 28, 36, 45, 55, 2, 14,
                                27, 41, 56, 8, 25, 43, 62, 18, 39, 61, 20, 44};
constexpr int KECCAK_PILN[24] = {10, 7, 11, 17, 18, 3, 5, 16, 8, 21, 24, 4,
                                 15, 23, 19, 13, 12, 2, 20, 14, 22, 9, 6, 1};
inline uint64_t rotl64(uint64_t x, int n) {
  if (n == 0) return x;
  return (x << n) | (x >> (64 - n));
}
}  // namespace

Shake256::Shake256() { reset(); }

void Shake256::reset() {
  std::memset(st_, 0, sizeof(st_));
  std::memset(buf_, 0, sizeof(buf_));
  pos_ = 0;
  squeezing_ = false;
}

void Shake256::keccakf() {
  uint64_t* a = st_;
  for (int round = 0; round < 24; ++round) {
    uint64_t bc[5], t;
    // Theta
    for (int i = 0; i < 5; ++i)
      bc[i] = a[i] ^ a[i + 5] ^ a[i + 10] ^ a[i + 15] ^ a[i + 20];
    for (int i = 0; i < 5; ++i) {
      t = bc[(i + 4) % 5] ^ rotl64(bc[(i + 1) % 5], 1);
      for (int j = 0; j < 25; j += 5) a[j + i] ^= t;
    }
    // Rho + Pi
    t = a[1];
    for (int i = 0; i < 24; ++i) {
      const int j = KECCAK_PILN[i];
      const uint64_t tmp = a[j];
      a[j] = rotl64(t, KECCAK_ROT[i]);
      t = tmp;
    }
    // Chi
    for (int j = 0; j < 25; j += 5) {
      for (int i = 0; i < 5; ++i) bc[i] = a[j + i];
      for (int i = 0; i < 5; ++i) a[j + i] ^= (~bc[(i + 1) % 5]) & bc[(i + 2) % 5];
    }
    // Iota
    a[0] ^= KECCAK_RC[round];
  }
}

void Shake256::absorb(const void* data, size_t len) {
  const uint8_t* p = static_cast<const uint8_t*>(data);
  while (len > 0) {
    const size_t take = (len < 136 - pos_) ? len : (136 - pos_);
    for (size_t i = 0; i < take; ++i) buf_[pos_ + i] ^= p[i];
    pos_ += take;
    p += take;
    len -= take;
    if (pos_ == 136) {  // absorb one full rate block, then permute
      for (int i = 0; i < 17; ++i) {
        uint64_t v = 0;
        for (int b = 7; b >= 0; --b) v = (v << 8) | buf_[i * 8 + b];
        st_[i] ^= v;
      }
      keccakf();
      std::memset(buf_, 0, sizeof(buf_));
      pos_ = 0;
    }
  }
}

void Shake256::squeeze(uint8_t* out, size_t len) {
  if (!squeezing_) {
    // pad10*1 with SHAKE domain byte 0x1F
    buf_[pos_] ^= 0x1F;
    buf_[135] ^= 0x80;
    for (int i = 0; i < 17; ++i) {
      uint64_t v = 0;
      for (int b = 7; b >= 0; --b) v = (v << 8) | buf_[i * 8 + b];
      st_[i] ^= v;
    }
    keccakf();
    std::memset(buf_, 0, sizeof(buf_));
    for (int i = 0; i < 17; ++i)
      for (int b = 0; b < 8; ++b) buf_[i * 8 + b] = uint8_t(st_[i] >> (8 * b));
    pos_ = 0;
    squeezing_ = true;
  }
  while (len > 0) {
    if (pos_ == 136) {
      keccakf();
      for (int i = 0; i < 17; ++i)
        for (int b = 0; b < 8; ++b) buf_[i * 8 + b] = uint8_t(st_[i] >> (8 * b));
      pos_ = 0;
    }
    const size_t take = (len < 136 - pos_) ? len : (136 - pos_);
    std::memcpy(out, buf_ + pos_, take);
    out += take;
    len -= take;
    pos_ += take;
  }
}

Bytes shake256(const void* data, size_t len, size_t out_len) {
  Shake256 s;
  s.absorb(data, len);
  Bytes out(out_len);
  if (out_len) s.squeeze(out.data(), out_len);
  return out;
}

// ---------------------------------------------------------------- scrypt --
namespace {
// PBKDF2-HMAC-SHA-256 (single block variant used by scrypt: dkLen <= 32 is the
// only case we need for the ROMix inner function, but we implement the general
// case because key derivation also uses 64-byte outputs).
Bytes pbkdf2_hmac_sha256(const uint8_t* pass, size_t pass_len, const uint8_t* salt, size_t salt_len,
                        uint32_t iterations, size_t dk_len) {
  Bytes dk(dk_len);
  const uint32_t blocks = uint32_t((dk_len + 31) / 32);
  for (uint32_t i = 1; i <= blocks; ++i) {
    HmacSha256 mac(pass, pass_len);
    mac.update(salt, salt_len);
    uint8_t idx[4] = {uint8_t(i >> 24), uint8_t(i >> 16), uint8_t(i >> 8), uint8_t(i)};
    mac.update(idx, 4);
    Digest32 u = mac.final();
    Digest32 t = u;
    for (uint32_t j = 1; j < iterations; ++j) {
      HmacSha256 m2(pass, pass_len);
      m2.update(u.data(), u.size());
      u = m2.final();
      for (size_t k = 0; k < 32; ++k) t[k] ^= u[k];
    }
    const size_t off = size_t(i - 1) * 32;
    const size_t n = (dk_len - off < 32) ? (dk_len - off) : 32;
    std::memcpy(dk.data() + off, t.data(), n);
    wipe(t.data(), t.size());
  }
  return dk;
}

inline void salsa20_8(uint32_t b[16]) {
  uint32_t x[16];
  std::memcpy(x, b, sizeof(x));
  for (int i = 0; i < 8; i += 2) {
    x[4] ^= rotl32(x[0] + x[12], 7);   x[8] ^= rotl32(x[4] + x[0], 9);
    x[12] ^= rotl32(x[8] + x[4], 13);  x[0] ^= rotl32(x[12] + x[8], 18);
    x[9] ^= rotl32(x[5] + x[1], 7);    x[13] ^= rotl32(x[9] + x[5], 9);
    x[1] ^= rotl32(x[13] + x[9], 13);  x[5] ^= rotl32(x[1] + x[13], 18);
    x[14] ^= rotl32(x[10] + x[6], 7);  x[2] ^= rotl32(x[14] + x[10], 9);
    x[6] ^= rotl32(x[2] + x[14], 13);  x[10] ^= rotl32(x[6] + x[2], 18);
    x[3] ^= rotl32(x[15] + x[11], 7);  x[7] ^= rotl32(x[3] + x[15], 9);
    x[11] ^= rotl32(x[7] + x[3], 13);  x[15] ^= rotl32(x[11] + x[7], 18);
    x[1] ^= rotl32(x[0] + x[3], 7);    x[2] ^= rotl32(x[1] + x[0], 9);
    x[3] ^= rotl32(x[2] + x[1], 13);   x[0] ^= rotl32(x[3] + x[2], 18);
    x[6] ^= rotl32(x[5] + x[4], 7);    x[7] ^= rotl32(x[6] + x[5], 9);
    x[4] ^= rotl32(x[7] + x[6], 13);   x[5] ^= rotl32(x[4] + x[7], 18);
    x[11] ^= rotl32(x[10] + x[9], 7);  x[8] ^= rotl32(x[11] + x[10], 9);
    x[9] ^= rotl32(x[8] + x[11], 13);  x[10] ^= rotl32(x[9] + x[8], 18);
    x[12] ^= rotl32(x[15] + x[14], 7); x[13] ^= rotl32(x[12] + x[15], 9);
    x[14] ^= rotl32(x[13] + x[12], 13); x[15] ^= rotl32(x[14] + x[13], 18);
  }
  for (int i = 0; i < 16; ++i) b[i] += x[i];
}

inline void blockmix_salsa8(uint32_t* B, uint32_t* Y, uint32_t r) {
  uint32_t X[16];
  std::memcpy(X, &B[(2 * r - 1) * 16], 64);
  for (uint32_t i = 0; i < 2 * r; ++i) {
    for (int k = 0; k < 16; ++k) X[k] ^= B[i * 16 + k];
    salsa20_8(X);
    // Even blocks land in the first half, odd blocks in the second half.
    const uint32_t dst = (i % 2 == 0) ? (i / 2) : (r + i / 2);
    std::memcpy(&Y[dst * 16], X, 64);
  }
  std::memcpy(B, Y, size_t(128) * r);
}
}  // namespace

bool scrypt(const uint8_t* pass, size_t pass_len, const uint8_t* salt, size_t salt_len,
            uint64_t N, uint32_t r, uint32_t p, uint8_t* out, size_t out_len, bool do_wipe) {
  if (N < 2 || (N & (N - 1)) != 0) return false;                          // power of two, N > 1
  if (r == 0 || p == 0) return false;
  if (uint64_t(r) * uint64_t(p) >= (1ULL << 30)) return false;            // RFC 7914 limits
  if (16 * uint64_t(r) < 64 && N > (1ULL << (16 * uint64_t(r)))) return false;
  if (uint64_t(N) * uint64_t(r) * 128ULL > (1ULL << 34)) return false;    // 16 GiB sanity guard

  Bytes B0 = pbkdf2_hmac_sha256(pass, pass_len, salt, salt_len, 1, size_t(128) * r * p);
  const size_t words_per_block = 32;  // 128 bytes
  std::vector<uint32_t> B(B0.size() / 4);
  std::memcpy(B.data(), B0.data(), B0.size());
  wipe(B0.data(), B0.size());

  std::vector<uint32_t> V(words_per_block * r * size_t(N));
  std::vector<uint32_t> Y(words_per_block * r);

  for (uint32_t pi = 0; pi < p; ++pi) {
    uint32_t* Xp = B.data() + words_per_block * r * pi;
    for (uint64_t i = 0; i < N; ++i) {
      // V[i] keeps the *input* block; X then becomes BlockMix(X).
      std::memcpy(&V[words_per_block * r * i], Xp, size_t(128) * r);
      blockmix_salsa8(Xp, Y.data(), r);
    }
    for (uint64_t i = 0; i < N; ++i) {
      // Integerify: little-endian 64-bit value of the last 64-byte half, mod N.
      const uint8_t* tail = reinterpret_cast<const uint8_t*>(Xp + words_per_block * r - 16);
      uint64_t j = 0;
      for (int b = 7; b >= 0; --b) j = (j << 8) | tail[b];
      const uint32_t* Vj = &V[words_per_block * r * (j & (N - 1))];
      for (size_t k = 0; k < words_per_block * r; ++k) Xp[k] ^= Vj[k];
      blockmix_salsa8(Xp, Y.data(), r);
    }
  }

  Bytes dk = pbkdf2_hmac_sha256(pass, pass_len, reinterpret_cast<const uint8_t*>(B.data()),
                                B.size() * 4, 1, out_len);
  std::memcpy(out, dk.data(), out_len);
  if (do_wipe) {
    wipe(B.data(), B.size() * 4);
    wipe(V.data(), V.size() * 4);
    wipe(Y.data(), Y.size() * 4);
    wipe(dk.data(), dk.size());
  }
  return true;
}

// ---------------------------------------------------------------- helpers --
Bytes random_bytes(size_t n) {
  Bytes out(n);
  size_t got = 0;
#if defined(__linux__)
  while (got < n) {
    const ssize_t r = ::getrandom(out.data() + got, n - got, 0);
    if (r <= 0) break;
    got += size_t(r);
  }
#endif
  if (got < n) {  // fall back to /dev/urandom
    FILE* f = std::fopen("/dev/urandom", "rb");
    if (!f) throw std::runtime_error("no entropy source available");
    got += std::fread(out.data() + got, 1, n - got, f);
    std::fclose(f);
  }
  if (got != n) throw std::runtime_error("failed to gather entropy");
  return out;
}

std::string hex(const uint8_t* p, size_t n) {
  static const char* k = "0123456789abcdef";
  std::string s(n * 2, '0');
  for (size_t i = 0; i < n; ++i) {
    s[2 * i] = k[p[i] >> 4];
    s[2 * i + 1] = k[p[i] & 0xf];
  }
  return s;
}

Bytes from_hex(const std::string& h) {
  auto val = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  Bytes out;
  out.reserve(h.size() / 2);
  int hi = -1;
  for (char c : h) {
    const int v = val(c);
    if (v < 0) continue;  // skip separators such as ':'
    if (hi < 0) {
      hi = v;
    } else {
      out.push_back(uint8_t((hi << 4) | v));
      hi = -1;
    }
  }
  return out;
}

void wipe(void* p, size_t n) {
#if defined(__GNUC__)
  std::memset(p, 0, n);
  __asm__ __volatile__("" : : "r"(p) : "memory");
#else
  volatile uint8_t* v = static_cast<volatile uint8_t*>(p);
  while (n--) *v++ = 0;
#endif
}

}  // namespace yui::crypto
