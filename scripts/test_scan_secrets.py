#!/usr/bin/env python3
"""Kiểm thử bộ quét bí mật (scripts/scan_secrets.py, P7.3).

Dựng một repo git tạm: sạch ⇒ 0 trúng; một token giả chỉ nằm trong **lịch sử
của một nhánh khác** (đã xoá khỏi cây) ⇒ quét cây không thấy, `--history`
thấy và mã thoát 1, chỉ in 4 ký tự đầu; dòng có chú thích cho phép và chuỗi
trong `.scan-secrets-ignore` được bỏ qua; tên tệp nhạy cảm (`id_ed25519`) bị bắt.

Token giả được ghép lúc chạy nên chính tệp này không làm bộ quét trúng.
"""
from __future__ import annotations

import os
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
SCAN = ROOT / "scripts/scan_secrets.py"
FAKE = "gh" + "p_" + "Q7" * 18          # dạng token GitHub, không phải token thật
FAKE_AWS = "AK" + "IA" + "Z" * 16

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


def git(repo: pathlib.Path, *args: str) -> None:
    env = dict(os.environ, GIT_AUTHOR_NAME="t", GIT_AUTHOR_EMAIL="t@t", GIT_COMMITTER_NAME="t",
               GIT_COMMITTER_EMAIL="t@t")
    subprocess.run(["git", "-C", str(repo), *args], check=True, capture_output=True, env=env)


def scan(repo: pathlib.Path, *args: str) -> tuple[int, str]:
    r = subprocess.run([sys.executable, str(SCAN), *args], cwd=str(repo), capture_output=True, text=True)
    return r.returncode, r.stdout


with tempfile.TemporaryDirectory(prefix="yui-scan-") as d:
    repo = pathlib.Path(d)
    git(repo, "init", "-q", "-b", "main")
    (repo / "a.txt").write_text("xin chào\n", encoding="utf-8")
    git(repo, "add", "-A")
    git(repo, "commit", "-qm", "sạch")
    code, out = scan(repo, "--history")
    check(code == 0 and "0 trúng" in out, "repo sạch ⇒ 0 trúng, mã 0")

    # bí mật chỉ nằm trong lịch sử của nhánh arena/x, đã bị xoá ở commit sau
    git(repo, "checkout", "-q", "-b", "arena/x")
    (repo / "cfg.txt").write_text(f"token = {FAKE}\n", encoding="utf-8")
    git(repo, "add", "-A")
    git(repo, "commit", "-qm", "lỡ tay")
    (repo / "cfg.txt").unlink()
    git(repo, "add", "-A")
    git(repo, "commit", "-qm", "xoá")
    git(repo, "checkout", "-q", "main")
    code, out = scan(repo)
    check(code == 0, "quét cây (không --history) không thấy bí mật đã xoá")
    code, out = scan(repo, "--history")
    check(code == 1 and "GitHub" in out, "--history thấy token trong lịch sử nhánh khác ⇒ mã 1")
    check(FAKE not in out and FAKE[:4] in out, "chỉ in 4 ký tự đầu, không in bí mật")

    with tempfile.TemporaryDirectory(prefix="yui-scan2-") as d2:
        r2 = pathlib.Path(d2)
        git(r2, "init", "-q", "-b", "main")
        (r2 / "t.cpp").write_text(f'auto k = "{FAKE_AWS}"; // scan-secrets: allow\n', encoding="utf-8")
        (r2 / "u.cpp").write_text(f'auto pw = "{FAKE}";\n', encoding="utf-8")
        (r2 / ".scan-secrets-ignore").write_text(f"# chuỗi giả trong test\n{FAKE}\n", encoding="utf-8")
        code, out = scan(r2)
        check(code == 0, "chú thích 'scan-secrets: allow' và .scan-secrets-ignore được tôn trọng")
        (r2 / "id_ed25519").write_text("x\n", encoding="utf-8")
        code, out = scan(r2)
        check(code == 1 and "tên tệp nhạy cảm" in out, "tên tệp id_ed25519 bị bắt")

if failed:
    print(f"\n✗ test_scan_secrets: {checks - failed}/{checks} check đạt")
    sys.exit(1)
print(f"\n✓ test_scan_secrets: {checks}/{checks} check đạt")
