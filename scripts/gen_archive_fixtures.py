#!/usr/bin/env python3
"""Sinh tests/archive_fixtures.hpp — mẫu nén THẬT từ zlib/gzip/zipfile/tarfile
cho test_archive (P4.33). Bộ giải DEFLATE tự viết phải đọc được đầu ra của
zlib (khối Huffman động), không chỉ đầu ra của bộ nén tối giản trong repo.

Chạy lại: python3 scripts/gen_archive_fixtures.py > tests/archive_fixtures.hpp
Kết quả tất định (mtime cố định). Phần zstd cần gói `zstandard` (PyPI; mình
dùng 0.25.0 = libzstd 1.5.7): `python3 -m venv v && v/bin/pip install zstandard`.

Vector "golden" của zstd chép nguyên từ github.com/facebook/zstd
(tests/golden-decompression*, commit 1658a42; zstd cấp phép kép BSD-3/GPLv2) —
là bộ vector chính thức gần nhất với RFC 8878 (RFC không kèm vector)."""
import gzip, io, tarfile, zipfile, zlib

import zstandard

GOLDEN_OK = {  # tên → (hex, nội dung giải ra mong đợi: mô tả cho C++)
    "fx_zg_empty_block": "28b52ffd00001500000000",
    "fx_zg_rle_first_block": "28b52ffda4000010000200100002001000020010000200100002001000020010000200100003001000f13e16e1",
    "fx_zg_zeroseq_2b": "28b52ffd00008500006848656c6c6f20576f726c64210a8000",
}
GOLDEN_ERR = {
    "fx_zg_err_off0": "28b52ffd0000450000080002002f430bae",
    "fx_zg_err_truncated_huff_state": "28b52ffd000055000072800104207e1f02aa00",
    "fx_zg_err_zeroseq_extraneous": "28b52ffd00009500006848656c6c6f20576f726c64210a80000000",
}


def text2(n):
    # Giống hệt fixture_text2() trong tests/test_archive.cpp (LCG 31 bit).
    words = "một hai ba bốn năm sáu bảy tám chín mười Yui Việt Nam gõ dấu nhanh".split()
    x, out = 2026, []
    size = 0
    while size < n:
        x = (x * 1103515245 + 12345) & 0x7FFFFFFF
        w = (words[(x >> 16) % len(words)] + ("\n" if (x >> 8) % 11 == 0 else " ")).encode()
        out.append(w)
        size += len(w)
    return b"".join(out)[:n]


def zst_blocks(data, level, chunk):
    o = zstandard.ZstdCompressor(level=level, write_checksum=True).compressobj()
    out = b""
    for i in range(0, len(data), chunk):
        out += o.compress(data[i:i + chunk]) + o.flush(zstandard.COMPRESSOBJ_FLUSH_BLOCK)
    return out + o.flush()


def zst(data, level, content_size=True, checksum=True):
    c = zstandard.ZstdCompressor(level=level, write_checksum=checksum, write_content_size=content_size)
    if content_size:
        return c.compress(data)
    o = c.compressobj()
    return o.compress(data) + o.flush()


def text():
    # Giống hệt fixture_text() trong tests/test_archive.cpp.
    return "".join("Dòng %d: YuiVieOS giải nén an toàn — %d\n" % (i, i * i % 97) for i in range(400)).encode()


def raw_deflate(data, level=9):
    c = zlib.compressobj(level, zlib.DEFLATED, -15)
    return c.compress(data) + c.flush()


def gz(data, name=None):
    b = io.BytesIO()
    with gzip.GzipFile(filename=name or "", mode="wb", fileobj=b, mtime=0) as f:
        f.write(data)
    return b.getvalue()


def zip_real(t):
    b = io.BytesIO()
    with zipfile.ZipFile(b, "w") as z:
        d = zipfile.ZipInfo("thu-muc/", (2020, 1, 1, 0, 0, 0))
        d.external_attr = (0o40755 << 16) | 0x10
        z.writestr(d, b"")
        a = zipfile.ZipInfo("thu-muc/a.txt", (2020, 1, 1, 0, 0, 0))
        a.compress_type = zipfile.ZIP_DEFLATED
        a.external_attr = 0o104755 << 16  # setuid ⇒ phải bị bỏ
        z.writestr(a, t)
        s = zipfile.ZipInfo("b.bin", (2020, 1, 1, 0, 0, 0))
        s.compress_type = zipfile.ZIP_STORED
        s.external_attr = 0o100644 << 16
        z.writestr(s, bytes(range(256)))
        v = zipfile.ZipInfo("tiếng việt.txt", (2020, 1, 1, 0, 0, 0))
        v.compress_type = zipfile.ZIP_DEFLATED
        z.writestr(v, "Xin chào Việt Nam\n".encode())
    return b.getvalue()


def tar_real(t):
    b = io.BytesIO()
    with tarfile.open(fileobj=b, mode="w", format=tarfile.PAX_FORMAT) as tf:
        def add(name, data=None, mode=0o644, kind=tarfile.REGTYPE):
            i = tarfile.TarInfo(name)
            i.mtime = 0
            i.mode = mode
            i.type = kind
            i.uname = i.gname = ""
            if data is not None:
                i.size = len(data)
                tf.addfile(i, io.BytesIO(data))
            else:
                tf.addfile(i)
        add("goi", mode=0o755, kind=tarfile.DIRTYPE)
        add("goi/a.txt", t, mode=0o4755)
        add("goi/" + "ten-rat-dai-" * 12 + ".txt", b"pax\n")  # > 100 byte ⇒ header pax
        add("goi/rong.txt", b"")
    return b.getvalue()


def tgz_real(t):
    return gz(tar_real(t))


def emit(name, data):
    out = ["inline const std::vector<uint8_t>& %s() {" % name,
           "  static const std::vector<uint8_t> v = {"]
    for i in range(0, len(data), 20):
        out.append("      " + ",".join("0x%02x" % x for x in data[i:i + 20]) + ",")
    out.append("  };")
    out.append("  return v;")
    out.append("}")
    return "\n".join(out)


def main():
    t = text()
    parts = [
        ("fx_raw_dynamic", raw_deflate(t)),
        ("fx_gz_named", gz(t, "vanban.txt")),
        ("fx_gz_multi", gz(b"phan mot\n") + gz(b"phan hai\n")),
        ("fx_zip", zip_real(t)),
        ("fx_tgz", tgz_real(t)),
        ("fx_zst_l19", zst(t, 19)),
        ("fx_zst_fast", zst(t, -5)),
        # Nhiều khối nhỏ (flush từng khối) ⇒ zstd dùng lại cây Huffman
        # (treeless), lại bảng FSE (repeat), bảng mặc định và RLE.
        ("fx_zst_blocks_l9", zst_blocks(text2(60000), 9, 2048)),
        ("fx_zst_blocks_l1", zst_blocks(text2(20000), 1, 512)),
        ("fx_tzst", zst(tar_real(t), 9)),
    ]
    parts += [(k, bytes.fromhex(v)) for k, v in GOLDEN_OK.items()]
    parts += [(k, bytes.fromhex(v)) for k, v in GOLDEN_ERR.items()]
    print("// SINH TỰ ĐỘNG bởi scripts/gen_archive_fixtures.py — đừng sửa tay.")
    print("// Mẫu nén thật (zlib %s, libzstd %s) + vector golden của zstd cho tests/test_archive.cpp (P4.33)."
          % (zlib.ZLIB_VERSION, ".".join(map(str, zstandard.ZSTD_VERSION))))
    print("#ifndef YUI_ARCHIVE_FIXTURES_HPP\n#define YUI_ARCHIVE_FIXTURES_HPP\n")
    print("#include <cstdint>\n#include <vector>\n\nnamespace fx {\n")
    for n, d in parts:
        print(emit(n, d))
        print()
    print("}  // namespace fx\n\n#endif  // YUI_ARCHIVE_FIXTURES_HPP")


if __name__ == "__main__":
    main()
