// YuiVieOS — YuiVault cryptography primitives.
//
// Clean-room implementations written for YuiVieOS so that Tầng 4 (`yuid`)
// never needs to link against OpenSSL/libsodium.  Only three primitives are
// required by the capability + vault design:
//
//   * SHA-256 / HMAC-SHA-256  — capability token MACs, key derivation, hashing
//   * Keccak / SHAKE-256      — expandable output for vault keystreams + KDF
//   * scrypt (N,r,p)          — memory-hard password stretching for the vault
//
// Everything is plain C++17, no exceptions, no allocation in the hot paths,
// and is validated against published test vectors in core/tests/test_crypto.cpp
// (vectors cross-checked against Python's hashlib).
#ifndef YUI_CRYPTO_HPP
#define YUI_CRYPTO_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace yui::crypto {

using Bytes = std::vector<uint8_t>;
using Digest32 = std::array<uint8_t, 32>;
using Digest64 = std::array<uint8_t, 64>;

// ---------------------------------------------------------------- SHA-256 --
class Sha256 {
public:
  Sha256();
  void update(const void* data, size_t len);
  void update(const Bytes& b) { update(b.data(), b.size()); }
  void update(const std::string& s) { update(s.data(), s.size()); }
  // Finalises and resets the context.
  Digest32 final();
  void reset();

private:
  void compress(const uint8_t block[64]);
  uint32_t h_[8];
  uint8_t buf_[64];
  size_t buf_len_;
  uint64_t total_len_;
};

Digest32 sha256(const void* data, size_t len);
inline Digest32 sha256(const Bytes& b) { return sha256(b.data(), b.size()); }
inline Digest32 sha256(const std::string& s) { return sha256(s.data(), s.size()); }

class HmacSha256 {
public:
  HmacSha256(const void* key, size_t key_len);
  HmacSha256(const Bytes& key) : HmacSha256(key.data(), key.size()) {}
  HmacSha256(const std::string& key) : HmacSha256(key.data(), key.size()) {}
  void update(const void* data, size_t len) { inner_.update(data, len); }
  void update(const Bytes& b) { update(b.data(), b.size()); }
  void update(const std::string& s) { update(s.data(), s.size()); }
  Digest32 final();

private:
  Sha256 inner_;
  // HMAC keeps the raw 64-byte opad block, not a hash of it.
  std::array<uint8_t, 64> okey_{};
};
Digest32 hmac_sha256(const Bytes& key, const Bytes& msg);

// Constant-time compare — used for every tag/MAC verification in the vault.
bool ct_equal(const uint8_t* a, const uint8_t* b, size_t len);

// ------------------------------------------------------- Keccak / SHAKE  --
// Keccak-f[1600] sponge with domain-separation byte 0x1F (SHA-3/SHAKE family).
class Shake256 {
public:
  Shake256();
  void absorb(const void* data, size_t len);
  // Squeeze `len` bytes into `out`. May be called repeatedly for streaming.
  void squeeze(uint8_t* out, size_t len);
  void reset();

private:
  void keccakf();
  void xor_byte(size_t i, uint8_t v);
  uint64_t st_[25];
  uint8_t buf_[136];  // rate = 136 bytes for SHAKE-256
  size_t pos_;
  bool squeezing_;
};

Bytes shake256(const void* data, size_t len, size_t out_len);
inline Bytes shake256(const Bytes& in, size_t out_len) { return shake256(in.data(), in.size(), out_len); }
inline Bytes shake256(const std::string& s, size_t out_len) { return shake256(s.data(), s.size(), out_len); }

// ---------------------------------------------------------------- scrypt --
// Memory-hard KDF.  N must be a power of two > 1, r*p < 2^30 and N < 2^(16*r)
// (RFC 7914 restrictions).  Returns false on invalid parameters.
bool scrypt(const uint8_t* pass, size_t pass_len, const uint8_t* salt, size_t salt_len,
            uint64_t N, uint32_t r, uint32_t p, uint8_t* out, size_t out_len, bool wipe = true);
inline bool scrypt(const std::string& pass, const std::string& salt, uint64_t N, uint32_t r, uint32_t p,
                   Bytes& out, size_t out_len) {
  out.assign(out_len, 0);
  return scrypt(reinterpret_cast<const uint8_t*>(pass.data()), pass.size(),
                reinterpret_cast<const uint8_t*>(salt.data()), salt.size(), N, r, p, out.data(), out_len);
}

// ---------------------------------------------------------------- helpers --
Bytes random_bytes(size_t n);                 // getrandom(2) / /dev/urandom
std::string hex(const uint8_t* p, size_t n);  // lowercase hex
inline std::string hex(const Bytes& b) { return hex(b.data(), b.size()); }
template <size_t N>
inline std::string hex(const std::array<uint8_t, N>& a) {
  return hex(a.data(), a.size());
}
Bytes from_hex(const std::string& h);
// Best-effort secure wipe that the optimiser is not allowed to elide.
void wipe(void* p, size_t n);

}  // namespace yui::crypto

#endif  // YUI_CRYPTO_HPP
