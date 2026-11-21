#!/usr/bin/env python3
"""Kiểm thử bộ kiểm nhãn AS-IS / pháp lý Lite (scripts/check_legal.py, P7.9).

Chép các tệp liên quan của repo sang thư mục tạm: bản thật ⇒ xanh; rồi mỗi ca
đột biến một chỗ (xoá một ý của khối AS-IS, xoá dòng "chưa luật sư rà", xoá
một mục điều khoản, thêm câu miễn trừ tuyệt đối, bỏ nhãn khỏi shell/nhân,
làm câu chuẩn thiếu ý, xoá đoạn AS IS của LICENSE) ⇒ đỏ và nêu đúng tệp.
"""
from __future__ import annotations

import pathlib
import shutil
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "scripts"))
import check_legal as cl  # noqa: E402

FILES = ["README.md", "docs/terms_vi.md", "docs/privacy_vi.md", "include/yui/legal.hpp",
         "userspace-vi/shell.cpp", "kernel-frame/src/main.rs", "LICENSE"]

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


def fresh() -> pathlib.Path:
    tmp = pathlib.Path(tempfile.mkdtemp(prefix="yui-legal-"))
    for rel in FILES:
        (tmp / rel).parent.mkdir(parents=True, exist_ok=True)
        shutil.copy(ROOT / rel, tmp / rel)
    return tmp


def mutate(rel: str, old: str, new: str, expect_file: str, what: str) -> None:
    tmp = fresh()
    try:
        p = tmp / rel
        s = p.read_text(encoding="utf-8")
        if old not in s:
            check(False, f"{what} (không tìm thấy chỗ để đột biến)")
            return
        p.write_text(s.replace(old, new, 1), encoding="utf-8")
        errs = cl.check(tmp)
        check(any(e.startswith(expect_file) for e in errs), f"{what} ⇒ đỏ ở {expect_file}")
    finally:
        shutil.rmtree(tmp)


# ── bản thật xanh ──
tmp = fresh()
errs = cl.check(tmp)
check(errs == [], "repo hiện tại đạt" + ("" if not errs else f" — {errs}"))
shutil.rmtree(tmp)

# ── đột biến ──
mutate("README.md", "nguyên trạng (AS-IS)", "nguyên trạng", "README.md", "README mất \"nguyên trạng (AS-IS)\"")
mutate("README.md", "y tế, pháp lý, tài chính", "y tế", "README.md", "README mất \"pháp lý, tài chính\"")
mutate("README.md", "docs/privacy_vi.md", "privacy", "README.md", "README không trỏ tới privacy")
mutate("docs/terms_vi.md", "chưa được luật sư rà soát", "đã xem", "docs/terms_vi.md", "terms mất dòng chưa luật sư rà")
mutate("docs/privacy_vi.md", "chưa được luật sư rà soát", "", "docs/privacy_vi.md", "privacy mất dòng chưa luật sư rà")
mutate("docs/terms_vi.md", "## 9. Giới hạn trách nhiệm", "## 9. Ghi chú", "docs/terms_vi.md", "terms mất mục Giới hạn trách nhiệm")
mutate("docs/terms_vi.md", "## 3. Ghi đĩa, cài song song, Live USB", "## 3. Cài đặt", "docs/terms_vi.md", "terms mất mục Ghi đĩa")
mutate("docs/terms_vi.md", "## 11. Thay đổi điều khoản", "## 11. Khác", "docs/terms_vi.md", "terms mất mục Thay đổi điều khoản")
mutate("README.md", "\n```\n", "\n\nTác giả được miễn mọi trách nhiệm.\n\n```\n", "README.md", "câu miễn trừ tuyệt đối trong README")
mutate("docs/privacy_vi.md", "5. **Quyền của bạn:**", "5. Tác giả miễn **toàn bộ** trách nhiệm. **Quyền của bạn:**",
       "docs/privacy_vi.md", "câu miễn trừ tuyệt đối (có ** Markdown) trong privacy")
mutate("include/yui/legal.hpp", "(AS-IS), không bảo hành —", "(AS-IS) —", "include/yui/legal.hpp", "câu chuẩn kAsIsVi thiếu \"không bảo hành\"")
mutate("include/yui/legal.hpp", "without warranty", "", "include/yui/legal.hpp", "câu chuẩn kAsIsEn thiếu \"without warranty\"")
mutate("userspace-vi/shell.cpp", "  os << legal::kAsIsVi << \"\\n\";\n", "", "userspace-vi/shell.cpp", "banner shell bỏ nhãn")
mutate("userspace-vi/shell.cpp", "       << legal::kAsIsVi << \"\\n\"\n", "", "userspace-vi/shell.cpp", "phiênbản bỏ nhãn")
mutate("kernel-frame/src/main.rs", "nguyen trang (AS-IS), khong bao hanh", "beta", "kernel-frame/src/main.rs", "banner nhân bỏ nhãn")
mutate("LICENSE", 'PROVIDED "AS IS"', "PROVIDED", "LICENSE", "LICENSE mất đoạn AS IS")

# ── câu đã rà được chấp nhận thay cho "chưa rà" ──
tmp = fresh()
p = tmp / "docs/terms_vi.md"
p.write_text(p.read_text(encoding="utf-8").replace(
    "chưa được luật sư rà soát", "Đã được luật sư rà soát: Văn phòng X, 2027-01-15", 1), encoding="utf-8")
check(not any(e.startswith("docs/terms_vi.md") for e in cl.check(tmp)), "dòng \"Đã được luật sư rà soát: <ai>, <ngày>\" được chấp nhận")
shutil.rmtree(tmp)

# ── đơn vị ──
check(cl._cpp_string('x kA = "ab" "cd";', "kA") == "abcd", "_cpp_string nối literal liền nhau")
check(cl.check_absolute("Miễn mọi trách nhiệm tối đa trong phạm vi pháp luật cho phép.", "x") == [],
      "câu tuyệt đối có kèm cụm giới hạn trong cùng đoạn ⇒ được")
check(len(cl.check_absolute("A.\n\nMiễn trừ tất cả trách nhiệm.\n\nphạm vi pháp luật cho phép", "x")) == 1,
      "cụm giới hạn ở đoạn khác ⇒ vẫn đỏ")

if failed:
    print(f"\n✗ test_check_legal: {checks - failed}/{checks} check đạt")
    sys.exit(1)
print(f"\n✓ test_check_legal: {checks}/{checks} check đạt")
