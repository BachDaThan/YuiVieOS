// yui_vault implementation.  See vault.hpp for the construction.
#include "yui/vault.hpp"

#include <cstring>
#include <stdexcept>

#include "yui/crypto.hpp"

namespace yui::vault {
namespace {

using crypto::Bytes;

constexpr char kMacDomain[] = "yui-vault/mac/v1";

void put_u32(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v & 0xff);
  p[1] = uint8_t((v >> 8) & 0xff);
  p[2] = uint8_t((v >> 16) & 0xff);
  p[3] = uint8_t((v >> 24) & 0xff);
}
uint32_t get_u32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

struct Keys {
  Bytes enc;
  Bytes mac;
};

Keys derive(const std::string& password, const uint8_t* salt, const Params& params) {
  const uint64_t n = uint64_t(1) << params.log2_n;
  Bytes master(64, 0);
  const bool ok = crypto::scrypt(reinterpret_cast<const uint8_t*>(password.data()), password.size(), salt,
                                 kSaltLen, n, params.r, params.p, master.data(), master.size());
  if (!ok) throw std::runtime_error("vault: invalid scrypt parameters");
  Keys k;
  k.enc.assign(master.begin(), master.begin() + 32);
  Bytes mac_input(master.begin() + 32, master.end());
  mac_input.insert(mac_input.end(), kMacDomain, kMacDomain + sizeof(kMacDomain) - 1);
  k.mac = crypto::shake256(mac_input, 32);
  crypto::wipe(master.data(), master.size());
  crypto::wipe(mac_input.data(), mac_input.size());
  return k;
}

Bytes keystream(const Bytes& enc_key, const uint8_t* nonce, size_t len) {
  Bytes seed = enc_key;
  seed.insert(seed.end(), nonce, nonce + kNonceLen);
  return crypto::shake256(seed, len);
}

// The tag field itself is excluded from the MAC (it is the last header field),
// so sealing and opening can share one code path.
constexpr size_t kTagOffset = 10 + kSaltLen + kNonceLen;

Bytes tag_of(const Bytes& mac_key, const uint8_t* blob, size_t total_len) {
  crypto::HmacSha256 h(mac_key);
  h.update(blob, kTagOffset);
  h.update(blob + kTagOffset + kTagLen, total_len - kTagOffset - kTagLen);
  const auto d = h.final();
  return Bytes(d.begin(), d.end());
}

}  // namespace

bool is_vault(const Bytes& blob) {
  return blob.size() >= kHeaderSize && get_u32(blob.data()) == kMagic && blob[4] == kVersion;
}

bool peek_params(const Bytes& blob, Params& out) {
  if (!is_vault(blob)) return false;
  const uint8_t log2_n = blob[5];
  out.log2_n = log2_n;
  out.r = blob[6];
  out.p = blob[7];
  return true;
}

Bytes seal(const std::string& password, const Bytes& plain, const Params& params) {
  Bytes blob(kHeaderSize + plain.size(), 0);
  put_u32(blob.data(), kMagic);
  blob[4] = kVersion;
  blob[5] = params.log2_n;
  blob[6] = params.r;
  blob[7] = params.p;
  blob[8] = blob[9] = 0;  // flags (reserved)

  const Bytes salt = crypto::random_bytes(kSaltLen);
  const Bytes nonce = crypto::random_bytes(kNonceLen);
  std::memcpy(blob.data() + 10, salt.data(), kSaltLen);
  std::memcpy(blob.data() + 10 + kSaltLen, nonce.data(), kNonceLen);
  // bytes 10+salt+nonce .. +tag stay zero until the tag is computed

  Keys keys = derive(password, salt.data(), params);
  Bytes ks = keystream(keys.enc, nonce.data(), plain.size());
  uint8_t* body = blob.data() + kHeaderSize;
  for (size_t i = 0; i < plain.size(); ++i) body[i] = uint8_t(plain[i] ^ ks[i]);

  Bytes tag = tag_of(keys.mac, blob.data(), blob.size());
  std::memcpy(blob.data() + kTagOffset, tag.data(), kTagLen);

  crypto::wipe(ks.data(), ks.size());
  crypto::wipe(keys.enc.data(), keys.enc.size());
  crypto::wipe(keys.mac.data(), keys.mac.size());
  return blob;
}

Bytes seal(const std::string& password, const std::string& plain, const Params& params) {
  return seal(password, Bytes(plain.begin(), plain.end()), params);
}

bool open(const std::string& password, const Bytes& blob, Bytes& out, std::string* error) {
  auto fail = [&](const char* msg) {
    if (error) *error = msg;
    out.clear();
    return false;
  };
  if (blob.size() < kHeaderSize) return fail("blob too short");
  if (get_u32(blob.data()) != kMagic) return fail("not a YuiVault blob");
  if (blob[4] != kVersion) return fail("unsupported vault version");
  const Params params{blob[5], blob[6], blob[7]};
  if (params.log2_n < 10 || params.log2_n > 22 || params.r < 1 || params.r > 32 || params.p < 1 ||
      params.p > 16)
    return fail("implausible KDF parameters (refusing to run)");

  const uint8_t* salt = blob.data() + 10;
  const uint8_t* nonce = salt + kSaltLen;
  const uint8_t* stored_tag = blob.data() + kTagOffset;

  Keys keys = derive(password, salt, params);
  Bytes tag = tag_of(keys.mac, blob.data(), blob.size());
  const bool mac_ok = crypto::ct_equal(tag.data(), stored_tag, kTagLen);
  crypto::wipe(tag.data(), tag.size());
  if (!mac_ok) {
    crypto::wipe(keys.enc.data(), keys.enc.size());
    crypto::wipe(keys.mac.data(), keys.mac.size());
    return fail("wrong password or corrupted vault");
  }

  const size_t body_len = blob.size() - kHeaderSize;
  out.assign(body_len, 0);
  Bytes ks = keystream(keys.enc, nonce, body_len);
  for (size_t i = 0; i < body_len; ++i) out[i] = uint8_t(blob[kHeaderSize + i] ^ ks[i]);

  crypto::wipe(ks.data(), ks.size());
  crypto::wipe(keys.enc.data(), keys.enc.size());
  crypto::wipe(keys.mac.data(), keys.mac.size());
  return true;
}

bool open_string(const std::string& password, const Bytes& blob, std::string& out, std::string* error) {
  Bytes tmp;
  if (!open(password, blob, tmp, error)) return false;
  out.assign(tmp.begin(), tmp.end());
  crypto::wipe(tmp.data(), tmp.size());
  return true;
}

}  // namespace yui::vault
