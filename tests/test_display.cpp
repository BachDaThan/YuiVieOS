// P2.16 — chế độ hiển thị gốc, hệ số phóng nguyên (HiDPI), viền đen, ngân
// sách khung hình. Bộ vector `tests/data/display_vectors.txt` (sinh bởi
// scripts/gen_display_vectors.py, một bản tham chiếu Python độc lập) cũng được
// bản Rust trong kernel-cap/src/display.rs chạy — ba bản phải khớp.
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "testutil.hpp"
#include "yui/display.hpp"
#include "yui/fb.hpp"
#include "yui/ui.hpp"

namespace d = yui::display;

namespace {

std::vector<std::string> split(const std::string& s, char sep) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) {
    if (c == sep) {
      out.push_back(cur);
      cur.clear();
    } else {
      cur.push_back(c);
    }
  }
  out.push_back(cur);
  return out;
}

std::vector<uint8_t> unhex(const std::string& h) {
  std::vector<uint8_t> out;
  if (h == "-") return out;
  out.reserve(h.size() / 2);
  for (size_t i = 0; i + 1 < h.size(); i += 2)
    out.push_back(uint8_t(std::strtoul(h.substr(i, 2).c_str(), nullptr, 16)));
  return out;
}

d::Mode parse_mode(const std::string& s) {
  d::Mode m;
  std::string body = s;
  const size_t at = s.find('@');
  if (at != std::string::npos) {
    body = s.substr(0, at);
    m.refresh_hz = uint16_t(std::atoi(s.c_str() + at + 1));
  }
  const auto p = split(body, 'x');
  m.width = uint32_t(std::atoi(p[0].c_str()));
  m.height = uint32_t(std::atoi(p[1].c_str()));
  m.bpp = p.size() > 2 ? uint8_t(std::atoi(p[2].c_str())) : 32;
  return m;
}

std::string wh(uint32_t w, uint32_t h) {
  if (w == 0 && h == 0) return "0x0";
  return std::to_string(w) + "x" + std::to_string(h);
}

// Dựng EDID tối thiểu trong C++ (độc lập với tệp vector) cho các test viết tay.
std::vector<uint8_t> make_edid(uint32_t w, uint32_t h, uint32_t w_mm, uint32_t h_mm) {
  std::vector<uint8_t> e(128, 0);
  const uint8_t hdr[8] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};
  for (int i = 0; i < 8; ++i) e[size_t(i)] = hdr[i];
  e[8] = 0x66;  // "YUI"
  e[9] = 0xA9;
  e[18] = 1;
  e[19] = 4;
  const uint32_t hb = 160, vb = 31;
  const uint32_t clock = ((w + hb) * (h + vb) * 60 + 5000) / 10000;
  uint8_t* t = e.data() + 54;
  t[0] = uint8_t(clock & 0xFF);
  t[1] = uint8_t(clock >> 8);
  t[2] = uint8_t(w & 0xFF);
  t[3] = uint8_t(hb & 0xFF);
  t[4] = uint8_t(((w >> 8) << 4) | (hb >> 8));
  t[5] = uint8_t(h & 0xFF);
  t[6] = uint8_t(vb & 0xFF);
  t[7] = uint8_t(((h >> 8) << 4) | (vb >> 8));
  t[12] = uint8_t(w_mm & 0xFF);
  t[13] = uint8_t(h_mm & 0xFF);
  t[14] = uint8_t(((w_mm >> 8) << 4) | (h_mm >> 8));
  unsigned sum = 0;
  for (int i = 0; i < 127; ++i) sum += e[size_t(i)];
  e[127] = uint8_t((256 - (sum & 0xFF)) & 0xFF);
  return e;
}

struct Canvas {
  Canvas(int w, int h) : px(size_t(w) * size_t(h), 0xFF102030u) {
    s.width = w;
    s.height = h;
    s.pixels = px.data();
  }
  uint32_t at(int x, int y) const { return px[size_t(y) * size_t(s.width) + size_t(x)]; }
  std::vector<uint32_t> px;
  yui::fb::Surface s;
};

}  // namespace

YUI_TEST(display, shared_vectors) {
  std::ifstream f("tests/data/display_vectors.txt");
  CHECK_MSG(bool(f), "không mở được tests/data/display_vectors.txt (chạy từ gốc repo)");
  std::string line;
  int cases = 0;
  while (std::getline(f, line)) {
    if (line.empty() || line[0] == '#') continue;
    const auto c = split(line, '|');
    CHECK_EQ(c.size(), size_t(17));
    if (c.size() != 17) continue;
    const auto edid = unhex(c[1]);
    std::vector<d::Mode> modes;
    if (c[2] != "-")
      for (const auto& m : split(c[2], ',')) modes.push_back(parse_mode(m));
    d::Mode cur;
    const bool have_cur = c[3] != "-";
    if (have_cur) cur = parse_mode(c[3]);
    const auto plan = d::plan(modes, edid.empty() ? nullptr : edid.data(), edid.size(),
                              have_cur ? &cur : nullptr, uint8_t(std::atoi(c[4].c_str())),
                              uint32_t(std::atoi(c[5].c_str())));
    const d::Edid e = d::parse_edid(edid.data(), edid.size());
    const std::string tag = c[0] + ": ";
    CHECK_MSG((e.valid ? "1" : "0") == c[6], tag + "edid valid");
    CHECK_MSG((e.has_native ? wh(e.native_width, e.native_height) : std::string("-")) == c[7],
              tag + "native");
    CHECK_MSG(wh(e.width_mm, e.height_mm) == c[8], tag + "mm " + wh(e.width_mm, e.height_mm));
    CHECK_MSG(std::to_string(plan.pick.index) == c[9], tag + "index " + std::to_string(plan.pick.index));
    CHECK_MSG(std::string(d::reason_code(plan.pick.reason)) == c[10],
              tag + "reason " + d::reason_code(plan.pick.reason));
    CHECK_MSG((plan.pick.distorted ? "1" : "0") == c[11], tag + "distorted");
    CHECK_MSG(std::to_string(plan.scale.k) == c[12], tag + "k " + std::to_string(plan.scale.k));
    CHECK_MSG(std::to_string(plan.scale.dpi_x10) == c[13],
              tag + "dpi " + std::to_string(plan.scale.dpi_x10));
    const d::Mode& m = plan.pick.reason == d::PickReason::NoModes ? cur : plan.pick.mode;
    CHECK_MSG(wh(m.width, m.height) == c[14], tag + "mode " + wh(m.width, m.height));
    CHECK_MSG(std::to_string(plan.scale.columns) == c[15], tag + "cols");
    CHECK_MSG(std::to_string(plan.scale.rows) == c[16], tag + "rows");
    CHECK_MSG(!plan.summary_vi.empty(), tag + "summary");
    ++cases;
  }
  CHECK_MSG(cases >= 12, "DoD P2.16: ít nhất 12 bảng chế độ/EDID tổng hợp");
}

YUI_TEST(display, edid_fields) {
  // Phần đầu của ca laptop14_1080p trong tệp vector đã kiểm số; ở đây kiểm các
  // trường không có trong vector (hãng, tên, tần số).
  std::ifstream f("tests/data/display_vectors.txt");
  std::string line, hex;
  while (std::getline(f, line))
    if (line.rfind("laptop14_1080p|", 0) == 0) hex = split(line, '|')[1];
  const auto b = unhex(hex);
  CHECK_EQ(b.size(), size_t(128));
  const auto e = d::parse_edid(b.data(), b.size());
  CHECK(e.valid);
  CHECK_EQ(std::string(e.vendor), std::string("YUI"));
  CHECK_EQ(e.product, uint16_t(0x1600));
  CHECK_EQ(e.native_refresh_hz, uint16_t(60));
  CHECK_EQ(e.name, std::string("YUI 14IN"));
  CHECK_EQ(int(e.revision), 4);

  // Khối mở rộng phía sau (256 byte) không làm hỏng khối cơ sở.
  std::vector<uint8_t> two = b;
  two.resize(256, 0x02);
  CHECK(d::parse_edid(two.data(), two.size()).valid);
  // Mọi bit lật trong 128 byte đều bị checksum bắt.
  int caught = 0;
  for (size_t i = 0; i < 128; ++i) {
    std::vector<uint8_t> x = b;
    x[i] ^= 0x10;
    caught += d::parse_edid(x.data(), x.size()).valid ? 0 : 1;
  }
  CHECK_EQ(caught, 128);
  CHECK(!d::parse_edid(nullptr, 128).valid);
  CHECK(!d::parse_edid(b.data(), 127).valid);
}

YUI_TEST(display, dpi_decides_k_not_resolution) {
  // Cùng 1920 px ngang: laptop 14" (≈157 dpi) và màn 24" (≈92 dpi) phải khác k.
  const std::vector<d::Mode> m = {d::Mode{1920, 1080, 32, 60}};
  const auto l14 = make_edid(1920, 1080, 309, 174);
  const auto l24 = make_edid(1920, 1080, 531, 299);
  const auto p14 = d::plan(m, l14.data(), l14.size(), nullptr);
  const auto p24 = d::plan(m, l24.data(), l24.size(), nullptr);
  CHECK(p14.edid_used && p24.edid_used);
  CHECK(p14.scale.dpi_measured && p24.scale.dpi_measured);
  CHECK_EQ(int(p14.scale.k), 2);
  CHECK_EQ(int(p24.scale.k), 1);
  CHECK(p14.summary_vi.find("EDID") != std::string::npos);
  // Người dùng ghi đè thắng DPI, và vượt mức vừa màn hình thì bị đánh dấu.
  const auto o = d::plan(m, l24.data(), l24.size(), nullptr, 3);
  CHECK_EQ(int(o.scale.k), 3);
  CHECK(!o.scale.cramped);
  const auto o4 = d::plan(m, l24.data(), l24.size(), nullptr, 4);
  CHECK(o4.scale.cramped);
  CHECK_EQ(int(d::plan(m, l24.data(), l24.size(), nullptr, 9).scale.k), 4);
  // Không có kích thước vật lý: ghi rõ là đoán.
  d::ScaleInput si;
  si.width_px = 1920;
  si.height_px = 1080;
  const auto g = d::choose_scale(si);
  CHECK(g.dpi_guessed && !g.dpi_measured);
  CHECK(g.reason_vi.find("ĐOÁN") != std::string::npos);
  // Ranh giới làm tròn: 143 dpi ⇒ 1, 144 ⇒ 2, 239 ⇒ 2, 240 ⇒ 3, 336 ⇒ 4 (nếu vừa).
  const uint32_t hints[] = {143, 144, 239, 240, 335, 336, 700};
  const int expect[] = {1, 2, 2, 3, 3, 4, 4};
  for (int i = 0; i < 7; ++i) {
    d::ScaleInput h;
    h.width_px = 3840;
    h.height_px = 2400;
    h.dpi_hint = hints[i];
    CHECK_EQ(int(d::choose_scale(h).k), expect[i]);
  }
  // k không bao giờ làm còn dưới 80 cột × 25 dòng khi tự chọn.
  for (uint32_t w = 320; w <= 7680; w += 160)
    for (uint32_t dpi = 40; dpi <= 700; dpi += 60) {
      d::ScaleInput h;
      h.width_px = w;
      h.height_px = w * 9 / 16;
      h.dpi_hint = dpi;
      const auto s = d::choose_scale(h);
      CHECK(s.k >= 1 && s.k <= 4);
      if (s.k > 1) CHECK(s.columns >= 80 && s.rows >= 25);
    }
}

YUI_TEST(display, aspect_and_letterbox) {
  CHECK(d::same_aspect(1366, 768, 1920, 1080));  // 1366×768 tính là 16:9
  CHECK(d::same_aspect(1360, 768, 1280, 720));
  CHECK(!d::same_aspect(1920, 1200, 1920, 1080));  // 16:10 ≠ 16:9
  CHECK(!d::same_aspect(1280, 1024, 1024, 768));   // 5:4 ≠ 4:3
  CHECK(!d::same_aspect(2560, 1080, 1920, 1080));  // 21:9 ≠ 16:9
  CHECK(!d::same_aspect(0, 768, 1920, 1080));

  // 640×480 lên 1920×1080: k = 2, viền trái/phải, không kéo giãn.
  const auto lb = d::letterbox(640, 480, 1920, 1080);
  CHECK_EQ(lb.k, 2u);
  CHECK_EQ(lb.w, 1280u);
  CHECK_EQ(lb.h, 960u);
  CHECK_EQ(lb.x, 320u);
  CHECK_EQ(lb.y, 60u);
  const auto full = d::letterbox(960, 540, 3840, 2160);
  CHECK_EQ(full.k, 4u);
  CHECK_EQ(full.x + full.y, 0u);
  CHECK_EQ(d::letterbox(1920, 1080, 1280, 720).k, 0u);
  CHECK_EQ(d::letterbox(0, 10, 100, 100).k, 0u);
}

YUI_TEST(display, present_scaled_blocks_and_black_borders) {
  Canvas src(40, 30), dst(130, 100);
  for (int y = 0; y < 30; ++y)
    for (int x = 0; x < 40; ++x) src.px[size_t(y * 40 + x)] = 0xFF000000u | uint32_t(x * 977 + y * 131);
  const int k = yui::fb::present_scaled(dst.s, src.s);
  CHECK_EQ(k, 3);  // min(130/40, 100/30) = 3
  const int ox = (130 - 120) / 2, oy = (100 - 90) / 2;
  bool blocks = true, borders = true;
  for (int y = 0; y < 100; ++y)
    for (int x = 0; x < 130; ++x) {
      const bool in = x >= ox && x < ox + 120 && y >= oy && y < oy + 90;
      if (in)
        blocks = blocks && dst.at(x, y) == src.at((x - ox) / 3, (y - oy) / 3);
      else
        borders = borders && dst.at(x, y) == 0xFF000000u;
    }
  CHECK(blocks);
  CHECK(borders);
  Canvas big(200, 200);
  CHECK_EQ(yui::fb::present_scaled(dst.s, big.s), 0);
}

YUI_TEST(display, text_at_k_is_k_by_k_blocks_of_k1) {
  // DoD P2.16: chữ ở hệ số k phải TRÙNG từng điểm ảnh với chữ k = 1 phóng khối
  // k×k — không nội suy, không làm mịn. Kiểm cả font 5×7 lẫn chữ Việt có dấu
  // chồng (dấu vẽ phía trên dòng nên gốc y chừa chỗ).
  const std::string vi = "Tiếng Việt: ỗ ặ ữ Ư Đ ẫ";
  const std::string ascii = "YUIVIEOS 2026";
  for (int k = 2; k <= 4; ++k) {
    const int w1 = yui::fb::text_width_utf8(vi, 1) + 4, h1 = yui::fb::text_height_utf8(1) + 8;
    Canvas a(w1, h1), b(w1 * k, h1 * k);
    yui::fb::draw_text_utf8(a.s, 2, 4, vi, 0xFFFFFFFFu, 1);
    yui::fb::draw_text_utf8(b.s, 2 * k, 4 * k, vi, 0xFFFFFFFFu, k);
    bool same = true;
    long lit = 0;
    for (int y = 0; y < h1 * k; ++y)
      for (int x = 0; x < w1 * k; ++x) {
        same = same && b.at(x, y) == a.at(x / k, y / k);
        lit += b.at(x, y) == 0xFFFFFFFFu;
      }
    CHECK_MSG(same, "utf8 k=" + std::to_string(k));
    CHECK(lit > 100);

    const int w2 = yui::fb::text_width(ascii, 1) + 2, h2 = yui::fb::text_height(1) + 2;
    Canvas c(w2, h2), e(w2 * k, h2 * k);
    yui::fb::draw_text(c.s, 1, 1, ascii, 0xFFFFFFFFu, 1);
    yui::fb::draw_text(e.s, k, k, ascii, 0xFFFFFFFFu, k);
    bool same2 = true;
    for (int y = 0; y < h2 * k; ++y)
      for (int x = 0; x < w2 * k; ++x) same2 = same2 && e.at(x, y) == c.at(x / k, y / k);
    CHECK_MSG(same2, "5x7 k=" + std::to_string(k));
  }
}

YUI_TEST(display, frame_budget_is_separate_and_4k_uses_dirty_rects) {
  const auto b1080 = d::frame_budget(1920, 1080);
  CHECK_EQ(b1080.frame_bytes, uint64_t(8294400));  // 7,9 MiB
  CHECK(b1080.double_buffer);
  CHECK(!b1080.dirty_rects);
  const auto b4k = d::frame_budget(3840, 2160);
  CHECK_EQ(b4k.frame_bytes, uint64_t(33177600));  // 31,6 MiB
  CHECK(!b4k.double_buffer);
  CHECK(b4k.dirty_rects);
  const auto b1440 = d::frame_budget(2560, 1440);
  CHECK(!b1440.double_buffer && b1440.dirty_rects);
  // Hai khung 4K (63,3 MiB) gần bằng cả ngân sách 64 MB của P2.11 ⇒ phải tính
  // tách riêng, nếu không máy 4K sẽ "vượt ngân sách" chỉ vì độ phân giải.
  CHECK(b4k.frame_bytes * 2 > (60ull << 20));
  const std::vector<d::Mode> m = {d::Mode{3840, 2160, 32, 60}};
  const auto p = d::plan(m, nullptr, 0, nullptr);
  CHECK(p.summary_vi.find("vùng bẩn") != std::string::npos);
}

YUI_TEST(display, splash_scales_with_k_at_three_resolutions) {
  // Splash phải vẽ đúng ở ≥ 3 độ phân giải (bản host; QEMU kiểm bản Rust).
  yui::profile::HostProfile host;
  host.tier = "can-bang";
  host.threads = 8;
  host.ram_gb = 16.0;
  const int sizes[][2] = {{800, 600}, {1280, 1024}, {1920, 1080}, {3840, 2160}};
  for (const auto& sz : sizes) {
    Canvas c(sz[0], sz[1]);
    CHECK_EQ(yui::fb::render_splash(c.s, host), long(sz[0]) * sz[1]);
    // Có chữ trắng ở dải tiêu đề, và không có điểm ảnh nào còn màu "chưa vẽ".
    long ink = 0, untouched = 0;
    for (uint32_t p : c.px) {
      untouched += p == 0xFF102030u;
      ink += (p & 0x00FFFFFFu) > 0x00E0E0E0u;
    }
    CHECK_EQ(untouched, 0L);
    CHECK(ink > 200);
  }
  // 4K tự chọn k = 2 ⇒ giống hệt ép k = 2; ép k = 1 thì chữ nhỏ hơn.
  Canvas a(3840, 2160), b(3840, 2160), c1(3840, 2160);
  yui::fb::render_splash(a.s, host);
  yui::fb::render_splash(b.s, host, 2);
  yui::fb::render_splash(c1.s, host, 1);
  CHECK(a.px == b.px);
  CHECK(a.px != c1.px);
}

YUI_TEST(display, ui_plan_uses_same_rule) {
  yui::ui::HardwareForUi hw{8.0, false, false, "avx2"};
  CHECK_EQ(yui::ui::plan(yui::ui::DisplayInfo{1920, 1080, 96}, hw).scale_px, 1);
  CHECK_EQ(yui::ui::plan(yui::ui::DisplayInfo{1920, 1080, 157}, hw).scale_px, 2);
  CHECK_EQ(yui::ui::plan(yui::ui::DisplayInfo{3840, 2160, 283}, hw).scale_px, 3);
  // 1366×768 ở 300 dpi (vô lý) vẫn bị giới hạn để còn ≥ 80 cột × 25 dòng.
  CHECK_EQ(yui::ui::plan(yui::ui::DisplayInfo{1366, 768, 300}, hw).scale_px, 2);
}

YUI_TEST_MAIN("test_display")
