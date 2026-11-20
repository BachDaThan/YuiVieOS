// Bộ giải Zstandard (RFC 8878) tự viết — phần zstd của P4.33.
//
// Theo đúng đặc tả và bộ giải "educational" của dự án zstd (cùng thứ tự đọc
// bit, cùng các phép kiểm hỏng). Chỉ giải frame KHÔNG từ điển (Dictionary_ID
// ≠ 0 ⇒ "chưa hỗ trợ"). Đọc/ghi theo luồng: vùng nhớ lịch sử lớn dần theo dữ
// liệu thật đã ra, tối đa = cửa sổ khai báo + 128 KiB, và cửa sổ bị chặn bởi
// `max_window` (mặc định 128 MiB, như giới hạn mặc định của zstd) — một tệp
// khai báo cửa sổ 2 TiB không làm hệ thống cấp phát gì.
#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "yui/archive.hpp"

namespace yui::archive {

// ================================================================ XXH64 =====
namespace {
constexpr uint64_t kP1 = 11400714785074694791ull, kP2 = 14029467366897019727ull, kP3 = 1609587929392839161ull,
                   kP4 = 9650029242287828579ull, kP5 = 2870177450012600261ull;
inline uint64_t rotl(uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }
inline uint64_t rd64(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
  return v;
}
inline uint32_t rd32(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }
inline uint64_t xround(uint64_t acc, uint64_t in) { return rotl(acc + in * kP2, 31) * kP1; }
inline uint64_t xmerge(uint64_t acc, uint64_t v) { return (acc ^ xround(0, v)) * kP1 + kP4; }

class Xxh64 {
 public:
  explicit Xxh64(uint64_t seed = 0) { reset(seed); }
  void reset(uint64_t seed) {
    seed_ = seed;
    v_[0] = seed + kP1 + kP2;
    v_[1] = seed + kP2;
    v_[2] = seed;
    v_[3] = seed - kP1;
    total_ = 0;
    mem_n_ = 0;
  }
  void update(const uint8_t* p, size_t n) {
    total_ += n;
    if (mem_n_ + n < 32) {
      if (n) std::memcpy(mem_ + mem_n_, p, n);
      mem_n_ += n;
      return;
    }
    if (mem_n_) {
      const size_t k = 32 - mem_n_;
      std::memcpy(mem_ + mem_n_, p, k);
      stripe(mem_);
      p += k;
      n -= k;
      mem_n_ = 0;
    }
    while (n >= 32) {
      stripe(p);
      p += 32;
      n -= 32;
    }
    if (n) std::memcpy(mem_, p, n);
    mem_n_ = n;
  }
  uint64_t digest() const {
    uint64_t h;
    if (total_ >= 32) {
      h = rotl(v_[0], 1) + rotl(v_[1], 7) + rotl(v_[2], 12) + rotl(v_[3], 18);
      for (uint64_t v : v_) h = xmerge(h, v);
    } else {
      h = seed_ + kP5;
    }
    h += total_;
    const uint8_t* p = mem_;
    size_t n = mem_n_;
    while (n >= 8) {
      h = rotl(h ^ xround(0, rd64(p)), 27) * kP1 + kP4;
      p += 8;
      n -= 8;
    }
    if (n >= 4) {
      h = rotl(h ^ (uint64_t(rd32(p)) * kP1), 23) * kP2 + kP3;
      p += 4;
      n -= 4;
    }
    while (n--) h = rotl(h ^ (uint64_t(*p++) * kP5), 11) * kP1;
    h ^= h >> 33;
    h *= kP2;
    h ^= h >> 29;
    h *= kP3;
    h ^= h >> 32;
    return h;
  }

 private:
  void stripe(const uint8_t* p) {
    for (int i = 0; i < 4; ++i) v_[i] = xround(v_[i], rd64(p + 8 * i));
  }
  uint64_t seed_ = 0, v_[4] = {0, 0, 0, 0}, total_ = 0;
  uint8_t mem_[32] = {0};
  size_t mem_n_ = 0;
};
}  // namespace

uint64_t xxh64(const uint8_t* p, size_t n, uint64_t seed) {
  Xxh64 h(seed);
  h.update(p, n);
  return h.digest();
}

// ============================================================ zstd lõi ======
namespace {

constexpr size_t kMaxBlock = 128 * 1024;
constexpr uint32_t kMagic = 0xFD2FB528u;

int hsb(uint32_t v) { return 31 - __builtin_clz(v); }  // v > 0

// Đọc bit "ngược" (luồng FSE/Huffman của zstd): bắt đầu từ bit cao nhất sau
// bit đánh dấu ở byte cuối, đi xuống. Đọc quá đầu luồng ⇒ phần thiếu là 0 và
// `off` âm (dùng để phát hiện hết luồng, đúng như đặc tả).
struct RevBits {
  const uint8_t* p = nullptr;
  size_t len = 0;
  int64_t off = 0;
  bool init(const uint8_t* src, size_t n) {
    p = src;
    len = n;
    if (n == 0 || src[n - 1] == 0) return false;
    off = int64_t(n) * 8 - (8 - hsb(src[n - 1]));
    return true;
  }
  uint64_t read(int nb) {
    if (nb <= 0) return 0;
    off -= nb;
    int64_t aoff = off;
    int ab = nb;
    if (off < 0) {
      ab += int(std::max<int64_t>(off, -64));
      aoff = 0;
      if (ab <= 0) return 0;
    }
    const size_t byte = size_t(aoff >> 3);
    const int sh = int(aoff & 7);
    uint64_t v = 0;
    const size_t avail = std::min<size_t>(8, len - byte);
    for (size_t i = 0; i < avail; ++i) v |= uint64_t(p[byte + i]) << (8 * i);
    v = (v >> sh) & ((uint64_t(1) << ab) - 1);
    if (off < 0) v <<= int(-off);
    return v;
  }
};

// Bit "xuôi" (mô tả bảng FSE): LSB trước, có kiểm biên.
struct FwdBits {
  const uint8_t* p;
  size_t len;
  size_t pos = 0;  // theo bit
  bool read(int nb, uint32_t& v) {
    if (pos + size_t(nb) > len * 8) return false;
    v = 0;
    for (int i = 0; i < nb; ++i, ++pos) v |= uint32_t((p[pos >> 3] >> (pos & 7)) & 1) << i;
    return true;
  }
};

struct Fse {
  int al = 0;
  bool valid = false;
  std::vector<uint8_t> sym, nb;
  std::vector<uint16_t> base;
};

bool fse_build(Fse& t, const int16_t* norm, int nsym, int al) {
  const uint32_t size = 1u << al;
  t.al = al;
  t.sym.assign(size, 0);
  t.nb.assign(size, 0);
  t.base.assign(size, 0);
  std::vector<uint16_t> next(size_t(nsym), 0);
  uint32_t high = size;
  for (int s = 0; s < nsym; ++s) {
    if (norm[s] == -1) {
      if (high == 0) return false;
      t.sym[--high] = uint8_t(s);
      next[size_t(s)] = 1;
    } else {
      next[size_t(s)] = uint16_t(std::max<int16_t>(norm[s], 0));
    }
  }
  const uint32_t step = (size >> 1) + (size >> 3) + 3, mask = size - 1;
  uint32_t pos = 0;
  for (int s = 0; s < nsym; ++s) {
    for (int i = 0; i < norm[s]; ++i) {
      t.sym[pos] = uint8_t(s);
      do {
        pos = (pos + step) & mask;
      } while (pos >= high);
    }
  }
  if (pos != 0) return false;
  for (uint32_t i = 0; i < size; ++i) {
    const uint16_t d = next[t.sym[i]]++;
    if (d == 0) return false;
    t.nb[i] = uint8_t(al - hsb(d));
    t.base[i] = uint16_t((uint32_t(d) << t.nb[i]) - size);
  }
  t.valid = true;
  return true;
}

void fse_rle(Fse& t, uint8_t s) {
  t.al = 0;
  t.sym.assign(1, s);
  t.nb.assign(1, 0);
  t.base.assign(1, 0);
  t.valid = true;
}

// Mô tả bảng FSE (RFC 8878 §4.1.1). `used` = số byte đã dùng.
bool fse_header(const uint8_t* p, size_t n, size_t& used, Fse& t, int max_al, int max_sym) {
  FwdBits in{p, n};
  uint32_t v;
  if (!in.read(4, v)) return false;
  const int al = 5 + int(v);
  if (al > max_al) return false;
  int32_t remaining = 1 << al;
  int16_t norm[256];
  int symb = 0;
  while (remaining > 0 && symb < 256) {
    const int bits = hsb(uint32_t(remaining + 1)) + 1;
    uint32_t val;
    if (!in.read(bits, val)) return false;
    const uint32_t lower = (1u << (bits - 1)) - 1;
    const uint32_t threshold = (1u << bits) - 1 - uint32_t(remaining + 1);
    if ((val & lower) < threshold) {
      in.pos -= 1;
      val &= lower;
    } else if (val > lower) {
      val -= threshold;
    }
    const int16_t proba = int16_t(int(val) - 1);
    remaining -= proba < 0 ? -proba : proba;
    norm[symb++] = proba;
    if (proba == 0) {
      uint32_t rep;
      if (!in.read(2, rep)) return false;
      for (;;) {
        for (uint32_t i = 0; i < rep && symb < 256; ++i) norm[symb++] = 0;
        if (rep != 3) break;
        if (!in.read(2, rep)) return false;
      }
    }
  }
  if (remaining != 0 || symb >= 256) return false;
  while (symb > 0 && norm[symb - 1] == 0) --symb;
  if (symb == 0 || symb - 1 > max_sym) return false;
  used = (in.pos + 7) / 8;
  return fse_build(t, norm, symb, al);
}

// Bảng mặc định (RFC 8878 §3.1.1.3.2.2).
const int16_t kLLDefault[36] = {4, 3, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1, 2, 2,
                                2, 2, 2, 2, 2, 2, 2, 3, 2, 1, 1, 1, 1, 1, -1, -1, -1, -1};
const int16_t kMLDefault[53] = {1, 4, 3, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
                                1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, -1, -1, -1, -1, -1, -1, -1};
const int16_t kOFDefault[29] = {1, 1, 1, 1, 1, 1, 2, 2, 2, 1, 1, 1, 1, 1, 1,
                                1, 1, 1, 1, 1, 1, 1, 1, 1, -1, -1, -1, -1, -1};

const uint32_t kLLBase[36] = {0,  1,  2,  3,  4,  5,  6,   7,   8,   9,   10,   11,   12,   13,   14,   15,    16,    18,
                              20, 22, 24, 28, 32, 40, 48, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768, 65536};
const uint8_t kLLBits[36] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 3, 3, 4, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
const uint32_t kMLBase[53] = {3,  4,  5,  6,  7,  8,  9,  10, 11, 12,  13,  14,  15,  16,   17,   18,   19,   20,
                              21, 22, 23, 24, 25, 26, 27, 28, 29, 30,  31,  32,  33,  34,   35,   37,   39,   41,
                              43, 47, 51, 59, 67, 83, 99, 131, 259, 515, 1027, 2051, 4099, 8195, 16387, 32771, 65539};
const uint8_t kMLBits[53] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                             0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 3, 3, 4, 4, 5, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};

const Fse& default_table(int which) {
  static Fse t[3];
  static bool init = [] {
    fse_build(t[0], kLLDefault, 36, 6);
    fse_build(t[1], kOFDefault, 29, 5);
    fse_build(t[2], kMLDefault, 53, 6);
    return true;
  }();
  (void)init;
  return t[which];
}

struct Huf {
  int max_bits = 0;
  bool valid = false;
  std::vector<uint8_t> sym, nb;
};

bool huf_from_weights(Huf& t, uint8_t* w, int n) {
  uint32_t sum = 0;
  for (int i = 0; i < n; ++i) {
    if (w[i] > 11) return false;
    if (w[i]) sum += 1u << (w[i] - 1);
  }
  if (sum == 0) return false;
  const int max_bits = hsb(sum) + 1;
  if (max_bits > 11) return false;
  const uint32_t left = (1u << max_bits) - sum;
  if (left & (left - 1)) return false;
  w[n++] = uint8_t(hsb(left) + 1);
  const uint32_t size = 1u << max_bits;
  uint8_t bits[256];
  uint32_t rank_count[13] = {0};
  for (int i = 0; i < n; ++i) {
    bits[i] = w[i] ? uint8_t(max_bits + 1 - w[i]) : 0;
    rank_count[bits[i]]++;
  }
  uint32_t rank_idx[13] = {0};
  t.sym.assign(size, 0);
  t.nb.assign(size, 0);
  rank_idx[max_bits] = 0;
  for (int i = max_bits; i >= 1; --i) {
    rank_idx[i - 1] = rank_idx[i] + rank_count[i] * (1u << (max_bits - i));
    if (rank_idx[i - 1] > size) return false;
    std::fill(t.nb.begin() + rank_idx[i], t.nb.begin() + rank_idx[i - 1], uint8_t(i));
  }
  if (rank_idx[0] != size) return false;
  for (int i = 0; i < n; ++i) {
    if (!bits[i]) continue;
    const uint32_t code = rank_idx[bits[i]], len = 1u << (max_bits - bits[i]);
    std::fill(t.sym.begin() + code, t.sym.begin() + code + len, uint8_t(i));
    rank_idx[bits[i]] += len;
  }
  t.max_bits = max_bits;
  t.valid = true;
  return true;
}

bool huf_tree(const uint8_t* p, size_t n, size_t& used, Huf& t) {
  if (n < 1) return false;
  const uint8_t hb = p[0];
  uint8_t w[257] = {0};
  int nw = 0;
  if (hb >= 128) {
    nw = hb - 127;
    const size_t bytes = size_t(nw + 1) / 2;
    if (1 + bytes > n) return false;
    for (int i = 0; i < nw; ++i) w[i] = (i % 2 == 0) ? uint8_t(p[1 + i / 2] >> 4) : uint8_t(p[1 + i / 2] & 15);
    used = 1 + bytes;
  } else {
    if (hb == 0 || 1 + size_t(hb) > n) return false;
    Fse ft;
    size_t hu = 0;
    if (!fse_header(p + 1, hb, hu, ft, 6, 12)) return false;
    if (hu >= hb) return false;
    RevBits rb;
    if (!rb.init(p + 1 + hu, hb - hu)) return false;
    uint32_t s1 = uint32_t(rb.read(ft.al)), s2 = uint32_t(rb.read(ft.al));
    for (;;) {
      if (nw >= 255) return false;
      w[nw++] = ft.sym[s1];
      s1 = ft.base[s1] + uint32_t(rb.read(ft.nb[s1]));
      if (rb.off < 0) {
        if (nw >= 255) return false;
        w[nw++] = ft.sym[s2];
        break;
      }
      if (nw >= 255) return false;
      w[nw++] = ft.sym[s2];
      s2 = ft.base[s2] + uint32_t(rb.read(ft.nb[s2]));
      if (rb.off < 0) {
        if (nw >= 255) return false;
        w[nw++] = ft.sym[s1];
        break;
      }
    }
    used = 1 + size_t(hb);
  }
  return huf_from_weights(t, w, nw);
}

bool huf_stream(const Huf& t, const uint8_t* p, size_t n, uint8_t* out, size_t cnt) {
  RevBits rb;
  if (!rb.init(p, n)) return false;
  const uint32_t mask = (1u << t.max_bits) - 1;
  uint32_t state = uint32_t(rb.read(t.max_bits));
  for (size_t i = 0; i < cnt; ++i) {
    out[i] = t.sym[state];
    const int nb = t.nb[state];
    state = ((state << nb) + uint32_t(rb.read(nb))) & mask;
  }
  return rb.off == -int64_t(t.max_bits);  // luồng phải dùng vừa hết
}

// Lịch sử ra (cửa sổ): lớn dần theo dữ liệu thật, rồi quay vòng.
class OutWin {
 public:
  OutWin(ByteSink& sink) : sink_(sink) {}
  void reset(uint64_t window) {
    window_ = window;
    cap_ = window + kMaxBlock;
    ring_.clear();
    pos_ = flushed_ = 0;
    hash_.reset(0);
  }
  uint64_t pos() const { return pos_; }
  void put_n(const uint8_t* p, size_t n) {
    while (n) {
      if (ring_.size() < cap_) {
        const size_t k = size_t(std::min<uint64_t>(n, cap_ - ring_.size()));
        ring_.insert(ring_.end(), p, p + k);
        pos_ += k;
        p += k;
        n -= k;
        continue;
      }
      const size_t idx = size_t(pos_ % cap_);
      const size_t k = size_t(std::min<uint64_t>(n, cap_ - idx));
      std::memcpy(&ring_[idx], p, k);
      pos_ += k;
      p += k;
      n -= k;
    }
  }
  void put_rle(uint8_t b, size_t n) {
    uint8_t buf[4096];
    std::memset(buf, b, sizeof buf);
    while (n) {
      const size_t k = std::min(n, sizeof buf);
      put_n(buf, k);
      n -= k;
    }
  }
  bool copy_match(uint64_t off, uint64_t len) {
    if (off == 0 || off > pos_ || off > window_) return false;
    const uint64_t s = pos_ - off;
    if (ring_.size() < cap_ && pos_ + len <= cap_) {
      ring_.resize(size_t(pos_ + len));
      uint8_t* d = ring_.data();
      for (uint64_t i = 0; i < len; ++i) d[pos_ + i] = d[s + i];
      pos_ += len;
      return true;
    }
    for (uint64_t i = 0; i < len; ++i) {
      const uint8_t b = ring_[size_t((s + i) % cap_)];
      if (ring_.size() < cap_) ring_.push_back(b);
      else ring_[size_t(pos_ % cap_)] = b;
      ++pos_;
    }
    return true;
  }
  bool flush() {
    while (flushed_ < pos_) {
      const size_t idx = size_t(flushed_ % cap_);
      const size_t k = size_t(std::min<uint64_t>(pos_ - flushed_, cap_ - idx));
      hash_.update(&ring_[idx], k);
      if (!sink_.write(&ring_[idx], k)) return false;
      flushed_ += k;
    }
    return true;
  }
  uint64_t digest() const { return hash_.digest(); }

 private:
  ByteSink& sink_;
  uint64_t window_ = 0, cap_ = 0, pos_ = 0, flushed_ = 0;
  std::vector<uint8_t> ring_;
  Xxh64 hash_;
};

class InBuf {
 public:
  explicit InBuf(ByteSource& s) : s_(s) {}
  bool read(uint8_t* out, size_t n) {
    while (n) {
      if (pos_ == len_ && !refill()) return false;
      const size_t k = std::min(n, len_ - pos_);
      std::memcpy(out, buf_ + pos_, k);
      pos_ += k;
      out += k;
      n -= k;
      consumed_ += k;
    }
    return true;
  }
  bool skip(uint64_t n) {
    while (n) {
      if (pos_ == len_ && !refill()) return false;
      const size_t k = size_t(std::min<uint64_t>(n, len_ - pos_));
      pos_ += k;
      n -= k;
      consumed_ += k;
    }
    return true;
  }
  bool at_end() { return pos_ == len_ && !refill(); }
  uint64_t consumed() const { return consumed_; }
  bool failed() const { return s_.failed(); }

 private:
  bool refill() {
    len_ = s_.read(buf_, sizeof buf_);
    pos_ = 0;
    return len_ > 0;
  }
  ByteSource& s_;
  uint8_t buf_[65536];
  size_t pos_ = 0, len_ = 0;
  uint64_t consumed_ = 0;
};

class Decoder {
 public:
  Decoder(ByteSource& src, ByteSink& sink) : in_(src), out_(sink) {}

  std::string run(uint64_t max_out, uint64_t max_window, InflateResult& r) {
    uint64_t total = 0;
    int frames = 0;
    for (;;) {
      if (frames > 0 && in_.at_end()) break;
      uint8_t m[4];
      if (!in_.read(m, 4)) return frames == 0 ? "zstd rỗng hoặc bị cắt cụt" : "zstd có rác sau dữ liệu";
      const uint32_t magic = rd32(m);
      ++frames;
      if ((magic & 0xFFFFFFF0u) == 0x184D2A50u) {  // frame bỏ qua (skippable)
        uint8_t s[4];
        if (!in_.read(s, 4) || !in_.skip(rd32(s))) return "zstd bị cắt cụt (frame bỏ qua)";
        continue;
      }
      if (magic != kMagic) return frames == 1 ? "không phải zstd (sai magic)" : "zstd có rác sau dữ liệu";
      std::string e = frame(max_out - std::min(max_out, total), max_window);
      if (!e.empty()) {
        r.out_bytes = total + out_.pos();
        return e;
      }
      total += out_.pos();
    }
    if (in_.failed()) return "lỗi đọc tệp nén";
    r.out_bytes = total;
    r.in_bytes = in_.consumed();
    return "";
  }

 private:
  std::string frame(uint64_t budget, uint64_t max_window) {
    uint8_t fhd;
    if (!in_.read(&fhd, 1)) return "zstd bị cắt cụt (header)";
    const int fcs_flag = fhd >> 6;
    const bool single = fhd & 0x20;
    const bool checksum = fhd & 0x04;
    const int dict_flag = fhd & 3;
    if (fhd & 0x08) return "zstd hỏng: bit dành riêng trong header được bật";
    uint64_t window = 0;
    if (!single) {
      uint8_t wd;
      if (!in_.read(&wd, 1)) return "zstd bị cắt cụt (window)";
      const int wlog = 10 + (wd >> 3);
      const uint64_t base = uint64_t(1) << wlog;
      window = base + (base / 8) * (wd & 7);
    }
    static const int kDictBytes[4] = {0, 1, 2, 4};
    uint8_t b[8] = {0};
    if (!in_.read(b, size_t(kDictBytes[dict_flag]))) return "zstd bị cắt cụt (dictionary id)";
    uint32_t dict = 0;
    for (int i = kDictBytes[dict_flag] - 1; i >= 0; --i) dict = (dict << 8) | b[i];
    if (dict != 0) return "zstd cần từ điển (Dictionary_ID " + std::to_string(dict) + ") — chưa hỗ trợ";
    static const int kFcsBytes[4] = {0, 2, 4, 8};
    const int fcs_n = fcs_flag == 0 ? (single ? 1 : 0) : kFcsBytes[fcs_flag];
    std::memset(b, 0, sizeof b);
    if (!in_.read(b, size_t(fcs_n))) return "zstd bị cắt cụt (kích thước nội dung)";
    uint64_t fcs = 0;
    for (int i = fcs_n - 1; i >= 0; --i) fcs = (fcs << 8) | b[i];
    if (fcs_n == 2) fcs += 256;
    const bool has_fcs = fcs_n > 0;
    if (single) window = fcs;
    if (window > max_window)
      return "zstd khai báo cửa sổ " + std::to_string(window >> 20) + " MiB — vượt giới hạn " +
             std::to_string(max_window >> 20) + " MiB (chưa hỗ trợ, kiểu `zstd --long`)";
    if (has_fcs && fcs > budget) return "zstd khai báo nội dung vượt trần dữ liệu ra (nghi bom nén)";
    const size_t block_max = size_t(std::min<uint64_t>(window, kMaxBlock));
    out_.reset(window);
    rep_[0] = 1;
    rep_[1] = 4;
    rep_[2] = 8;
    huf_.valid = false;
    for (auto& t : tab_) t.valid = false;
    for (;;) {
      uint8_t h[3];
      if (!in_.read(h, 3)) return "zstd bị cắt cụt (header khối)";
      const uint32_t bh = uint32_t(h[0]) | (uint32_t(h[1]) << 8) | (uint32_t(h[2]) << 16);
      const bool last = bh & 1;
      const int type = (bh >> 1) & 3;
      const size_t size = bh >> 3;
      if (type == 3) return "zstd hỏng: loại khối dành riêng (3)";
      if (size > block_max) return "zstd hỏng: khối lớn hơn giới hạn khối";
      if (type == 0) {
        buf_.resize(size);
        if (size && !in_.read(buf_.data(), size)) return "zstd bị cắt cụt (khối raw)";
        out_.put_n(buf_.data(), size);
      } else if (type == 1) {
        uint8_t v;
        if (!in_.read(&v, 1)) return "zstd bị cắt cụt (khối RLE)";
        out_.put_rle(v, size);
      } else {
        buf_.resize(size);
        if (!in_.read(buf_.data(), size)) return "zstd bị cắt cụt (khối nén)";
        const std::string e = compressed_block(buf_.data(), size, block_max);
        if (!e.empty()) return e;
      }
      if (out_.pos() > budget) return "dữ liệu giải nén vượt trần (nghi bom nén)";
      if (!out_.flush()) return "dừng ghi (bên nhận từ chối)";
      if (last) break;
    }
    if (has_fcs && out_.pos() != fcs) return "zstd hỏng: kích thước thật khác Frame_Content_Size";
    if (checksum) {
      uint8_t c[4];
      if (!in_.read(c, 4)) return "zstd bị cắt cụt (thiếu checksum)";
      if (rd32(c) != uint32_t(out_.digest() & 0xFFFFFFFFu))
        return "zstd hỏng: checksum XXH64 không khớp (dữ liệu bị sửa hoặc hỏng)";
    }
    return "";
  }

  std::string literals(const uint8_t* b, size_t n, size_t& used, size_t block_max) {
    if (n < 1) return "zstd hỏng: khối nén rỗng";
    const int type = b[0] & 3, sf = (b[0] >> 2) & 3;
    if (type <= 1) {
      size_t hl, regen;
      if (sf == 0 || sf == 2) {
        hl = 1;
        regen = b[0] >> 3;
      } else if (sf == 1) {
        hl = 2;
        if (n < 2) return "zstd hỏng: header literals cụt";
        regen = (b[0] >> 4) + (size_t(b[1]) << 4);
      } else {
        hl = 3;
        if (n < 3) return "zstd hỏng: header literals cụt";
        regen = (b[0] >> 4) + (size_t(b[1]) << 4) + (size_t(b[2]) << 12);
      }
      if (regen > block_max) return "zstd hỏng: literals quá lớn";
      if (type == 0) {
        if (hl + regen > n) return "zstd hỏng: literals raw tràn khối";
        lit_.assign(b + hl, b + hl + regen);
        used = hl + regen;
      } else {
        if (hl + 1 > n) return "zstd hỏng: literals RLE cụt";
        lit_.assign(regen, b[hl]);
        used = hl + 1;
      }
      return "";
    }
    size_t hl, regen, comp;
    const int streams = sf == 0 ? 1 : 4;
    if (sf <= 1) {
      hl = 3;
      if (n < 3) return "zstd hỏng: header literals cụt";
      const uint32_t h = uint32_t(b[0]) | (uint32_t(b[1]) << 8) | (uint32_t(b[2]) << 16);
      regen = (h >> 4) & 0x3FF;
      comp = (h >> 14) & 0x3FF;
    } else if (sf == 2) {
      hl = 4;
      if (n < 4) return "zstd hỏng: header literals cụt";
      const uint32_t h = rd32(b);
      regen = (h >> 4) & 0x3FFF;
      comp = (h >> 18) & 0x3FFF;
    } else {
      hl = 5;
      if (n < 5) return "zstd hỏng: header literals cụt";
      const uint64_t h = uint64_t(rd32(b)) | (uint64_t(b[4]) << 32);
      regen = size_t((h >> 4) & 0x3FFFF);
      comp = size_t((h >> 22) & 0x3FFFF);
    }
    if (regen > block_max) return "zstd hỏng: literals quá lớn";
    if (hl + comp > n) return "zstd hỏng: literals nén tràn khối";
    const uint8_t* c = b + hl;
    size_t cn = comp;
    if (type == 2) {
      size_t tu = 0;
      Huf t;
      if (!huf_tree(c, cn, tu, t)) return "zstd hỏng: cây Huffman sai";
      huf_ = std::move(t);
      c += tu;
      cn -= tu;
    } else if (!huf_.valid) {
      return "zstd hỏng: literals dùng lại cây Huffman khi chưa có";
    }
    lit_.resize(regen);
    if (streams == 1) {
      if (!huf_stream(huf_, c, cn, lit_.data(), regen)) return "zstd hỏng: luồng Huffman literals sai";
    } else {
      if (cn < 6) return "zstd hỏng: bảng nhảy 4 luồng cụt";
      const size_t s1 = c[0] | (size_t(c[1]) << 8), s2 = c[2] | (size_t(c[3]) << 8), s3 = c[4] | (size_t(c[5]) << 8);
      if (6 + s1 + s2 + s3 > cn) return "zstd hỏng: bảng nhảy 4 luồng sai";
      const size_t s4 = cn - 6 - s1 - s2 - s3;
      const size_t seg = (regen + 3) / 4;
      if (seg * 3 > regen) return "zstd hỏng: literals 4 luồng quá ít";
      const uint8_t* q = c + 6;
      const size_t sz[4] = {s1, s2, s3, s4};
      size_t o = 0;
      for (int i = 0; i < 4; ++i) {
        const size_t cnt = i < 3 ? seg : regen - 3 * seg;
        if (!huf_stream(huf_, q, sz[i], lit_.data() + o, cnt)) return "zstd hỏng: luồng Huffman literals sai";
        q += sz[i];
        o += cnt;
      }
    }
    used = hl + comp;
    return "";
  }

  std::string compressed_block(const uint8_t* b, size_t n, size_t block_max) {
    size_t used = 0;
    std::string e = literals(b, n, used, block_max);
    if (!e.empty()) return e;
    b += used;
    n -= used;
    if (n < 1) return "zstd hỏng: thiếu phần sequences";
    size_t p = 0, nseq = 0;
    if (b[0] < 128) {
      nseq = b[0];
      p = 1;
    } else if (b[0] < 255) {
      if (n < 2) return "zstd hỏng: số sequence cụt";
      nseq = (size_t(b[0] - 128) << 8) + b[1];
      p = 2;
    } else {
      if (n < 3) return "zstd hỏng: số sequence cụt";
      nseq = b[1] + (size_t(b[2]) << 8) + 0x7F00;
      p = 3;
    }
    if (nseq == 0) {  // chỉ có literals (số 0 có thể mã hoá 1, 2 hay 3 byte)
      if (p != n) return "zstd hỏng: dư dữ liệu sau 0 sequence";
      out_.put_n(lit_.data(), lit_.size());
      return "";
    }
    if (p >= n) return "zstd hỏng: thiếu chế độ nén ký hiệu";
    const uint8_t modes = b[p++];
    if (modes & 3) return "zstd hỏng: bit dành riêng trong chế độ nén ký hiệu";
    static const int kMaxAl[3] = {9, 8, 9}, kMaxSym[3] = {35, 31, 52};
    const int mode[3] = {(modes >> 6) & 3, (modes >> 4) & 3, (modes >> 2) & 3};  // LL, OF, ML
    for (int k = 0; k < 3; ++k) {
      switch (mode[k]) {
        case 0:
          tab_[k] = default_table(k);
          break;
        case 1:
          if (p >= n || b[p] > kMaxSym[k]) return "zstd hỏng: bảng RLE sai";
          fse_rle(tab_[k], b[p++]);
          break;
        case 2: {
          size_t u = 0;
          if (!fse_header(b + p, n - p, u, tab_[k], kMaxAl[k], kMaxSym[k])) return "zstd hỏng: bảng FSE sai";
          p += u;
          break;
        }
        default:
          if (!tab_[k].valid) return "zstd hỏng: dùng lại bảng FSE khi chưa có";
      }
    }
    RevBits rb;
    if (!rb.init(b + p, n - p)) return "zstd hỏng: luồng sequences sai";
    const Fse &ll = tab_[0], &of = tab_[1], &ml = tab_[2];
    uint32_t sll = uint32_t(rb.read(ll.al)), sof = uint32_t(rb.read(of.al)), sml = uint32_t(rb.read(ml.al));
    size_t litpos = 0, produced = 0;
    for (size_t s = 0; s < nseq; ++s) {
      const uint8_t ofc = of.sym[sof], llc = ll.sym[sll], mlc = ml.sym[sml];
      if (llc > 35 || mlc > 52 || ofc > 31) return "zstd hỏng: mã sequence ngoài phạm vi";
      const uint64_t offv = (uint64_t(1) << ofc) + rb.read(ofc);
      const uint64_t mlen = kMLBase[mlc] + rb.read(kMLBits[mlc]);
      const uint64_t llen = kLLBase[llc] + rb.read(kLLBits[llc]);
      if (s + 1 < nseq) {
        sll = ll.base[sll] + uint32_t(rb.read(ll.nb[sll]));
        sml = ml.base[sml] + uint32_t(rb.read(ml.nb[sml]));
        sof = of.base[sof] + uint32_t(rb.read(of.nb[sof]));
      }
      uint64_t off;
      if (offv > 3) {
        off = offv - 3;
        rep_[2] = rep_[1];
        rep_[1] = rep_[0];
        rep_[0] = off;
      } else {
        uint32_t idx = uint32_t(offv - 1);
        if (llen == 0) ++idx;
        if (idx == 0) {
          off = rep_[0];
        } else {
          off = idx < 3 ? rep_[idx] : rep_[0] - 1;
          if (idx > 1) rep_[2] = rep_[1];
          rep_[1] = rep_[0];
          rep_[0] = off;
        }
      }
      if (llen > lit_.size() - litpos) return "zstd hỏng: sequence đòi nhiều literals hơn có";
      produced += size_t(llen + mlen);
      if (produced > block_max) return "zstd hỏng: khối giải ra vượt giới hạn khối";
      out_.put_n(lit_.data() + litpos, size_t(llen));
      litpos += size_t(llen);
      if (!out_.copy_match(off, mlen)) return "zstd hỏng: khoảng cách tham chiếu không hợp lệ";
    }
    if (rb.off != 0) return "zstd hỏng: luồng sequences không dùng vừa hết";
    const size_t rest = lit_.size() - litpos;
    if (produced + rest > block_max) return "zstd hỏng: khối giải ra vượt giới hạn khối";
    out_.put_n(lit_.data() + litpos, rest);
    return "";
  }

  InBuf in_;
  OutWin out_;
  std::vector<uint8_t> buf_, lit_;
  Huf huf_;
  Fse tab_[3];
  uint64_t rep_[3] = {1, 4, 8};
};

}  // namespace

std::vector<uint8_t> zstd_store(const std::vector<uint8_t>& in) {
  std::vector<uint8_t> out = {0x28, 0xB5, 0x2F, 0xFD, 0xC4, 0x38};  // FCS 8 byte + checksum; cửa sổ 128 KiB
  for (int i = 0; i < 8; ++i) out.push_back(uint8_t(uint64_t(in.size()) >> (8 * i)));
  size_t off = 0;
  do {
    const size_t k = std::min(in.size() - off, kMaxBlock);
    const bool last = off + k == in.size();
    const uint32_t bh = uint32_t(k << 3) | (last ? 1u : 0u);  // loại 0 = raw
    for (int i = 0; i < 3; ++i) out.push_back(uint8_t(bh >> (8 * i)));
    out.insert(out.end(), in.begin() + long(off), in.begin() + long(off + k));
    off += k;
  } while (off < in.size());
  const uint64_t h = xxh64(in.data(), in.size(), 0);
  for (int i = 0; i < 4; ++i) out.push_back(uint8_t(h >> (8 * i)));
  return out;
}

InflateResult unzstd(ByteSource& src, ByteSink& sink, uint64_t max_out, uint64_t max_window) {
  InflateResult r;
  auto d = std::make_unique<Decoder>(src, sink);
  r.error = d->run(max_out, max_window, r);
  r.ok = r.error.empty();
  return r;
}

InflateResult unzstd(const std::vector<uint8_t>& in, std::vector<uint8_t>& out, uint64_t max_out,
                     uint64_t max_window) {
  MemorySource s(in);
  VectorSink v;
  auto r = unzstd(s, v, max_out, max_window);
  out.swap(v.data);
  return r;
}

}  // namespace yui::archive
