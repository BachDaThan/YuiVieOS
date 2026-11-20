#!/usr/bin/env python3
"""Sinh tests/data/display_vectors.txt cho P2.16 (chế độ hiển thị + hệ số phóng).

Tệp này là BẢN THỨ BA của cùng quy tắc (sau userspace-vi/display.cpp và
kernel-cap/src/display.rs), viết lại độc lập bằng Python để tính kết quả mong
đợi. C++ (`tests/test_display.cpp`) và Rust (`cargo test`, mô-đun `display`)
đều đọc cùng tệp vector; ba bản phải khớp từng trường.

Mỗi EDID được dựng từ đầu theo VESA E-EDID 1.4 (header, mã hãng 3 chữ cái
nén 5 bit, Detailed Timing Descriptor đầu tiên = chế độ ưu tiên, kích thước
cm ở byte 21/22, checksum byte 127). Không dùng EDID của hãng thật nào.

    python3 scripts/gen_display_vectors.py          # ghi lại tệp vector
    python3 scripts/gen_display_vectors.py --check  # chỉ so, khác ⇒ exit 1
"""
import os
import sys

OUT = os.path.join(os.path.dirname(__file__), "..", "tests", "data", "display_vectors.txt")

# --------------------------------------------------------------- dựng EDID --


def make_edid(w, h, hz=60, mm=(0, 0), cm=(0, 0), vendor="YUI", product=0x1600,
              name=None, interlaced=False, dtd_slot=0):
    e = bytearray(128)
    e[0:8] = bytes([0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00])
    v = 0
    for ch in vendor:
        v = (v << 5) | (ord(ch) - ord("A") + 1)
    e[8], e[9] = v >> 8, v & 0xFF
    e[10], e[11] = product & 0xFF, product >> 8
    e[16], e[17] = 1, 34          # tuần 1, năm 1990 + 34 = 2024
    e[18], e[19] = 1, 4           # EDID 1.4
    e[20] = 0xA5                  # đầu vào số, 8 bit/màu, DisplayPort
    e[21], e[22] = cm
    e[23] = 120                   # gamma 2.2
    e[24] = 0x0A                  # RGB 4:4:4, DTD đầu = chế độ ưu tiên
    for i in range(38, 54):       # standard timings: không dùng
        e[i] = 0x01
    # Slot không dùng: descriptor "dummy" (tag 0x10).
    for slot in range(4):
        t = 54 + 18 * slot
        e[t:t + 18] = bytes([0, 0, 0, 0x10, 0] + [0] * 13)
    if w:
        vl = h // 2 if interlaced else h
        hb, vb = 160, 31
        clock = round((w + hb) * (vl + vb) * hz / 10000)
        assert 0 < clock < 65536, (w, h, hz)
        t = 54 + 18 * dtd_slot
        d = bytearray(18)
        d[0], d[1] = clock & 0xFF, clock >> 8
        d[2], d[3], d[4] = w & 0xFF, hb & 0xFF, ((w >> 8) << 4) | (hb >> 8)
        d[5], d[6], d[7] = vl & 0xFF, vb & 0xFF, ((vl >> 8) << 4) | (vb >> 8)
        d[8], d[9], d[10], d[11] = 48, 32, 0x35, 0
        d[12], d[13], d[14] = mm[0] & 0xFF, mm[1] & 0xFF, ((mm[0] >> 8) << 4) | (mm[1] >> 8)
        d[17] = 0x1E | (0x80 if interlaced else 0)
        e[t:t + 18] = d
    if name is not None:
        slot = 1 if dtd_slot == 0 else 0
        t = 54 + 18 * slot
        txt = name.encode("ascii")[:13]
        if len(txt) < 13:
            txt += b"\n" + b" " * (12 - len(txt))
        e[t:t + 18] = bytes([0, 0, 0, 0xFC, 0]) + txt
    e[127] = (-sum(e[:127])) & 0xFF
    return bytes(e)


# ------------------------------------------------- bản tham chiếu độc lập --

DPI_MIN, DPI_MAX = 400, 7000   # dpi × 10


def plausible(px, mm):
    return mm > 0 and px > 0 and DPI_MIN <= px * 254 // mm <= DPI_MAX


def parse_edid(b):
    r = dict(valid=False, native=None, hz=0, mm=(0, 0), vendor="", name="")
    if b is None or len(b) < 128:
        return r
    if b[:8] != bytes([0, 255, 255, 255, 255, 255, 255, 0]) or sum(b[:128]) % 256 or b[18] != 1:
        return r
    ident = (b[8] << 8) | b[9]
    r["vendor"] = "".join(
        chr(ord("A") + c - 1) if 1 <= c <= 26 else "?"
        for c in ((ident >> 10) & 31, (ident >> 5) & 31, ident & 31))
    dmm = (0, 0)
    for slot in range(4):
        t = b[54 + 18 * slot: 72 + 18 * slot]
        clock = t[0] | (t[1] << 8)
        if clock:
            if r["native"]:
                continue
            ha = t[2] | ((t[4] & 0xF0) << 4)
            hb = t[3] | ((t[4] & 0x0F) << 8)
            va = t[5] | ((t[7] & 0xF0) << 4)
            vb = t[6] | ((t[7] & 0x0F) << 8)
            if not ha or not va:
                continue
            inter = bool(t[17] & 0x80)
            total = (ha + hb) * (va + vb)
            r["native"] = (ha, va * 2 if inter else va)
            hz = (clock * 10000 + total // 2) // total
            r["hz"] = 0 if hz > 1000 else hz
            dmm = (t[12] | ((t[14] & 0xF0) << 4), t[13] | ((t[14] & 0x0F) << 8))
        elif t[2] == 0 and t[3] == 0xFC and not r["name"]:
            s = ""
            for c in t[5:18]:
                if c in (0x0A, 0):
                    break
                s += chr(c) if 0x20 <= c < 0x7F else "?"
            r["name"] = s.rstrip(" ")
    cmw, cmh = b[21] * 10, b[22] * 10
    if r["native"]:
        nw, nh = r["native"]
        if plausible(nw, dmm[0]) and plausible(nh, dmm[1]):
            r["mm"] = dmm
        elif plausible(nw, cmw) and plausible(nh, cmh):
            r["mm"] = (cmw, cmh)
    elif cmw and cmh:
        r["mm"] = (cmw, cmh)
    r["valid"] = True
    return r


def same_aspect(aw, ah, bw, bh):
    if 0 in (aw, ah, bw, bh):
        return False
    x, y = aw * bh, bw * ah
    return abs(x - y) * 1000 <= 15 * max(x, y)


def aspect_err(aw, ah, bw, bh):
    x, y = aw * bh, bw * ah
    return abs(x - y) * 1000000 // max(x, y) if max(x, y) else 1000000


def usable(m):
    return m[2] in (24, 32) and 1 <= m[0] <= 16384 and 1 <= m[1] <= 16384


def key(m, hz):
    # Python so tuple từ trái sang phải; max() giữ phần tử ĐẦU khi bằng nhau
    # nhờ ta duyệt và chỉ thay khi lớn hơn hẳn.
    return (m[0] * m[1], m[2], 1 if hz and m[3] == hz else 0)


def best_by(items, keyf):
    best = None
    for i, m in items:
        if best is None or keyf(m) > keyf(best[1]):
            best = (i, m)
    return best


def pick(modes, ed, cur):
    native = ed["native"] if ed and ed["valid"] else None
    hz = ed["hz"] if native else 0
    ok = [(i, m) for i, m in enumerate(modes) if usable(m)]
    if not ok:
        return -1, None, "no_modes", False
    if native:
        c = [(i, m) for i, m in ok if (m[0], m[1]) == native]
        if c:
            i, m = best_by(c, lambda m: key(m, hz))
            return i, m, "native", False
    have_cur = cur is not None and cur[0] > 0 and cur[1] > 0
    if native or have_cur:
        rw, rh = native if native else cur[:2]
        c = [(i, m) for i, m in ok if same_aspect(m[0], m[1], rw, rh)]
        below = [(i, m) for i, m in c if not native or (m[0] <= rw and m[1] <= rh)]
        above = [(i, m) for i, m in c if native and (m[0] > rw or m[1] > rh)]
        reason = "edid_aspect" if native else "current_aspect"
        if below:
            i, m = best_by(below, lambda m: key(m, hz))
            return i, m, reason, False
        if above:
            i, m = best_by(above, lambda m: (-(m[0] * m[1]), m[2]))
            return i, m, reason, False
    if have_cur:
        c = [(i, m) for i, m in ok if (m[0], m[1]) == tuple(cur[:2])]
        b = best_by(c, lambda m: key(m, hz)) if c else None
        dist = bool(native) and not same_aspect(cur[0], cur[1], native[0], native[1])
        if b:
            return b[0], b[1], "keep_current", dist
        return -1, cur, "keep_current", dist
    if native:
        i, m = best_by(ok, lambda m: (-aspect_err(m[0], m[1], native[0], native[1]),) + key(m, hz))
        return i, m, "closest_aspect", True
    i, m = best_by(ok, lambda m: key(m, 0))
    return i, m, "largest", False


def clamp_k(k):
    return max(1, min(4, k))


def scale(w, h, mm, dpi_hint, override):
    dpi, measured = 960, False
    if mm[0] and mm[1] and w and h:
        avg = (w * 254 // mm[0] + h * 254 // mm[1]) // 2
        if DPI_MIN <= avg <= DPI_MAX:
            dpi, measured = avg, True
    if not measured:
        dpi = dpi_hint * 10 if 40 <= dpi_hint <= 700 else (1920 if h >= 2160 else 960)
    k_dpi = clamp_k((dpi + 480) // 960)
    k_fit = clamp_k(min(w // 480, h // 300))
    k = clamp_k(override) if override >= 1 else min(k_dpi, k_fit)
    return k, dpi, k > k_fit and override >= 1


# ------------------------------------------------------------------ ca thử --

VBE = "640x480x32,800x600x32,1024x768x32,1280x720x32,1280x1024x32,1600x900x32"


def modes_of(s):
    out = []
    if s == "-":
        return out
    for part in s.split(","):
        hz = 0
        if "@" in part:
            part, hzs = part.split("@")
            hz = int(hzs)
        w, h, b = (int(x) for x in part.split("x"))
        out.append((w, h, b, hz))
    return out


def corrupt(b, i, x=0x01):
    b = bytearray(b)
    b[i] ^= x
    return bytes(b)


def fix_sum(b):
    b = bytearray(b)
    b[127] = (-sum(b[:127])) & 0xFF
    return bytes(b)


L14 = make_edid(1920, 1080, mm=(309, 174), cm=(31, 17), name="YUI 14IN")
CASES = [
    # (tên, edid, modes, current, override, dpi_hint)
    ("laptop14_1080p", L14, VBE + ",1920x1080x32", "1024x768x32", 0, 0),
    ("desktop24_1080p", make_edid(1920, 1080, mm=(531, 299), cm=(53, 30)),
     VBE + ",1920x1080x32", "1024x768x32", 0, 0),
    ("crt_4_3_cm_only", make_edid(1024, 768, hz=85, cm=(32, 24)),
     "640x480x32,800x600x32,1024x768x32,1280x1024x32", "640x480x32", 0, 0),
    ("lcd17_5_4", make_edid(1280, 1024, mm=(338, 270), cm=(34, 27)), VBE, "800x600x32", 0, 0),
    ("lcd22_16_10", make_edid(1680, 1050, mm=(474, 296), cm=(47, 30)),
     "1024x768x32,1280x800x32,1440x900x32,1680x1050x32", "1024x768x32", 0, 0),
    ("laptop13_2560x1600", make_edid(2560, 1600, mm=(286, 179), cm=(29, 18)),
     "1280x800x32,1440x900x32,2560x1600x32", "1280x800x32", 0, 0),
    ("ultrawide34_21_9", make_edid(3440, 1440, hz=50, mm=(800, 335), cm=(80, 34)),
     "1920x1080x32,2560x1080x32,3440x1440x32", "1920x1080x32", 0, 0),
    ("laptop156_1366x768", make_edid(1366, 768, mm=(344, 194), cm=(34, 19)),
     "800x600x32,1024x768x32,1280x720x32,1366x768x32", "1024x768x32", 0, 0),
    ("uhd156_4k", make_edid(3840, 2160, mm=(344, 194), cm=(34, 19)),
     "1920x1080x32,2560x1440x32,3840x2160x32", "1920x1080x32", 0, 0),
    ("uhd27_4k", make_edid(3840, 2160, mm=(597, 336), cm=(60, 34)),
     "1920x1080x32,3840x2160x32", "1920x1080x32", 0, 0),
    ("tv43_4k", make_edid(3840, 2160, hz=30, mm=(952, 535), cm=(95, 54)),
     "1920x1080x32,3840x2160x32", "1920x1080x32", 0, 0),
    ("native_missing_vbe", L14, VBE, "1024x768x32", 0, 0),
    ("native_missing_only_larger", make_edid(1366, 768, mm=(344, 194)),
     "800x600x32,1024x768x32,1920x1080x32", "1024x768x32", 0, 0),
    # Có cả chế độ cùng tỉ lệ lớn hơn tấm nền (bị màn hình thu nhỏ ⇒ nhoè) lẫn
    # nhỏ hơn: phải chọn cái lớn nhất KHÔNG vượt tấm nền.
    ("native_missing_prefer_below", make_edid(1366, 768, mm=(344, 194)),
     "1024x768x32,1920x1080x32,1280x720x32,1600x900x32", "1024x768x32", 0, 0),
    ("no_edid_4_3", None, "640x480x32,800x600x32,1024x768x32,1280x1024x32,1600x1200x32,1920x1080x32",
     "1024x768x32", 0, 0),
    ("no_edid_dpi_hint_192", None, VBE + ",1920x1080x32", "1920x1080x32", 0, 192),
    ("edid_bad_checksum", corrupt(L14, 60), "1280x800x32,1440x900x32,1920x1080x32",
     "1280x800x32", 0, 0),
    ("edid_bad_header", fix_sum(corrupt(L14, 3)), VBE + ",1920x1080x32", "1024x768x32", 0, 0),
    ("edid_truncated", L14[:100], VBE + ",1920x1080x32", "1280x720x32", 0, 0),
    ("edid_version2", fix_sum(corrupt(L14, 18, 0x03)), VBE, "1280x720x32", 0, 0),
    ("edid_bogus_mm_aspect", make_edid(1920, 1080, mm=(16, 9)), VBE + ",1920x1080x32",
     "1024x768x32", 0, 0),
    ("edid_dtd_mm0_cm_ok", make_edid(1920, 1080, cm=(53, 30)), VBE + ",1920x1080x32",
     "1024x768x32", 0, 0),
    ("keep_current_distorted", L14, "640x480x32,800x600x32,1024x768x32", "1024x768x32", 0, 0),
    ("closest_aspect_no_current", L14, "1280x1024x32,1024x768x32", "-", 0, 0),
    ("largest_no_info", None, "1600x1200x8,800x600x32,1280x1024x16,1280x720x24,1024x768x32",
     "-", 0, 0),
    ("no_usable_modes", L14, "1920x1080x8,1280x1024x16", "1280x1024x16", 0, 0),
    ("override_4_cramped", make_edid(1366, 768, mm=(344, 194)), "1366x768x32", "1366x768x32", 4, 0),
    ("override_1_on_4k", make_edid(3840, 2160, mm=(344, 194)), "3840x2160x32", "3840x2160x32", 1, 0),
    ("refresh_bpp_tiebreak", L14, "1920x1080x24@60,1920x1080x32@75,1920x1080x32@60,1024x768x32",
     "1024x768x32", 0, 0),
    ("keep_current_no_edid", None, "800x600x32,1024x768x32", "1920x1080x32", 0, 0),
    ("interlaced_1080i", make_edid(1920, 1080, interlaced=True, mm=(1018, 573)),
     VBE + ",1920x1080x32", "1024x768x32", 0, 0),
    ("embedded7_800x480", make_edid(800, 480, mm=(154, 86), dtd_slot=2, name="PANEL7"),
     "640x480x32,800x480x32", "640x480x32", 0, 0),
    ("dpi_hint_invalid", None, "1920x1080x32", "1920x1080x32", 0, 5),
    ("no_dtd_cm_only", make_edid(0, 0, cm=(34, 19)), VBE, "1280x720x32", 0, 0),
]


def fmt_wh(t):
    return "-" if not t else "%dx%d" % (t[0], t[1])


def render():
    lines = [
        "# tests/data/display_vectors.txt — SINH TỰ ĐỘNG bởi scripts/gen_display_vectors.py, đừng sửa tay.",
        "# Dùng chung cho tests/test_display.cpp (C++) và kernel-cap/src/display.rs (Rust).",
        "# tên|edid_hex|modes|current|override|dpi_hint|"
        "valid|native|mm|index|reason|distorted|k|dpi_x10|mode|cols|rows",
    ]
    names = set()
    for name, edid, ms, cur, ov, hint in CASES:
        assert name not in names, name
        names.add(name)
        modes = modes_of(ms)
        curm = modes_of(cur)[0] if cur != "-" else None
        ed = parse_edid(edid)
        i, m, reason, dist = pick(modes, ed if ed["valid"] else None, curm)
        sm = m if m is not None else curm
        k, dpi, _ = scale(sm[0], sm[1], ed["mm"] if ed["valid"] else (0, 0), hint, ov)
        cols, rows = sm[0] // (6 * k), sm[1] // (12 * k)
        lines.append("|".join([
            name, edid.hex() if edid is not None else "-", ms, cur, str(ov), str(hint),
            "1" if ed["valid"] else "0", fmt_wh(ed["native"]), fmt_wh(ed["mm"]) if ed["valid"] else "0x0",
            str(i), reason, "1" if dist else "0", str(k), str(dpi), fmt_wh(sm), str(cols), str(rows)]))
    return "\n".join(lines) + "\n"


def main():
    text = render()
    if "--check" in sys.argv:
        cur = open(OUT, encoding="utf-8").read() if os.path.exists(OUT) else ""
        if cur != text:
            print("display_vectors.txt lỗi thời: chạy scripts/gen_display_vectors.py", file=sys.stderr)
            return 1
        print("display_vectors.txt khớp (%d ca)" % len(CASES))
        return 0
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "w", encoding="utf-8") as f:
        f.write(text)
    print("đã ghi %s (%d ca)" % (os.path.relpath(OUT), len(CASES)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
