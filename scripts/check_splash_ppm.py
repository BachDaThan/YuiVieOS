#!/usr/bin/env python3
"""Kiểm ảnh chụp màn hình chào của nhân (P2.16) — dùng bởi scripts/qemu-splash.sh.

    python3 scripts/check_splash_ppm.py splash.ppm W H serial.log [BPP]
        [--source=multiboot2|devicetree] [--raw=FORMAT:STRIDE] [--save-ppm=out.ppm]

--raw: tệp ảnh là bản sao THÔ vùng nhớ khung hình (QEMU `pmemsave`, đường
device tree của aarch64/riscv64) theo định dạng simple-framebuffer
(a8r8g8b8, x8r8g8b8, a8b8g8r8, x8b8g8r8, r8g8b8) và số byte mỗi hàng; được đổi
sang RGB trước khi kiểm (và lưu thành PPM nếu có --save-ppm).

Không so với một "ảnh vàng" (bảng thông số đổi theo máy), mà kiểm các bất biến
của bố cục trong kernel-frame/src/splash.rs:
  * đúng kích thước W×H; nhân báo đúng WxHxBPP (mặc định 32), đúng nguồn, và k đúng
    như quy tắc của yui_cap::display::choose_scale (DPI đoán theo độ phân giải);
  * dải đỏ trên cùng cao đúng h/30 + 2, nền đúng màu;
  * tiêu đề "YUIVIEOS BETA" TRÙNG TỪNG ĐIỂM ẢNH với bản dựng lại từ font 5×7
    (đọc thẳng từ splash.rs) phóng khối s×s — tức là phóng nguyên, không nội
    suy, căn giữa đúng;
  * bảng thông số (nền + vạch đỏ) và dòng chân trang có mặt.
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
BG = (0x0B, 0x10, 0x20)
INK = (0xF5, 0xF7, 0xFF)
ACCENT = (0xDA, 0x25, 0x1D)
SOFT = (0x8A, 0x94, 0xB8)
PANEL = (0x15, 0x1B, 0x33)
TITLE = "YUIVIEOS BETA"


def load_font():
    src = (ROOT / "kernel-frame/src/splash.rs").read_text(encoding="utf-8")
    font = {}
    names = {"SPACE": " ", "DASH": "-", "DOT": ".", "EQ": "=", "QUESTION": "?"}
    for name, body in re.findall(r"const FONT_(\w+): \[u8; 7\] = \[([^\]]*)\];", src):
        font[names.get(name, name)] = [int(x, 16) for x in body.split(",")]
    return font


def read_ppm(path):
    data = pathlib.Path(path).read_bytes()
    parts, pos = [], 0
    while len(parts) < 4:
        while data[pos:pos + 1].isspace():
            pos += 1
        if data[pos:pos + 1] == b"#":
            while data[pos:pos + 1] != b"\n":
                pos += 1
            continue
        start = pos
        while not data[pos:pos + 1].isspace():
            pos += 1
        parts.append(data[start:pos])
    if parts[0] != b"P6" or parts[3] != b"255":
        raise ValueError("không phải PPM P6 8 bit")
    w, h = int(parts[1]), int(parts[2])
    return w, h, data[pos + 1:pos + 1 + w * h * 3]


RAW_ORDER = {  # vị trí byte R, G, B trong một điểm ảnh (little-endian), số byte/điểm
    "a8r8g8b8": (2, 1, 0, 4), "x8r8g8b8": (2, 1, 0, 4),
    "a8b8g8r8": (0, 1, 2, 4), "x8b8g8r8": (0, 1, 2, 4),
    "r8g8b8": (2, 1, 0, 3),
}


def read_raw(path, w, h, fmt, stride):
    ri, gi, bi, bpp = RAW_ORDER[fmt]
    data = pathlib.Path(path).read_bytes()
    if len(data) < stride * (h - 1) + w * bpp:
        raise ValueError("bản sao thô chỉ %d byte" % len(data))
    px = bytearray(w * h * 3)
    for y in range(h):
        row = data[y * stride:y * stride + w * bpp]
        o = y * w * 3
        px[o + 0:o + w * 3:3] = row[ri::bpp]
        px[o + 1:o + w * 3:3] = row[gi::bpp]
        px[o + 2:o + w * 3:3] = row[bi::bpp]
    return w, h, bytes(px)


def expected_k(w, h):
    dpi_x10 = 1920 if h >= 2160 else 960  # không có EDID ⇒ đoán
    k_dpi = max(1, min(4, (dpi_x10 + 480) // 960))
    k_fit = max(1, min(4, min(w // 480, h // 300)))
    return min(k_dpi, k_fit)


def text_width(text, scale):
    return len(text) * 6 * scale - scale if text else 0


def main():
    opts = dict(a.split("=", 1) for a in sys.argv[1:] if a.startswith("--") and "=" in a)
    pos = [a for a in sys.argv[1:] if not a.startswith("--")]
    if len(pos) not in (4, 5) or set(opts) - {"--source", "--raw", "--save-ppm"}:
        print(__doc__)
        return 2
    path, W, H, log_path = pos[0], int(pos[1]), int(pos[2]), pos[3]
    bpp = int(pos[4]) if len(pos) == 5 else 32
    source = opts.get("--source", "multiboot2")
    errors = []
    tag = "%dx%dx%d" % (W, H, bpp)

    def bad(msg):
        errors.append(msg)

    try:
        if "--raw" in opts:
            fmt, stride = opts["--raw"].split(":")
            w, h, px = read_raw(path, W, H, fmt, int(stride))
            if "--save-ppm" in opts:
                pathlib.Path(opts["--save-ppm"]).write_bytes(b"P6\n%d %d\n255\n" % (w, h) + px)
        else:
            w, h, px = read_ppm(path)
    except (OSError, ValueError, IndexError, KeyError) as e:
        print("✗ %s: không đọc được ảnh chụp %s: %s" % (tag, path, e), file=sys.stderr)
        return 1
    if (w, h) != (W, H) or len(px) != w * h * 3:
        print("✗ %s: ảnh chụp %dx%d (%d byte), mong đợi %s" % (tag, w, h, len(px), tag),
              file=sys.stderr)
        return 1

    def at(x, y):
        i = (y * w + x) * 3
        return tuple(px[i:i + 3])

    k = expected_k(w, h)
    log = pathlib.Path(log_path).read_text(encoding="utf-8", errors="replace")
    m = re.search(r"Man hinh : ve xong (\d+) pixel \((\d+)x(\d+)x(\d+), k=(\d+), nguon (\w+)\)", log)
    if not m:
        bad("log serial thiếu dòng 'Man hinh : ve xong … k=…'")
    else:
        got = (int(m.group(2)), int(m.group(3)), int(m.group(4)), int(m.group(5)), m.group(6))
        if got != (w, h, bpp, k, source):
            bad("nhân báo %s, mong đợi %dx%dx%d k=%d nguồn %s" % (got, w, h, bpp, k, source))

    # Dải đỏ + nền.
    band = h // 30 + 2
    row0 = sum(1 for x in range(w) if at(x, 0) == ACCENT)
    if row0 < w * 95 // 100:
        bad("dải đỏ: chỉ %d/%d điểm ở hàng 0 đúng màu" % (row0, w))
    if at(1, band - 1) != ACCENT or at(1, band) != BG:
        bad("dải đỏ cao sai (mong đợi %d hàng): %s / %s" % (band, at(1, band - 1), at(1, band)))
    if at(1, h // 2) != BG:
        bad("nền sai màu ở (1, %d): %s" % (h // 2, at(1, h // 2)))

    # Tiêu đề: dựng lại từng điểm ảnh từ font và so.
    font = load_font()
    ts = (4 if w >= 640 else (3 if w >= 400 else 2)) * k
    while ts > 1 and text_width(TITLE, ts) > w - 16:
        ts -= 1
    tw = text_width(TITLE, ts)
    x0, y0 = max(0, (w - tw) // 2), h // 3
    mismatch = 0
    first = None
    for gy in range(7 * ts):
        for gx in range(tw):
            ci, col = divmod(gx // ts, 6)
            row = gy // ts
            on = col < 5 and bool(font[TITLE[ci]][row] & (1 << (4 - col)))
            want = INK if on else BG
            got = at(x0 + gx, y0 + gy)
            if got != want:
                mismatch += 1
                first = first or (x0 + gx, y0 + gy, got, want)
    if mismatch:
        bad("tiêu đề lệch %d điểm ảnh so với font 5x7 phóng %dx%d (đầu tiên: %s)"
            % (mismatch, ts, ts, first))
    # Không có mực ngoài khung tiêu đề trên cùng các hàng (căn giữa, không tràn).
    stray = sum(1 for y in range(y0, y0 + 7 * ts) for x in list(range(0, x0)) + list(range(x0 + tw, w))
                if at(x, y) == INK)
    if stray:
        bad("%d điểm mực nằm ngoài khung tiêu đề" % stray)

    # Bảng thông số: cột ngay sau vạch đỏ phải có một đoạn màu PANEL đủ cao.
    px_ = w // 20
    col = [at(px_ + 4 * k + 1, y) for y in range(h)]
    run = best = 0
    for c in col:
        run = run + 1 if c == PANEL else 0
        best = max(best, run)
    if best < 2 * 7 + 6 * k + 16 * k:
        bad("bảng thông số: đoạn nền PANEL dài nhất chỉ %d hàng" % best)
    if sum(1 for y in range(h // 2, h) if at(px_, y) == ACCENT) < best:
        bad("bảng thông số: thiếu vạch đỏ bên trái")
    # Dòng chân trang (chữ SOFT, cỡ k) ở đáy màn hình.
    fy = h - 7 * k - 8 * k
    soft = sum(1 for y in range(fy, fy + 7 * k) for x in range(w) if at(x, y) == SOFT)
    if soft < 50 * k * k:
        bad("dòng chân trang: chỉ %d điểm chữ" % soft)

    if errors:
        for e in errors:
            print("✗ %s: %s" % (tag, e), file=sys.stderr)
        return 1
    print("✓ %s: splash đúng (k=%d, tiêu đề %dx%d khối, %d điểm ảnh khớp font, bảng %d hàng)"
          % (tag, k, ts, ts, 7 * ts * tw, best))
    return 0


if __name__ == "__main__":
    sys.exit(main())
