#!/usr/bin/env python3
"""Máy kiểm ranh giới "không giả mạo, không né kiểm tra" (P7.10).

Quy tắc của dự án: YuiVieOS KHÔNG chứa và KHÔNG hỗ trợ giả IMEI/ID thiết bị/vị
trí, vượt Play Integrity/SafetyNet, giấu root để qua kiểm tra, né hay tắt chống
gian lận, can thiệp bộ nhớ game, và không quảng bá "treo/cày nhiều tài khoản".

Bước này quét mã, chính sách và tài liệu tìm một danh sách NGẮN từ khoá bị cấm
(mỗi từ có lý do ngay bên dưới). Tài liệu được phép nhắc tới chúng để CẢNH BÁO
hoặc TỪ CHỐI, nhưng mỗi dòng như vậy phải mang chú thích loại trừ có lý do:

    ranh-giới-ok: <lý do, ít nhất 10 ký tự>

Trong Markdown đặt trong chú thích HTML để không hiện ra khi đọc:
    <!-- ranh-giới-ok: cảnh báo, dự án không hỗ trợ -->
Trong mã: `// ranh-giới-ok: ...` hoặc `# ranh-giới-ok: ...`.
Một đoạn dài dùng cặp khối:
    <!-- ranh-giới-ok-bắt-đầu: <lý do> --> ... <!-- ranh-giới-ok-kết-thúc -->

Chú thích thiếu lý do bị coi là vi phạm. Đây là lưới thô bằng từ khoá — không
hiểu ngữ nghĩa — nên nó chặn được việc vô tình thêm, không thay được việc người
review đọc kỹ. Chỉ dùng thư viện chuẩn.

  python3 scripts/check_boundaries.py          # quét repo, thoát 1 nếu có vi phạm
  python3 scripts/check_boundaries.py --rules  # in danh sách từ khoá + lý do
"""
from __future__ import annotations

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent

# (mã, biểu thức, lý do). Không phân biệt hoa thường.
RULES: list[tuple[str, str, str]] = [
    ("imei",
     r"\bimei\b",
     "giả/đổi IMEI là giả mạo định danh thiết bị viễn thông — vi phạm điều khoản nhà mạng, "
     "có thể trái luật; OS không có lý do kỹ thuật nào để chạm vào IMEI"),
    ("device-id-spoof",
     r"(spoof|fake|giả\s*mạo|giả)\s*(android[_\s-]?id|device[_\s-]?id|id\s+thiết\s+bị|hwid|"
     r"serial\s+number|số\s+serial|mac\s+address|địa\s+chỉ\s+mac)|hwid\s*spoof",
     "giả định danh thiết bị để lừa ứng dụng/dịch vụ (né khoá thiết bị, lách giới hạn tài khoản)"),
    ("hardware-spoof",
     r"giả\s+mạo\s+phần\s+cứng|spoof\s+(tpm|hardware|gpu|vram)",
     "giả mạo phần cứng để qua kiểm tra của ứng dụng/game là né kiểm tra, không phải tương thích"),
    ("location-spoof",
     r"(fake|spoof|mock)[\s_-]*(gps|location)|giả\s+(lập\s+)?(vị\s*trí|gps|vùng)|vị\s*trí\s+giả",
     "giả vị trí/vùng để lừa dịch vụ (game theo vị trí, nội dung theo vùng) — vi phạm điều khoản"),
    ("play-integrity",
     r"play[\s_-]*integrity|safety[\s_-]*net",
     "Play Integrity/SafetyNet là cơ chế chứng thực của Google; dự án không vượt/qua mặt nó"),
    ("root-hiding",
     r"magisk[\s_-]*hide|zygisk|shamiko|playintegrityfix|universal[\s_-]*safetynet[\s_-]*fix|"
     r"hide[\s_-]*my[\s_-]*applist",
     "công cụ giấu root/sửa hệ thống để qua kiểm tra của ứng dụng — không đóng gói, không hướng dẫn"),
    ("anti-cheat",
     r"anti[\s_-]?cheat|chống\s+gian\s+lận",
     "né/tắt chống gian lận vi phạm điều khoản game và có thể khiến người dùng bị khoá tài khoản"),
    ("game-memory",
     r"cheat[\s_-]*engine|game[\s_-]*guardian|frida[\s_-]*gadget|aimbot|wallhack|speed[\s_-]*hack|"
     r"memory[\s_-]*hack|(sửa|can\s+thiệp)\s+bộ\s+nhớ\s+(của\s+)?game",
     "can thiệp bộ nhớ/tiến trình game là gian lận; OS không chứa công cụ như vậy"),
    ("account-farming",
     r"(treo|cày)\s*(/\s*(treo|cày)\s*)?(nhiều\s+)?(acc\b|tài\s+khoản)|multi[\s_-]?account[\s_-]*farm",
     "README/tài liệu không quảng bá treo/cày nhiều tài khoản (nhiều game cấm; sổ pháp lý §I)"),
    ("evasion",
     r"\b(né|vượt|qua\s+mặt)\s+(kiểm\s+tra|khoá|chứng\s+thực)|bypass[\s_-]+(detection|attestation|integrity)",
     "\"né kiểm tra\" là đúng điều dự án cam kết không làm"),
]
COMPILED = [(rid, re.compile(rx, re.IGNORECASE), why) for rid, rx, why in RULES]

MARK = re.compile(r"ranh-gi(?:ới|oi)-ok\s*:\s*(.*?)\s*(?:-->|\*/|$)", re.IGNORECASE)
BLOCK_BEGIN = re.compile(r"ranh-gi(?:ới|oi)-ok-b(?:ắt|at)-đ?(?:ầu|au)\s*:\s*(.*?)\s*(?:-->|\*/|$)", re.IGNORECASE)
BLOCK_END = re.compile(r"ranh-gi(?:ới|oi)-ok-k(?:ết|et)-th(?:úc|uc)", re.IGNORECASE)
MIN_REASON = 10

EXTS = {".cpp", ".hpp", ".h", ".c", ".rs", ".py", ".sh", ".json", ".md", ".toml", ".yml",
        ".yaml", ".dart", ".in", ".xml", ".ld", ".txt", ".inc"}
SKIP_DIRS = {".git", "build", "target", "node_modules", "__pycache__", ".dart_tool"}
# Chính máy kiểm và bộ test của nó phải chứa từ khoá để định nghĩa/kiểm thử.
SELF = {"scripts/check_boundaries.py", "scripts/test_check_boundaries.py"}


def iter_files(root: pathlib.Path):
    for path in sorted(root.rglob("*")):
        if not path.is_file() or path.suffix not in EXTS:
            continue
        rel = path.relative_to(root)
        if any(part in SKIP_DIRS for part in rel.parts):
            continue
        if rel.as_posix() in SELF:
            continue
        yield path, rel.as_posix()


def scan_text(text: str, where: str) -> list[str]:
    """Trả về danh sách vi phạm dạng 'tệp:dòng: [mã] ...'."""
    problems: list[str] = []
    in_block = False
    for n, line in enumerate(text.split("\n"), 1):
        begin = BLOCK_BEGIN.search(line)
        if begin:
            if len(begin.group(1).strip()) < MIN_REASON:
                problems.append(f"{where}:{n}: khối ranh-giới-ok thiếu lý do (≥ {MIN_REASON} ký tự)")
            in_block = True
        if BLOCK_END.search(line):
            in_block = False
            continue
        if in_block or begin:
            continue
        hits = [(rid, why) for rid, rx, why in COMPILED if rx.search(line)]
        if not hits:
            continue
        mark = MARK.search(line)
        if mark and len(mark.group(1).strip()) >= MIN_REASON:
            continue
        for rid, why in hits:
            if mark:
                problems.append(f"{where}:{n}: [{rid}] có chú thích ranh-giới-ok nhưng thiếu lý do")
            else:
                problems.append(f"{where}:{n}: [{rid}] {why}")
    if in_block:
        problems.append(f"{where}: khối ranh-giới-ok-bắt-đầu không có ranh-giới-ok-kết-thúc")
    return problems


def scan(root: pathlib.Path = ROOT) -> tuple[int, list[str]]:
    files = 0
    problems: list[str] = []
    for path, rel in iter_files(root):
        try:
            text = path.read_text(encoding="utf-8")
        except (UnicodeDecodeError, OSError):
            continue
        files += 1
        problems += scan_text(text, rel)
    return files, problems


def main(argv: list[str]) -> int:
    if "--rules" in argv:
        for rid, rx, why in RULES:
            print(f"[{rid}] {why}\n    {rx}")
        return 0
    files, problems = scan(ROOT)
    if problems:
        for p in problems:
            print(f"VI PHẠM {p}")
        print(f"\n{len(problems)} vi phạm ranh giới trong {files} tệp. Nếu dòng chỉ CẢNH BÁO/TỪ CHỐI, "
              f"thêm chú thích 'ranh-giới-ok: <lý do>'.")
        return 1
    print(f"ranh giới sạch: {files} tệp, {len(RULES)} nhóm từ khoá, 0 vi phạm")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
