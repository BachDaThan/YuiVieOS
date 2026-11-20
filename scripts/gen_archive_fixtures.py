#!/usr/bin/env python3
"""Sinh tests/archive_fixtures.hpp — mẫu nén THẬT từ zlib/gzip/zipfile/tarfile
cho test_archive (P4.33). Bộ giải DEFLATE tự viết phải đọc được đầu ra của
zlib (khối Huffman động), không chỉ đầu ra của bộ nén tối giản trong repo.

Chạy lại: python3 scripts/gen_archive_fixtures.py > tests/archive_fixtures.hpp
Kết quả tất định (mtime cố định)."""
import gzip, io, tarfile, zipfile, zlib


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


def tgz_real(t):
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
    raw = b.getvalue()
    return gz(raw)


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
    ]
    print("// SINH TỰ ĐỘNG bởi scripts/gen_archive_fixtures.py — đừng sửa tay.")
    print("// Mẫu nén thật (zlib %s) cho tests/test_archive.cpp (P4.33)." % zlib.ZLIB_VERSION)
    print("#ifndef YUI_ARCHIVE_FIXTURES_HPP\n#define YUI_ARCHIVE_FIXTURES_HPP\n")
    print("#include <cstdint>\n#include <vector>\n\nnamespace fx {\n")
    for n, d in parts:
        print(emit(n, d))
        print()
    print("}  // namespace fx\n\n#endif  // YUI_ARCHIVE_FIXTURES_HPP")


if __name__ == "__main__":
    main()
