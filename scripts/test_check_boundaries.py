#!/usr/bin/env python3
"""Kiểm thử máy kiểm ranh giới P7.10.

1. Đơn vị: dòng vi phạm bị bắt; dòng cảnh báo có `ranh-giới-ok: <lý do>` qua;
   chú thích thiếu lý do vẫn bị bắt; khối bắt-đầu/kết-thúc hoạt động.
2. Đầu-cuối (đúng DoD): đặt tạm MỘT dòng vi phạm vào repo ⇒ `scripts/verify.sh`
   thoát khác 0 và nêu mã luật; gỡ ra ⇒ bước ranh giới xanh trở lại.
"""
from __future__ import annotations

import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "scripts"))
import check_boundaries as cb  # noqa: E402

checks = 0
failed = 0


def check(cond: bool, what: str) -> None:
    global checks, failed
    checks += 1
    if cond:
        print(f"  ok     {what}")
    else:
        failed += 1
        print(f"  FAIL   {what}")


def rules_hit(text: str) -> set[str]:
    out = set()
    for p in cb.scan_text(text, "x.md"):
        if "[" in p:
            out.add(p.split("[", 1)[1].split("]", 1)[0])
    return out


# ---------------------------------------------------------------- đơn vị --
VIOLATIONS = {
    "imei": "Hướng dẫn đổi IMEI cho từng phiên Android",
    "device-id-spoof": "spoof android_id mỗi lần khởi động",
    "hardware-spoof": "giả mạo phần cứng để game tưởng là máy thật",
    "location-spoof": "bật mock location cho phiên game",
    "play-integrity": "module giúp qua Play Integrity",
    "root-hiding": "cài sẵn Zygisk + Shamiko",
    "anti-cheat": "tắt anti-cheat trước khi vào trận",
    "game-memory": "tích hợp GameGuardian để sửa vàng",
    "account-farming": "Treo nhiều acc cùng lúc, cày tài khoản 24/7",
    "evasion": "chế độ né kiểm tra thiết bị",
}
for rid, line in VIOLATIONS.items():
    check(rid in rules_hit(line), f"bắt được [{rid}]: {line!r}")

check(not rules_hit("Ứng dụng ghi chú, đồng bộ thư mục, hộp cát mặc định chặn mạng"), "văn bản thường không bị bắt")
check(not rules_hit("pin, nhiệt độ, IME Telex"), "'IME' không bị nhầm thành 'IMEI'")
ok_line = "Dự án không vượt Play Integrity. <!-- ranh-giới-ok: cảnh báo phạm vi, không hỗ trợ -->"
check(not cb.scan_text(ok_line, "x.md"), "dòng cảnh báo có lý do được qua")
check(not cb.scan_text("// IMEI không bao giờ được đọc  ranh-gioi-ok: chú thích mã, không hỗ trợ", "x.cpp"),
      "dạng không dấu 'ranh-gioi-ok' trong chú thích mã được qua")
no_reason = "Không vượt Play Integrity. <!-- ranh-giới-ok: -->"
probs = cb.scan_text(no_reason, "x.md")
check(bool(probs) and "thiếu lý do" in probs[0], "chú thích thiếu lý do vẫn bị bắt")
short = "Không vượt Play Integrity. <!-- ranh-giới-ok: ok -->"
check(bool(cb.scan_text(short, "x.md")), "lý do quá ngắn (< 10 ký tự) vẫn bị bắt")
block = "\n".join([
    "<!-- ranh-giới-ok-bắt-đầu: bảng đánh giá tư vấn, toàn bộ là bác bỏ -->",
    "| C1 | giả IMEI | Không làm |",
    "| C2 | anti-cheat | Không làm |",
    "<!-- ranh-giới-ok-kết-thúc -->",
    "dòng sau khối: tắt anti-cheat",
])
bp = cb.scan_text(block, "x.md")
check(len(bp) == 1 and ":5:" in bp[0], "khối bắt-đầu/kết-thúc miễn trong khối, dòng sau khối vẫn bị bắt")
open_block = "<!-- ranh-giới-ok-bắt-đầu: khối không đóng, phải báo -->\nabc"
check(any("không có" in p for p in cb.scan_text(open_block, "x.md")), "khối không đóng bị báo")
check(not cb.scan(ROOT)[1], "repo hiện tại sạch (0 vi phạm)")

# ------------------------------------------------------------- đầu-cuối --
probe = ROOT / "docs" / "_ranh_gioi_probe.md"
try:
    probe.write_text("# thử\n\nTính năng mới: tự đổi IMEI cho mỗi phiên.\n", encoding="utf-8")
    res = subprocess.run([str(ROOT / "scripts/verify.sh")], cwd=str(ROOT), capture_output=True, text=True)
    check(res.returncode != 0, "thêm 1 dòng vi phạm ⇒ verify.sh đỏ")
    check("[imei]" in res.stdout and "_ranh_gioi_probe.md:3" in res.stdout, "verify.sh nêu đúng tệp:dòng và mã luật")
finally:
    probe.unlink(missing_ok=True)
res = subprocess.run([sys.executable, str(ROOT / "scripts/check_boundaries.py")], cwd=str(ROOT),
                     capture_output=True, text=True)
check(res.returncode == 0, "gỡ dòng vi phạm ⇒ bước ranh giới xanh lại")

print()
if failed:
    print(f"✗ test_check_boundaries: {failed} of {checks} checks FAILED")
    sys.exit(1)
print(f"✓ test_check_boundaries: {checks} checks passed, 0 failed")
