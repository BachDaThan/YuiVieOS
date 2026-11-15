// Ed25519 + SHA-512 clean-room — xem include/yui/ed25519.hpp.
//
// Cấu trúc bám sát RFC 8032 §5.1 (và minh hoạ Python chuẩn hoá của RFC):
//   SHA-512 → số học trường GF(p), p = 2^255-19 → điểm trên đường cong
//   twisted Edwards a=-1 → scalar mod L, L = 2^252 + 27742317777372353535851937790883648493.
//
// Điểm dùng toạ độ mở rộng (X,Y,Z,T) với công thức **cộng đầy đủ** của RFC
// §5.1.4 — đúng cho mọi cặp điểm, kể cả P == Q, nên scalar multiplication
// chỉ cần một routine cộng duy nhất (giống minh hoạ Python), giảm đường sai.
//
// Số học: u256/u512 dạng limb 32-bit little-endian, mọi phép đều rút gọn mod
// m bằng trừ dài theo bit (đơn giản, đúng với mọi đầu vào, không cần lazy
// reduction) — tốc độ không phải mục tiêu của gói cập nhật ngoại tuyến.
#include "yui/ed25519.hpp"

#include <algorithm>
#include <cstring>

namespace yui::ed25519 {
namespace {

// ------------------------------------------------------------- SHA-512 core --
const uint64_t K512[80] = {
    0x428a2f98d728ae22ull, 0x7137449123ef65cdull, 0xb5c0fbcfec4d3b2full, 0xe9b5dba58189dbbcull,
    0x3956c25bf348b538ull, 0x59f111f1b605d019ull, 0x923f82a4af194f9bull, 0xab1c5ed5da6d8118ull,
    0xd807aa98a3030242ull, 0x12835b0145706fbeull, 0x243185be4ee4b28cull, 0x550c7dc3d5ffb4e2ull,
    0x72be5d74f27b896full, 0x80deb1fe3b1696b1ull, 0x9bdc06a725c71235ull, 0xc19bf174cf692694ull,
    0xe49b69c19ef14ad2ull, 0xefbe4786384f25e3ull, 0x0fc19dc68b8cd5b5ull, 0x240ca1cc77ac9c65ull,
    0x2de92c6f592b0275ull, 0x4a7484aa6ea6e483ull, 0x5cb0a9dcbd41fbd4ull, 0x76f988da831153b5ull,
    0x983e5152ee66dfabull, 0xa831c66d2db43210ull, 0xb00327c898fb213full, 0xbf597fc7beef0ee4ull,
    0xc6e00bf33da88fc2ull, 0xd5a79147930aa725ull, 0x06ca6351e003826full, 0x142929670a0e6e70ull,
    0x27b70a8546d22ffcull, 0x2e1b21385c26c926ull, 0x4d2c6dfc5ac42aedull, 0x53380d139d95b3dfull,
    0x650a73548baf63deull, 0x766a0abb3c77b2a8ull, 0x81c2c92e47edaee6ull, 0x92722c851482353bull,
    0xa2bfe8a14cf10364ull, 0xa81a664bbc423001ull, 0xc24b8b70d0f89791ull, 0xc76c51a30654be30ull,
    0xd192e819d6ef5218ull, 0xd69906245565a910ull, 0xf40e35855771202aull, 0x106aa07032bbd1b8ull,
    0x19a4c116b8d2d0c8ull, 0x1e376c085141ab53ull, 0x2748774cdf8eeb99ull, 0x34b0bcb5e19b48a8ull,
    0x391c0cb3c5c95a63ull, 0x4ed8aa4ae3418acbull, 0x5b9cca4f7763e373ull, 0x682e6ff3d6b2b8a3ull,
    0x748f82ee5defb2fcull, 0x78a5636f43172f60ull, 0x84c87814a1f0ab72ull, 0x8cc702081a6439ecull,
    0x90befffa23631e28ull, 0xa4506cebde82bde9ull, 0xbef9a3f7b2c67915ull, 0xc67178f2e372532bull,
    0xca273eceea26619cull, 0xd186b8c721c0c207ull, 0xeada7dd6cde0eb1eull, 0xf57d4f7fee6ed178ull,
    0x06f067aa72176fbaull, 0x0a637dc5a2c898a6ull, 0x113f9804bef90daeull, 0x1b710b35131c471bull,
    0x28db77f523047d84ull, 0x32caab7b40c72493ull, 0x3c9ebe0a15c9bebcull, 0x431d67c49c100d4cull,
    0x4cc5d4becb3e42b6ull, 0x597f299cfc657e2aull, 0x5fcb6fab3ad6faecull, 0x6c44198c4a475817ull,
};

inline uint64_t rotr64(uint64_t x, int n) { return (x >> n) | (x << (64 - n)); }

void sha512_compress(uint64_t state[8], const uint8_t* block) {
  uint64_t w[80];
  for (int i = 0; i < 16; ++i) {
    w[i] = ((uint64_t)block[i * 8] << 56) | ((uint64_t)block[i * 8 + 1] << 48) |
           ((uint64_t)block[i * 8 + 2] << 40) | ((uint64_t)block[i * 8 + 3] << 32) |
           ((uint64_t)block[i * 8 + 4] << 24) | ((uint64_t)block[i * 8 + 5] << 16) |
           ((uint64_t)block[i * 8 + 6] << 8) | (uint64_t)block[i * 8 + 7];
  }
  for (int i = 16; i < 80; ++i) {
    const uint64_t s0 = rotr64(w[i - 15], 1) ^ rotr64(w[i - 15], 8) ^ (w[i - 15] >> 7);
    const uint64_t s1 = rotr64(w[i - 2], 19) ^ rotr64(w[i - 2], 61) ^ (w[i - 2] >> 6);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint64_t a = state[0], b = state[1], c = state[2], d = state[3];
  uint64_t e = state[4], f = state[5], g = state[6], h = state[7];
  for (int i = 0; i < 80; ++i) {
    const uint64_t s1 = rotr64(e, 14) ^ rotr64(e, 18) ^ rotr64(e, 41);
    const uint64_t ch = (e & f) ^ (~e & g);
    const uint64_t t1 = h + s1 + ch + K512[i] + w[i];
    const uint64_t s0 = rotr64(a, 28) ^ rotr64(a, 34) ^ rotr64(a, 39);
    const uint64_t maj = (a & b) ^ (a & c) ^ (b & c);
    const uint64_t t2 = s0 + maj;
    h = g; g = f; f = e; e = d + t1;
    d = c; c = b; b = a; a = t1 + t2;
  }
  state[0] += a; state[1] += b; state[2] += c; state[3] += d;
  state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

// --------------------------------------------------------------- big numbers --
using u256 = std::array<uint32_t, 8>;
using u512 = std::array<uint32_t, 16>;

int bn_cmp(const uint32_t* a, int na, const uint32_t* b, int nb) {
  const int n = na > nb ? na : nb;
  for (int i = n - 1; i >= 0; --i) {
    const uint32_t av = i < na ? a[i] : 0;
    const uint32_t bv = i < nb ? b[i] : 0;
    if (av != bv) return av < bv ? -1 : 1;
  }
  return 0;
}

// a -= b (a là mảng dài na, b dài nb; giả sử a >= b về giá trị nguyên).
void bn_sub_into(uint32_t* a, int na, const uint32_t* b, int nb) {
  int64_t borrow = 0;
  for (int i = 0; i < na; ++i) {
    const int64_t bv = i < nb ? (int64_t)b[i] : 0;
    int64_t d = (int64_t)a[i] - bv - borrow;
    if (d < 0) {
      d += (int64_t)1 << 32;
      borrow = 1;
    } else {
      borrow = 0;
    }
    a[i] = (uint32_t)d;
  }
}

// r = x mod m (x 512-bit, m 256-bit, m > 0) — trừ dài theo bit, luôn đúng.
u256 bn_mod(const u512& x, const u256& m) {
  u512 r{};
  for (int i = 511; i >= 0; --i) {
    uint32_t carry = (x[i >> 5] >> (i & 31)) & 1u;
    for (int k = 0; k < 16; ++k) {
      const uint32_t next = r[k] >> 31;
      r[k] = (r[k] << 1) | carry;
      carry = next;
    }
    if (bn_cmp(r.data(), 16, m.data(), 8) >= 0) bn_sub_into(r.data(), 16, m.data(), 8);
  }
  u256 out{};
  for (int i = 0; i < 8; ++i) out[i] = r[i];
  return out;
}

u512 bn_mul256(const u256& a, const u256& b) {
  u512 r{};
  for (int i = 0; i < 8; ++i) {
    uint64_t carry = 0;
    for (int j = 0; j < 8; ++j) {
      const uint64_t cur = (uint64_t)r[i + j] + carry + (uint64_t)a[i] * b[j];
      r[i + j] = (uint32_t)cur;
      carry = cur >> 32;
    }
    int k = i + 8;
    while (carry && k < 16) {
      const uint64_t cur = (uint64_t)r[k] + carry;
      r[k] = (uint32_t)cur;
      carry = cur >> 32;
      ++k;
    }
  }
  return r;
}

// ------------------------------------------------------------------- field --
// p = 2^255 - 19 = 0x7fff…ffed (limb LE 32-bit).
const u256 FE_P = {0xffffffedu, 0xffffffffu, 0xffffffffu, 0xffffffffu,
                   0xffffffffu, 0xffffffffu, 0xffffffffu, 0x7fffffffu};
// L = 2^252 + 27742317777372353535851937790883648493 (thứ tự nhóm B, RFC 8032 §5.1).
const u256 SC_L = {0x5cf5d3edu, 0x5812631au, 0xa2f79cd6u, 0x14def9deu,
                   0u, 0u, 0u, 0x10000000u};

u256 fe_zero() { return u256{}; }
u256 fe_one() { return u256{1, 0, 0, 0, 0, 0, 0, 0}; }
u256 fe_from_u64(uint64_t v) {
  u256 r{};
  r[0] = (uint32_t)v;
  r[1] = (uint32_t)(v >> 32);
  return r;
}

u256 fe_add(const u256& a, const u256& b) {
  u512 t{};
  uint64_t carry = 0;
  for (int i = 0; i < 8; ++i) {
    const uint64_t s = (uint64_t)a[i] + b[i] + carry;
    t[i] = (uint32_t)s;
    carry = s >> 32;
  }
  t[8] = (uint32_t)carry;
  return bn_mod(t, FE_P);
}

u256 fe_sub(const u256& a, const u256& b) {
  // a - b mod p = (a + p - b) mod p — tính trong u512 để không tràn.
  u512 t{};
  uint64_t carry = 0;
  for (int i = 0; i < 8; ++i) {
    const uint64_t s = (uint64_t)a[i] + FE_P[i] + carry;
    t[i] = (uint32_t)s;
    carry = s >> 32;
  }
  t[8] = (uint32_t)carry;
  bn_sub_into(t.data(), 16, b.data(), 8);
  return bn_mod(t, FE_P);
}

u256 fe_mul(const u256& a, const u256& b) { return bn_mod(bn_mul256(a, b), FE_P); }
u256 fe_sq(const u256& a) { return fe_mul(a, a); }

bool fe_is_zero(const u256& a) { return bn_cmp(a.data(), 8, fe_zero().data(), 8) == 0; }
bool fe_eq(const u256& a, const u256& b) { return bn_cmp(a.data(), 8, b.data(), 8) == 0; }
bool fe_is_odd(const u256& a) { return (a[0] & 1u) != 0; }
u256 fe_neg(const u256& a) {
  if (fe_is_zero(a)) return a;
  return fe_sub(fe_zero(), a);
}

u256 fe_pow(const u256& a, const u256& e) {
  u256 r = fe_one();
  for (int i = 255; i >= 0; --i) {
    r = fe_sq(r);
    if ((e[i >> 5] >> (i & 31)) & 1u) r = fe_mul(r, a);
  }
  return r;
}

u256 fe_inv(const u256& a) {
  // a^(p-2) mod p — nghịch đảo Fermat (p nguyên tố).
  u256 e = FE_P;
  // e = p - 2
  bn_sub_into(e.data(), 8, fe_from_u64(2).data(), 8);
  return fe_pow(a, e);
}

u256 fe_from_bytes(const uint8_t in[32]) {
  u256 r{};
  for (int i = 0; i < 8; ++i) {
    r[i] = (uint32_t)in[i * 4] | ((uint32_t)in[i * 4 + 1] << 8) | ((uint32_t)in[i * 4 + 2] << 16) |
           ((uint32_t)in[i * 4 + 3] << 24);
  }
  r[7] &= 0x7fffffffu;  // RFC 8032 §5.1.3: "red simply by clearing this bit"
  return r;
}

void u256_to_bytes(const u256& a, uint8_t out[32]) {
  for (int i = 0; i < 8; ++i) {
    out[i * 4] = (uint8_t)a[i];
    out[i * 4 + 1] = (uint8_t)(a[i] >> 8);
    out[i * 4 + 2] = (uint8_t)(a[i] >> 16);
    out[i * 4 + 3] = (uint8_t)(a[i] >> 24);
  }
}

// hằng số (tính một lần, không ghi tay giá trị hex để tránh lỗi đánh máy):
const u256& fe_d() {
  // d = -121665 * inv(121666) mod p (RFC 8032 §5.1)
  static const u256 d = fe_mul(fe_neg(fe_from_u64(121665)), fe_inv(fe_from_u64(121666)));
  return d;
}
const u256& fe_sqrt_m1() {
  // căn bậc hai của -1 mod p = 2^((p-1)/4); lũy thừa (p-1)/4 = 2^253 - 5.
  static const u256 e = [] {
    // e = 2^253 - 5: limb7 = 0x20000000 trừ 5 có mượn
    u256 x{};
    x[7] = 0x20000000u;
    bn_sub_into(x.data(), 8, fe_from_u64(5).data(), 8);
    return x;
  }();
  static const u256 m1 = fe_pow(fe_from_u64(2), e);
  return m1;
}

// -------------------------------------------------------------------- point --
struct Point {
  u256 X, Y, Z, T;
};

Point point_identity() { return Point{fe_zero(), fe_one(), fe_one(), fe_zero()}; }

// Cộng đầy đủ trên twisted Edwards a = -1 — RFC 8032 §5.1.4 (EFD add-2008-hwcd-3).
Point point_add(const Point& p1, const Point& p2) {
  const u256 a = fe_mul(fe_sub(p1.Y, p1.X), fe_sub(p2.Y, p2.X));
  const u256 b = fe_mul(fe_add(p1.Y, p1.X), fe_add(p2.Y, p2.X));
  const u256 two = fe_from_u64(2);
  const u256 c = fe_mul(fe_mul(fe_mul(p1.T, two), fe_d()), p2.T);
  const u256 d = fe_mul(fe_mul(p1.Z, two), p2.Z);
  const u256 e = fe_sub(b, a);
  const u256 f = fe_sub(d, c);
  const u256 g = fe_add(d, c);
  const u256 h = fe_add(b, a);
  return Point{fe_mul(e, f), fe_mul(g, h), fe_mul(f, g), fe_mul(e, h)};
}

bool point_equal(const Point& p1, const Point& p2) {
  // x1/z1 == x2/z2  <=>  x1*z2 == x2*z1  (tương tự với y).
  if (!fe_eq(fe_mul(p1.X, p2.Z), fe_mul(p2.X, p1.Z))) return false;
  return fe_eq(fe_mul(p1.Y, p2.Z), fe_mul(p2.Y, p1.Z));
}

Point point_mul(const u256& s, const Point& p) {
  Point q = point_identity();
  for (int i = 255; i >= 0; --i) {
    q = point_add(q, q);
    if ((s[i >> 5] >> (i & 31)) & 1u) q = point_add(q, p);
  }
  return q;
}

// recover_x: RFC 8032 §5.1.3 (mũ (p+3)/8, nhân sqrt(-1) nếu cần).
bool recover_x(const u256& y, int sign, u256& x_out) {
  if (bn_cmp(y.data(), 8, FE_P.data(), 8) >= 0) return false;
  const u256 y2 = fe_sq(y);
  const u256 u = fe_sub(y2, fe_one());
  const u256 v = fe_add(fe_mul(fe_d(), y2), fe_one());
  const u256 x2 = fe_mul(u, fe_inv(v));
  if (fe_is_zero(x2)) {
    if (sign) return false;
    x_out = fe_zero();
    return true;
  }
  // (p+3)/8 = 2^252 - 2
  static const u256 e = [] {
    u256 x{};
    x[7] = 0x10000000u;
    bn_sub_into(x.data(), 8, fe_from_u64(2).data(), 8);
    return x;
  }();
  u256 x = fe_pow(x2, e);
  if (!fe_eq(fe_sq(x), x2)) {
    x = fe_mul(x, fe_sqrt_m1());
    if (!fe_eq(fe_sq(x), x2)) return false;
  }
  if (fe_is_odd(x) != (sign != 0)) x = fe_neg(x);
  x_out = x;
  return true;
}

bool point_decompress(const uint8_t in[32], Point& out) {
  const int sign = (in[31] >> 7) & 1;
  const u256 y = fe_from_bytes(in);
  u256 x;
  if (!recover_x(y, sign, x)) return false;
  out = Point{x, y, fe_one(), fe_mul(x, y)};
  return true;
}

void point_compress(const Point& p, uint8_t out[32]) {
  const u256 zinv = fe_inv(p.Z);
  const u256 x = fe_mul(p.X, zinv);
  const u256 y = fe_mul(p.Y, zinv);
  u256_to_bytes(y, out);
  if (fe_is_odd(x)) out[31] |= 0x80u;
}

const Point& base_point() {
  // B: y = 4/5, x = recover_x(y, 0) — RFC 8032 §5.1 (điểm cơ sở).
  static const Point g = [] {
    const u256 gy = fe_mul(fe_from_u64(4), fe_inv(fe_from_u64(5)));
    u256 gx;
    recover_x(gy, 0, gx);
    return Point{gx, gy, fe_one(), fe_mul(gx, gy)};
  }();
  return g;
}

// ------------------------------------------------------------------ scalars --
u256 sc_reduce64(const uint8_t le[64]) {
  u512 t{};
  for (int i = 0; i < 16; ++i) {
    t[i] = (uint32_t)le[i * 4] | ((uint32_t)le[i * 4 + 1] << 8) | ((uint32_t)le[i * 4 + 2] << 16) |
           ((uint32_t)le[i * 4 + 3] << 24);
  }
  return bn_mod(t, SC_L);
}

// s = (k * a + r) mod L
u256 sc_muladd(const u256& k, const u256& a, const u256& r) {
  u512 t = bn_mul256(k, a);
  uint64_t carry = 0;
  for (int i = 0; i < 8; ++i) {
    const uint64_t s = (uint64_t)t[i] + r[i] + carry;
    t[i] = (uint32_t)s;
    carry = s >> 32;
  }
  for (int i = 8; carry && i < 16; ++i) {
    const uint64_t s = (uint64_t)t[i] + carry;
    t[i] = (uint32_t)s;
    carry = s >> 32;
  }
  return bn_mod(t, SC_L);
}

// secret_expand: RFC 8032 §5.1.5 — a = clamp(SHA-512(seed)[0..32]), prefix = [32..64].
void secret_expand(const Seed& seed, u256& a_out, uint8_t prefix[32]) {
  const Sha512Digest h = sha512(seed.data(), seed.size());
  uint8_t tmp[32];
  std::memcpy(tmp, h.bytes.data(), 32);
  tmp[0] &= 248;    // xóa 3 bit thấp nhất
  tmp[31] &= 63;    // xóa bit cao nhất
  tmp[31] |= 64;    // đặt bit cao thứ hai
  a_out = fe_from_bytes(tmp);
  std::memcpy(prefix, h.bytes.data() + 32, 32);
}

}  // namespace

// -------------------------------------------------------------- SHA-512 API --
Sha512::Sha512() {
  state_ = {0x6a09e667f3bcc908ull, 0xbb67ae8584caa73bull, 0x3c6ef372fe94f82bull, 0xa54ff53a5f1d36f1ull,
            0x510e527fade682d1ull, 0x9b05688c2b3e6c1full, 0x1f83d9abfb41bd6bull, 0x5be0cd19137e2179ull};
  buf_.fill(0);
}

void Sha512::compress(const uint8_t* block) { sha512_compress(state_.data(), block); }

void Sha512::update(const void* data, size_t len) {
  total_len_ += len;
  if (len == 0) return;
  const uint8_t* p = static_cast<const uint8_t*>(data);
  while (len > 0) {
    const size_t take = std::min(128 - buf_len_, len);
    std::memcpy(buf_.data() + buf_len_, p, take);
    buf_len_ += take;
    p += take;
    len -= take;
    if (buf_len_ == 128) {
      compress(buf_.data());
      buf_len_ = 0;
    }
  }
}

void Sha512::update(const std::vector<uint8_t>& data) { update(data.data(), data.size()); }
void Sha512::update(const std::string& text) { update(text.data(), text.size()); }

Sha512Digest Sha512::finalize() const {
  Sha512 s = *this;  // bản sao: finalize không phá trạng thái
  const uint64_t bits = s.total_len_ * 8;
  s.buf_[s.buf_len_++] = 0x80;
  if (s.buf_len_ > 112) {
    while (s.buf_len_ < 128) s.buf_[s.buf_len_++] = 0;
    s.compress(s.buf_.data());
    s.buf_len_ = 0;
  }
  while (s.buf_len_ < 120) s.buf_[s.buf_len_++] = 0;
  // SHA-512: trường độ dài là 128-bit big-endian chiếm 16 byte CUỐI khối
  // (112..128) — 8 byte cao để 0, 8 byte thấp chứa số bit. (Ghi vào 112..120
  // là lỗi đã từng có: SHA-512("") vẫn đúng vì độ dài 0, mọi thông điệp khác
  // sai — vector FIPS bắt được ngay.)
  for (int i = 0; i < 8; ++i) {
    s.buf_[120 + i] = (uint8_t)(bits >> (56 - 8 * i));
  }
  s.buf_len_ = 128;
  s.compress(s.buf_.data());
  // nén khối thứ hai nếu padding tràn (buf_len_ > 112 đã xử lý ở trên) —
  // còn lại: hai khối padding luôn nằm gọn vì buf_len_ <= 112 sau khi nén.
  Sha512Digest out;
  for (int i = 0; i < 8; ++i) {
    for (int j = 0; j < 8; ++j) {
      out.bytes[i * 8 + j] = (uint8_t)(s.state_[i] >> (56 - 8 * j));
    }
  }
  return out;
}

Sha512Digest sha512(const void* data, size_t len) {
  Sha512 h;
  h.update(data, len);
  return h.finalize();
}

Sha512Digest sha512(const std::vector<uint8_t>& data) { return sha512(data.data(), data.size()); }

// --------------------------------------------------------------- Ed25519 API --
PublicKey public_key(const Seed& seed) {
  u256 a;
  uint8_t prefix[32];
  secret_expand(seed, a, prefix);
  PublicKey out;
  point_compress(point_mul(a, base_point()), out.data());
  return out;
}

Signature sign(const Seed& seed, const void* msg, size_t len) {
  u256 a;
  uint8_t prefix[32];
  secret_expand(seed, a, prefix);
  const uint8_t* m = static_cast<const uint8_t*>(msg);

  // A = [a]B (tính trực tiếp, không gọi public_key để khỏi nhân cơ sở lần hai)
  PublicKey pub;
  point_compress(point_mul(a, base_point()), pub.data());

  // r = SHA-512(prefix || M) mod L
  Sha512 h1;
  h1.update(prefix, 32);
  h1.update(m, len);
  const Sha512Digest d1 = h1.finalize();
  const u256 r = sc_reduce64(d1.bytes.data());

  Signature sig{};
  point_compress(point_mul(r, base_point()), sig.data());

  // k = SHA-512(R || A || M) mod L
  Sha512 h2;
  h2.update(sig.data(), 32);
  h2.update(pub.data(), 32);
  h2.update(m, len);
  const Sha512Digest d2 = h2.finalize();
  const u256 k = sc_reduce64(d2.bytes.data());

  // S = (r + k*a) mod L
  const u256 s = sc_muladd(k, a, r);
  u256_to_bytes(s, sig.data() + 32);
  return sig;
}

Signature sign(const Seed& seed, const std::vector<uint8_t>& msg) {
  return sign(seed, msg.data(), msg.size());
}

bool verify(const PublicKey& pub, const void* msg, size_t len, const Signature& sig) {
  Point a_point;
  if (!point_decompress(pub.data(), a_point)) return false;
  Point r_point;
  if (!point_decompress(sig.data(), r_point)) return false;
  // S phải < L (RFC 8032 §5.1.7 bước 1: "S being out of range") là từ chối.
  u256 s{};
  for (int i = 0; i < 8; ++i) {
    s[i] = (uint32_t)sig[32 + i * 4] | ((uint32_t)sig[32 + i * 4 + 1] << 8) |
           ((uint32_t)sig[32 + i * 4 + 2] << 16) | ((uint32_t)sig[32 + i * 4 + 3] << 24);
  }
  if (bn_cmp(s.data(), 8, SC_L.data(), 8) >= 0) return false;

  const uint8_t* m = static_cast<const uint8_t*>(msg);
  Sha512 h;
  h.update(sig.data(), 32);
  h.update(pub.data(), 32);
  h.update(m, len);
  const Sha512Digest d = h.finalize();
  const u256 k = sc_reduce64(d.bytes.data());

  // [S]B == R + [k]A
  const Point s_b = point_mul(s, base_point());
  const Point k_a = point_mul(k, a_point);
  return point_equal(s_b, point_add(r_point, k_a));
}

bool verify(const PublicKey& pub, const std::vector<uint8_t>& msg, const Signature& sig) {
  return verify(pub, msg.data(), msg.size(), sig);
}

std::string to_hex(const uint8_t* data, size_t len) {
  static const char* digits = "0123456789abcdef";
  std::string out;
  out.reserve(len * 2);
  for (size_t i = 0; i < len; ++i) {
    out.push_back(digits[data[i] >> 4]);
    out.push_back(digits[data[i] & 0xF]);
  }
  return out;
}

bool from_hex(const std::string& hex, uint8_t* out, size_t len) {
  if (hex.size() != len * 2) return false;
  auto nib = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  for (size_t i = 0; i < len; ++i) {
    const int hi = nib(hex[i * 2]);
    const int lo = nib(hex[i * 2 + 1]);
    if (hi < 0 || lo < 0) return false;
    out[i] = (uint8_t)((hi << 4) | lo);
  }
  return true;
}

}  // namespace yui::ed25519
