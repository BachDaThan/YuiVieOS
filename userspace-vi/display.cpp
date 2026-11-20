// Chọn chế độ hiển thị gốc + hệ số phóng nguyên — P2.16. Xem include/yui/display.hpp.
//
// Mọi phép tính là số nguyên (không float) để bản Rust trong nhân
// (kernel-cap/src/display.rs, no_std) cho ra từng bit giống hệt.
#include "yui/display.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace yui::display {
namespace {

constexpr uint32_t kMaxDim = 16384;
constexpr uint64_t kDpiX10Min = 400;   // 40 dpi: dưới mức này là số đo hỏng
constexpr uint64_t kDpiX10Max = 7000;  // 700 dpi: trên mức này là số đo hỏng

bool usable(const Mode& m) {
  return (m.bpp == 24 || m.bpp == 32) && m.width >= 1 && m.width <= kMaxDim && m.height >= 1 &&
         m.height <= kMaxDim;
}

uint64_t area(const Mode& m) { return uint64_t(m.width) * m.height; }

bool plausible(uint32_t px, uint32_t mm) {
  if (mm == 0 || px == 0) return false;
  const uint64_t d = uint64_t(px) * 254 / mm;
  return d >= kDpiX10Min && d <= kDpiX10Max;
}

// Sai lệch tỉ lệ khung hình × 10⁶ (tương đối so với tích lớn hơn).
uint64_t aspect_err(uint32_t aw, uint32_t ah, uint32_t bw, uint32_t bh) {
  const uint64_t x = uint64_t(aw) * bh, y = uint64_t(bw) * ah;
  const uint64_t mx = x > y ? x : y;
  if (mx == 0) return 1000000;
  const uint64_t d = x > y ? x - y : y - x;
  return d * 1000000 / mx;
}

// So sánh khoá (diện tích, bpp, đúng tần số gốc). true khi a tốt hơn b.
bool key_better(const Mode& a, const Mode& b, uint16_t native_hz) {
  if (area(a) != area(b)) return area(a) > area(b);
  if (a.bpp != b.bpp) return a.bpp > b.bpp;
  const bool ha = native_hz != 0 && a.refresh_hz == native_hz;
  const bool hb = native_hz != 0 && b.refresh_hz == native_hz;
  return ha && !hb;
}

std::string fmt_mode(const Mode& m) {
  char buf[48];
  std::snprintf(buf, sizeof buf, "%ux%u", unsigned(m.width), unsigned(m.height));
  return buf;
}

uint8_t clamp_k(uint64_t k) { return uint8_t(k < 1 ? 1 : (k > 4 ? 4 : k)); }

}  // namespace

// ------------------------------------------------------------------ EDID --
Edid parse_edid(const uint8_t* d, size_t n) {
  Edid e;
  if (d == nullptr || n < 128) {
    e.error = "EDID ngắn hơn 128 byte";
    return e;
  }
  static const uint8_t kHeader[8] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};
  for (int i = 0; i < 8; ++i)
    if (d[i] != kHeader[i]) {
      e.error = "sai header EDID";
      return e;
    }
  unsigned sum = 0;
  for (int i = 0; i < 128; ++i) sum += d[i];
  if ((sum & 0xFF) != 0) {
    e.error = "sai checksum EDID";
    return e;
  }
  if (d[18] != 1) {
    e.error = "phiên bản EDID không phải 1.x";
    return e;
  }
  e.version = d[18];
  e.revision = d[19];
  const unsigned id = (unsigned(d[8]) << 8) | d[9];
  for (int i = 0; i < 3; ++i) {
    const unsigned v = (id >> (10 - 5 * i)) & 0x1F;
    e.vendor[i] = (v >= 1 && v <= 26) ? char('A' + v - 1) : '?';
  }
  e.product = uint16_t(d[10] | (d[11] << 8));

  uint32_t dtd_w_mm = 0, dtd_h_mm = 0;
  for (int slot = 0; slot < 4; ++slot) {
    const uint8_t* t = d + 54 + 18 * slot;
    const unsigned clock = unsigned(t[0]) | (unsigned(t[1]) << 8);  // đơn vị 10 kHz
    if (clock != 0) {
      if (e.has_native) continue;  // chỉ DTD đầu tiên là chế độ ưu tiên
      const uint32_t ha = t[2] | (uint32_t(t[4] & 0xF0) << 4);
      const uint32_t hb = t[3] | (uint32_t(t[4] & 0x0F) << 8);
      uint32_t va = t[5] | (uint32_t(t[7] & 0xF0) << 4);
      const uint32_t vb = t[6] | (uint32_t(t[7] & 0x0F) << 8);
      if (ha == 0 || va == 0) continue;
      const bool interlaced = (t[17] & 0x80) != 0;
      if (interlaced) va *= 2;  // DTD xen kẽ ghi chiều cao một trường
      e.has_native = true;
      e.native_width = ha;
      e.native_height = va;
      const uint64_t total = uint64_t(ha + hb) * (interlaced ? va / 2 + vb : va + vb);
      if (total > 0) {
        // Xen kẽ: đây là tần số trường (vd "1080i60"), đúng cách màn hình báo.
        const uint64_t hz = (uint64_t(clock) * 10000 + total / 2) / total;
        e.native_refresh_hz = uint16_t(hz > 1000 ? 0 : hz);
      }
      dtd_w_mm = t[12] | (uint32_t(t[14] & 0xF0) << 4);
      dtd_h_mm = t[13] | (uint32_t(t[14] & 0x0F) << 8);
    } else if (t[2] == 0 && t[3] == 0xFC && e.name.empty()) {
      std::string name;
      for (int i = 5; i < 18; ++i) {
        const uint8_t c = t[i];
        if (c == 0x0A || c == 0) break;
        name.push_back((c >= 0x20 && c < 0x7F) ? char(c) : '?');
      }
      while (!name.empty() && name.back() == ' ') name.pop_back();
      e.name = name;
    }
  }

  // Kích thước vật lý: DTD (mm) chính xác hơn byte 21/22 (cm). Số vô lý (vd
  // EDID ghi 16×9 mm làm "tỉ lệ") bị bỏ ⇒ k sẽ theo DPI gợi ý/đoán.
  const uint32_t cm_w = d[21], cm_h = d[22];
  if (e.has_native) {
    if (plausible(e.native_width, dtd_w_mm) && plausible(e.native_height, dtd_h_mm)) {
      e.width_mm = uint16_t(dtd_w_mm);
      e.height_mm = uint16_t(dtd_h_mm);
    } else if (plausible(e.native_width, cm_w * 10) && plausible(e.native_height, cm_h * 10)) {
      e.width_mm = uint16_t(cm_w * 10);
      e.height_mm = uint16_t(cm_h * 10);
    }
  } else if (cm_w != 0 && cm_h != 0) {
    e.width_mm = uint16_t(cm_w * 10);
    e.height_mm = uint16_t(cm_h * 10);
  }
  e.valid = true;
  return e;
}

bool same_aspect(uint32_t aw, uint32_t ah, uint32_t bw, uint32_t bh) {
  if (aw == 0 || ah == 0 || bw == 0 || bh == 0) return false;
  const uint64_t x = uint64_t(aw) * bh, y = uint64_t(bw) * ah;
  const uint64_t mx = x > y ? x : y;
  const uint64_t d = x > y ? x - y : y - x;
  return d * 1000 <= 15 * mx;
}

const char* reason_code(PickReason r) {
  switch (r) {
    case PickReason::NoModes: return "no_modes";
    case PickReason::Native: return "native";
    case PickReason::EdidAspect: return "edid_aspect";
    case PickReason::CurrentAspect: return "current_aspect";
    case PickReason::KeepCurrent: return "keep_current";
    case PickReason::ClosestAspect: return "closest_aspect";
    case PickReason::Largest: return "largest";
  }
  return "?";
}

// ------------------------------------------------------------- pick_mode --
Pick pick_mode(const std::vector<Mode>& modes, const Edid* edid, const Mode* current) {
  Pick p;
  const bool native = edid != nullptr && edid->valid && edid->has_native;
  const uint16_t hz = native ? edid->native_refresh_hz : 0;
  const uint32_t nw = native ? edid->native_width : 0, nh = native ? edid->native_height : 0;

  bool any = false;
  for (const Mode& m : modes) any = any || usable(m);
  if (!any) {
    p.reason = PickReason::NoModes;
    p.reason_vi = "firmware không cung cấp chế độ 24/32 bit nào";
    return p;
  }
  auto take = [&](int i, PickReason r) {
    p.index = i;
    p.mode = modes[size_t(i)];
    p.reason = r;
  };

  // 1. Đúng gốc.
  if (native) {
    int best = -1;
    for (size_t i = 0; i < modes.size(); ++i) {
      const Mode& m = modes[i];
      if (!usable(m) || m.width != nw || m.height != nh) continue;
      if (best < 0 || key_better(m, modes[size_t(best)], hz)) best = int(i);
    }
    if (best >= 0) {
      take(best, PickReason::Native);
      p.reason_vi = "đúng độ phân giải gốc của màn hình (" + fmt_mode(p.mode) + ", theo EDID)";
      return p;
    }
  }

  // 2. Lớn nhất cùng tỉ lệ.
  const bool have_cur = current != nullptr && current->width > 0 && current->height > 0;
  if (native || have_cur) {
    const uint32_t rw = native ? nw : current->width, rh = native ? nh : current->height;
    int best = -1, smallest_above = -1;
    for (size_t i = 0; i < modes.size(); ++i) {
      const Mode& m = modes[i];
      if (!usable(m) || !same_aspect(m.width, m.height, rw, rh)) continue;
      if (native && (m.width > nw || m.height > nh)) {
        if (smallest_above < 0 || area(m) < area(modes[size_t(smallest_above)]) ||
            (area(m) == area(modes[size_t(smallest_above)]) &&
             m.bpp > modes[size_t(smallest_above)].bpp))
          smallest_above = int(i);
        continue;
      }
      if (best < 0 || key_better(m, modes[size_t(best)], hz)) best = int(i);
    }
    if (best < 0) best = smallest_above;
    if (best >= 0) {
      take(best, native ? PickReason::EdidAspect : PickReason::CurrentAspect);
      p.reason_vi = native ? "không có chế độ gốc " + fmt_mode(Mode{nw, nh, 32, 0}) +
                                 ": chọn " + fmt_mode(p.mode) + " cùng tỉ lệ tấm nền"
                           : "không có EDID: chọn " + fmt_mode(p.mode) +
                                 " lớn nhất cùng tỉ lệ với chế độ đang chạy";
      return p;
    }
  }

  // 3. Không có chế độ cùng tỉ lệ.
  if (have_cur) {
    int best = -1;
    for (size_t i = 0; i < modes.size(); ++i) {
      const Mode& m = modes[i];
      if (!usable(m) || m.width != current->width || m.height != current->height) continue;
      if (best < 0 || key_better(m, modes[size_t(best)], hz)) best = int(i);
    }
    p.index = best;
    p.mode = best >= 0 ? modes[size_t(best)] : *current;
    p.reason = PickReason::KeepCurrent;
    p.distorted = native && !same_aspect(current->width, current->height, nw, nh);
    p.reason_vi = "không có chế độ nào cùng tỉ lệ màn hình: giữ nguyên " + fmt_mode(p.mode) +
                  " (không tự đổi sang tỉ lệ khác)";
    return p;
  }
  if (native) {
    int best = -1;
    uint64_t best_err = 0;
    for (size_t i = 0; i < modes.size(); ++i) {
      const Mode& m = modes[i];
      if (!usable(m)) continue;
      const uint64_t err = aspect_err(m.width, m.height, nw, nh);
      if (best < 0 || err < best_err ||
          (err == best_err && key_better(m, modes[size_t(best)], hz))) {
        best = int(i);
        best_err = err;
      }
    }
    take(best, PickReason::ClosestAspect);
    p.distorted = true;
    p.reason_vi = "không có chế độ cùng tỉ lệ " + fmt_mode(Mode{nw, nh, 32, 0}) + ": tạm dùng " +
                  fmt_mode(p.mode) + " (sẽ méo — cần hỏi người dùng)";
    return p;
  }
  int best = -1;
  for (size_t i = 0; i < modes.size(); ++i) {
    if (!usable(modes[i])) continue;
    if (best < 0 || key_better(modes[i], modes[size_t(best)], 0)) best = int(i);
  }
  take(best, PickReason::Largest);
  p.reason_vi = "không biết gì về màn hình: chọn chế độ lớn nhất " + fmt_mode(p.mode);
  return p;
}

// ---------------------------------------------------------- choose_scale --
Scale choose_scale(const ScaleInput& in) {
  Scale s;
  const uint32_t w = in.width_px, h = in.height_px;
  if (in.width_mm > 0 && in.height_mm > 0 && w > 0 && h > 0) {
    const uint64_t dx = uint64_t(w) * 254 / in.width_mm;
    const uint64_t dy = uint64_t(h) * 254 / in.height_mm;
    const uint64_t avg = (dx + dy) / 2;
    if (avg >= kDpiX10Min && avg <= kDpiX10Max) {
      s.dpi_x10 = uint32_t(avg);
      s.dpi_measured = true;
    }
  }
  if (!s.dpi_measured) {
    if (in.dpi_hint >= 40 && in.dpi_hint <= 700) {
      s.dpi_x10 = in.dpi_hint * 10;
    } else {
      s.dpi_guessed = true;
      s.dpi_x10 = h >= 2160 ? 1920 : 960;
    }
  }
  // k ≈ DPI/96 làm tròn: ranh giới 144 / 240 / 336 dpi.
  s.k_dpi = clamp_k((uint64_t(s.dpi_x10) + 480) / 960);
  // Không phóng tới mức còn dưới 80 cột (ô 6k) hoặc 25 dòng (ô 12k).
  const uint32_t fw = w / 480, fh = h / 300;
  s.k_fit = clamp_k(fw < fh ? fw : fh);
  char buf[160];
  if (in.user_override >= 1) {
    s.k = clamp_k(in.user_override);
    s.cramped = s.k > s.k_fit;
    std::snprintf(buf, sizeof buf, "người dùng chọn %ux%s", unsigned(s.k),
                  s.cramped ? " (vượt mức vừa màn hình, sẽ ít hơn 80 cột/25 dòng)" : "");
  } else {
    s.k = s.k_dpi < s.k_fit ? s.k_dpi : s.k_fit;
    std::snprintf(buf, sizeof buf, "%ux theo %u dpi %s%s", unsigned(s.k),
                  unsigned((s.dpi_x10 + 5) / 10),
                  s.dpi_measured ? "đo từ kích thước EDID"
                                 : (s.dpi_guessed ? "ĐOÁN (không có kích thước màn hình)"
                                                  : "do hệ thống báo"),
                  s.k < s.k_dpi ? ", giới hạn để còn ≥ 80 cột × 25 dòng" : "");
  }
  s.reason_vi = buf;
  s.columns = w / (6u * s.k);
  s.rows = h / (12u * s.k);
  return s;
}

Letterbox letterbox(uint32_t src_w, uint32_t src_h, uint32_t dst_w, uint32_t dst_h) {
  Letterbox l;
  if (src_w == 0 || src_h == 0 || dst_w < src_w || dst_h < src_h) return l;
  const uint32_t kx = dst_w / src_w, ky = dst_h / src_h;
  l.k = kx < ky ? kx : ky;
  l.w = src_w * l.k;
  l.h = src_h * l.k;
  l.x = (dst_w - l.w) / 2;
  l.y = (dst_h - l.h) / 2;
  return l;
}

FrameBudget frame_budget(uint32_t width, uint32_t height) {
  FrameBudget b;
  b.frame_bytes = uint64_t(width) * height * 4;
  b.double_buffer = b.frame_bytes * 2 <= (16ull << 20);
  b.dirty_rects = b.frame_bytes > (8ull << 20);
  return b;
}

Plan plan(const std::vector<Mode>& modes, const uint8_t* edid, size_t edid_len, const Mode* current,
          uint8_t user_override, uint32_t dpi_hint) {
  Plan out;
  Edid e;
  if (edid != nullptr && edid_len > 0) e = parse_edid(edid, edid_len);
  out.edid_used = e.valid;
  out.pick = pick_mode(modes, e.valid ? &e : nullptr, current);
  Mode m = out.pick.mode;
  if (out.pick.reason == PickReason::NoModes && current != nullptr) m = *current;
  ScaleInput si;
  si.width_px = m.width;
  si.height_px = m.height;
  if (e.valid) {
    si.width_mm = e.width_mm;
    si.height_mm = e.height_mm;
  }
  si.dpi_hint = dpi_hint;
  si.user_override = user_override;
  out.scale = choose_scale(si);
  out.budget = frame_budget(m.width, m.height);
  char buf[96];
  std::snprintf(buf, sizeof buf, " · khung %.1f MiB%s", double(out.budget.frame_bytes) / 1048576.0,
                out.budget.dirty_rects ? " (vẽ theo vùng bẩn)" : "");
  out.summary_vi = out.pick.reason_vi + " · phóng " + out.scale.reason_vi + buf;
  return out;
}

}  // namespace yui::display

// ------------------------------------------------- Device Tree (đường c) --
namespace yui::display {
namespace {

constexpr uint32_t kFdtMagic = 0xD00DFEEDu;
constexpr uint32_t kTokBegin = 1, kTokEndNode = 2, kTokProp = 3, kTokNop = 4, kTokEnd = 9;
constexpr size_t kFdtMaxDepth = 32;
constexpr uint32_t kFbMaxStride = 262144;

uint32_t rd_be32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

struct Span {
  const uint8_t* p = nullptr;
  size_t n = 0;
  bool present = false;
};

// Chuỗi DT: các byte tới NUL đầu tiên (hoặc cả giá trị nếu không có NUL).
bool cstr_is(const Span& v, const char* lit) {
  size_t len = 0;
  while (len < v.n && v.p[len] != 0) ++len;
  const size_t want = std::strlen(lit);
  return len == want && std::memcmp(v.p, lit, len) == 0;
}

// stringlist: tách theo NUL, so khớp đúng từng mục.
bool list_has(const Span& v, const char* lit) {
  const size_t want = std::strlen(lit);
  size_t start = 0;
  for (size_t i = 0; i <= v.n; ++i) {
    if (i == v.n || v.p[i] == 0) {
      if (i - start == want && std::memcmp(v.p + start, lit, want) == 0) return true;
      start = i + 1;
    }
  }
  return false;
}

struct NodeProps {
  Span compatible, status, format, width, height, stride, reg, address_cells, size_cells;
  bool had_child = false;
};

struct FbFormat {
  const char* name;
  uint8_t bpp, r, g, b;
};
constexpr FbFormat kFbFormats[] = {
    {"a8r8g8b8", 32, 16, 8, 0}, {"x8r8g8b8", 32, 16, 8, 0}, {"a8b8g8r8", 32, 0, 8, 16},
    {"x8b8g8r8", 32, 0, 8, 16}, {"r8g8b8", 24, 16, 8, 0},
};

uint32_t cell_count(const Span& v, uint32_t dflt) {
  if (!v.present) return dflt;
  return v.n == 4 ? rd_be32(v.p) : 0;
}

uint64_t read_cells(const uint8_t* p, uint32_t cells) {
  uint64_t v = 0;
  for (uint32_t i = 0; i < cells; ++i) v = (v << 32) | rd_be32(p + 4 * i);
  return v;
}

FdtReason judge(const NodeProps& me, const NodeProps* parent, FdtFramebuffer& out) {
  if (me.status.present && !cstr_is(me.status, "okay") && !cstr_is(me.status, "ok"))
    return FdtReason::Disabled;
  const FbFormat* fmt = nullptr;
  if (me.format.present)
    for (const FbFormat& f : kFbFormats)
      if (cstr_is(me.format, f.name)) fmt = &f;
  if (fmt == nullptr) return FdtReason::BadFormat;
  const Span* dims[3] = {&me.width, &me.height, &me.stride};
  uint32_t v[3] = {0, 0, 0};
  for (int i = 0; i < 3; ++i) {
    if (!dims[i]->present || dims[i]->n != 4) return FdtReason::BadGeometry;
    v[i] = rd_be32(dims[i]->p);
  }
  const uint32_t w = v[0], h = v[1], stride = v[2];
  if (w < 1 || w > kMaxDim || h < 1 || h > kMaxDim || uint64_t(stride) < uint64_t(w) * (fmt->bpp / 8) ||
      stride > kFbMaxStride)
    return FdtReason::BadGeometry;
  const Span none;
  const uint32_t ac = cell_count(parent ? parent->address_cells : none, 2);
  const uint32_t sc = cell_count(parent ? parent->size_cells : none, 1);
  if ((ac != 1 && ac != 2) || (sc != 1 && sc != 2) || !me.reg.present) return FdtReason::BadReg;
  const size_t entry = 4 * size_t(ac + sc);
  if (me.reg.n == 0 || me.reg.n % entry != 0) return FdtReason::BadReg;
  const uint64_t addr = read_cells(me.reg.p, ac);
  const uint64_t size = read_cells(me.reg.p + 4 * ac, sc);
  if (addr == 0 || size < uint64_t(stride) * h || size > UINT64_MAX - addr) return FdtReason::BadReg;
  out.addr = addr;
  out.size = size;
  out.width = w;
  out.height = h;
  out.stride = stride;
  out.bpp = fmt->bpp;
  out.red_shift = fmt->r;
  out.green_shift = fmt->g;
  out.blue_shift = fmt->b;
  out.format = fmt->name;
  return FdtReason::Ok;
}

}  // namespace

const char* fdt_reason_code(FdtReason r) {
  switch (r) {
    case FdtReason::Ok: return "ok";
    case FdtReason::NoNode: return "no_node";
    case FdtReason::Disabled: return "disabled";
    case FdtReason::BadFormat: return "bad_format";
    case FdtReason::BadGeometry: return "bad_geometry";
    case FdtReason::BadReg: return "bad_reg";
    case FdtReason::BadBlob: return "bad_blob";
  }
  return "?";
}

FdtFramebuffer parse_fdt_framebuffer(const uint8_t* data, size_t n) {
  FdtFramebuffer bad;
  bad.reason = FdtReason::BadBlob;
  if (data == nullptr || n < 40) return bad;
  uint32_t h[10];
  for (int i = 0; i < 10; ++i) h[i] = rd_be32(data + 4 * i);
  const uint64_t total = h[1], off_st = h[2], off_str = h[3], ver = h[5], last = h[6];
  const uint64_t sz_str = h[8], sz_st = h[9];
  if (h[0] != kFdtMagic || total < 40 || total > n || ver < 17 || last > 17) return bad;
  if (off_st % 4 != 0 || sz_st % 4 != 0 || off_st < 40 || off_st + sz_st > total || off_str + sz_str > total)
    return bad;
  const uint8_t* st = data + off_st;
  const size_t L = size_t(sz_st);
  const uint8_t* strs = data + off_str;
  const size_t SL = size_t(sz_str);

  std::vector<NodeProps> stack;
  stack.reserve(kFdtMaxDepth);
  FdtFramebuffer first_ok;
  bool have_ok = false;
  FdtReason first_bad = FdtReason::NoNode;
  bool have_bad = false;
  uint32_t count = 0;
  size_t pos = 0;
  auto token = [&](uint32_t& t) {
    if (pos > L || L - pos < 4) return false;
    t = rd_be32(st + pos);
    pos += 4;
    return true;
  };
  uint32_t t = 0;
  if (!token(t) || t != kTokBegin) return bad;
  pos = 0;
  for (;;) {
    if (!token(t)) return bad;
    if (t == kTokBegin) {
      const void* z = pos < L ? std::memchr(st + pos, 0, L - pos) : nullptr;
      if (z == nullptr) return bad;
      const size_t j = size_t(static_cast<const uint8_t*>(z) - st);
      if (!stack.empty()) stack.back().had_child = true;
      if (stack.size() >= kFdtMaxDepth) return bad;
      stack.emplace_back();
      pos = (j + 1 + 3) & ~size_t(3);
    } else if (t == kTokEndNode) {
      if (stack.empty()) return bad;
      const NodeProps me = stack.back();
      stack.pop_back();
      if (me.compatible.present && list_has(me.compatible, "simple-framebuffer")) {
        ++count;
        FdtFramebuffer fb;
        const FdtReason r = judge(me, stack.empty() ? nullptr : &stack.back(), fb);
        if (r == FdtReason::Ok && !have_ok) {
          first_ok = fb;
          have_ok = true;
        } else if (r != FdtReason::Ok && !have_bad) {
          first_bad = r;
          have_bad = true;
        }
      }
      if (stack.empty()) {
        for (;;) {  // sau nút gốc chỉ được có NOP rồi END
          if (!token(t)) return bad;
          if (t == kTokNop) continue;
          if (t != kTokEnd) return bad;
          break;
        }
        break;
      }
    } else if (t == kTokProp) {
      if (stack.empty() || stack.back().had_child) return bad;
      uint32_t len = 0, nameoff = 0;
      if (!token(len) || !token(nameoff)) return bad;
      if (len > L - pos) return bad;
      if (nameoff >= SL) return bad;
      const void* z = std::memchr(strs + nameoff, 0, SL - nameoff);
      if (z == nullptr) return bad;
      const char* name = reinterpret_cast<const char*>(strs + nameoff);
      const Span v{st + pos, len, true};
      NodeProps& np = stack.back();
      if (std::strcmp(name, "compatible") == 0) np.compatible = v;
      else if (std::strcmp(name, "status") == 0) np.status = v;
      else if (std::strcmp(name, "format") == 0) np.format = v;
      else if (std::strcmp(name, "width") == 0) np.width = v;
      else if (std::strcmp(name, "height") == 0) np.height = v;
      else if (std::strcmp(name, "stride") == 0) np.stride = v;
      else if (std::strcmp(name, "reg") == 0) np.reg = v;
      else if (std::strcmp(name, "#address-cells") == 0) np.address_cells = v;
      else if (std::strcmp(name, "#size-cells") == 0) np.size_cells = v;
      pos = (pos + len + 3) & ~size_t(3);
    } else if (t != kTokNop) {
      return bad;
    }
  }
  if (have_ok) {
    first_ok.reason = FdtReason::Ok;
    first_ok.candidates = count;
    return first_ok;
  }
  FdtFramebuffer out;
  out.reason = have_bad ? first_bad : FdtReason::NoNode;
  out.candidates = count;
  return out;
}

}  // namespace yui::display
