// Ed25519 + SHA-512, clean-room, RFC 8032 / FIPS 180-4.
//
// Tầng 1 ký gói cập nhật `.bundle` bằng Ed25519 — nhưng repo không kéo thư
// viện mật mã ngoài, nên đây là bản tự viết theo đúng thuật toán chuẩn
// (RFC 8032 §5.1 kèm minh hoạ Python chuẩn hoá, và bảng vector §7.1).
//
// **Giới hạn đã biết** (xem docs/SECURITY.md §4):
//   * mật mã phần mềm thuần: không chống tấn công kênh bên (timing/cache);
//   * scalar multiplication dùng double-and-add không hằng thời gian — đủ
//     cho xác thực gói cập nhật (khóa công khai, không có oracle), không dùng
//     cho ký số liên tục trong môi trường không tin cậy;
//   * chỉ Ed25519 (không Ed25519ctx/ph, không Ed448).
//
// Cặp đối chiếu: `kernel-cap/src/ed25519.rs` (Rust, deny(unsafe_code)) dùng
// đúng thuật toán và cùng bảng vector RFC 8032.
#ifndef YUI_ED25519_HPP
#define YUI_ED25519_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace yui::ed25519 {

// ------------------------------------------------------------------ SHA-512 --
struct Sha512Digest {
  std::array<uint8_t, 64> bytes{};
};

/// Bộ đệm SHA-512 streaming (FIPS 180-4) — không trạng thái toàn cục.
class Sha512 {
 public:
  Sha512();
  void update(const void* data, size_t len);
  void update(const std::vector<uint8_t>& data);
  void update(const std::string& text);
  Sha512Digest finalize() const;  // không phá trạng thái: gọi được nhiều lần

 private:
  void compress(const uint8_t* block);
  std::array<uint64_t, 8> state_{};
  std::array<uint8_t, 128> buf_{};
  size_t buf_len_ = 0;
  uint64_t total_len_ = 0;  // byte, không bit (đổi khi finalize)
};

Sha512Digest sha512(const void* data, size_t len);
Sha512Digest sha512(const std::vector<uint8_t>& data);

// ----------------------------------------------------------------- Ed25519 --
using Seed = std::array<uint8_t, 32>;   // khóa riêng (ngẫu nhiên 32 byte)
using PublicKey = std::array<uint8_t, 32>;
using Signature = std::array<uint8_t, 64>;

/// Khóa công khai A = [clamp(SHA-512(seed)[0..32])]·B — RFC 8032 §5.1.5.
PublicKey public_key(const Seed& seed);

/// Chữ ký Ed25519 thuần (dom2 = rỗng) — RFC 8032 §5.1.6.
Signature sign(const Seed& seed, const void* msg, size_t len);
Signature sign(const Seed& seed, const std::vector<uint8_t>& msg);

/// Xác minh — RFC 8032 §5.1.7: S < L, nạp được R và A, [S]B = R + [k]A.
bool verify(const PublicKey& pub, const void* msg, size_t len, const Signature& sig);
bool verify(const PublicKey& pub, const std::vector<uint8_t>& msg, const Signature& sig);

// Hex tiện cho test và cho định dạng manifest (64/128 ký tự thường).
std::string to_hex(const uint8_t* data, size_t len);
bool from_hex(const std::string& hex, uint8_t* out, size_t len);

}  // namespace yui::ed25519

#endif  // YUI_ED25519_HPP
