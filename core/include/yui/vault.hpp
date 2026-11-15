// yui_vault — the encrypted store in Tầng 4 ("Kho mã hóa scrypt + SHAKE-256").
//
// Construction (documented, deterministic, and deliberately boring):
//
//   master  = scrypt(password, salt, N=2^log2_n, r, p, dkLen=64)
//   enc_key = master[0..32)
//   mac_key = SHAKE-256(master[32..64) || "yui-vault/mac/v1", 32)
//   stream  = SHAKE-256(enc_key || nonce, len(plaintext))       (XOR keystream)
//   tag     = HMAC-SHA-256(mac_key, header || ciphertext)        (encrypt-then-MAC)
//
// The file header is authenticated, so parameters cannot be downgraded without
// invalidating the tag.  Verification is constant-time.  This is a purpose-built
// authenticated stream cipher for YuiVieOS's own vault files; it is *not*
// advertised as a general-purpose AEAD (no nonce-reuse hardening beyond random
// 128-bit nonces, which is sufficient for <2^32 seals per key).
#ifndef YUI_VAULT_HPP
#define YUI_VAULT_HPP

#include <cstdint>
#include <string>
#include <vector>

namespace yui::vault {

using Bytes = std::vector<uint8_t>;

constexpr uint32_t kMagic = 0x56495559;  // "YUIV" little-endian
constexpr uint8_t kVersion = 1;
constexpr size_t kSaltLen = 16;
constexpr size_t kNonceLen = 16;
constexpr size_t kTagLen = 32;
constexpr size_t kHeaderSize = 4 + 1 + 1 + 1 + 1 + 2 + kSaltLen + kNonceLen + kTagLen;  // 74 bytes

struct Params {
  uint8_t log2_n = 15;  // 32768 -> ~32 MiB of scrypt scratch, ~60 ms on a T480
  uint8_t r = 8;
  uint8_t p = 1;
};

// Encrypt `plain` under `password`.  Returns the sealed blob (header + body).
Bytes seal(const std::string& password, const Bytes& plain, const Params& params = {});
Bytes seal(const std::string& password, const std::string& plain, const Params& params = {});

// Decrypt and authenticate.  On failure returns false and fills `error`
// ("wrong password" and "corrupt/tampered" are reported identically on purpose,
// so an attacker learns nothing from the message).
bool open(const std::string& password, const Bytes& blob, Bytes& out, std::string* error = nullptr);
bool open_string(const std::string& password, const Bytes& blob, std::string& out,
                 std::string* error = nullptr);

bool is_vault(const Bytes& blob);

// Reads the KDF parameters from a blob header without decrypting (for tuning UI).
bool peek_params(const Bytes& blob, Params& out);

}  // namespace yui::vault

#endif  // YUI_VAULT_HPP
