#!/usr/bin/env python3
"""Sinh tests/data/fdt_fb_vectors.txt cho P2.16 (c): framebuffer từ Device Tree.

Nút `simple-framebuffer` (binding Linux
Documentation/devicetree/bindings/display/simple-framebuffer.yaml) là đường thứ
ba để có màn hình khi không có driver GPU: firmware (U-Boot, bootloader của
board ARM/RISC-V) đã dựng sẵn khung hình và ghi địa chỉ + kích thước + định
dạng vào cây thiết bị.

Tệp này là BẢN THỨ BA của cùng quy tắc (sau `userspace-vi/display.cpp` —
`parse_fdt_framebuffer` — và `kernel-cap/src/fdt.rs`), viết lại độc lập bằng
Python. Mỗi DTB được dựng từ đầu theo Devicetree Specification v0.4 §5 (header
40 byte big-endian, khối cấu trúc FDT_BEGIN_NODE/END_NODE/PROP/NOP/END, khối
chuỗi); các ca hỏng được cắt/vá có chủ đích. Mỗi ca ghi sẵn kết quả mong đợi
bằng tay, và bộ đọc Python bên dưới phải ra đúng kết quả đó — tức là bản tham
chiếu cũng bị kiểm.

Quy tắc (không đổi nếu không đổi cả ba bản + vector):
  1. DTB hỏng ở BẤT KỲ đâu (header, offset, token lạ, chuỗi không kết thúc,
     thuộc tính sau nút con, lồng > 32 mức, thiếu FDT_END) ⇒ `bad_blob`, không
     tin bất kỳ nút nào trong đó.
  2. Ứng viên = nút có `compatible` chứa đúng chuỗi "simple-framebuffer".
     Xét theo thứ tự đóng nút (FDT_END_NODE). Ứng viên hợp lệ đầu tiên thắng.
  3. Mỗi ứng viên: `status` (vắng, "okay", "ok" = bật; khác ⇒ `disabled`) →
     `format` (a8r8g8b8/x8r8g8b8 = 32 bit 16/8/0, a8b8g8r8/x8b8g8r8 = 32 bit
     0/8/16, r8g8b8 = 24 bit 16/8/0; khác hoặc vắng ⇒ `bad_format`) →
     `width`/`height`/`stride` đúng 4 byte, 1..16384, stride ≥ width×bpp/8,
     stride ≤ 262144 (⇒ `bad_geometry`) → `reg` theo #address-cells /
     #size-cells của nút CHA (vắng ⇒ 2/1 theo đặc tả), mỗi loại 1 hoặc 2 ô,
     độ dài là bội số của một mục, địa chỉ ≠ 0, size ≥ stride×height,
     addr + size ≤ 2⁶⁴ − 1 (⇒ `bad_reg`). Chỉ mục reg đầu tiên được dùng.
  4. Không có ứng viên hợp lệ ⇒ lý do của ứng viên hỏng ĐẦU TIÊN, hoặc
     `no_node` nếu không có ứng viên nào.

    python3 scripts/gen_fdt_vectors.py          # ghi lại tệp vector
    python3 scripts/gen_fdt_vectors.py --check  # chỉ so, khác ⇒ exit 1
    python3 scripts/gen_fdt_vectors.py --dtc    # thêm: mọi DTB lành phải qua `dtc -I dtb`
"""
import os
import shutil
import struct
import subprocess
import sys
import tempfile

OUT = os.path.join(os.path.dirname(__file__), "..", "tests", "data", "fdt_fb_vectors.txt")

FDT_MAGIC = 0xD00DFEED
BEGIN, END_NODE, PROP, NOP, END = 1, 2, 3, 4, 9

# ------------------------------------------------------------ dựng DTB ------


def node(name, props=(), children=()):
    return ("node", name, list(props), list(children))


def u32(*vals):
    return b"".join(struct.pack(">I", v & 0xFFFFFFFF) for v in vals)


def cells(value, n):
    """Giá trị `value` viết thành n ô 32 bit big-endian."""
    return b"".join(struct.pack(">I", (value >> (32 * (n - 1 - i))) & 0xFFFFFFFF) for i in range(n))


def s(text):
    return text.encode("ascii") + b"\0"


def slist(*texts):
    return b"".join(s(t) for t in texts)


def build(root, version=17, last_comp=16, nops_after_props=False):
    strings = bytearray()
    offsets = {}

    def stroff(name):
        if name not in offsets:
            offsets[name] = len(strings)
            strings.extend(name.encode("ascii") + b"\0")
        return offsets[name]

    st = bytearray()

    def pad():
        while len(st) % 4:
            st.append(0)

    def emit(n):
        _, name, props, children = n
        st.extend(u32(BEGIN))
        st.extend(name.encode("ascii") + b"\0")
        pad()
        for item in props:
            if item == "NOP":
                st.extend(u32(NOP))
                continue
            pname, value = item
            st.extend(u32(PROP, len(value), stroff(pname)))
            st.extend(value)
            pad()
        if nops_after_props:
            st.extend(u32(NOP))
        for c in children:
            if c == "NOP":
                st.extend(u32(NOP))
            elif c[0] == "prop":  # thuộc tính SAU nút con (ca hỏng có chủ đích)
                st.extend(u32(PROP, len(c[2]), stroff(c[1])))
                st.extend(c[2])
                pad()
            else:
                emit(c)
        st.extend(u32(END_NODE))

    emit(root)
    st.extend(u32(END))
    rsv = bytes(16)  # bảng memreserve: chỉ mục kết thúc (0, 0)
    off_rsv = 40
    off_struct = off_rsv + len(rsv)
    off_strings = off_struct + len(st)
    total = off_strings + len(strings)
    hdr = struct.pack(">10I", FDT_MAGIC, total, off_struct, off_strings, off_rsv,
                      version, last_comp, 0, len(strings), len(st))
    return bytearray(hdr + rsv + bytes(st) + bytes(strings))


def fb_node(name="framebuffer@48000000", addr=0x48000000, w=800, h=600, stride=None,
            fmt="a8r8g8b8", bpp=32, status=None, compatible=("simple-framebuffer",),
            ac=2, sc=2, size=None, extra=(), drop=()):
    stride = stride if stride is not None else w * bpp // 8
    size = size if size is not None else stride * h
    props = [("compatible", slist(*compatible))]
    if "reg" not in drop:
        props.append(("reg", cells(addr, ac) + cells(size, sc)))
    for key, val in (("width", w), ("height", h), ("stride", stride)):
        if key not in drop:
            props.append((key, u32(val)))
    if fmt is not None:
        props.append(("format", s(fmt)))
    if status is not None:
        props.append(("status", s(status)))
    props.extend(extra)
    return node(name, props)


def root(children, ac=2, sc=2, model="yui,synthetic-virt"):
    props = [("#address-cells", u32(ac)), ("#size-cells", u32(sc)),
             ("compatible", s("linux,dummy-virt")), ("model", s(model))]
    base = [node("chosen", [("bootargs", s(""))]),
            node("memory@40000000", [("device_type", s("memory")),
                                     ("reg", cells(0x40000000, ac) + cells(0x40000000, sc))])]
    return node("", props, base + list(children))


def set_u32(blob, off, val):
    blob[off:off + 4] = struct.pack(">I", val & 0xFFFFFFFF)
    return blob


def hdr(blob, idx):
    return struct.unpack_from(">I", blob, 4 * idx)[0]


# ------------------------------------------------- bản tham chiếu độc lập --

FORMATS = {
    "a8r8g8b8": (32, 16, 8, 0), "x8r8g8b8": (32, 16, 8, 0),
    "a8b8g8r8": (32, 0, 8, 16), "x8b8g8r8": (32, 0, 8, 16),
    "r8g8b8": (24, 16, 8, 0),
}
MAX_DEPTH = 32
MAX_DIM = 16384
MAX_STRIDE = 262144


class Bad(Exception):
    pass


def cstr(b):
    i = b.find(b"\0")
    return b if i < 0 else b[:i]


def parse(blob):
    """→ dict(reason, count, fb) — fb là dict hoặc None."""
    b = bytes(blob)
    try:
        return _parse(b)
    except Bad:
        return dict(reason="bad_blob", count=0, fb=None)


def _parse(b):
    n = len(b)
    if n < 40:
        raise Bad
    magic, total, off_st, off_str, _rsv, ver, last, _cpu, sz_str, sz_st = struct.unpack_from(">10I", b, 0)
    if magic != FDT_MAGIC or total < 40 or total > n or ver < 17 or last > 17:
        raise Bad
    if off_st % 4 or sz_st % 4 or off_st < 40 or off_st + sz_st > total or off_str + sz_str > total:
        raise Bad
    st = b[off_st:off_st + sz_st]
    strs = b[off_str:off_str + sz_str]

    def be(off):
        if off + 4 > len(st):
            raise Bad
        return struct.unpack_from(">I", st, off)[0]

    def name_at(off):
        if off >= len(strs):
            raise Bad
        j = strs.find(b"\0", off)
        if j < 0:
            raise Bad
        return strs[off:j]

    stack = []   # mỗi mức: dict(props={}, had_child=False)
    pos = 0
    done = False
    count = 0
    first_ok = None
    first_bad = None
    if be(0) != BEGIN:
        raise Bad
    while not done:
        tok = be(pos)
        pos += 4
        if tok == BEGIN:
            j = st.find(b"\0", pos)
            if j < 0:
                raise Bad
            if stack:
                stack[-1]["had_child"] = True
            stack.append(dict(props={}, had_child=False))
            if len(stack) > MAX_DEPTH:
                raise Bad
            pos = (j + 1 + 3) & ~3
        elif tok == END_NODE:
            if not stack:
                raise Bad
            me = stack.pop()
            if b"simple-framebuffer" in me["props"].get(b"compatible", b"").split(b"\0"):
                count += 1
                parent = stack[-1]["props"] if stack else {}
                fb, why = candidate(me["props"], parent)
                if fb is not None and first_ok is None:
                    first_ok = fb
                if fb is None and first_bad is None:
                    first_bad = why
            if not stack:
                # sau nút gốc chỉ còn NOP rồi END
                while True:
                    t = be(pos)
                    pos += 4
                    if t == NOP:
                        continue
                    if t != END:
                        raise Bad
                    done = True
                    break
        elif tok == PROP:
            if not stack or stack[-1]["had_child"]:
                raise Bad
            ln, nameoff = be(pos), be(pos + 4)
            pos += 8
            if ln > len(st) - pos:
                raise Bad
            stack[-1]["props"][name_at(nameoff)] = st[pos:pos + ln]
            pos = (pos + ln + 3) & ~3
        elif tok == NOP:
            pass
        else:
            raise Bad
    if first_ok is not None:
        return dict(reason="ok", count=count, fb=first_ok)
    return dict(reason=first_bad or "no_node", count=count, fb=None)


def candidate(p, parent):
    status = p.get(b"status")
    if status is not None and cstr(status) not in (b"okay", b"ok"):
        return None, "disabled"
    fmt = p.get(b"format")
    if fmt is None or cstr(fmt).decode("latin-1") not in FORMATS:
        return None, "bad_format"
    fmt = cstr(fmt).decode("ascii")
    bpp, rs, gs, bs = FORMATS[fmt]
    dims = []
    for key in (b"width", b"height", b"stride"):
        v = p.get(key)
        if v is None or len(v) != 4:
            return None, "bad_geometry"
        dims.append(struct.unpack(">I", v)[0])
    w, h, stride = dims
    if not (1 <= w <= MAX_DIM and 1 <= h <= MAX_DIM) or stride < w * (bpp // 8) or stride > MAX_STRIDE:
        return None, "bad_geometry"

    def cellcount(key, default):
        v = parent.get(key)
        if v is None:
            return default
        return struct.unpack(">I", v)[0] if len(v) == 4 else 0

    ac = cellcount(b"#address-cells", 2)
    sc = cellcount(b"#size-cells", 1)
    reg = p.get(b"reg")
    if ac not in (1, 2) or sc not in (1, 2) or reg is None:
        return None, "bad_reg"
    entry = 4 * (ac + sc)
    if len(reg) == 0 or len(reg) % entry:
        return None, "bad_reg"
    addr = int.from_bytes(reg[:4 * ac], "big")
    size = int.from_bytes(reg[4 * ac:entry], "big")
    if addr == 0 or size < stride * h or addr + size > (1 << 64) - 1:
        return None, "bad_reg"
    return dict(addr=addr, size=size, width=w, height=h, stride=stride, bpp=bpp,
                shifts=(rs, gs, bs), format=fmt), None


# ------------------------------------------------------------------ ca ----

def mk_cases():
    C = []

    def add(name, blob, want):
        C.append((name, bytes(blob), want))

    ok800 = ("ok", 0x48000000, 800 * 600 * 4, 800, 600, 3200, 32, (16, 8, 0), "a8r8g8b8")
    add("qemu_virt_800x600_argb", build(root([fb_node()])), ok800)
    add("bgr_1920x1080", build(root([fb_node(name="framebuffer@50000000", addr=0x50000000, w=1920, h=1080,
                                             fmt="x8b8g8r8")])),
        ("ok", 0x50000000, 1920 * 1080 * 4, 1920, 1080, 7680, 32, (0, 8, 16), "x8b8g8r8"))
    add("rgb24_1024x768", build(root([fb_node(w=1024, h=768, fmt="r8g8b8", bpp=24)])),
        ("ok", 0x48000000, 1024 * 768 * 3, 1024, 768, 3072, 24, (16, 8, 0), "r8g8b8"))
    add("uhd_above_4gib", build(root([fb_node(name="framebuffer@100000000", addr=0x1_0000_0000, w=3840,
                                              h=2160, fmt="x8r8g8b8")])),
        ("ok", 0x1_0000_0000, 3840 * 2160 * 4, 3840, 2160, 15360, 32, (16, 8, 0), "x8r8g8b8"))
    add("padded_stride_1366", build(root([fb_node(w=1366, h=768, stride=5504, fmt="x8r8g8b8")])),
        ("ok", 0x48000000, 5504 * 768, 1366, 768, 5504, 32, (16, 8, 0), "x8r8g8b8"))
    # Linux/U-Boot: nút đặt trong /chosen, thường "disabled" cho tới khi firmware bật.
    chosen_fb = fb_node(status="disabled", addr=0x47000000, name="framebuffer@47000000")
    r = root([fb_node(name="framebuffer@48000000")])
    r[3][0] = node("chosen", [("#address-cells", u32(2)), ("#size-cells", u32(2)), ("ranges", b"")],
                   [chosen_fb])
    add("chosen_disabled_then_root_ok", build(r), ok800)
    r = root([])
    r[3][0] = node("chosen", [("#address-cells", u32(2)), ("#size-cells", u32(2)), ("ranges", b"")],
                   [fb_node(status="okay")])
    add("chosen_enabled", build(r), ok800)
    add("only_disabled", build(root([fb_node(status="disabled")])), ("disabled",))
    add("status_fail", build(root([fb_node(status="fail")])), ("disabled",))
    add("status_ok_short", build(root([fb_node(status="ok")])), ok800)
    add("no_node", build(root([node("pl011@9000000", [("compatible", slist("arm,pl011", "arm,primecell"))])])),
        ("no_node",))
    add("compatible_prefix_only", build(root([fb_node(compatible=("simple-framebuffer-x",))])), ("no_node",))
    add("compatible_list_second", build(root([fb_node(compatible=("yui,fb", "simple-framebuffer"))])), ok800)
    add("format_r5g6b5", build(root([fb_node(fmt="r5g6b5", bpp=16)])), ("bad_format",))
    add("format_missing", build(root([fb_node(fmt=None)])), ("bad_format",))
    add("bad_format_then_ok", build(root([fb_node(fmt="r5g6b5", bpp=16, name="framebuffer@47000000"),
                                          fb_node()])), ok800)
    add("two_ok_first_wins", build(root([fb_node(), fb_node(name="framebuffer@50000000", addr=0x50000000)])),
        ok800)
    add("stride_too_large", build(root([fb_node(stride=MAX_STRIDE + 4)])), ("bad_geometry",))
    add("stride_max_ok", build(root([fb_node(w=16384, h=4, stride=MAX_STRIDE, fmt="a8b8g8r8")])),
        ("ok", 0x48000000, MAX_STRIDE * 4, 16384, 4, MAX_STRIDE, 32, (0, 8, 16), "a8b8g8r8"))
    add("stride_too_small", build(root([fb_node(stride=3196)])), ("bad_geometry",))
    add("width_zero", build(root([fb_node(w=0, stride=3200, size=3200 * 600)])), ("bad_geometry",))
    add("width_huge", build(root([fb_node(w=20000, stride=80000)])), ("bad_geometry",))
    add("height_missing", build(root([fb_node(drop=("height",))])), ("bad_geometry",))
    r = root([fb_node()])
    r[3][2][2][2] = ("width", u32(0, 800))  # width 8 byte
    add("width_8_bytes", build(r), ("bad_geometry",))
    add("reg_size_too_small", build(root([fb_node(size=800 * 600 * 4 - 1)])), ("bad_reg",))
    add("reg_wrong_length", build(root([fb_node(ac=2, sc=1)])), ("bad_reg",))
    add("reg_addr_zero", build(root([fb_node(addr=0)])), ("bad_reg",))
    add("reg_overflow", build(root([fb_node(addr=0xFFFF_FFFF_FFFF_F000)])), ("bad_reg",))
    add("reg_missing", build(root([fb_node(drop=("reg",))])), ("bad_reg",))
    soc = node("soc", [("#address-cells", u32(1)), ("#size-cells", u32(1)), ("ranges", b"")],
               [fb_node(name="framebuffer@3e000000", addr=0x3E000000, ac=1, sc=1, w=640, h=480,
                        fmt="x8r8g8b8")])
    add("soc_cells_1_1", build(root([soc])),
        ("ok", 0x3E000000, 640 * 480 * 4, 640, 480, 2560, 32, (16, 8, 0), "x8r8g8b8"))
    bus = node("bus", [("compatible", s("simple-bus"))],
               [fb_node(name="framebuffer@3e000000", addr=0x3E000000, ac=2, sc=1, w=640, h=480,
                        fmt="x8r8g8b8")])
    add("default_cells_2_1", build(root([bus])),
        ("ok", 0x3E000000, 640 * 480 * 4, 640, 480, 2560, 32, (16, 8, 0), "x8r8g8b8"))
    bus3 = node("bus", [("#address-cells", u32(3)), ("#size-cells", u32(1))],
                [node("framebuffer@0", [("compatible", s("simple-framebuffer")),
                                        ("reg", cells(0x48000000, 3) + cells(1920000, 1)),
                                        ("width", u32(800)), ("height", u32(600)), ("stride", u32(3200)),
                                        ("format", s("a8r8g8b8"))])])
    add("address_cells_3", build(root([bus3])), ("bad_reg",))
    add("nops_everywhere", build(root([fb_node(extra=("NOP",)), "NOP"]), nops_after_props=True), ok800)

    # ---- DTB hỏng: không tin nút nào ----
    good = build(root([fb_node()]))
    add("truncated_half", good[:len(good) // 2], ("bad_blob",))
    add("bad_magic", set_u32(bytearray(good), 0, 0xD00DFEEE), ("bad_blob",))
    add("totalsize_beyond_buffer", set_u32(bytearray(good), 4, len(good) + 4), ("bad_blob",))
    add("version_16", set_u32(bytearray(good), 20, 16), ("bad_blob",))
    b = bytearray(good)
    set_u32(b, 32, hdr(b, 8) + 64)  # size_dt_strings tràn ra ngoài totalsize
    add("strings_size_overflow", b, ("bad_blob",))
    b = bytearray(good)
    off_st = hdr(b, 2)
    # PROP đầu tiên của nút gốc (ngay sau BEGIN + tên rỗng đệm 4 byte): nameoff vượt khối chuỗi
    assert struct.unpack_from(">I", b, off_st + 8)[0] == PROP
    set_u32(b, off_st + 16, 0x7FFF)
    add("nameoff_out_of_range", b, ("bad_blob",))
    b = bytearray(good)
    set_u32(b, off_st + 8, 7)  # token lạ
    add("unknown_token", b, ("bad_blob",))
    b = bytearray(good)
    end_at = off_st + hdr(b, 9) - 4
    assert struct.unpack_from(">I", b, end_at)[0] == END
    set_u32(b, end_at, NOP)  # mất FDT_END
    add("missing_end", b, ("bad_blob",))
    r = root([fb_node()])
    r[3].append(("prop", "late", u32(1)))  # thuộc tính sau nút con
    add("prop_after_subnode", build(r), ("bad_blob",))
    deep = fb_node()
    for i in range(MAX_DEPTH):
        deep = node("n%d" % i, [("#address-cells", u32(2)), ("#size-cells", u32(2))], [deep])
    add("too_deep", build(root([deep])), ("bad_blob",))
    deep = fb_node()
    for i in range(MAX_DEPTH - 2):  # gốc + 30 mức + nút framebuffer = đúng 32
        deep = node("n%d" % i, [("#address-cells", u32(2)), ("#size-cells", u32(2))], [deep])
    add("depth_32_ok", build(root([deep])), ok800)
    deep = fb_node()
    for i in range(MAX_DEPTH - 1):  # gốc + 31 mức + nút framebuffer = 33 ⇒ quá sâu
        deep = node("n%d" % i, [("#address-cells", u32(2)), ("#size-cells", u32(2))], [deep])
    add("depth_33_bad", build(root([deep])), ("bad_blob",))
    add("trailing_bytes_ok", bytes(good) + b"\xde\xad\xbe\xef" * 4, ok800)
    b = bytearray(good)
    set_u32(b, 8, hdr(b, 2) + 2)  # off_dt_struct không căn 4
    add("struct_misaligned", b, ("bad_blob",))
    # Hai nút gốc: FDT_END_NODE của gốc rồi một BEGIN_NODE nữa.
    b = bytearray(good[:end_at] + u32(BEGIN) + bytes(4) + u32(END_NODE) + u32(END) + good[hdr(good, 3):])
    set_u32(b, 4, len(b))
    set_u32(b, 12, hdr(good, 3) + 12)   # off_dt_strings dời theo
    set_u32(b, 36, hdr(good, 9) + 12)   # size_dt_struct
    add("second_root", b, ("bad_blob",))
    return C


def fmt_result(r):
    if r["fb"] is None:
        return [r["reason"], str(r["count"])] + ["-"] * 8
    f = r["fb"]
    return [r["reason"], str(r["count"]), "%x" % f["addr"], "%x" % f["size"], str(f["width"]), str(f["height"]),
            str(f["stride"]), str(f["bpp"]), "%d,%d,%d" % f["shifts"], f["format"]]


def render():
    lines = [
        "# tests/data/fdt_fb_vectors.txt — SINH TỰ ĐỘNG bởi scripts/gen_fdt_vectors.py, đừng sửa tay.",
        "# Dùng chung cho tests/test_display.cpp (C++) và kernel-cap/src/fdt.rs (Rust).",
        "# tên|dtb_hex|reason|count|addr_hex|size_hex|width|height|stride|bpp|shifts_rgb|format",
    ]
    names = set()
    cases = mk_cases()
    for name, blob, want in cases:
        assert name not in names, name
        names.add(name)
        r = parse(blob)
        got = (r["reason"],) if r["fb"] is None else (
            r["reason"], r["fb"]["addr"], r["fb"]["size"], r["fb"]["width"], r["fb"]["height"],
            r["fb"]["stride"], r["fb"]["bpp"], r["fb"]["shifts"], r["fb"]["format"])
        assert got == want, "%s: bản tham chiếu ra %r, mong đợi %r" % (name, got, want)
        lines.append("|".join([name, blob.hex()] + fmt_result(r)))
    return "\n".join(lines) + "\n", cases


def dtc_check(cases):
    dtc = shutil.which("dtc")
    if not dtc:
        print("--dtc: không có dtc (gói device-tree-compiler)", file=sys.stderr)
        return 1
    bad = 0
    with tempfile.TemporaryDirectory() as d:
        for name, blob, want in cases:
            if want[0] == "bad_blob":
                continue
            p = os.path.join(d, name + ".dtb")
            with open(p, "wb") as f:
                f.write(blob)
            res = subprocess.run([dtc, "-q", "-I", "dtb", "-O", "dts", p], capture_output=True, text=True)
            if res.returncode != 0:
                print("✗ dtc từ chối DTB lành '%s': %s" % (name, res.stderr.strip()), file=sys.stderr)
                bad += 1
    if bad == 0:
        print("dtc đọc được mọi DTB lành (%d)" % sum(1 for c in cases if c[2][0] != "bad_blob"))
    return 1 if bad else 0


def main():
    text, cases = render()
    rc = 0
    if "--dtc" in sys.argv:
        rc = dtc_check(cases)
    if "--check" in sys.argv:
        cur = open(OUT, encoding="utf-8").read() if os.path.exists(OUT) else ""
        if cur != text:
            print("fdt_fb_vectors.txt lỗi thời: chạy scripts/gen_fdt_vectors.py", file=sys.stderr)
            return 1
        print("fdt_fb_vectors.txt khớp (%d ca)" % len(cases))
        return rc
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "w", encoding="utf-8") as f:
        f.write(text)
    print("đã ghi %s (%d ca)" % (os.path.relpath(OUT), len(cases)))
    return rc


if __name__ == "__main__":
    sys.exit(main())
