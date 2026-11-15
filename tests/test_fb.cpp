// Linear framebuffer + boot splash tests (Phase P1 item 4 / P2.6 groundwork).
//
// These tests exist so the framebuffer writer can be trusted *before* it ever
// touches real video memory: every write is bounds-checked against a guard
// band, and the splash output is verified pixel-by-pixel for determinism.
#include <cstdint>
#include <string>
#include <vector>

#include "testutil.hpp"
#include "yui/fb.hpp"

namespace {

struct Canvas {
  explicit Canvas(int w, int h) : width(w), height(h), storage(size_t(w) * size_t(h) + 2, 0xDEADBEEFu) {
    surface.width = w;
    surface.height = h;
    surface.pixels = storage.data() + 1;  // 1 pixel of guard band on each side
  }
  bool guards_intact() const {
    return storage.front() == 0xDEADBEEFu && storage.back() == 0xDEADBEEFu;
  }
  int width;
  int height;
  std::vector<uint32_t> storage;
  yui::fb::Surface surface;
};

yui::profile::HostProfile host(const std::string& tier, int threads, double ram_gb, int k, int w) {
  yui::profile::HostProfile h;
  h.tier = tier;
  h.threads = threads;
  h.ram_gb = ram_gb;
  h.tune.k = k;
  h.tune.window = w;
  h.simd.best = "avx2";
  return h;
}

int count_not(const Canvas& c, uint32_t colour) {
  int n = 0;
  for (long i = 0; i < long(c.width) * long(c.height); ++i) {
    if (c.surface.pixels[i] != colour) ++n;
  }
  return n;
}

}  // namespace

// ------------------------------------------------------------------ surface --
YUI_TEST(fb, validity_checks) {
  yui::fb::Surface none;
  CHECK(!yui::fb::valid(none));
  std::vector<uint32_t> pixels(4, 0);
  yui::fb::Surface zero_width;
  zero_width.width = 0;
  zero_width.height = 2;
  zero_width.pixels = pixels.data();
  CHECK(!yui::fb::valid(zero_width));
  yui::fb::Surface ok{2, 2, pixels.data()};
  CHECK(yui::fb::valid(ok));
}

YUI_TEST(fb, clear_fills_every_pixel) {
  Canvas c(7, 5);
  yui::fb::clear(c.surface, 0xFF102030);
  for (int i = 0; i < c.width * c.height; ++i) CHECK_EQ(c.surface.pixels[i], 0xFF102030u);
  CHECK(c.guards_intact());
}

YUI_TEST(fb, put_pixel_is_clipped) {
  Canvas c(4, 3);
  yui::fb::clear(c.surface, 0);
  yui::fb::put_pixel(c.surface, 0, 0, 0xFFAABBCC);
  yui::fb::put_pixel(c.surface, 3, 2, 0xFFAABBCC);
  yui::fb::put_pixel(c.surface, -1, 1, 0xFFFFFFFF);  // ignored
  yui::fb::put_pixel(c.surface, 4, 1, 0xFFFFFFFF);   // ignored
  yui::fb::put_pixel(c.surface, 1, -1, 0xFFFFFFFF);  // ignored
  yui::fb::put_pixel(c.surface, 1, 3, 0xFFFFFFFF);   // ignored
  CHECK_EQ(c.surface.pixels[0], 0xFFAABBCCu);
  CHECK_EQ(c.surface.pixels[2 * 4 + 3], 0xFFAABBCCu);  // (x=3, y=2), width=4
  int white = 0;
  for (int i = 0; i < c.width * c.height; ++i) {
    if (c.surface.pixels[i] == 0xFFFFFFFFu) ++white;
  }
  CHECK_EQ(white, 0);
  CHECK(c.guards_intact());
}

YUI_TEST(fb, fill_rect_clips_to_the_surface) {
  Canvas c(10, 10);
  yui::fb::clear(c.surface, 0);
  yui::fb::fill_rect(c.surface, yui::fb::Rect{-5, -5, 6, 6}, 0xFF00FF00);
  int green = 0;
  for (int i = 0; i < 100; ++i) {
    if (c.surface.pixels[i] == 0xFF00FF00u) ++green;
  }
  CHECK_EQ(green, 1);  // only the (0,0) corner survives clipping

  yui::fb::clear(c.surface, 0);
  yui::fb::fill_rect(c.surface, yui::fb::Rect{8, 8, 100, 100}, 0xFF0000FF);
  int blue = 0;
  for (int i = 0; i < 100; ++i) {
    if (c.surface.pixels[i] == 0xFF0000FFu) ++blue;
  }
  CHECK_EQ(blue, 4);  // 2x2 visible corner
  CHECK(c.guards_intact());
}

YUI_TEST(fb, empty_rect_is_a_no_op) {
  Canvas c(6, 6);
  yui::fb::clear(c.surface, 0x11223344);
  yui::fb::fill_rect(c.surface, yui::fb::Rect{1, 1, 0, 4}, 0xFFFFFFFF);
  yui::fb::fill_rect(c.surface, yui::fb::Rect{1, 1, 4, -3}, 0xFFFFFFFF);
  for (int i = 0; i < 36; ++i) CHECK_EQ(c.surface.pixels[i], 0x11223344u);
}

// --------------------------------------------------------------------- text --
YUI_TEST(fb, text_metrics) {
  CHECK_EQ(yui::fb::text_width("", 1), 0);
  CHECK_EQ(yui::fb::text_width("A", 1), 5);
  CHECK_EQ(yui::fb::text_width("AB", 1), 11);   // 6*2 - 1
  CHECK_EQ(yui::fb::text_width("AB", 2), 22);
  CHECK_EQ(yui::fb::text_height(1), 7);
  CHECK_EQ(yui::fb::text_height(3), 21);
  CHECK_EQ(yui::fb::text_height(0), 7);  // clamped, never zero
}

YUI_TEST(fb, draw_text_marks_pixels_inside_the_box) {
  Canvas c(64, 16);
  yui::fb::clear(c.surface, 0);
  yui::fb::draw_text(c.surface, 1, 1, "A", 0xFFFFFFFF, 1);
  int lit = count_not(c, 0);
  CHECK(lit > 0);
  CHECK(lit <= 35);  // 5x7 glyph can never exceed 35 pixels
  CHECK(c.guards_intact());
}

YUI_TEST(fb, lowercase_uses_the_uppercase_glyph) {
  Canvas lower(32, 16), upper(32, 16);
  yui::fb::clear(lower.surface, 0);
  yui::fb::clear(upper.surface, 0);
  yui::fb::draw_text(lower.surface, 0, 0, "yui", 0xFFFFFFFF, 1);
  yui::fb::draw_text(upper.surface, 0, 0, "YUI", 0xFFFFFFFF, 1);
  for (int i = 0; i < 32 * 16; ++i) CHECK_EQ(lower.surface.pixels[i], upper.surface.pixels[i]);
}

YUI_TEST(fb, unknown_glyph_becomes_question_mark) {
  Canvas unknown(32, 16), question(32, 16);
  yui::fb::clear(unknown.surface, 0);
  yui::fb::clear(question.surface, 0);
  yui::fb::draw_text(unknown.surface, 0, 0, "~", 0xFFFFFFFF, 1);
  yui::fb::draw_text(question.surface, 0, 0, "?", 0xFFFFFFFF, 1);
  for (int i = 0; i < 32 * 16; ++i) CHECK_EQ(unknown.surface.pixels[i], question.surface.pixels[i]);
}

YUI_TEST(fb, text_never_writes_outside_the_given_box) {
  Canvas c(40, 12);  // shorter than the glyph height at scale 1 (7 rows) + offset
  yui::fb::clear(c.surface, 0);
  yui::fb::draw_text(c.surface, 36, 8, "WWW", 0xFFFFFFFF, 2);  // would overflow right/bottom
  CHECK(c.guards_intact());
}

YUI_TEST(fb, ascii_upper_is_transliterated_and_safe) {
  CHECK_EQ(yui::fb::ascii_upper("may-yeu"), std::string("MAY-YEU"));
  CHECK_EQ(yui::fb::ascii_upper("k=2, W=16"), std::string("K=2, W=16"));
  const std::string vi = yui::fb::ascii_upper("Tiếng Việt");
  for (unsigned char ch : vi) CHECK(ch < 0x80);
  CHECK_EQ(vi.size(), std::string("Tiếng Việt").size());  // one space per byte, no shrinking
  CHECK_EQ(yui::fb::ascii_upper("\xF0\x9F\x8C\xB8"), std::string("    "));  // emoji → spaces
}

// ------------------------------------------------------------------- splash --
YUI_TEST(fb, splash_refuses_a_surface_too_small) {
  Canvas tiny(100, 60);
  CHECK_EQ(yui::fb::render_splash(tiny.surface, host("may-yeu", 2, 2.0, 2, 1024)), 0L);
  yui::fb::Surface invalid;
  CHECK_EQ(yui::fb::render_splash(invalid, host("may-manh", 16, 32.0, 8, 8192)), 0L);
}

YUI_TEST(fb, splash_draws_the_expected_chrome) {
  Canvas c(320, 200);
  CHECK_EQ(yui::fb::render_splash(c.surface, host("may-yeu", 2, 2.0, 2, 1024)), 320L * 200L);
  // Red accent band across the top.
  CHECK_EQ(c.surface.pixels[0], 0xFFDA251Du);
  CHECK_EQ(c.surface.pixels[319], 0xFFDA251Du);
  // The panel sits at the bottom, so the middle of the image is not all ink.
  const int lit = count_not(c, 0xFF0B1020u);
  CHECK(lit > 200);              // title + panel + motif
  CHECK(lit < 320 * 200 / 2);    // but it is still mostly background
  CHECK(c.guards_intact());
}

YUI_TEST(fb, splash_is_deterministic) {
  Canvas a(320, 200), b(320, 200);
  yui::fb::render_splash(a.surface, host("can-bang", 8, 16.0, 4, 4096));
  yui::fb::render_splash(b.surface, host("can-bang", 8, 16.0, 4, 4096));
  bool same = true;
  for (int i = 0; i < 320 * 200; ++i) {
    if (a.surface.pixels[i] != b.surface.pixels[i]) {
      same = false;
      break;
    }
  }
  CHECK_MSG(same, "cùng đầu vào phải cho cùng ảnh");
}

YUI_TEST(fb, splash_reflects_the_machine_class_and_tuning) {
  Canvas weak(320, 200), strong(320, 200);
  yui::fb::render_splash(weak.surface, host("may-yeu", 2, 2.0, 1, 16));
  yui::fb::render_splash(strong.surface, host("may-manh", 16, 32.0, 4, 1024));
  int diff = 0;
  for (int i = 0; i < 320 * 200; ++i) {
    if (weak.surface.pixels[i] != strong.surface.pixels[i]) ++diff;
  }
  CHECK_MSG(diff > 50, "panel phải khác nhau khi máy khác nhau");
}

YUI_TEST(fb, panel_text_never_overflows_on_a_small_screen) {
  // 200x120 is the smallest screen we promise to support; the panel must stay
  // inside the accent bar on the left and the surface on the right.
  Canvas c(200, 120);
  CHECK_EQ(yui::fb::render_splash(c.surface, host("may-manh", 64, 128.0, 4, 1024)), 200L * 120L);
  CHECK(c.guards_intact());
}

// ---------------------------------------------------------------------- PPM --
YUI_TEST(fb, ppm_header_and_pixel_order) {
  Canvas c(1, 1);
  c.surface.pixels[0] = 0xFF112233;  // A=FF, R=11, G=22, B=33
  const std::string ppm = yui::fb::to_ppm(c.surface);
  CHECK_EQ(ppm.substr(0, 11), std::string("P6\n1 1\n255\n"));
  CHECK_EQ(ppm.size(), size_t(11 + 3));
  CHECK_EQ(int(uint8_t(ppm[11])), 0x11);
  CHECK_EQ(int(uint8_t(ppm[12])), 0x22);
  CHECK_EQ(int(uint8_t(ppm[13])), 0x33);
}

YUI_TEST(fb, ppm_of_an_invalid_surface_is_empty) {
  yui::fb::Surface none;
  CHECK(yui::fb::to_ppm(none).empty());
}

YUI_TEST(fb, ppm_of_the_splash_has_the_expected_size) {
  Canvas c(320, 200);
  yui::fb::render_splash(c.surface, host("may-yeu", 2, 2.0, 2, 1024));
  const std::string ppm = yui::fb::to_ppm(c.surface);
  const std::string header = "P6\n320 200\n255\n";
  CHECK_EQ(ppm.substr(0, header.size()), header);
  CHECK_EQ(ppm.size(), header.size() + size_t(320) * 200 * 3);
}

YUI_TEST_MAIN("test_fb")
