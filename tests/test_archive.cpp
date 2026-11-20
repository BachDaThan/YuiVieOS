// Giải nén an toàn đa định dạng + bộ giải DEFLATE tự viết (P4.33).
//
// DoD: đọc đúng đầu ra THẬT của zlib/gzip/zipfile/tarfile (khối Huffman động,
// pax, gzip nhiều member); chặn zip-slip (`..`, tuyệt đối, `\`, `C:`, ký tự
// điều khiển, UTF-8 hỏng), liên kết, thiết bị; bom nén (tỉ lệ, trần mỗi tệp,
// trần tổng, số mục, zip chồng vùng); CRC sai/cắt cụt ⇒ từ chối và KHÔNG để
// lại tệp nào; zstd/7z/RAR ⇒ "chưa hỗ trợ" rõ ràng; bỏ setuid; không ghi đè.
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <string>
#include <vector>

#include "archive_fixtures.hpp"
#include "testutil.hpp"
#include "yui/archive.hpp"

using namespace yui::archive;
using Bytes = std::vector<uint8_t>;

namespace {

std::string fixture_text() {  // giống hệt text() trong scripts/gen_archive_fixtures.py
  std::string s;
  char buf[128];
  for (int i = 0; i < 400; ++i) {
    std::snprintf(buf, sizeof buf, "Dòng %d: YuiVieOS giải nén an toàn — %d\n", i, i * i % 97);
    s += buf;
  }
  return s;
}

Bytes B(const std::string& s) { return Bytes(s.begin(), s.end()); }
std::string S(const Bytes& b) { return std::string(b.begin(), b.end()); }

std::string mkdtemp_dir(const std::string& tag) {
  std::string tmpl = "/tmp/yuitest-" + tag + "-XXXXXX";
  std::vector<char> buf(tmpl.begin(), tmpl.end());
  buf.push_back('\0');
  const char* dir = ::mkdtemp(buf.data());
  return dir ? std::string(dir) : std::string();
}

void write_bytes(const std::string& path, const Bytes& b) {
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return;
  if (!b.empty()) std::fwrite(b.data(), 1, b.size(), f);
  std::fclose(f);
}

bool read_bytes(const std::string& path, Bytes& out) {
  out.clear();
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  uint8_t buf[65536];
  size_t k;
  while ((k = std::fread(buf, 1, sizeof buf, f)) > 0) out.insert(out.end(), buf, buf + k);
  std::fclose(f);
  return true;
}

bool exists(const std::string& p) {
  struct stat st {};
  return ::lstat(p.c_str(), &st) == 0;
}

size_t count_entries(const std::string& dir) {  // đệ quy, không tính dir
  DIR* d = ::opendir(dir.c_str());
  if (!d) return 0;
  size_t n = 0;
  while (dirent* e = ::readdir(d)) {
    if (!std::strcmp(e->d_name, ".") || !std::strcmp(e->d_name, "..")) continue;
    ++n;
    const std::string sub = dir + "/" + e->d_name;
    struct stat st {};
    if (::lstat(sub.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) n += count_entries(sub);
  }
  ::closedir(d);
  return n;
}

void rm_rf(const std::string& p) {
  struct stat st {};
  if (::lstat(p.c_str(), &st) != 0) return;
  if (S_ISDIR(st.st_mode)) {
    DIR* d = ::opendir(p.c_str());
    if (d) {
      while (dirent* e = ::readdir(d))
        if (std::strcmp(e->d_name, ".") && std::strcmp(e->d_name, "..")) rm_rf(p + "/" + e->d_name);
      ::closedir(d);
    }
    ::rmdir(p.c_str());
  } else {
    ::unlink(p.c_str());
  }
}

// Nguồn trả từng byte một — ép BitReader qua mọi ranh giới bộ đệm.
class DribbleSource : public ByteSource {
 public:
  explicit DribbleSource(const Bytes& v) : v_(v) {}
  size_t read(uint8_t* buf, size_t n) override {
    if (off_ >= v_.size() || n == 0) return 0;
    buf[0] = v_[off_++];
    return 1;
  }

 private:
  const Bytes& v_;
  size_t off_ = 0;
};

// ---------------------------------------------------------- dựng tar --------
struct TarBuilder {
  Bytes out;
  void header(const std::string& name, char type, uint64_t size, uint32_t mode = 0644,
              const std::string& link = "") {
    uint8_t h[512] = {0};
    std::memcpy(h, name.data(), std::min<size_t>(name.size(), 100));
    std::snprintf(reinterpret_cast<char*>(h + 100), 8, "%07o", mode);
    std::snprintf(reinterpret_cast<char*>(h + 108), 8, "%07o", 0);
    std::snprintf(reinterpret_cast<char*>(h + 116), 8, "%07o", 0);
    std::snprintf(reinterpret_cast<char*>(h + 124), 12, "%011llo", static_cast<unsigned long long>(size));
    std::snprintf(reinterpret_cast<char*>(h + 136), 12, "%011o", 0);
    h[156] = uint8_t(type);
    std::memcpy(h + 157, link.data(), std::min<size_t>(link.size(), 100));
    std::memcpy(h + 257, "ustar\0" "00", 8);
    std::memset(h + 148, ' ', 8);
    unsigned sum = 0;
    for (uint8_t b : h) sum += b;
    std::snprintf(reinterpret_cast<char*>(h + 148), 8, "%06o", sum);
    h[155] = ' ';
    out.insert(out.end(), h, h + 512);
  }
  void body(const Bytes& data) {
    out.insert(out.end(), data.begin(), data.end());
    out.insert(out.end(), (512 - data.size() % 512) % 512, 0);
  }
  void file(const std::string& name, const Bytes& data, uint32_t mode = 0644) {
    header(name, '0', data.size(), mode);
    body(data);
  }
  void dir(const std::string& name) { header(name, '5', 0, 0755); }
  void gnu_long(const std::string& name, const Bytes& data) {
    Bytes n = B(name);
    n.push_back(0);
    header("././@LongLink", 'L', n.size());
    body(n);
    header(name.substr(0, 99), '0', data.size());
    body(data);
  }
  void pax(const std::string& key, const std::string& value) {
    std::string rec = " " + key + "=" + value + "\n";
    size_t len = rec.size() + 1;
    while (std::to_string(len).size() + rec.size() != len) ++len;
    const std::string r = std::to_string(len) + rec;
    header("PaxHeader", 'x', r.size());
    body(B(r));
  }
  Bytes finish() {
    Bytes r = out;
    r.insert(r.end(), 1024, 0);
    return r;
  }
};

// ---------------------------------------------------------- dựng zip --------
struct ZipItem {
  ZipItem(std::string n, Bytes d, uint16_t m = 8, uint32_t md = 0100644)
      : name(std::move(n)), data(std::move(d)), method(m), mode(md) {}
  std::string name;
  Bytes data;
  uint16_t method = 8;
  uint32_t mode = 0100644;
  uint16_t flags = 0x800;  // tên UTF-8
  std::string local_name;  // rỗng = giống name
  int64_t usize_override = -1;
  int64_t crc_override = -1;
};

void put16(Bytes& b, uint32_t v) {
  b.push_back(uint8_t(v));
  b.push_back(uint8_t(v >> 8));
}
void put32(Bytes& b, uint32_t v) {
  for (int i = 0; i < 4; ++i) b.push_back(uint8_t(v >> (8 * i)));
}

// share_first: mọi mục trong central directory trỏ về local header đầu tiên
// (kiểu "better zip bomb" — các mục dùng chung một vùng dữ liệu).
Bytes build_zip(const std::vector<ZipItem>& items, bool share_first = false, uint16_t eocd_count = 0) {
  Bytes out, cd;
  std::vector<uint32_t> offs;
  std::vector<Bytes> comp;
  for (const auto& it : items) {
    const Bytes c = it.method == 8 ? deflate_fixed(it.data) : it.data;
    comp.push_back(c);
    const uint32_t crc = it.crc_override >= 0 ? uint32_t(it.crc_override) : crc32(it.data.data(), it.data.size());
    const uint32_t us = it.usize_override >= 0 ? uint32_t(it.usize_override) : uint32_t(it.data.size());
    const std::string& ln = it.local_name.empty() ? it.name : it.local_name;
    offs.push_back(uint32_t(out.size()));
    if (!(share_first && offs.size() > 1)) {
      put32(out, 0x04034b50);
      put16(out, 20);
      put16(out, it.flags);
      put16(out, it.method);
      put16(out, 0);
      put16(out, 0x5021);
      put32(out, crc);
      put32(out, uint32_t(c.size()));
      put32(out, us);
      put16(out, uint32_t(ln.size()));
      put16(out, 0);
      out.insert(out.end(), ln.begin(), ln.end());
      out.insert(out.end(), c.begin(), c.end());
    } else {
      offs.back() = offs.front();
    }
  }
  const uint32_t cd_off = uint32_t(out.size());
  for (size_t i = 0; i < items.size(); ++i) {
    const auto& it = items[i];
    const uint32_t crc = it.crc_override >= 0 ? uint32_t(it.crc_override) : crc32(it.data.data(), it.data.size());
    const uint32_t us = it.usize_override >= 0 ? uint32_t(it.usize_override) : uint32_t(it.data.size());
    put32(cd, 0x02014b50);
    put16(cd, (3 << 8) | 20);
    put16(cd, 20);
    put16(cd, it.flags);
    put16(cd, it.method);
    put16(cd, 0);
    put16(cd, 0x5021);
    put32(cd, crc);
    put32(cd, uint32_t(comp[i].size()));
    put32(cd, us);
    put16(cd, uint32_t(it.name.size()));
    put16(cd, 0);
    put16(cd, 0);
    put16(cd, 0);
    put16(cd, 0);
    put32(cd, it.mode << 16);
    put32(cd, offs[i]);
    cd.insert(cd.end(), it.name.begin(), it.name.end());
  }
  out.insert(out.end(), cd.begin(), cd.end());
  const uint16_t n = eocd_count ? eocd_count : uint16_t(items.size());
  put32(out, 0x06054b50);
  put16(out, 0);
  put16(out, 0);
  put16(out, n);
  put16(out, n);
  put32(out, uint32_t(cd.size()));
  put32(out, cd_off);
  put16(out, 0);
  return out;
}

// Ghi bit LSB trước (để dựng luồng DEFLATE hỏng có chủ đích).
struct Bits {
  Bytes out;
  uint32_t acc = 0;
  int n = 0;
  void put(uint32_t v, int k) {  // giá trị, LSB trước
    for (int i = 0; i < k; ++i) bit((v >> i) & 1);
  }
  void code(uint32_t c, int k) {  // mã Huffman, MSB trước
    for (int i = k - 1; i >= 0; --i) bit((c >> i) & 1);
  }
  void bit(uint32_t b) {
    acc |= b << n;
    if (++n == 8) {
      out.push_back(uint8_t(acc));
      acc = 0;
      n = 0;
    }
  }
  Bytes done() {
    if (n) out.push_back(uint8_t(acc));
    acc = 0;
    n = 0;
    return out;
  }
};

struct Env {
  std::string root;
  explicit Env(const std::string& tag) : root(mkdtemp_dir(tag)) {}
  ~Env() { rm_rf(root); }
  std::string put(const std::string& name, const Bytes& b) const {
    const std::string p = root + "/" + name;
    write_bytes(p, b);
    return p;
  }
  std::string dest(const std::string& name = "ra") const { return root + "/" + name; }
};

}  // namespace

// ================================================================ CRC + DEFLATE
YUI_TEST(archive, crc32_known_values) {
  const std::string s = "123456789";
  CHECK_EQ(crc32(reinterpret_cast<const uint8_t*>(s.data()), s.size()), 0xCBF43926u);
  CHECK_EQ(crc32(nullptr, 0), 0u);
  const Bytes t = B(fixture_text());
  uint32_t inc = 0;
  for (size_t i = 0; i < t.size(); i += 777) inc = crc32_update(inc, t.data() + i, std::min<size_t>(777, t.size() - i));
  CHECK_EQ(inc, crc32(t.data(), t.size()));
}

YUI_TEST(archive, inflate_roundtrip_stored_and_fixed) {
  std::vector<Bytes> inputs = {Bytes{}, Bytes{'A'}, B(fixture_text()), Bytes(100000, 0)};
  Bytes rnd(140000);  // > 2 khối stored (65535)
  uint32_t x = 12345;
  for (auto& b : rnd) {
    x = x * 1103515245u + 12345u;
    b = uint8_t(x >> 16);
  }
  inputs.push_back(rnd);
  bool all_ok = true, all_eq = true;
  for (const auto& in : inputs) {
    for (int kind = 0; kind < 2; ++kind) {
      const Bytes z = kind ? deflate_fixed(in) : deflate_stored(in);
      Bytes out;
      const auto r = inflate(z, out, in.size() + 1);
      all_ok = all_ok && r.ok;
      all_eq = all_eq && out == in && r.out_bytes == in.size() && r.in_bytes == z.size() &&
               r.crc == crc32(in.data(), in.size());
    }
  }
  CHECK(all_ok);
  CHECK(all_eq);
  // Lặp byte: 100000 số 0 nén cố định phải nhỏ hơn nhiều (đường độ dài/khoảng cách).
  CHECK(deflate_fixed(Bytes(100000, 0)).size() < 1000);
}

YUI_TEST(archive, inflate_real_zlib_dynamic_huffman) {
  const Bytes& z = fx::fx_raw_dynamic();
  CHECK_EQ(int((z[0] >> 1) & 3), 2);  // khối Huffman động thật từ zlib
  Bytes out;
  const auto r = inflate(z, out, 1 << 20);
  CHECK_MSG(r.ok, r.error);
  CHECK(S(out) == fixture_text());
  CHECK_EQ(r.in_bytes, uint64_t(z.size()));
  // Đọc từng byte một: kết quả phải y hệt.
  DribbleSource src(z);
  VectorSink sink;
  const auto r2 = inflate(src, sink, 1 << 20);
  CHECK(r2.ok);
  CHECK(sink.data == out);
}

YUI_TEST(archive, inflate_truncation_and_garbage_never_crash) {
  const Bytes& z = fx::fx_raw_dynamic();
  size_t ok_prefix = 0;
  for (size_t n = 0; n < z.size(); ++n) {
    Bytes cut(z.begin(), z.begin() + long(n)), out;
    if (inflate(cut, out, 1 << 20).ok) ++ok_prefix;
  }
  CHECK_EQ(ok_prefix, size_t(0));  // mọi tiền tố bị cắt đều bị từ chối
  // Lật bit ngẫu nhiên: không được treo/vỡ; nếu "thành công" thì phải bị trần chặn.
  uint32_t x = 99;
  size_t ran = 0;
  for (int i = 0; i < 300; ++i) {
    Bytes m = z, out;
    x = x * 1664525u + 1013904223u;
    m[(x >> 8) % m.size()] ^= uint8_t(1u << (x % 8));
    const auto r = inflate(m, out, 1 << 20);
    if (r.ok) CHECK(out.size() <= (1u << 20));
    ++ran;
  }
  CHECK_EQ(ran, size_t(300));
}

YUI_TEST(archive, inflate_rejects_malformed_blocks) {
  Bytes out;
  CHECK(!inflate(Bytes{0x07}, out, 1000).ok);  // BTYPE = 3 (dành riêng)
  CHECK(!inflate(Bytes{0x01, 0x05, 0x00, 0x00, 0x00}, out, 1000).ok);  // LEN/NLEN không khớp
  Bits b;  // khối cố định: tham chiếu khoảng cách 1 khi chưa có byte nào ra
  b.put(1, 1);
  b.put(1, 2);
  b.code(0x01, 7);  // ký hiệu 257 (độ dài 3)
  b.code(0, 5);     // mã khoảng cách 0 (= 1)
  b.code(0, 7);     // 256 kết thúc
  const auto r = inflate(b.done(), out, 1000);
  CHECK(!r.ok);
  CHECK(!r.error.empty());
  Bits c;  // mã khoảng cách 30 (không hợp lệ)
  c.put(1, 1);
  c.put(1, 2);
  c.code(0x30 + 'a', 8);
  c.code(0x01, 7);
  c.code(30, 5);
  c.code(0, 7);
  CHECK(!inflate(c.done(), out, 1000).ok);
  // Vượt trần dữ liệu ra (bom): 8 MiB số 0 với trần 4096.
  const Bytes bomb = deflate_fixed(Bytes(8u << 20, 0));
  const auto rb = inflate(bomb, out, 4096);
  CHECK(!rb.ok);
  CHECK(out.size() <= 4096 + 65536);
}

YUI_TEST(archive, gunzip_real_and_multi_member) {
  Bytes out;
  auto r = gunzip(fx::fx_gz_named(), out, 1 << 20);
  CHECK_MSG(r.ok, r.error);
  CHECK(S(out) == fixture_text());
  r = gunzip(fx::fx_gz_multi(), out, 1 << 20);
  CHECK(r.ok);
  CHECK(S(out) == "phan mot\nphan hai\n");
  const Bytes t = B("xin chào");
  r = gunzip(gzip_wrap(deflate_fixed(t), t, "a.txt"), out, 1000);
  CHECK(r.ok);
  CHECK(out == t);
}

YUI_TEST(archive, gunzip_rejects_bad_crc_size_trailing_truncation) {
  Bytes out;
  Bytes g = fx::fx_gz_named();
  g[g.size() - 8] ^= 1;  // CRC
  CHECK(!gunzip(g, out, 1 << 20).ok);
  g = fx::fx_gz_named();
  g[g.size() - 1] ^= 1;  // ISIZE
  CHECK(!gunzip(g, out, 1 << 20).ok);
  g = fx::fx_gz_named();
  g.push_back('x');
  g.push_back('y');  // rác sau cùng
  CHECK(!gunzip(g, out, 1 << 20).ok);
  g = fx::fx_gz_named();
  g.resize(g.size() - 3);
  CHECK(!gunzip(g, out, 1 << 20).ok);
  g = fx::fx_gz_named();
  g[2] = 7;  // phương pháp ≠ deflate
  CHECK(!gunzip(g, out, 1 << 20).ok);
  CHECK(!gunzip(Bytes{}, out, 100).ok);
}

YUI_TEST(archive, detect_by_signature_only) {
  auto d = [](Bytes h) {
    h.resize(std::max<size_t>(h.size(), 8), 0);
    return detect(h.data(), h.size());
  };
  CHECK(d({'P', 'K', 3, 4}) == Format::Zip);
  CHECK(d({0x1f, 0x8b, 8}) == Format::Gzip);
  CHECK(d({0x28, 0xb5, 0x2f, 0xfd}) == Format::Zstd);
  CHECK(d({'7', 'z', 0xBC, 0xAF, 0x27, 0x1C}) == Format::SevenZip);
  CHECK(d({'R', 'a', 'r', '!', 0x1a, 0x07, 0}) == Format::Rar);
  CHECK(d({0xfd, '7', 'z', 'X', 'Z', 0}) == Format::Xz);
  CHECK(d({'B', 'Z', 'h', '9'}) == Format::Bzip2);
  CHECK(d(B("hello world")) == Format::Unknown);
  TarBuilder tb;
  tb.file("a.txt", B("a"));
  const Bytes t = tb.finish();
  CHECK(detect(t.data(), t.size()) == Format::Tar);
  Bytes bad = t;
  bad[0] ^= 1;  // checksum sai ⇒ không phải tar
  CHECK(detect(bad.data(), bad.size()) == Format::Unknown);
  CHECK(std::string(format_name(Format::Zstd)).size() > 0);
}

YUI_TEST(archive, safe_entry_path_rules) {
  const Limits L;
  CHECK(safe_entry_path("a.txt", L));
  CHECK(safe_entry_path("thu-muc/con/tệp việt.txt", L));
  CHECK(safe_entry_path("..a/b..", L));  // ".." chỉ cấm khi là cả thành phần
  const char* bad[] = {"", "/etc/passwd", "../x", "a/../../x", "a/..", ".", "./a", "a//b",
                       "a\\..\\x", "C:x", "c:/win", "a\x01" "b", "a\x7f", "\xff\xfe", "\xc0\xaf",
                       "\xed\xa0\x80", "a\xe1\x80"};
  size_t rejected = 0;
  for (const char* p : bad) rejected += safe_entry_path(p, L) ? 0 : 1;
  CHECK_EQ(rejected, sizeof bad / sizeof bad[0]);
  std::string deep;
  for (int i = 0; i < 70; ++i) deep += "d/";
  deep += "f";
  CHECK(!safe_entry_path(deep, L));
  CHECK(!safe_entry_path(std::string(300, 'a'), L));
  std::string why;
  CHECK(!safe_entry_path("../x", L, &why));
  CHECK(why.find("..") != std::string::npos);
}

// ================================================================ giải nén
YUI_TEST(archive, extract_real_zip) {
  Env e("arch-zip");
  const std::string z = e.put("goi.zip", fx::fx_zip());
  const auto r = extract_file(z, e.dest(), Limits{}, "thử nghiệm");
  CHECK_MSG(r.ok, r.reason_vi);
  CHECK(r.format == "zip");
  CHECK_EQ(r.files, size_t(3));
  Bytes b;
  CHECK(read_bytes(e.dest() + "/thu-muc/a.txt", b) && S(b) == fixture_text());
  CHECK(read_bytes(e.dest() + "/b.bin", b) && b.size() == 256 && b[255] == 255);
  CHECK(read_bytes(e.dest() + "/tiếng việt.txt", b) && S(b) == "Xin chào Việt Nam\n");
  struct stat st {};
  CHECK(::stat((e.dest() + "/thu-muc/a.txt").c_str(), &st) == 0);
  CHECK((st.st_mode & 07777) == 0755);  // setuid bị bỏ, vẫn giữ quyền chạy
  CHECK(!(st.st_mode & S_ISUID));
  CHECK(::stat((e.dest() + "/b.bin").c_str(), &st) == 0 && (st.st_mode & 0777) == 0644);
  CHECK(r.labeled <= r.files);
  if (r.labeled > 0) {  // tmpfs có thể không hỗ trợ user.* xattr
    char v[128] = {0};
    const ssize_t n = ::getxattr((e.dest() + "/b.bin").c_str(), "user.yui.nguon", v, sizeof v - 1);
    CHECK(n > 0 && std::string(v, size_t(n)) == "thử nghiệm");
  }
  CHECK(r.reason_vi.find("Không tệp nào được tự chạy") != std::string::npos);
}

YUI_TEST(archive, extract_real_tar_gz_with_pax) {
  Env e("arch-tgz");
  const std::string z = e.put("goi.tgz", fx::fx_tgz());
  const auto r = extract_file(z, e.dest());
  CHECK_MSG(r.ok, r.reason_vi);
  CHECK(r.format == "tar.gz");
  CHECK_EQ(r.files, size_t(3));
  Bytes b;
  CHECK(read_bytes(e.dest() + "/goi/a.txt", b) && S(b) == fixture_text());
  std::string longname = "goi/";
  for (int i = 0; i < 12; ++i) longname += "ten-rat-dai-";
  longname += ".txt";
  CHECK(read_bytes(e.dest() + "/" + longname, b) && S(b) == "pax\n");
  CHECK(read_bytes(e.dest() + "/goi/rong.txt", b) && b.empty());
  struct stat st {};
  CHECK(::stat((e.dest() + "/goi/a.txt").c_str(), &st) == 0 && (st.st_mode & 07777) == 0755);
}

YUI_TEST(archive, extract_plain_tar_gnu_longname_and_single_gz) {
  Env e("arch-tar");
  TarBuilder tb;
  tb.dir("d/");
  tb.file("d/x.txt", B("x"), 0640);
  const std::string ln = "d/" + std::string(150, 'n') + ".txt";
  tb.gnu_long(ln, B("dai"));
  const auto r = extract_file(e.put("g.tar", tb.finish()), e.dest());
  CHECK_MSG(r.ok, r.reason_vi);
  CHECK(r.format == "tar");
  Bytes b;
  CHECK(read_bytes(e.dest() + "/" + ln, b) && S(b) == "dai");
  struct stat st {};
  CHECK(::stat((e.dest() + "/d/x.txt").c_str(), &st) == 0 && (st.st_mode & 0777) == 0644);
  // .gz đơn: tên lấy từ tên tệp nén bỏ ".gz".
  const auto r2 = extract_file(e.put("ghichu.txt.gz", fx::fx_gz_named()), e.dest("ra2"));
  CHECK_MSG(r2.ok, r2.reason_vi);
  CHECK(r2.format == "gz");
  CHECK(read_bytes(e.dest("ra2") + "/ghichu.txt", b) && S(b) == fixture_text());
}

YUI_TEST(archive, zip_slip_rejected_everything_rolled_back) {
  Env e("arch-slip");
  const char* evil[] = {"../thoat.txt", "/tmp/yui-tuyet-doi.txt", "a/../../thoat.txt", "a\\..\\..\\thoat.txt",
                        "C:thoat.txt", "a\x01" "b", "\xff\xfe.txt", "a//b"};
  size_t tar_ok = 0, zip_ok = 0;
  for (const char* p : evil) {
    TarBuilder tb;
    tb.file("tot.txt", B("tốt"));  // mục tốt đứng TRƯỚC ⇒ phải được xoá khi từ chối
    tb.file(p, B("xấu"));
    const auto rt = extract_file(e.put("s.tar", tb.finish()), e.dest("t"));
    if (!rt.ok && rt.code == "unsafe_path" && !exists(e.dest("t"))) ++tar_ok;
    ZipItem good{"tot.txt", B("tốt")}, bad{p, B("xấu")};
    const auto rz = extract_file(e.put("s.zip", build_zip({good, bad})), e.dest("z"));
    if (!rz.ok && rz.code == "unsafe_path" && !exists(e.dest("z"))) ++zip_ok;
  }
  CHECK_EQ(tar_ok, sizeof evil / sizeof evil[0]);
  CHECK_EQ(zip_ok, sizeof evil / sizeof evil[0]);
  CHECK(!exists(e.root + "/thoat.txt"));
  CHECK(!exists("/tmp/yui-tuyet-doi.txt"));
  // Thư mục đích có sẵn (rỗng): bị từ chối thì vẫn còn, nhưng rỗng.
  ::mkdir(e.dest("san").c_str(), 0755);
  TarBuilder tb;
  tb.dir("con/");
  tb.file("con/tot.txt", B("1"));
  tb.file("../x", B("2"));
  const auto r = extract_file(e.put("s2.tar", tb.finish()), e.dest("san"));
  CHECK(!r.ok);
  CHECK(exists(e.dest("san")));
  CHECK_EQ(count_entries(e.dest("san")), size_t(0));
  CHECK(r.reason_vi.find("không để lại tệp nào") != std::string::npos);
  // Tên local header khác central directory (đánh lừa trình xem trước).
  ZipItem lie{"vo-hai.txt", B("x")};
  lie.local_name = "../xx.txt";
  const auto rl = extract_file(e.put("l.zip", build_zip({lie})), e.dest("l"));
  CHECK(!rl.ok && rl.code == "unsafe_path");
}

YUI_TEST(archive, links_and_devices_rejected) {
  Env e("arch-link");
  struct Case {
    char type;
    const char* code;
  } cases[] = {{'2', "link"}, {'1', "link"}, {'3', "unsupported"}, {'4', "unsupported"}, {'6', "unsupported"}};
  size_t good = 0;
  for (const auto& c : cases) {
    TarBuilder tb;
    tb.file("a.txt", B("a"));
    tb.header("lk", c.type, 0, 0777, "/etc/passwd");
    const auto r = extract_file(e.put("k.tar", tb.finish()), e.dest());
    if (!r.ok && r.code == c.code && !exists(e.dest())) ++good;
  }
  CHECK_EQ(good, sizeof cases / sizeof cases[0]);
  TarBuilder tp;
  tp.pax("linkpath", "/etc/shadow");
  tp.header("lk", '0', 0);
  auto r = extract_file(e.put("p.tar", tp.finish()), e.dest());
  CHECK(!r.ok && r.code == "link");
  ZipItem sl{"lk", B("/etc/passwd"), 0, 0120777};
  r = extract_file(e.put("k.zip", build_zip({sl})), e.dest());
  CHECK(!r.ok && r.code == "link");
  ZipItem fifo{"f", Bytes{}, 0, 0010644};
  r = extract_file(e.put("f.zip", build_zip({fifo})), e.dest());
  CHECK(!r.ok && r.code == "unsupported");
  // Tệp rồi thư mục con cùng tên: không được đi xuyên qua tệp.
  TarBuilder tf;
  tf.file("a", B("tệp"));
  tf.file("a/b", B("x"));
  r = extract_file(e.put("ab.tar", tf.finish()), e.dest());
  CHECK(!r.ok && !exists(e.dest()));
}

YUI_TEST(archive, no_overwrite_and_nonempty_destination) {
  Env e("arch-exist");
  ::mkdir(e.dest().c_str(), 0755);
  write_bytes(e.dest() + "/cu.txt", B("dữ liệu cũ"));
  TarBuilder tb;
  tb.file("cu.txt", B("mới"));
  auto r = extract_file(e.put("a.tar", tb.finish()), e.dest());
  CHECK(!r.ok && r.code == "exists");
  Bytes b;
  CHECK(read_bytes(e.dest() + "/cu.txt", b) && S(b) == "dữ liệu cũ");
  TarBuilder dup;
  dup.file("x.txt", B("1"));
  dup.file("x.txt", B("2"));
  r = extract_file(e.put("d.tar", dup.finish()), e.dest("moi"));
  CHECK(!r.ok && r.code == "exists" && !exists(e.dest("moi")));
}

YUI_TEST(archive, bombs_ratio_size_count_overlap) {
  Env e("arch-bomb");
  // Tỉ lệ: tar.gz chứa 4 MiB số 0.
  TarBuilder tb;
  tb.file("khong.bin", Bytes(4u << 20, 0));
  const Bytes tar = tb.finish();
  const std::string tgz = e.put("bom.tar.gz", gzip_wrap(deflate_fixed(tar), tar));
  Limits strict;
  strict.ratio_floor_bytes = 64 << 10;
  strict.max_ratio = 20;
  auto r = extract_file(tgz, e.dest(), strict);
  CHECK(!r.ok && r.code == "bomb" && !exists(e.dest()));
  r = extract_file(tgz, e.dest());  // trần mặc định: hợp lệ
  CHECK_MSG(r.ok, r.reason_vi);
  CHECK_EQ(r.bytes, uint64_t(4u << 20));
  rm_rf(e.dest());
  // Trần mỗi tệp / tổng.
  Limits small;
  small.max_entry_bytes = 1000;
  r = extract_file(tgz, e.dest(), small);
  CHECK(!r.ok && r.code == "bomb" && !exists(e.dest()));
  Limits tot;
  tot.max_total_bytes = 100000;
  r = extract_file(tgz, e.dest(), tot);
  CHECK(!r.ok && r.code == "bomb" && !exists(e.dest()));
  // Số mục.
  TarBuilder many;
  for (int i = 0; i < 30; ++i) many.file("f" + std::to_string(i), B("x"));
  Limits few;
  few.max_entries = 10;
  r = extract_file(e.put("many.tar", many.finish()), e.dest(), few);
  CHECK(!r.ok && r.code == "bomb" && !exists(e.dest()));
  std::vector<ZipItem> zi;
  for (int i = 0; i < 30; ++i) zi.emplace_back("f" + std::to_string(i), B("x"));
  r = extract_file(e.put("many.zip", build_zip(zi)), e.dest(), few);
  CHECK(!r.ok && r.code == "bomb");
  // Zip chồng vùng dữ liệu ("better zip bomb"): các mục trỏ cùng một local header.
  ZipItem a{"a", Bytes(1000, 'a')}, a2{"a", Bytes(1000, 'a')};
  r = extract_file(e.put("ov.zip", build_zip({a, a2, a2}, true)), e.dest());
  CHECK(!r.ok && r.code == "bomb" && !exists(e.dest()));
  // Zip khai báo nhỏ hơn thật.
  ZipItem lie{"l", Bytes(50000, 'z')};
  lie.usize_override = 100;
  r = extract_file(e.put("lie.zip", build_zip({lie})), e.dest());
  CHECK(!r.ok && (r.code == "bomb" || r.code == "corrupt") && !exists(e.dest()));
  // Không giải lồng nhau: zip trong zip chỉ ra một tệp .zip.
  ZipItem inner{"trong.zip", build_zip({ZipItem{"x.txt", B("x")}}), 0};
  r = extract_file(e.put("ngoai.zip", build_zip({inner})), e.dest());
  CHECK(r.ok && r.files == 1);
  CHECK(exists(e.dest() + "/trong.zip") && !exists(e.dest() + "/x.txt"));
}

YUI_TEST(archive, corrupt_and_unsupported) {
  Env e("arch-bad");
  ZipItem c{"c.txt", B("dữ liệu")};
  c.crc_override = 0x12345678;
  auto r = extract_file(e.put("crc.zip", build_zip({ZipItem{"ok.txt", B("1")}, c})), e.dest());
  CHECK(!r.ok && r.code == "corrupt" && !exists(e.dest()));
  ZipItem enc{"m.txt", B("x")};
  enc.flags = 0x801;
  r = extract_file(e.put("enc.zip", build_zip({enc})), e.dest());
  CHECK(!r.ok && r.code == "unsupported");
  ZipItem bz{"m.txt", B("x"), 0};
  bz.method = 12;
  r = extract_file(e.put("bz.zip", build_zip({bz})), e.dest());
  CHECK(!r.ok && r.code == "unsupported");
  r = extract_file(e.put("z64.zip", build_zip({ZipItem{"a", B("a")}}, false, 0xFFFF)), e.dest());
  CHECK(!r.ok && r.code == "unsupported");
  // zstd/7z/RAR/không rõ — kể cả khi đặt đuôi .zip.
  const Bytes sigs[] = {{0x28, 0xb5, 0x2f, 0xfd, 0, 0, 0, 0}, {'7', 'z', 0xBC, 0xAF, 0x27, 0x1C, 0, 4},
                        {'R', 'a', 'r', '!', 0x1a, 0x07, 1, 0}, B("chỉ là văn bản thường, không nén")};
  size_t uns = 0;
  for (const auto& s : sigs) {
    r = extract_file(e.put("gia.zip", s), e.dest());
    if (!r.ok && r.code == "unsupported" && !exists(e.dest())) ++uns;
  }
  CHECK_EQ(uns, size_t(4));
  r = extract_file(e.put("rar.rar", sigs[2]), e.dest());
  CHECK(r.reason_vi.find("RAR") != std::string::npos);
  // tar cắt cụt (thiếu khối kết thúc), gzip hỏng.
  TarBuilder tb;
  tb.file("a.txt", Bytes(3000, 'a'));
  Bytes cut = tb.out;
  cut.resize(cut.size() - 700);
  r = extract_file(e.put("cut.tar", cut), e.dest());
  CHECK(!r.ok && r.code == "corrupt" && !exists(e.dest()));
  Bytes g = fx::fx_tgz();
  g[g.size() / 2] ^= 0x55;
  r = extract_file(e.put("hong.tgz", g), e.dest());
  CHECK(!r.ok && !exists(e.dest()));
  r = extract_file(e.root + "/khong-co.zip", e.dest());
  CHECK(!r.ok && r.code == "io");
  r = extract_file(e.root, e.dest());  // thư mục, không phải tệp
  CHECK(!r.ok && r.code == "io");
}

YUI_TEST_MAIN("test_archive")
