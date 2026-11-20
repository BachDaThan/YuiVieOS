// Giải nén an toàn đa định dạng (Tầng 5, P4.33).
//
// Một bộ giải DEFLATE (RFC 1951) tự viết, dùng chung cho Kho Ứng Dụng (P4.15,
// `.tar.gz`/`.zip`), lớp ảnh OCI (P3.28) và trình quản lý tệp. Đọc và ghi
// THEO LUỒNG: bộ nhớ dùng cố định (cửa sổ 64 KiB + bộ đệm vào/ra), không phụ
// thuộc kích thước tệp nén.
//
// Định dạng: `.zip` (stored + deflate), `.gz` (RFC 1952, nhiều member),
// `.tar` (ustar/pax/GNU), `.tar.gz`/`.tgz`. Nhận diện bằng chữ ký đầu tệp,
// KHÔNG theo đuôi tên. zstd (RFC 8878), 7z/LZMA, xz, bzip2: nhận ra và báo
// "chưa hỗ trợ" rõ ràng. RAR là định dạng đóng: không làm (⚖️ P7.18).
//
// An toàn (điều nguy hiểm nhất khi giải nén):
//   * zip-slip: đường dẫn tuyệt đối, `..`, `\`, ký tự điều khiển, `\0`, tên
//     không phải UTF-8 hợp lệ ⇒ từ chối CẢ tệp nén;
//   * liên kết mềm/cứng, thiết bị, fifo trong gói ⇒ từ chối; thư mục đích
//     được mở bằng `openat` + `O_NOFOLLOW` từng tầng, tệp tạo bằng `O_EXCL`
//     (không ghi đè, không đi theo liên kết có sẵn);
//   * bom nén: trần tổng dung lượng, trần mỗi tệp, trần số mục, trần tỉ lệ
//     nén (khi đã vượt ngưỡng sàn), không giải nén lồng nhau (42.zip chỉ ra
//     các tệp .zip con, không bung tiếp), zip có vùng dữ liệu chồng nhau
//     ("better zip bomb") ⇒ từ chối;
//   * kiểm CRC-32 và kích thước khai báo; lỗi giữa chừng ⇒ xoá mọi thứ đã tạo;
//   * không tự chạy gì; bit setuid/setgid/sticky bị bỏ; mỗi tệp được gắn nhãn
//     nguồn `user.yui.nguon` (xattr, nếu hệ tệp hỗ trợ — như P3.30).
#ifndef YUI_ARCHIVE_HPP
#define YUI_ARCHIVE_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace yui::archive {

// ------------------------------------------------------------- luồng byte --
class ByteSource {
 public:
  virtual ~ByteSource() = default;
  // Đọc tối đa n byte; 0 = hết dữ liệu. < 0 không dùng (lỗi I/O ⇒ trả 0 + error()).
  virtual size_t read(uint8_t* buf, size_t n) = 0;
  virtual bool failed() const { return false; }
};

class ByteSink {
 public:
  virtual ~ByteSink() = default;
  // false ⇒ dừng giải nén (sink tự giữ lý do).
  virtual bool write(const uint8_t* data, size_t n) = 0;
};

class MemorySource : public ByteSource {
 public:
  MemorySource(const uint8_t* p, size_t n) : p_(p), n_(n) {}
  explicit MemorySource(const std::vector<uint8_t>& v) : p_(v.data()), n_(v.size()) {}
  size_t read(uint8_t* buf, size_t n) override;

 private:
  const uint8_t* p_;
  size_t n_;
  size_t off_ = 0;
};

class VectorSink : public ByteSink {
 public:
  std::vector<uint8_t> data;
  bool write(const uint8_t* p, size_t n) override {
    data.insert(data.end(), p, p + n);
    return true;
  }
};

// ------------------------------------------------------------ CRC-32 (IEEE) --
uint32_t crc32_update(uint32_t crc, const uint8_t* p, size_t n);  // crc khởi đầu 0
inline uint32_t crc32(const uint8_t* p, size_t n) { return crc32_update(0, p, n); }

// ------------------------------------------------------- DEFLATE (RFC 1951) --
struct InflateResult {
  bool ok = false;
  std::string error;      // tiếng Việt
  uint64_t in_bytes = 0;  // số byte nén đã dùng (tới hết khối cuối, làm tròn byte)
  uint64_t out_bytes = 0;
  uint32_t crc = 0;       // CRC-32 của dữ liệu ra
};

// Giải một luồng DEFLATE thô. Vượt `max_out` byte ra ⇒ dừng, báo lỗi (bom nén).
InflateResult inflate(ByteSource& src, ByteSink& sink, uint64_t max_out);
// Tiện dụng: trong bộ nhớ.
InflateResult inflate(const std::vector<uint8_t>& in, std::vector<uint8_t>& out, uint64_t max_out);

// Gzip (RFC 1952): kiểm header, CRC-32 và ISIZE từng member; nhiều member nối
// nhau được ghép (đúng chuẩn); rác sau cùng ⇒ lỗi.
InflateResult gunzip(ByteSource& src, ByteSink& sink, uint64_t max_out);
InflateResult gunzip(const std::vector<uint8_t>& in, std::vector<uint8_t>& out, uint64_t max_out);

// Bộ nén tối giản (để test và để tạo gói thử): khối stored, hoặc Huffman cố
// định + lặp byte (khoảng cách 1, độ dài ≤ 258). Không nhắm tỉ lệ nén tốt.
std::vector<uint8_t> deflate_stored(const std::vector<uint8_t>& in);
std::vector<uint8_t> deflate_fixed(const std::vector<uint8_t>& in);
std::vector<uint8_t> gzip_wrap(const std::vector<uint8_t>& raw_deflate, const std::vector<uint8_t>& original,
                               const std::string& name = "");

// ------------------------------------------------------------- nhận dạng --
enum class Format { Unknown, Zip, Gzip, Tar, Zstd, SevenZip, Rar, Xz, Bzip2 };
const char* format_name(Format f);
// Theo chữ ký đầu tệp (cần ≥ 512 byte để nhận tar).
Format detect(const uint8_t* head, size_t n);

// ------------------------------------------------------------ giải nén an toàn --
struct Limits {
  uint64_t max_total_bytes = 4ull << 30;   // tổng dữ liệu ra
  uint64_t max_entry_bytes = 2ull << 30;   // mỗi tệp
  size_t max_entries = 65536;
  size_t max_path_bytes = 1024;
  size_t max_depth = 64;
  // Tỉ lệ (dữ liệu ra / kích thước tệp nén) chỉ xét khi dữ liệu ra đã vượt sàn.
  uint64_t max_ratio = 250;
  uint64_t ratio_floor_bytes = 32ull << 20;
};

struct ExtractReport {
  bool ok = false;
  std::string format;             // "zip" | "tar" | "tar.gz" | "gz" | …
  std::string reason_vi;          // lý do từ chối/thành công
  std::string code;               // "", "unsafe_path", "link", "bomb", "corrupt", "unsupported", "io", "exists"
  size_t files = 0;
  size_t dirs = 0;
  uint64_t bytes = 0;
  size_t labeled = 0;             // số tệp gắn được nhãn nguồn xattr
  std::vector<std::string> paths; // đường dẫn tương đối đã tạo (khi ok)
};

// Kiểm một đường dẫn trong gói theo đúng quy tắc giải nén (dùng chung).
bool safe_entry_path(const std::string& p, const Limits& lim, std::string* why = nullptr);

// Giải `archive_path` vào `dest_dir`. `dest_dir` phải chưa tồn tại hoặc rỗng
// (sẽ được tạo, quyền 0755). Lỗi ⇒ không còn tệp nào do lần chạy này tạo ra.
// `origin` ghi vào nhãn nguồn (mặc định: tên tệp nén).
ExtractReport extract_file(const std::string& archive_path, const std::string& dest_dir,
                           const Limits& lim = Limits{}, const std::string& origin = "");

}  // namespace yui::archive

#endif  // YUI_ARCHIVE_HPP
