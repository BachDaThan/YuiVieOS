#!/usr/bin/env python3
"""Kiểm nhãn "AS-IS" và tài liệu pháp lý mức Lite (P7.9; một phần P7.13).

Luật (đổi thì đổi cả test `scripts/test_check_legal.py`):

  1. README.md — khối AS-IS nằm trong 12 dòng đầu, đủ các ý: dự án cá nhân,
     nguyên trạng (AS-IS), tự chịu trách nhiệm, không dùng cho quyết định
     y tế/pháp lý/tài chính, trỏ tới docs/terms_vi.md và docs/privacy_vi.md,
     có câu tiếng Anh "AS-IS".
  2. docs/terms_vi.md và docs/privacy_vi.md — dòng "chưa được luật sư rà soát"
     trong 6 dòng đầu **cho tới khi được rà** (khi rà xong: thay bằng dòng
     "Đã được luật sư rà soát: <ai>, <ngày>" — luật này chấp nhận cả hai).
  3. docs/terms_vi.md — đủ các mục bắt buộc (danh sách cố định TERMS_SECTIONS,
     phần kiểm tài liệu của P7.13) và có cụm "phạm vi pháp luật cho phép".
     Không ở đâu (README, terms, privacy) được hứa miễn trừ **tuyệt đối**
     ("miễn mọi trách nhiệm" …) mà không kèm cụm đó trên cùng đoạn — câu tuyệt
     đối có thể làm cả điều khoản mất hiệu lực (sổ pháp lý câu F15).
  4. include/yui/legal.hpp — câu chuẩn `kAsIsVi`/`kAsIsEn` đủ ý như README.
  5. Nơi hiển thị thật dùng câu chuẩn: shell (`banner`, `phiênbản`) gọi
     `legal::kAsIsVi`; nhân in bản ASCII "(AS-IS)" ở banner khởi động.
  6. LICENSE (MIT) còn đoạn từ chối bảo hành 'PROVIDED "AS IS"'.

  python3 scripts/check_legal.py            # in lỗi, mã thoát 1 nếu có lỗi
"""
from __future__ import annotations

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent

README_PHRASES = [
    "mã nguồn mở cá nhân",
    "nguyên trạng (AS-IS)",
    "tự chịu trách nhiệm",
    "y tế, pháp lý, tài chính",
    "docs/terms_vi.md",
    "docs/privacy_vi.md",
    "AS-IS; use at your own risk",
]
NOT_REVIEWED = "chưa được luật sư rà soát"
REVIEWED_RE = re.compile(r"Đã được luật sư rà soát: \S.*\d{4}-\d{2}-\d{2}")
TERMS_SECTIONS = [
    "Bản chất dự án",
    "Nguyên trạng, không bảo hành, tự chịu rủi ro",
    "Ghi đĩa, cài song song, Live USB",
    "Phần cứng, driver, firmware",
    "Thành phần và gói bên thứ ba",
    "Giới hạn trách nhiệm",
    "Luật áp dụng, phần không loại trừ được, tách điều khoản",
    "Thay đổi điều khoản",
]
LIMIT_PHRASE = "phạm vi pháp luật cho phép"
ABSOLUTE_RE = re.compile(r"miễn (?:trừ )?(?:mọi|toàn bộ|tất cả) trách nhiệm", re.IGNORECASE)
LABEL_VI_PHRASES = ["Bản beta miễn phí", "nguyên trạng (AS-IS)", "không bảo hành",
                    "tự chịu trách nhiệm", "docs/terms_vi.md", "docs/privacy_vi.md"]
LABEL_EN_PHRASES = ["AS-IS", "without warranty", "own risk"]
KERNEL_LABEL = "nguyen trang (AS-IS), khong bao hanh"


def _read(root: pathlib.Path, rel: str, errs: list[str]) -> str | None:
    p = root / rel
    if not p.is_file():
        errs.append(f"{rel}: không tồn tại")
        return None
    return p.read_text(encoding="utf-8")


def _cpp_string(src: str, name: str) -> str | None:
    """Ghép chuỗi C++ `name = "a" "b";` (nối literal liền nhau)."""
    m = re.search(name + r"\s*=\s*((?:\s*\"(?:[^\"\\]|\\.)*\")+)\s*;", src)
    if not m:
        return None
    return "".join(re.findall(r"\"((?:[^\"\\]|\\.)*)\"", m.group(1)))


def check_absolute(text: str, rel: str) -> list[str]:
    errs = []
    text = text.replace("*", "")  # bỏ nhấn mạnh Markdown: "miễn **mọi** trách nhiệm"
    for para in re.split(r"\n\s*\n", text):
        for m in ABSOLUTE_RE.finditer(para):
            if LIMIT_PHRASE not in para:
                line = text[: text.find(para)].count("\n") + para[: m.start()].count("\n") + 1
                errs.append(f"{rel}:{line}: hứa \"{m.group(0)}\" tuyệt đối — phải kèm "
                            f"\"{LIMIT_PHRASE}\" trong cùng đoạn (F15)")
    return errs


def check(root: pathlib.Path = ROOT) -> list[str]:
    errs: list[str] = []

    readme = _read(root, "README.md", errs)
    if readme is not None:
        head = "\n".join(readme.splitlines()[:12])
        for ph in README_PHRASES:
            if ph not in head:
                errs.append(f"README.md: khối AS-IS (12 dòng đầu) thiếu \"{ph}\"")
        errs += check_absolute(readme, "README.md")

    for rel in ("docs/terms_vi.md", "docs/privacy_vi.md"):
        text = _read(root, rel, errs)
        if text is None:
            continue
        head = "\n".join(text.splitlines()[:6])
        if NOT_REVIEWED not in head and not REVIEWED_RE.search(head):
            errs.append(f"{rel}: 6 dòng đầu thiếu \"{NOT_REVIEWED}\" (hoặc dòng "
                        "\"Đã được luật sư rà soát: <ai>, <YYYY-MM-DD>\")")
        errs += check_absolute(text, rel)
        if rel == "docs/terms_vi.md":
            headings = [h.strip() for h in re.findall(r"^##\s+(?:\d+\.\s*)?(.+)$", text, re.M)]
            for sec in TERMS_SECTIONS:
                if not any(h.startswith(sec) for h in headings):
                    errs.append(f"{rel}: thiếu mục bắt buộc \"{sec}\"")
            if LIMIT_PHRASE not in text:
                errs.append(f"{rel}: thiếu cụm \"{LIMIT_PHRASE}\"")

    legal = _read(root, "include/yui/legal.hpp", errs)
    if legal is not None:
        vi = _cpp_string(legal, "kAsIsVi")
        en = _cpp_string(legal, "kAsIsEn")
        if vi is None or en is None:
            errs.append("include/yui/legal.hpp: không tìm thấy kAsIsVi/kAsIsEn")
        else:
            errs += [f"include/yui/legal.hpp: kAsIsVi thiếu \"{p}\"" for p in LABEL_VI_PHRASES if p not in vi]
            errs += [f"include/yui/legal.hpp: kAsIsEn thiếu \"{p}\"" for p in LABEL_EN_PHRASES if p not in en]

    shell = _read(root, "userspace-vi/shell.cpp", errs)
    if shell is not None:
        for fn, pat in (("banner", r"Shell::banner\(\)[^{]*\{(.*?)\n\}"),
                        ("phiênbản", r"cmd == \"phiênbản\"\)\s*\{(.*?)\n  \}")):
            m = re.search(pat, shell, re.S)
            if not m or "legal::kAsIsVi" not in m.group(1):
                errs.append(f"userspace-vi/shell.cpp: {fn} không in legal::kAsIsVi")

    kmain = _read(root, "kernel-frame/src/main.rs", errs)
    if kmain is not None and KERNEL_LABEL not in kmain:
        errs.append(f"kernel-frame/src/main.rs: banner khởi động thiếu \"{KERNEL_LABEL}\"")

    lic = _read(root, "LICENSE", errs)
    if lic is not None and 'PROVIDED "AS IS"' not in lic:
        errs.append("LICENSE: thiếu đoạn từ chối bảo hành 'PROVIDED \"AS IS\"'")
    return errs


def main() -> int:
    errs = check()
    if errs:
        for e in errs:
            print("FAIL " + e)
        return 1
    print(f"nhãn AS-IS + tài liệu pháp lý Lite đạt ({len(README_PHRASES)} ý README, "
          f"{len(TERMS_SECTIONS)} mục điều khoản, shell + nhân dùng câu chuẩn)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
