// Chọn chế độ hiển thị đúng gốc + hệ số phóng nguyên (HiDPI) — P2.16.
//
// Gốc rễ cảnh "mờ / to đùng" trên máy thiếu driver: chạy một chế độ KHÔNG phải
// gốc của màn hình, rồi màn hình (hoặc firmware) kéo giãn. Tệp này là phần
// thuần logic, không chạm phần cứng: đọc EDID, chọn chế độ, chọn hệ số phóng
// nguyên `k`, đặt khung chứa nội dung có viền đen thay vì kéo giãn, và tính
// ngân sách bộ nhớ khung hình.
//
// Bản Rust `kernel-cap/src/display.rs` làm ĐÚNG cùng phép tính (số nguyên, cùng
// thứ tự ưu tiên); cả hai cùng chạy bộ vector chung
// `tests/data/display_vectors.txt` (sinh bởi `scripts/gen_display_vectors.py`),
// nên hai bản không thể lệch nhau mà test không đỏ.
//
// Quy tắc (không đổi nếu không đổi cả hai bản + vector):
//   1. EDID hợp lệ và danh sách có đúng chế độ gốc ⇒ chọn nó.
//   2. Không có chế độ gốc ⇒ chế độ LỚN NHẤT cùng tỉ lệ khung hình (sai lệch
//      ≤ 1,5 %, nên 1366×768 tính là 16:9) với màn hình — tỉ lệ lấy từ EDID,
//      không có EDID thì lấy từ chế độ đang chạy. Có EDID thì không chọn chế
//      độ lớn hơn tấm nền (bị thu nhỏ ⇒ nhoè) trừ khi không còn cách nào.
//   3. Không có chế độ cùng tỉ lệ ⇒ GIỮ chế độ đang chạy (không bao giờ tự đổi
//      sang tỉ lệ khác) và báo `distorted` để giao diện hỏi người dùng.
//   4. Hệ số `k` (1–4) theo DPI THẬT (kích thước vật lý trong EDID): chiều cao
//      chữ 5×7 ≈ 2,4 mm; nhưng không lớn tới mức còn < 80 cột hoặc < 25 dòng.
//      Không có kích thước vật lý (máy chiếu, EDID hỏng) ⇒ dùng DPI gợi ý của
//      bên gọi, không có thì đoán theo độ phân giải — và ghi rõ là đoán.
//      Người dùng ghi đè 1–4 thì theo người dùng.
#ifndef YUI_DISPLAY_HPP
#define YUI_DISPLAY_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace yui::display {

struct Mode {
  uint32_t width = 0;
  uint32_t height = 0;
  uint8_t bpp = 32;
  uint16_t refresh_hz = 0;  // 0 = không rõ (GOP/VBE thường không báo)
};

struct Edid {
  bool valid = false;
  std::string error;           // lý do không hợp lệ (tiếng Việt)
  char vendor[4] = {0, 0, 0, 0};
  uint16_t product = 0;
  uint8_t version = 0, revision = 0;
  bool has_native = false;     // có Detailed Timing Descriptor đầu tiên
  uint32_t native_width = 0, native_height = 0;
  uint16_t native_refresh_hz = 0;
  uint16_t width_mm = 0, height_mm = 0;  // 0 = không rõ (máy chiếu, EDID thiếu)
  std::string name;            // descriptor 0xFC (nếu có)
};

/// Đọc khối EDID cơ sở (128 byte; khối mở rộng bị bỏ qua). Kiểm header,
/// checksum, phiên bản 1.x. Kích thước vật lý vô lý (DPI ngoài 40–700) bị bỏ.
Edid parse_edid(const uint8_t* data, size_t n);

/// Cùng tỉ lệ khung hình (sai lệch tương đối ≤ 1,5 %).
bool same_aspect(uint32_t aw, uint32_t ah, uint32_t bw, uint32_t bh);

// Giá trị số cố định (bản Rust và tệp vector dùng chung): đừng xếp lại.
enum class PickReason {
  NoModes = 0,        // không có chế độ 24/32 bit nào
  Native = 1,         // đúng độ phân giải gốc trong EDID
  EdidAspect = 2,     // không có gốc ⇒ lớn nhất cùng tỉ lệ tấm nền (≤ gốc)
  CurrentAspect = 3,  // không có EDID ⇒ lớn nhất cùng tỉ lệ chế độ đang chạy
  KeepCurrent = 4,    // không có chế độ cùng tỉ lệ ⇒ giữ nguyên chế độ đang chạy
  ClosestAspect = 5,  // có EDID, không biết chế độ đang chạy ⇒ tỉ lệ gần nhất
  Largest = 6,        // không biết gì về màn hình ⇒ chế độ lớn nhất
};
const char* reason_code(PickReason r);  // "native", "edid_aspect", …

struct Pick {
  // Vị trí trong danh sách. -1 khi NoModes, hoặc KeepCurrent mà chế độ đang
  // chạy không có trong danh sách (giữ nguyên, không đặt lại chế độ).
  int index = -1;
  Mode mode;
  PickReason reason = PickReason::NoModes;
  bool distorted = false;  // tỉ lệ chế độ ≠ tỉ lệ tấm nền ⇒ sẽ bị méo
  std::string reason_vi;
};

/// `edid` có thể null; `current` có thể null (không biết chế độ đang chạy).
/// Chỉ nhận chế độ 24/32 bit.
Pick pick_mode(const std::vector<Mode>& modes, const Edid* edid, const Mode* current);

struct ScaleInput {
  uint32_t width_px = 0, height_px = 0;
  uint16_t width_mm = 0, height_mm = 0;  // từ EDID; 0 = không rõ
  uint32_t dpi_hint = 0;                 // DPI do bên gọi biết (vd `--dpi`); 0 = không
  uint8_t user_override = 0;             // 1–4; 0 = tự chọn
};

struct Scale {
  uint8_t k = 1;
  uint32_t dpi_x10 = 960;   // DPI × 10 đã dùng
  bool dpi_measured = false;  // true = tính từ kích thước vật lý EDID
  bool dpi_guessed = false;   // true = không có số đo, không có gợi ý ⇒ đoán
  uint8_t k_dpi = 1, k_fit = 1;
  bool cramped = false;     // người dùng ghi đè vượt k_fit (ít hơn 80 cột/25 dòng)
  uint32_t columns = 0, rows = 0;  // ô chữ 6k × 12k (chữ Việt có dấu)
  std::string reason_vi;
};

Scale choose_scale(const ScaleInput& in);

/// Đặt nội dung src_w×src_h vào khung dst_w×dst_h bằng hệ số NGUYÊN lớn nhất
/// (không nội suy), căn giữa, phần còn lại là viền đen. k = 0 ⇒ không vừa.
struct Letterbox {
  uint32_t k = 0, x = 0, y = 0, w = 0, h = 0;
};
Letterbox letterbox(uint32_t src_w, uint32_t src_h, uint32_t dst_w, uint32_t dst_h);

/// Ngân sách khung hình (tính RIÊNG khỏi ngân sách < 64 MB của P2.11).
struct FrameBudget {
  uint64_t frame_bytes = 0;  // một khung 32-bit
  bool double_buffer = false;  // ≤ 16 MiB cho hai khung
  bool dirty_rects = false;    // khung > 8 MiB (vd 4K = 31,6 MiB) ⇒ chỉ vẽ vùng bẩn
};
FrameBudget frame_budget(uint32_t width, uint32_t height);

/// Gộp tất cả: chọn chế độ rồi chọn k cho chế độ đó.
struct Plan {
  Pick pick;
  Scale scale;
  FrameBudget budget;
  bool edid_used = false;
  std::string summary_vi;
};
Plan plan(const std::vector<Mode>& modes, const uint8_t* edid, size_t edid_len, const Mode* current,
          uint8_t user_override = 0, uint32_t dpi_hint = 0);

// ---------------------------------------------------------------------------
// Đường (c): framebuffer do firmware mô tả trong Device Tree (nút
// `simple-framebuffer`, binding Linux display/simple-framebuffer.yaml) — cách
// board ARM/RISC-V (U-Boot…) trao màn hình cho nhân khi chưa có driver GPU.
//
// Bộ đọc FDT tự viết, KHÔNG tin đầu vào: mọi offset/độ dài đều bị chặn; DTB
// hỏng ở bất kỳ đâu ⇒ BadBlob và không dùng nút nào. Bản Rust
// `kernel-cap/src/fdt.rs` (nhân gọi đúng hàm đó) làm cùng quy tắc; cả hai chạy
// bộ vector `tests/data/fdt_fb_vectors.txt` sinh bởi scripts/gen_fdt_vectors.py.
// Quy tắc chi tiết: xem đầu scripts/gen_fdt_vectors.py.
enum class FdtReason {
  Ok = 0,           // có framebuffer dùng được
  NoNode = 1,       // không có nút compatible = "simple-framebuffer"
  Disabled = 2,     // status khác "okay"/"ok"
  BadFormat = 3,    // định dạng điểm ảnh vắng hoặc chưa hỗ trợ (vd r5g6b5)
  BadGeometry = 4,  // width/height/stride vắng, sai cỡ, vô lý
  BadReg = 5,       // reg sai theo #address-cells/#size-cells, địa chỉ 0, quá nhỏ, tràn
  BadBlob = 6,      // DTB hỏng (header, offset, token, chuỗi, độ sâu…)
};
const char* fdt_reason_code(FdtReason r);  // "ok", "no_node", "disabled", …

struct FdtFramebuffer {
  FdtReason reason = FdtReason::NoNode;
  uint32_t candidates = 0;  // số nút simple-framebuffer đã xét (0 nếu BadBlob)
  uint64_t addr = 0, size = 0;
  uint32_t width = 0, height = 0, stride = 0;
  uint8_t bpp = 0;
  uint8_t red_shift = 0, green_shift = 0, blue_shift = 0;
  std::string format;  // "a8r8g8b8", "x8b8g8r8", "r8g8b8", …
};

/// Tìm framebuffer đầu tiên dùng được trong một DTB (FDT v17). `n` là số byte
/// thật có trong bộ đệm; header nói lớn hơn ⇒ BadBlob.
FdtFramebuffer parse_fdt_framebuffer(const uint8_t* data, size_t n);

}  // namespace yui::display

#endif  // YUI_DISPLAY_HPP
