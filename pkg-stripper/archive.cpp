// Giải nén an toàn đa định dạng (P4.33) — xem include/yui/archive.hpp.
#include "yui/archive.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace yui::archive {

// ================================================================ luồng ====
size_t MemorySource::read(uint8_t* buf, size_t n) {
  const size_t k = std::min(n, n_ - off_);
  if (k) std::memcpy(buf, p_ + off_, k);
  off_ += k;
  return k;
}

// ================================================================ CRC-32 ===
namespace {
struct CrcTable {
  uint32_t t[256];
  CrcTable() {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      t[i] = c;
    }
  }
};
const CrcTable& crc_table() {
  static const CrcTable tab;
  return tab;
}
}  // namespace

uint32_t crc32_update(uint32_t crc, const uint8_t* p, size_t n) {
  const auto& t = crc_table().t;
  crc = ~crc;
  for (size_t i = 0; i < n; ++i) crc = t[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
  return ~crc;
}

// ============================================================== DEFLATE ====
namespace {

class BitReader {
 public:
  explicit BitReader(ByteSource& s) : src_(s) {}

  bool fill_byte() {
    if (pos_ == len_) {
      len_ = src_.read(buf_, sizeof buf_);
      pos_ = 0;
      if (len_ == 0) return false;
    }
    bitbuf_ |= uint64_t(buf_[pos_++]) << bitcnt_;
    bitcnt_ += 8;
    ++consumed_;
    return true;
  }
  bool need(int n) {
    while (bitcnt_ < n)
      if (!fill_byte()) return false;
    return true;
  }
  uint32_t peek(int n, int& avail) {
    while (bitcnt_ < n && fill_byte()) {
    }
    avail = std::min(bitcnt_, n);
    return uint32_t(bitbuf_ & ((uint64_t(1) << n) - 1));
  }
  void drop(int n) {
    bitbuf_ >>= n;
    bitcnt_ -= n;
  }
  bool bits(int n, uint32_t& v) {
    if (n == 0) {
      v = 0;
      return true;
    }
    if (!need(n)) return false;
    v = uint32_t(bitbuf_ & ((uint64_t(1) << n) - 1));
    drop(n);
    return true;
  }
  void align() { drop(bitcnt_ % 8); }
  bool byte(uint8_t& b) {
    if (bitcnt_ < 8 && !fill_byte()) return false;
    b = uint8_t(bitbuf_ & 0xFF);
    drop(8);
    return true;
  }
  bool u16le(uint32_t& v) {
    uint8_t a, b;
    if (!byte(a) || !byte(b)) return false;
    v = uint32_t(a) | (uint32_t(b) << 8);
    return true;
  }
  bool u32le(uint32_t& v) {
    uint32_t lo, hi;
    if (!u16le(lo) || !u16le(hi)) return false;
    v = lo | (hi << 16);
    return true;
  }
  // Còn byte nào chưa dùng không (sau align)?
  bool at_end() {
    if (bitcnt_ >= 8) return false;
    if (pos_ < len_) return false;
    len_ = src_.read(buf_, sizeof buf_);
    pos_ = 0;
    return len_ == 0;
  }
  uint64_t consumed_bytes() const { return consumed_ - uint64_t(bitcnt_ / 8); }
  bool source_failed() const { return src_.failed(); }

 private:
  ByteSource& src_;
  uint8_t buf_[65536];
  size_t pos_ = 0, len_ = 0;
  uint64_t bitbuf_ = 0;
  int bitcnt_ = 0;
  uint64_t consumed_ = 0;
};

constexpr int kMaxBits = 15;
constexpr int kFastBits = 10;
constexpr int kMaxLCodes = 286;
constexpr int kMaxDCodes = 30;
constexpr int kFixLCodes = 288;

struct Huffman {
  uint16_t count[kMaxBits + 1];
  uint16_t symbol[kFixLCodes];
  uint16_t fast[1 << kFastBits];
};

// Trả 0 = đủ, > 0 = thiếu (incomplete), < 0 = thừa (over-subscribed).
int build(Huffman& h, const uint8_t* length, int n) {
  std::memset(h.count, 0, sizeof h.count);
  std::memset(h.fast, 0, sizeof h.fast);
  for (int s = 0; s < n; ++s) h.count[length[s]]++;
  if (h.count[0] == n) return 0;  // không có mã nào: đủ, nhưng giải sẽ báo lỗi
  int left = 1;
  for (int len = 1; len <= kMaxBits; ++len) {
    left <<= 1;
    left -= h.count[len];
    if (left < 0) return left;
  }
  uint16_t offs[kMaxBits + 1];
  offs[1] = 0;
  for (int len = 1; len < kMaxBits; ++len) offs[len + 1] = uint16_t(offs[len] + h.count[len]);
  for (int s = 0; s < n; ++s)
    if (length[s] != 0) h.symbol[offs[length[s]]++] = uint16_t(s);
  // Bảng tra nhanh cho mã ≤ kFastBits bit (mã đọc đảo bit, LSB trước).
  // Mã chính tắc: next[1] = 0; next[len] = (next[len-1] + count[len-1]) << 1.
  uint32_t next[kMaxBits + 1] = {0};
  for (int len = 2; len <= kMaxBits; ++len) next[len] = (next[len - 1] + h.count[len - 1]) << 1;
  for (int s = 0; s < n; ++s) {
    const int len = length[s];
    if (len == 0) continue;
    const uint32_t c = next[len]++;
    if (len > kFastBits) continue;
    uint32_t r = 0;
    for (int i = 0; i < len; ++i) r |= ((c >> i) & 1u) << (len - 1 - i);
    for (uint32_t k = r; k < (1u << kFastBits); k += (1u << len)) h.fast[k] = uint16_t((len << 9) | s);
  }
  return left;
}

// < 0: -1 hết dữ liệu, -2 mã không hợp lệ.
int decode(BitReader& br, const Huffman& h) {
  int avail = 0;
  const uint32_t p = br.peek(kFastBits, avail);
  const uint16_t e = h.fast[p];
  if (e != 0) {
    const int len = e >> 9;
    if (len <= avail) {
      br.drop(len);
      return e & 511;
    }
  }
  int code = 0, first = 0, index = 0;
  for (int len = 1; len <= kMaxBits; ++len) {
    uint32_t b;
    if (!br.bits(1, b)) return -1;
    code |= int(b);
    const int count = h.count[len];
    if (code - count < first) return h.symbol[index + (code - first)];
    index += count;
    first += count;
    first <<= 1;
    code <<= 1;
  }
  return -2;
}

constexpr uint16_t kLenBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                   31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr uint8_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                   2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr uint16_t kDistBase[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                                    193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6,
                                    6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

// Cửa sổ trượt 64 KiB: giữ 32 KiB cho tham chiếu lùi, đẩy ra sink theo khối.
class Window {
 public:
  Window(ByteSink& sink, uint64_t max_out) : sink_(sink), max_out_(max_out), buf_(kSize) {}

  bool put(uint8_t b) {
    if (total_ >= max_out_) {
      overflow = true;
      return false;
    }
    buf_[pos_ & kMask] = b;
    ++pos_;
    ++pending_;
    ++total_;
    if (pending_ >= 32768) return flush();
    return true;
  }
  bool copy(uint32_t dist, uint32_t len) {
    if (dist > total_ || dist > 32768) {
      bad_distance = true;
      return false;
    }
    for (uint32_t i = 0; i < len; ++i)
      if (!put(buf_[(pos_ - dist) & kMask])) return false;
    return true;
  }
  bool flush() {
    size_t start = (pos_ - pending_) & kMask;
    size_t left = pending_;
    while (left) {
      const size_t k = std::min(left, kSize - start);
      crc = crc32_update(crc, buf_.data() + start, k);
      if (!sink_.write(buf_.data() + start, k)) {
        sink_failed = true;
        return false;
      }
      start = (start + k) & kMask;
      left -= k;
    }
    pending_ = 0;
    return true;
  }
  uint64_t total() const { return total_; }

  bool overflow = false, bad_distance = false, sink_failed = false;
  uint32_t crc = 0;

 private:
  static constexpr size_t kSize = 65536, kMask = kSize - 1;
  ByteSink& sink_;
  uint64_t max_out_;
  std::vector<uint8_t> buf_;
  uint64_t pos_ = 0;
  size_t pending_ = 0;
  uint64_t total_ = 0;
};

const Huffman& fixed_len() {
  static const Huffman h = [] {
    Huffman t;
    uint8_t l[kFixLCodes];
    int s = 0;
    for (; s < 144; ++s) l[s] = 8;
    for (; s < 256; ++s) l[s] = 9;
    for (; s < 280; ++s) l[s] = 7;
    for (; s < kFixLCodes; ++s) l[s] = 8;
    build(t, l, kFixLCodes);
    return t;
  }();
  return h;
}
const Huffman& fixed_dist() {
  static const Huffman h = [] {
    Huffman t;
    uint8_t l[kMaxDCodes];
    for (auto& x : l) x = 5;
    build(t, l, kMaxDCodes);
    return t;
  }();
  return h;
}

std::string window_error(const Window& w) {
  if (w.overflow) return "dữ liệu giải nén vượt trần cho phép (nghi bom nén) — dừng";
  if (w.bad_distance) return "DEFLATE hỏng: khoảng cách tham chiếu lùi quá xa";
  if (w.sink_failed) return "ghi dữ liệu ra bị dừng";
  return "DEFLATE hỏng";
}

std::string codes(BitReader& br, Window& w, const Huffman& lc, const Huffman& dc) {
  for (;;) {
    int sym = decode(br, lc);
    if (sym < 0) return sym == -1 ? "DEFLATE bị cắt cụt (hết dữ liệu giữa khối)" : "DEFLATE hỏng: mã độ dài/ký tự không hợp lệ";
    if (sym < 256) {
      if (!w.put(uint8_t(sym))) return window_error(w);
      continue;
    }
    if (sym == 256) return "";
    sym -= 257;
    if (sym >= 29) return "DEFLATE hỏng: mã độ dài ngoài bảng (286/287)";
    uint32_t extra;
    if (!br.bits(kLenExtra[sym], extra)) return "DEFLATE bị cắt cụt";
    const uint32_t len = kLenBase[sym] + extra;
    const int ds = decode(br, dc);
    if (ds < 0) return ds == -1 ? "DEFLATE bị cắt cụt" : "DEFLATE hỏng: mã khoảng cách không hợp lệ";
    if (ds >= 30) return "DEFLATE hỏng: mã khoảng cách ngoài bảng (30/31)";
    if (!br.bits(kDistExtra[ds], extra)) return "DEFLATE bị cắt cụt";
    if (!w.copy(kDistBase[ds] + extra, len)) return window_error(w);
  }
}

std::string dynamic_block(BitReader& br, Window& w) {
  static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
  uint32_t nlen, ndist, ncode;
  if (!br.bits(5, nlen) || !br.bits(5, ndist) || !br.bits(4, ncode)) return "DEFLATE bị cắt cụt";
  nlen += 257;
  ndist += 1;
  ncode += 4;
  if (nlen > kMaxLCodes || ndist > kMaxDCodes) return "DEFLATE hỏng: số mã trong khối động quá lớn";
  uint8_t lengths[kMaxLCodes + kMaxDCodes] = {0};
  for (uint32_t i = 0; i < ncode; ++i) {
    uint32_t v;
    if (!br.bits(3, v)) return "DEFLATE bị cắt cụt";
    lengths[order[i]] = uint8_t(v);
  }
  Huffman lc, dc;
  if (build(lc, lengths, 19) != 0) return "DEFLATE hỏng: bảng mã độ dài không đầy đủ";
  uint32_t idx = 0;
  while (idx < nlen + ndist) {
    int sym = decode(br, lc);
    if (sym < 0) return "DEFLATE hỏng: mã độ dài mã không hợp lệ";
    if (sym < 16) {
      lengths[idx++] = uint8_t(sym);
      continue;
    }
    uint8_t len = 0;
    uint32_t rep;
    if (sym == 16) {
      if (idx == 0) return "DEFLATE hỏng: lặp độ dài khi chưa có độ dài trước";
      len = lengths[idx - 1];
      if (!br.bits(2, rep)) return "DEFLATE bị cắt cụt";
      rep += 3;
    } else if (sym == 17) {
      if (!br.bits(3, rep)) return "DEFLATE bị cắt cụt";
      rep += 3;
    } else {
      if (!br.bits(7, rep)) return "DEFLATE bị cắt cụt";
      rep += 11;
    }
    if (idx + rep > nlen + ndist) return "DEFLATE hỏng: lặp độ dài vượt số mã";
    while (rep--) lengths[idx++] = len;
  }
  if (lengths[256] == 0) return "DEFLATE hỏng: thiếu mã kết thúc khối";
  int err = build(lc, lengths, int(nlen));
  if (err < 0 || (err > 0 && int(nlen) - lc.count[0] != 1)) return "DEFLATE hỏng: bảng mã ký tự/độ dài sai";
  err = build(dc, lengths + nlen, int(ndist));
  if (err < 0 || (err > 0 && int(ndist) - dc.count[0] != 1)) return "DEFLATE hỏng: bảng mã khoảng cách sai";
  return codes(br, w, lc, dc);
}

std::string stored_block(BitReader& br, Window& w) {
  br.align();
  uint32_t len, nlen;
  if (!br.u16le(len) || !br.u16le(nlen)) return "DEFLATE bị cắt cụt (khối stored)";
  if (len != (~nlen & 0xFFFF)) return "DEFLATE hỏng: LEN/NLEN của khối stored không khớp";
  for (uint32_t i = 0; i < len; ++i) {
    uint8_t b;
    if (!br.byte(b)) return "DEFLATE bị cắt cụt (khối stored)";
    if (!w.put(b)) return window_error(w);
  }
  return "";
}

std::string inflate_core(BitReader& br, Window& w) {
  uint32_t last = 0;
  do {
    uint32_t type;
    if (!br.bits(1, last) || !br.bits(2, type)) return "DEFLATE bị cắt cụt (thiếu header khối)";
    std::string e;
    if (type == 0) e = stored_block(br, w);
    else if (type == 1) e = codes(br, w, fixed_len(), fixed_dist());
    else if (type == 2) e = dynamic_block(br, w);
    else e = "DEFLATE hỏng: loại khối 3 (dành riêng)";
    if (!e.empty()) return e;
  } while (!last);
  if (!w.flush()) return window_error(w);
  br.align();
  return "";
}

}  // namespace

InflateResult inflate(ByteSource& src, ByteSink& sink, uint64_t max_out) {
  InflateResult r;
  BitReader br(src);
  Window w(sink, max_out);
  r.error = inflate_core(br, w);
  if (r.error.empty() && br.source_failed()) r.error = "lỗi đọc tệp nén";
  r.ok = r.error.empty();
  r.in_bytes = br.consumed_bytes();
  r.out_bytes = w.total();
  r.crc = w.crc;
  return r;
}

InflateResult inflate(const std::vector<uint8_t>& in, std::vector<uint8_t>& out, uint64_t max_out) {
  MemorySource s(in);
  VectorSink v;
  auto r = inflate(s, v, max_out);
  out.swap(v.data);
  return r;
}

InflateResult gunzip(ByteSource& src, ByteSink& sink, uint64_t max_out) {
  InflateResult r;
  BitReader br(src);
  uint64_t total = 0;
  uint32_t crc_all = 0;
  int members = 0;
  for (;;) {
    if (members > 0 && br.at_end()) break;
    uint8_t h[10];
    for (int i = 0; i < 10; ++i) {
      if (!br.byte(h[i])) {
        r.error = members == 0 ? "gzip rỗng hoặc bị cắt cụt (thiếu header)" : "gzip có rác sau dữ liệu";
        return r;
      }
    }
    if (h[0] != 0x1f || h[1] != 0x8b) {
      r.error = members == 0 ? "không phải gzip (sai chữ ký 1f 8b)" : "gzip có rác sau dữ liệu";
      return r;
    }
    if (h[2] != 8) {
      r.error = "gzip dùng phương pháp nén lạ (CM ≠ 8)";
      return r;
    }
    const uint8_t flg = h[3];
    if (flg & 0xE0) {
      r.error = "gzip hỏng: bit cờ dành riêng được bật";
      return r;
    }
    uint32_t hcrc = crc32_update(0, h, 10);
    auto take = [&](uint8_t& b) {
      if (!br.byte(b)) return false;
      hcrc = crc32_update(hcrc, &b, 1);
      return true;
    };
    if (flg & 0x04) {  // FEXTRA
      uint8_t a, b;
      if (!take(a) || !take(b)) {
        r.error = "gzip bị cắt cụt (FEXTRA)";
        return r;
      }
      const uint32_t xlen = uint32_t(a) | (uint32_t(b) << 8);
      for (uint32_t i = 0; i < xlen; ++i) {
        uint8_t x;
        if (!take(x)) {
          r.error = "gzip bị cắt cụt (FEXTRA)";
          return r;
        }
      }
    }
    for (uint8_t f : {uint8_t(0x08), uint8_t(0x10)}) {  // FNAME, FCOMMENT
      if (!(flg & f)) continue;
      size_t n = 0;
      for (;;) {
        uint8_t c;
        if (!take(c)) {
          r.error = "gzip bị cắt cụt (tên/ghi chú)";
          return r;
        }
        if (c == 0) break;
        if (++n > 65536) {
          r.error = "gzip hỏng: tên/ghi chú quá dài";
          return r;
        }
      }
    }
    if (flg & 0x02) {  // FHCRC
      uint32_t want;
      if (!br.u16le(want)) {
        r.error = "gzip bị cắt cụt (FHCRC)";
        return r;
      }
      if ((hcrc & 0xFFFF) != want) {
        r.error = "gzip hỏng: CRC của header không khớp";
        return r;
      }
    }
    Window w(sink, max_out - total);
    r.error = inflate_core(br, w);
    if (!r.error.empty()) {
      r.out_bytes = total + w.total();
      return r;
    }
    uint32_t crc, isize;
    if (!br.u32le(crc) || !br.u32le(isize)) {
      r.error = "gzip bị cắt cụt (thiếu CRC/ISIZE cuối)";
      return r;
    }
    if (crc != w.crc) {
      r.error = "gzip hỏng: CRC-32 không khớp (dữ liệu bị sửa hoặc hỏng)";
      return r;
    }
    if (isize != uint32_t(w.total() & 0xFFFFFFFFu)) {
      r.error = "gzip hỏng: ISIZE không khớp kích thước thật";
      return r;
    }
    crc_all = members == 0 ? w.crc : 0;  // CRC tổng chỉ báo cho gzip một member
    total += w.total();
    ++members;
  }
  if (br.source_failed()) {
    r.error = "lỗi đọc tệp nén";
    return r;
  }
  r.ok = true;
  r.in_bytes = br.consumed_bytes();
  r.out_bytes = total;
  r.crc = crc_all;
  return r;
}

InflateResult gunzip(const std::vector<uint8_t>& in, std::vector<uint8_t>& out, uint64_t max_out) {
  MemorySource s(in);
  VectorSink v;
  auto r = gunzip(s, v, max_out);
  out.swap(v.data);
  return r;
}

// ============================================================ bộ nén nhỏ ===
std::vector<uint8_t> deflate_stored(const std::vector<uint8_t>& in) {
  std::vector<uint8_t> out;
  size_t off = 0;
  do {
    const size_t k = std::min<size_t>(65535, in.size() - off);
    const bool last = off + k == in.size();
    out.push_back(last ? 1 : 0);
    out.push_back(uint8_t(k & 0xFF));
    out.push_back(uint8_t(k >> 8));
    out.push_back(uint8_t(~k & 0xFF));
    out.push_back(uint8_t((~k >> 8) & 0xFF));
    out.insert(out.end(), in.begin() + long(off), in.begin() + long(off + k));
    off += k;
  } while (off < in.size());
  return out;
}

namespace {
class BitWriter {
 public:
  std::vector<uint8_t> out;
  void put(uint32_t v, int n) {  // LSB trước
    for (int i = 0; i < n; ++i) {
      if (cnt_ == 0) out.push_back(0);
      out.back() |= uint8_t(((v >> i) & 1u) << cnt_);
      cnt_ = (cnt_ + 1) & 7;
    }
  }
  void code(uint32_t c, int n) {  // mã Huffman: MSB của mã trước
    for (int i = n - 1; i >= 0; --i) put((c >> i) & 1u, 1);
  }

 private:
  int cnt_ = 0;
};

void fixed_symbol(BitWriter& w, int sym) {
  if (sym < 144) w.code(0x30 + uint32_t(sym), 8);
  else if (sym < 256) w.code(0x190 + uint32_t(sym - 144), 9);
  else if (sym < 280) w.code(uint32_t(sym - 256), 7);
  else w.code(0xC0 + uint32_t(sym - 280), 8);
}
}  // namespace

std::vector<uint8_t> deflate_fixed(const std::vector<uint8_t>& in) {
  BitWriter w;
  w.put(1, 1);
  w.put(1, 2);
  size_t i = 0;
  while (i < in.size()) {
    size_t run = 0;
    if (i > 0)
      while (i + run < in.size() && run < 258 && in[i + run] == in[i - 1]) ++run;
    if (run >= 3) {
      int idx = 28;
      if (run < 258) {
        idx = 0;
        while (idx + 1 < 28 && kLenBase[idx + 1] <= run) ++idx;
      }
      fixed_symbol(w, 257 + idx);
      w.put(uint32_t(run - kLenBase[idx]), kLenExtra[idx]);
      w.code(0, 5);  // khoảng cách 1
      i += run;
    } else {
      fixed_symbol(w, in[i]);
      ++i;
    }
  }
  fixed_symbol(w, 256);
  return w.out;
}

std::vector<uint8_t> gzip_wrap(const std::vector<uint8_t>& raw, const std::vector<uint8_t>& original,
                               const std::string& name) {
  std::vector<uint8_t> out;
  out.reserve(10 + name.size() + 1 + raw.size() + 8);
  const uint8_t head[10] = {0x1f, 0x8b, 8, uint8_t(name.empty() ? 0 : 0x08), 0, 0, 0, 0, 0, 3};
  for (uint8_t b : head) out.push_back(b);
  if (!name.empty()) {
    for (char ch : name) out.push_back(uint8_t(ch));
    out.push_back(0);
  }
  for (uint8_t b : raw) out.push_back(b);
  const uint32_t c = crc32(original.data(), original.size());
  const uint32_t n = uint32_t(original.size() & 0xFFFFFFFFu);
  for (int i = 0; i < 4; ++i) out.push_back(uint8_t(c >> (8 * i)));
  for (int i = 0; i < 4; ++i) out.push_back(uint8_t(n >> (8 * i)));
  return out;
}

// ============================================================ nhận dạng ====
namespace {
bool tar_checksum_ok(const uint8_t* h) {
  uint64_t want = 0;
  bool digits = false;
  for (int i = 148; i < 156; ++i) {
    const uint8_t c = h[i];
    if (c >= '0' && c <= '7') {
      want = want * 8 + (c - '0');
      digits = true;
    } else if (c == ' ' || c == 0) {
      if (digits) break;
    } else {
      return false;
    }
  }
  if (!digits) return false;
  uint64_t sum = 0;
  for (int i = 0; i < 512; ++i) sum += (i >= 148 && i < 156) ? uint8_t(' ') : h[i];
  return sum == want;
}
}  // namespace

const char* format_name(Format f) {
  switch (f) {
    case Format::Zip: return "zip";
    case Format::Gzip: return "gz";
    case Format::Tar: return "tar";
    case Format::Zstd: return "zstd";
    case Format::SevenZip: return "7z";
    case Format::Rar: return "rar";
    case Format::Xz: return "xz";
    case Format::Bzip2: return "bzip2";
    case Format::Unknown: break;
  }
  return "không rõ";
}

Format detect(const uint8_t* h, size_t n) {
  auto is = [&](std::initializer_list<uint8_t> sig) {
    if (n < sig.size()) return false;
    size_t i = 0;
    for (uint8_t b : sig)
      if (h[i++] != b) return false;
    return true;
  };
  if (is({'P', 'K', 3, 4}) || is({'P', 'K', 5, 6})) return Format::Zip;
  if (is({0x1f, 0x8b})) return Format::Gzip;
  if (is({0x28, 0xb5, 0x2f, 0xfd})) return Format::Zstd;
  if (is({'7', 'z', 0xBC, 0xAF, 0x27, 0x1C})) return Format::SevenZip;
  if (is({'R', 'a', 'r', '!', 0x1a, 0x07})) return Format::Rar;
  if (is({0xfd, '7', 'z', 'X', 'Z', 0})) return Format::Xz;
  if (is({'B', 'Z', 'h'})) return Format::Bzip2;
  if (n >= 512 && tar_checksum_ok(h)) return Format::Tar;
  return Format::Unknown;
}

// ============================================================ đường dẫn ====
namespace {
bool valid_utf8(const std::string& s) {
  size_t i = 0;
  while (i < s.size()) {
    const uint8_t c = uint8_t(s[i]);
    if (c < 0x80) {
      ++i;
      continue;
    }
    int n;
    uint32_t cp;
    if ((c & 0xE0) == 0xC0) {
      n = 1;
      cp = c & 0x1F;
    } else if ((c & 0xF0) == 0xE0) {
      n = 2;
      cp = c & 0x0F;
    } else if ((c & 0xF8) == 0xF0) {
      n = 3;
      cp = c & 0x07;
    } else {
      return false;
    }
    if (i + size_t(n) >= s.size()) return false;  // thiếu byte tiếp nối
    for (int k = 1; k <= n; ++k) {
      const uint8_t d = uint8_t(s[i + size_t(k)]);
      if ((d & 0xC0) != 0x80) return false;
      cp = (cp << 6) | (d & 0x3F);
    }
    if ((n == 1 && cp < 0x80) || (n == 2 && cp < 0x800) || (n == 3 && cp < 0x10000)) return false;  // overlong
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
    i += size_t(n) + 1;
  }
  return true;
}
}  // namespace

bool safe_entry_path(const std::string& p, const Limits& lim, std::string* why) {
  auto no = [&](const std::string& m) {
    if (why) *why = m;
    return false;
  };
  if (p.empty()) return no("tên rỗng");
  if (p.size() > lim.max_path_bytes) return no("đường dẫn quá dài");
  if (p[0] == '/') return no("đường dẫn tuyệt đối");
  for (char c : p) {
    const uint8_t u = uint8_t(c);
    if (u < 0x20 || u == 0x7F) return no("tên chứa ký tự điều khiển hoặc \\0");
    if (c == '\\') return no("tên chứa dấu '\\' (đường dẫn kiểu Windows, có thể thoát thư mục)");
  }
  if (!valid_utf8(p)) return no("tên không phải UTF-8 hợp lệ");
  if (p.size() >= 2 && p[1] == ':' && ((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')))
    return no("đường dẫn có ổ đĩa kiểu Windows");
  size_t depth = 0, start = 0;
  while (start <= p.size()) {
    size_t slash = p.find('/', start);
    if (slash == std::string::npos) slash = p.size();
    const std::string part = p.substr(start, slash - start);
    if (part.empty()) return no("đường dẫn có thành phần rỗng ('//')");
    if (part == "." || part == "..") return no("đường dẫn chứa '" + part + "' (chống zip-slip)");
    if (part.size() > 255) return no("tên một thành phần quá 255 byte");
    if (++depth > lim.max_depth) return no("thư mục lồng quá sâu");
    start = slash + 1;
    if (slash == p.size()) break;
  }
  return true;
}

// ============================================================ ghi an toàn ==
namespace {

struct Failure {
  std::string code, msg;
  bool set(const std::string& c, const std::string& m) {
    if (code.empty()) {
      code = c;
      msg = m;
    }
    return false;
  }
};

class Writer {
 public:
  Writer(std::string dest, const Limits& lim, std::string origin, uint64_t archive_size, Failure& f)
      : dest_(std::move(dest)), lim_(lim), origin_(std::move(origin)), archive_size_(archive_size), f_(f) {}
  ~Writer() {
    if (file_fd_ >= 0) ::close(file_fd_);
    if (dest_fd_ >= 0) ::close(dest_fd_);
  }

  bool open() {
    if (::mkdir(dest_.c_str(), 0755) == 0) {
      created_dest_ = true;
    } else if (errno != EEXIST) {
      return f_.set("io", "không tạo được thư mục đích " + dest_ + ": " + std::strerror(errno));
    }
    dest_fd_ = ::open(dest_.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (dest_fd_ < 0) return f_.set("io", "thư mục đích không mở được (hoặc là liên kết): " + dest_);
    if (!created_dest_) {
      DIR* d = ::fdopendir(::dup(dest_fd_));
      if (!d) return f_.set("io", "không đọc được thư mục đích");
      bool empty = true;
      while (dirent* e = ::readdir(d)) {
        if (std::strcmp(e->d_name, ".") && std::strcmp(e->d_name, "..")) {
          empty = false;
          break;
        }
      }
      ::closedir(d);
      if (!empty) return f_.set("exists", "thư mục đích đã có dữ liệu — chọn thư mục mới hoặc rỗng (không ghi đè)");
    }
    return true;
  }

  bool check_entry(const std::string& path) {
    std::string why;
    if (!safe_entry_path(path, lim_, &why)) return f_.set("unsafe_path", "đường dẫn không an toàn '" + printable(path) + "': " + why);
    if (++entries_ > lim_.max_entries) return f_.set("bomb", "quá nhiều mục trong tệp nén (> " + std::to_string(lim_.max_entries) + ")");
    return true;
  }

  bool make_dir(const std::string& path) {
    if (!check_entry(path)) return false;
    int fd = open_dir(path);
    if (fd < 0) return false;
    ::close(fd);
    return true;
  }

  bool begin_file(const std::string& path, uint32_t mode) {
    if (!check_entry(path)) return false;
    const size_t slash = path.rfind('/');
    const std::string parent = slash == std::string::npos ? "" : path.substr(0, slash);
    const std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    int pfd = parent.empty() ? ::dup(dest_fd_) : open_dir(parent);
    if (pfd < 0) return false;
    const mode_t m = (mode & 0111) ? 0755 : 0644;  // bỏ setuid/setgid/sticky, quyền nhóm/khác ghi
    file_fd_ = ::openat(pfd, name.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, m);
    const int e = errno;
    ::close(pfd);
    if (file_fd_ < 0) {
      if (e == EEXIST) return f_.set("exists", "tệp trùng tên trong gói (không ghi đè): '" + printable(path) + "'");
      return f_.set("io", "không tạo được tệp '" + printable(path) + "': " + std::strerror(e));
    }
    created_.push_back({path, false});
    cur_path_ = path;
    cur_bytes_ = 0;
    return true;
  }

  bool write(const uint8_t* p, size_t n) {
    if (file_fd_ < 0) return f_.set("corrupt", "dữ liệu ngoài tệp");
    cur_bytes_ += n;
    total_ += n;
    if (cur_bytes_ > lim_.max_entry_bytes)
      return f_.set("bomb", "tệp '" + printable(cur_path_) + "' vượt trần kích thước mỗi tệp (nghi bom nén)");
    if (total_ > lim_.max_total_bytes) return f_.set("bomb", "tổng dữ liệu giải nén vượt trần (nghi bom nén)");
    if (total_ > lim_.ratio_floor_bytes && total_ / std::max<uint64_t>(archive_size_, 1) > lim_.max_ratio)
      return f_.set("bomb", "tỉ lệ nén vượt " + std::to_string(lim_.max_ratio) + ":1 (nghi bom nén)");
    while (n) {
      const ssize_t w = ::write(file_fd_, p, n);
      if (w < 0) {
        if (errno == EINTR) continue;
        return f_.set("io", std::string("lỗi ghi tệp: ") + std::strerror(errno));
      }
      p += w;
      n -= size_t(w);
    }
    return true;
  }

  bool end_file() {
    if (file_fd_ < 0) return true;
    if (!origin_.empty() &&
        ::fsetxattr(file_fd_, "user.yui.nguon", origin_.data(), origin_.size(), 0) == 0)
      ++labeled_;
    const int rc = ::close(file_fd_);
    file_fd_ = -1;
    if (rc != 0) return f_.set("io", "lỗi đóng tệp");
    ++files_;
    return true;
  }

  void rollback() {
    if (file_fd_ >= 0) {
      ::close(file_fd_);
      file_fd_ = -1;
    }
    for (auto it = created_.rbegin(); it != created_.rend(); ++it)
      ::unlinkat(dest_fd_, it->first.c_str(), it->second ? AT_REMOVEDIR : 0);
    created_.clear();
    if (created_dest_) {
      ::close(dest_fd_);
      dest_fd_ = -1;
      ::rmdir(dest_.c_str());
    }
  }

  std::vector<std::string> paths() const {
    std::vector<std::string> v;
    for (const auto& c : created_) v.push_back(c.first + (c.second ? "/" : ""));
    return v;
  }
  size_t files() const { return files_; }
  size_t dirs() const {
    size_t n = 0;
    for (const auto& c : created_) n += c.second ? 1 : 0;
    return n;
  }
  uint64_t total() const { return total_; }
  size_t labeled() const { return labeled_; }

  static std::string printable(const std::string& s) {
    std::string o;
    for (char c : s) {
      const uint8_t u = uint8_t(c);
      if (u < 0x20 || u == 0x7F) {
        char b[8];
        std::snprintf(b, sizeof b, "\\x%02x", u);
        o += b;
      } else {
        o += c;
      }
      if (o.size() > 200) {
        o += "…";
        break;
      }
    }
    return o;
  }

 private:
  // Mở (và tạo nếu cần) thư mục tương đối, từng tầng bằng openat + O_NOFOLLOW.
  int open_dir(const std::string& rel) {
    int fd = ::dup(dest_fd_);
    std::string sofar;
    size_t start = 0;
    while (fd >= 0 && start < rel.size()) {
      size_t slash = rel.find('/', start);
      if (slash == std::string::npos) slash = rel.size();
      const std::string part = rel.substr(start, slash - start);
      sofar += (sofar.empty() ? "" : "/") + part;
      if (::mkdirat(fd, part.c_str(), 0755) == 0) {
        created_.push_back({sofar, true});
      } else if (errno != EEXIST) {
        f_.set("io", "không tạo được thư mục '" + printable(sofar) + "': " + std::strerror(errno));
        ::close(fd);
        return -1;
      }
      const int nfd = ::openat(fd, part.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
      ::close(fd);
      if (nfd < 0) {
        f_.set("unsafe_path", "'" + printable(sofar) + "' không phải thư mục thật (liên kết hoặc tệp) — dừng");
        return -1;
      }
      fd = nfd;
      start = slash + 1;
    }
    return fd;
  }

  std::string dest_;
  const Limits& lim_;
  std::string origin_;
  uint64_t archive_size_;
  Failure& f_;
  int dest_fd_ = -1, file_fd_ = -1;
  bool created_dest_ = false;
  std::vector<std::pair<std::string, bool>> created_;  // (đường dẫn, là thư mục)
  std::string cur_path_;
  uint64_t cur_bytes_ = 0, total_ = 0;
  size_t entries_ = 0, files_ = 0, labeled_ = 0;
};

class WriterSink : public ByteSink {
 public:
  explicit WriterSink(Writer& w) : w_(w) {}
  bool write(const uint8_t* p, size_t n) override { return w_.write(p, n); }

 private:
  Writer& w_;
};

// ------------------------------------------------------------ nguồn tệp ----
class FdSource : public ByteSource {
 public:
  FdSource(int fd, uint64_t off, uint64_t len) : fd_(fd), off_(off), left_(len) {}
  size_t read(uint8_t* buf, size_t n) override {
    if (left_ == 0 || failed_) return 0;
    const size_t want = size_t(std::min<uint64_t>(n, left_));
    ssize_t r;
    do {
      r = ::pread(fd_, buf, want, off_t(off_));
    } while (r < 0 && errno == EINTR);
    if (r <= 0) {
      if (r < 0) failed_ = true;
      return 0;
    }
    off_ += uint64_t(r);
    left_ -= uint64_t(r);
    return size_t(r);
  }
  bool failed() const override { return failed_; }

 private:
  int fd_;
  uint64_t off_, left_;
  bool failed_ = false;
};

// ---------------------------------------------------------------- tar ------
// Bộ đọc tar kiểu đẩy (push): nhận dữ liệu từng mẩu (từ tệp hoặc từ gunzip).
class TarSink : public ByteSink {
 public:
  TarSink(Writer& w, Failure& f) : w_(w), f_(f) {}

  bool write(const uint8_t* p, size_t n) override {
    while (n) {
      if (state_ == End) return true;  // phần đệm sau khối kết thúc: bỏ qua
      if (state_ == Header) {
        const size_t k = std::min(n, size_t(512) - hdr_len_);
        std::memcpy(hdr_ + hdr_len_, p, k);
        hdr_len_ += k;
        p += k;
        n -= k;
        if (hdr_len_ == 512) {
          hdr_len_ = 0;
          if (!on_header()) return false;
        }
        continue;
      }
      if (state_ == Body) {
        const size_t k = size_t(std::min<uint64_t>(n, left_));
        if (body_kind_ == ToFile) {
          if (!w_.write(p, k)) return false;
        } else if (body_kind_ == ToMeta) {
          if (meta_.size() + k > 1 << 20) return f_.set("corrupt", "tar hỏng: header mở rộng quá lớn");
          meta_.append(reinterpret_cast<const char*>(p), k);
        }
        left_ -= k;
        p += k;
        n -= k;
        if (left_ == 0 && !end_body()) return false;
        continue;
      }
      // Pad
      const size_t k = size_t(std::min<uint64_t>(n, left_));
      left_ -= k;
      p += k;
      n -= k;
      if (left_ == 0) state_ = Header;
    }
    return true;
  }

  bool finish() {
    if (state_ == End) return true;
    if (state_ == Header && hdr_len_ == 0 && saw_any_ && zero_blocks_ > 0) return true;
    return f_.set("corrupt", "tar bị cắt cụt (thiếu khối kết thúc) — tải dở?");
  }

 private:
  enum State { Header, Body, Pad, End };
  enum BodyKind { ToFile, ToMeta, Skip };

  static std::string cstr(const uint8_t* p, size_t n) {
    size_t k = 0;
    while (k < n && p[k]) ++k;
    return std::string(reinterpret_cast<const char*>(p), k);
  }
  static bool octal(const uint8_t* p, size_t n, uint64_t& v) {
    v = 0;
    size_t i = 0;
    while (i < n && (p[i] == ' ' || p[i] == 0)) ++i;
    bool any = false;
    for (; i < n && p[i] >= '0' && p[i] <= '7'; ++i) {
      v = v * 8 + (p[i] - '0');
      any = true;
    }
    for (; i < n; ++i)
      if (p[i] != ' ' && p[i] != 0) return false;
    if (!any) v = 0;
    return true;
  }

  bool on_header() {
    bool zero = true;
    for (uint8_t b : hdr_)
      if (b) {
        zero = false;
        break;
      }
    if (zero) {
      if (++zero_blocks_ >= 2) state_ = End;
      return true;
    }
    if (zero_blocks_ > 0) return f_.set("corrupt", "tar hỏng: dữ liệu sau khối rỗng");
    saw_any_ = true;
    if (!tar_checksum_ok(hdr_)) return f_.set("corrupt", "tar hỏng: sai checksum header");
    if (hdr_[124] & 0x80) return f_.set("unsupported", "tar dùng kích thước base-256 (tệp > 8 GiB) — chưa hỗ trợ");
    uint64_t size = 0, mode = 0;
    if (!octal(hdr_ + 124, 12, size) || !octal(hdr_ + 100, 8, mode))
      return f_.set("corrupt", "tar hỏng: trường số không phải bát phân");
    if (pax_size_set_) {
      size = pax_size_;
      pax_size_set_ = false;
    }
    const char type = char(hdr_[156]);
    std::string name = cstr(hdr_, 100);
    if (std::memcmp(hdr_ + 257, "ustar", 5) == 0) {
      const std::string prefix = cstr(hdr_ + 345, 155);
      if (!prefix.empty()) name = prefix + "/" + name;
    }
    if (!long_name_.empty()) {
      name = long_name_;
      long_name_.clear();
    }
    left_ = size;
    pad_ = (512 - size % 512) % 512;
    body_kind_ = Skip;
    meta_kind_ = type;
    meta_.clear();
    switch (type) {
      case '0':
      case '\0':
      case '7':
      case '5': {
        while (name.size() > 2 && name.compare(0, 2, "./") == 0) name = name.substr(2);
        const bool dir = type == '5' || (!name.empty() && name.back() == '/');
        while (!name.empty() && name.back() == '/') name.pop_back();
        if (dir) {
          if (name.empty() || name == ".") break;  // "./" gốc
          if (!w_.make_dir(name)) return false;
          break;
        }
        if (!w_.begin_file(name, uint32_t(mode))) return false;
        body_kind_ = ToFile;
        in_file_ = true;
        break;
      }
      case 'x':
      case 'L':
        body_kind_ = ToMeta;
        break;
      case 'g':
        break;
      case '1':
      case '2':
      case 'K':
        return f_.set("link", "gói chứa liên kết '" + Writer::printable(name) + "' — bị từ chối (chống thoát thư mục)");
      case '3':
      case '4':
      case '6':
        return f_.set("unsupported", "gói chứa tệp thiết bị/fifo '" + Writer::printable(name) + "' — bị từ chối");
      default:
        return f_.set("unsupported", std::string("gói chứa loại mục tar không hỗ trợ ('") + type + "')");
    }
    if (left_ == 0) return end_body();
    state_ = Body;
    return true;
  }

  bool end_body() {
    if (in_file_) {
      in_file_ = false;
      if (!w_.end_file()) return false;
    }
    if (body_kind_ == ToMeta) {
      if (meta_kind_ == 'L') {
        long_name_ = cstr(reinterpret_cast<const uint8_t*>(meta_.data()), meta_.size());
      } else if (!parse_pax()) {
        return false;
      }
    }
    left_ = pad_;
    state_ = left_ ? Pad : Header;
    return true;
  }

  bool parse_pax() {
    size_t p = 0;
    while (p < meta_.size()) {
      size_t sp = meta_.find(' ', p);
      if (sp == std::string::npos) return f_.set("corrupt", "tar hỏng: bản ghi pax sai");
      const size_t len = size_t(std::strtoull(meta_.substr(p, sp - p).c_str(), nullptr, 10));
      if (len == 0 || p + len > meta_.size() || len < sp - p + 2) return f_.set("corrupt", "tar hỏng: bản ghi pax sai độ dài");
      const std::string rec = meta_.substr(sp + 1, len - (sp + 1 - p) - 1);
      if (rec.compare(0, 5, "path=") == 0) long_name_ = rec.substr(5);
      if (rec.compare(0, 9, "linkpath=") == 0) return f_.set("link", "gói chứa liên kết (pax linkpath) — bị từ chối");
      if (rec.compare(0, 5, "size=") == 0) {
        pax_size_ = std::strtoull(rec.c_str() + 5, nullptr, 10);
        pax_size_set_ = true;
      }
      p += len;
    }
    return true;
  }

  Writer& w_;
  Failure& f_;
  State state_ = Header;
  uint8_t hdr_[512];
  size_t hdr_len_ = 0;
  uint64_t left_ = 0, pad_ = 0;
  BodyKind body_kind_ = Skip;
  char meta_kind_ = 0;
  std::string meta_, long_name_;
  uint64_t pax_size_ = 0;
  bool pax_size_set_ = false;
  bool in_file_ = false, saw_any_ = false;
  int zero_blocks_ = 0;
};

// Gzip: 512 byte đầu quyết định là tar.gz hay một tệp đơn.
class GzContentSink : public ByteSink {
 public:
  GzContentSink(Writer& w, Failure& f, std::string single_name)
      : w_(w), f_(f), tar_(w, f), single_name_(std::move(single_name)) {}
  bool write(const uint8_t* p, size_t n) override {
    if (mode_ == Undecided) {
      const size_t k = std::min(n, size_t(512) - head_.size());
      head_.insert(head_.end(), p, p + k);
      p += k;
      n -= k;
      if (head_.size() < 512) return true;
      if (!decide()) return false;
    }
    if (n == 0) return true;
    return mode_ == AsTar ? tar_.write(p, n) : w_.write(p, n);
  }
  bool finish() {
    if (mode_ == Undecided && !decide()) return false;
    if (mode_ == AsTar) return tar_.finish();
    return w_.end_file();
  }
  bool is_tar() const { return mode_ == AsTar; }

 private:
  bool decide() {
    if (head_.size() == 512 && tar_checksum_ok(head_.data())) {
      mode_ = AsTar;
      return tar_.write(head_.data(), head_.size());
    }
    mode_ = AsFile;
    if (!w_.begin_file(single_name_, 0644)) return false;
    return head_.empty() || w_.write(head_.data(), head_.size());
  }
  enum Mode { Undecided, AsTar, AsFile };
  Writer& w_;
  Failure& f_;
  TarSink tar_;
  std::string single_name_;
  Mode mode_ = Undecided;
  std::vector<uint8_t> head_;
};

// ---------------------------------------------------------------- zip ------
uint16_t le16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
uint32_t le32(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }

bool pread_all(int fd, uint8_t* buf, size_t n, uint64_t off) {
  while (n) {
    const ssize_t r = ::pread(fd, buf, n, off_t(off));
    if (r < 0 && errno == EINTR) continue;
    if (r <= 0) return false;
    buf += r;
    n -= size_t(r);
    off += uint64_t(r);
  }
  return true;
}

struct ZipEntry {
  std::string name;
  uint16_t flags = 0, method = 0, host = 0;
  uint32_t crc = 0, csize = 0, usize = 0, ext_attr = 0, local_off = 0;
  uint64_t data_start = 0;
  bool dir = false;
};

bool extract_zip(int fd, uint64_t size, Writer& w, const Limits& lim, Failure& f) {
  if (size < 22) return f.set("corrupt", "zip hỏng: quá ngắn");
  const size_t tail = size_t(std::min<uint64_t>(size, 22 + 65535));
  std::vector<uint8_t> t(tail);
  if (!pread_all(fd, t.data(), tail, size - tail)) return f.set("io", "không đọc được cuối tệp zip");
  long eocd = -1;
  for (long i = long(tail) - 22; i >= 0; --i) {
    if (le32(&t[size_t(i)]) == 0x06054b50 && size_t(i) + 22 + le16(&t[size_t(i) + 20]) == tail) {
      eocd = i;
      break;
    }
  }
  if (eocd < 0) return f.set("corrupt", "zip hỏng: không thấy End of Central Directory");
  const uint8_t* e = &t[size_t(eocd)];
  const uint64_t eocd_pos = size - tail + uint64_t(eocd);
  const uint16_t disk = le16(e + 4), cd_disk = le16(e + 6), n_disk = le16(e + 8), n_total = le16(e + 10);
  const uint32_t cd_size = le32(e + 12), cd_off = le32(e + 16);
  if (n_total == 0xFFFF || cd_size == 0xFFFFFFFFu || cd_off == 0xFFFFFFFFu)
    return f.set("unsupported", "zip dùng ZIP64 (> 65535 mục hoặc > 4 GiB) — chưa hỗ trợ");
  if (disk != 0 || cd_disk != 0 || n_disk != n_total) return f.set("unsupported", "zip nhiều phần (multi-disk) — chưa hỗ trợ");
  if (n_total > lim.max_entries) return f.set("bomb", "quá nhiều mục trong zip (> " + std::to_string(lim.max_entries) + ")");
  if (uint64_t(cd_off) + cd_size > eocd_pos) return f.set("corrupt", "zip hỏng: central directory nằm ngoài tệp");
  std::vector<uint8_t> cd(cd_size);
  if (cd_size && !pread_all(fd, cd.data(), cd_size, cd_off)) return f.set("io", "không đọc được central directory");

  std::vector<ZipEntry> ents;
  size_t p = 0;
  uint64_t declared_total = 0;
  for (uint32_t i = 0; i < n_total; ++i) {
    if (p + 46 > cd.size() || le32(&cd[p]) != 0x02014b50) return f.set("corrupt", "zip hỏng: mục central directory sai");
    const uint8_t* c = &cd[p];
    ZipEntry z;
    z.host = uint16_t(le16(c + 4) >> 8);
    z.flags = le16(c + 8);
    z.method = le16(c + 10);
    z.crc = le32(c + 16);
    z.csize = le32(c + 20);
    z.usize = le32(c + 24);
    const uint16_t nlen = le16(c + 28), xlen = le16(c + 30), clen = le16(c + 32);
    z.ext_attr = le32(c + 38);
    z.local_off = le32(c + 42);
    if (p + 46 + nlen + xlen + clen > cd.size()) return f.set("corrupt", "zip hỏng: mục central directory bị cắt");
    z.name.assign(reinterpret_cast<const char*>(c + 46), nlen);
    p += 46 + size_t(nlen) + xlen + clen;
    if (z.csize == 0xFFFFFFFFu || z.usize == 0xFFFFFFFFu || z.local_off == 0xFFFFFFFFu)
      return f.set("unsupported", "zip dùng ZIP64 — chưa hỗ trợ");
    if (z.flags & 0x41) return f.set("unsupported", "zip có mục mã hoá ('" + Writer::printable(z.name) + "') — không giải được");
    if (z.method != 0 && z.method != 8)
      return f.set("unsupported", "zip dùng phương pháp nén " + std::to_string(z.method) + " — chỉ hỗ trợ stored/deflate");
    if (z.name.find('\0') != std::string::npos) return f.set("unsafe_path", "tên trong zip chứa ký tự \\0 — bị từ chối");
    const uint32_t unix_mode = z.host == 3 ? (z.ext_attr >> 16) : 0;
    const uint32_t ftype = unix_mode & 0170000;
    if (ftype == 0120000) return f.set("link", "zip chứa liên kết '" + Writer::printable(z.name) + "' — bị từ chối (chống thoát thư mục)");
    if (ftype && ftype != 0100000 && ftype != 0040000)
      return f.set("unsupported", "zip chứa tệp thiết bị/fifo/socket — bị từ chối");
    z.dir = (!z.name.empty() && z.name.back() == '/') || ftype == 0040000 || (z.host == 0 && (z.ext_attr & 0x10));
    if (z.dir && z.usize != 0) return f.set("corrupt", "zip hỏng: thư mục có dữ liệu");
    if (z.usize > lim.max_entry_bytes)
      return f.set("bomb", "tệp '" + Writer::printable(z.name) + "' khai báo vượt trần mỗi tệp (nghi bom nén)");
    declared_total += z.usize;
    if (declared_total > lim.max_total_bytes) return f.set("bomb", "tổng kích thước khai báo vượt trần (nghi bom nén)");
    if (z.method == 0 && z.csize != z.usize) return f.set("corrupt", "zip hỏng: mục stored có kích thước nén ≠ thật");
    ents.push_back(std::move(z));
  }

  // Kiểm local header + vùng dữ liệu: khớp tên/phương pháp, không chồng nhau.
  std::vector<std::pair<uint64_t, uint64_t>> ranges;
  for (auto& z : ents) {
    uint8_t lh[30];
    if (uint64_t(z.local_off) + 30 > cd_off || !pread_all(fd, lh, 30, z.local_off) || le32(lh) != 0x04034b50)
      return f.set("corrupt", "zip hỏng: local header của '" + Writer::printable(z.name) + "' sai");
    const uint16_t nlen = le16(lh + 26), xlen = le16(lh + 28);
    if (le16(lh + 8) != z.method) return f.set("corrupt", "zip hỏng: local header khác central directory (phương pháp)");
    std::string lname(nlen, '\0');
    if (nlen && !pread_all(fd, reinterpret_cast<uint8_t*>(&lname[0]), nlen, uint64_t(z.local_off) + 30))
      return f.set("io", "không đọc được local header");
    if (lname != z.name) return f.set("unsafe_path", "zip hỏng/giả: tên ở local header khác central directory");
    z.data_start = uint64_t(z.local_off) + 30 + nlen + xlen;
    if (z.data_start + z.csize > cd_off) return f.set("corrupt", "zip hỏng: dữ liệu của mục tràn vào central directory");
    ranges.push_back({z.local_off, z.data_start + z.csize});
  }
  std::sort(ranges.begin(), ranges.end());
  for (size_t i = 1; i < ranges.size(); ++i)
    if (ranges[i].first < ranges[i - 1].second)
      return f.set("bomb", "zip có các mục dùng chung/chồng vùng dữ liệu (kiểu \"better zip bomb\") — bị từ chối");

  for (const auto& z : ents) {
    std::string name = z.name;
    if (z.dir) {
      while (!name.empty() && name.back() == '/') name.pop_back();
      if (!w.make_dir(name)) return false;
      continue;
    }
    const uint32_t unix_mode = z.host == 3 ? (z.ext_attr >> 16) : 0644;
    if (!w.begin_file(name, unix_mode)) return false;
    FdSource src(fd, z.data_start, z.csize);
    WriterSink sink(w);
    uint32_t crc = 0;
    uint64_t out = 0;
    if (z.method == 0) {
      uint8_t buf[65536];
      size_t k;
      while ((k = src.read(buf, sizeof buf)) > 0) {
        crc = crc32_update(crc, buf, k);
        if (!sink.write(buf, k)) return false;
        out += k;
      }
      if (src.failed()) return f.set("io", "lỗi đọc zip");
    } else {
      const auto r = inflate(src, sink, uint64_t(z.usize));
      if (!r.ok) {
        if (!f.code.empty()) return false;  // lỗi do Writer (trần, ghi) đã ghi lý do
        if (r.out_bytes >= z.usize && r.error.find("vượt trần") != std::string::npos)
          return f.set("bomb", "'" + Writer::printable(name) + "' giải ra nhiều hơn kích thước khai báo (nghi bom nén)");
        return f.set("corrupt", "'" + Writer::printable(name) + "': " + r.error);
      }
      crc = r.crc;
      out = r.out_bytes;
    }
    if (out != z.usize) return f.set("corrupt", "'" + Writer::printable(name) + "': kích thước thật khác khai báo");
    if (crc != z.crc) return f.set("corrupt", "'" + Writer::printable(name) + "': CRC-32 không khớp (dữ liệu hỏng/bị sửa)");
    if (!w.end_file()) return false;
  }
  return true;
}

std::string basename_of(const std::string& p) {
  const size_t s = p.rfind('/');
  return s == std::string::npos ? p : p.substr(s + 1);
}

bool ends_with(const std::string& s, const std::string& suf) {
  return s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
}

}  // namespace

ExtractReport extract_file(const std::string& archive_path, const std::string& dest_dir, const Limits& lim,
                           const std::string& origin_in) {
  ExtractReport rep;
  Failure f;
  const int fd = ::open(archive_path.c_str(), O_RDONLY | O_CLOEXEC);
  struct stat st {};
  if (fd < 0 || ::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
    if (fd >= 0) ::close(fd);
    rep.code = "io";
    rep.reason_vi = "không mở được tệp nén (hoặc không phải tệp thường): " + archive_path;
    return rep;
  }
  const uint64_t size = uint64_t(st.st_size);
  uint8_t head[512] = {0};
  const size_t hn = size_t(std::min<uint64_t>(size, 512));
  if (hn && !pread_all(fd, head, hn, 0)) {
    ::close(fd);
    rep.code = "io";
    rep.reason_vi = "không đọc được tệp nén";
    return rep;
  }
  const Format fmt = detect(head, hn);
  rep.format = format_name(fmt);
  if (fmt == Format::Zstd || fmt == Format::SevenZip || fmt == Format::Xz || fmt == Format::Bzip2 ||
      fmt == Format::Rar || fmt == Format::Unknown) {
    ::close(fd);
    rep.code = "unsupported";
    rep.reason_vi = fmt == Format::Rar
                        ? "RAR là định dạng đóng — YuiVieOS chưa giải (xem P7.18)"
                        : fmt == Format::Unknown ? "không nhận ra định dạng tệp nén (chỉ theo chữ ký đầu tệp, không theo đuôi tên)"
                                                 : std::string("định dạng ") + format_name(fmt) + " chưa được hỗ trợ (đang làm: zstd, 7z)";
    return rep;
  }
  const std::string origin = origin_in.empty() ? "giải nén từ " + basename_of(archive_path) : origin_in;
  Writer w(dest_dir, lim, origin, size, f);
  bool ok = w.open();
  if (ok) {
    if (fmt == Format::Zip) {
      ok = extract_zip(fd, size, w, lim, f);
    } else if (fmt == Format::Tar) {
      TarSink tar(w, f);
      FdSource src(fd, 0, size);
      uint8_t buf[65536];
      size_t k;
      while (ok && (k = src.read(buf, sizeof buf)) > 0) ok = tar.write(buf, k);
      if (ok && src.failed()) ok = f.set("io", "lỗi đọc tệp tar");
      if (ok) ok = tar.finish();
    } else {  // Gzip
      std::string base = basename_of(archive_path);
      if (ends_with(base, ".gz") && base.size() > 3) base = base.substr(0, base.size() - 3);
      else base += ".giai-nen";
      FdSource src(fd, 0, size);
      GzContentSink sink(w, f, base);
      const auto r = gunzip(src, sink, lim.max_total_bytes);
      if (!r.ok) {
        ok = f.code.empty() ? f.set(r.error.find("vượt trần") != std::string::npos ? "bomb" : "corrupt", r.error) : false;
      } else {
        ok = sink.finish();
      }
      rep.format = sink.is_tar() ? "tar.gz" : "gz";
    }
  }
  ::close(fd);
  if (!ok) {
    w.rollback();
    rep.code = f.code.empty() ? "corrupt" : f.code;
    rep.reason_vi = "Từ chối giải nén: " + (f.msg.empty() ? std::string("lỗi không rõ") : f.msg) +
                    " — không để lại tệp nào";
    return rep;
  }
  rep.ok = true;
  rep.files = w.files();
  rep.dirs = w.dirs();
  rep.bytes = w.total();
  rep.labeled = w.labeled();
  rep.paths = w.paths();
  rep.reason_vi = "Đã giải nén " + std::to_string(rep.files) + " tệp, " + std::to_string(rep.dirs) + " thư mục (" +
                  std::to_string(rep.bytes) + " byte, định dạng " + rep.format + ") vào " + dest_dir +
                  ". Không tệp nào được tự chạy.";
  return rep;
}

}  // namespace yui::archive
