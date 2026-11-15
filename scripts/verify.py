#!/usr/bin/env python3
"""Static verification for YuiVieOS Beta (no cargo/qemu needed).

Checks, in order:
  1. the monorepo layout the docs promise actually exists;
  2. every shipped sandbox policy and the micro-VM schema parse;
  3. kernel crates contain no `unsafe`; the single hardware frame
     (`kernel-frame/`) is the only place where `unsafe` may appear, and only in
     the files that touch hardware; all Rust sources have balanced brackets;
  4. internal documentation links resolve;
  5. every repository path quoted in the docs exists;
  6. sources are LF-only.

Exit code 0 only if everything passes.  Run through scripts/verify.sh.
"""
from __future__ import annotations

import json
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
failures: list[str] = []


def ok(msg: str) -> None:
    print(f"  \033[32mok\033[0m   {msg}")


def bad(msg: str) -> None:
    print(f"  \033[31mFAIL\033[0m {msg}")
    failures.append(msg)


def header(msg: str) -> None:
    print(f"\n── {msg}")


# --------------------------------------------------------------------------- 1
REQUIRED_DIRS = [
    "docs", "kernel-cap", "kernel-services", "vmm-sandbox", "ai-bridge-yuid",
    "pkg-stripper", "userspace-vi", "tools", "arch", "include", "tests",
    "scripts", ".github/workflows",
]
REQUIRED_FILES = [
    "Cargo.toml", "Makefile", "README.md", "LICENSE", "include/yui/yui.h",
    "kernel-cap/src/lib.rs", "kernel-cap/src/sha256.rs",
    "kernel-cap/src/sha512.rs", "kernel-cap/src/ed25519.rs",
    "kernel-cap/src/keyring.rs", "kernel-cap/src/bundle.rs",
    "kernel-services/yui-sched/src/lib.rs", "kernel-services/yui-sched/src/guard.rs",
    "kernel-services/yui-zram/src/lib.rs",
    "kernel-services/yui-zram/src/lz.rs", "kernel-services/yui-zram/src/store.rs",
    "kernel-services/ed25519.cpp", "kernel-services/bundle.cpp",
    "kernel-services/power.cpp", "kernel-services/guard.cpp",
    "kernel-services/fsstore.cpp", "kernel-services/linuxabi.cpp",
    "include/yui/ed25519.hpp", "include/yui/bundle.hpp",
    "include/yui/power.hpp", "include/yui/guard.hpp",
    "include/yui/fsstore.hpp", "include/yui/linuxabi.hpp",
    "docs/ARCHITECTURE.md", "docs/PLAN.md", "docs/PROTOCOL.md",
    "docs/SECURITY.md", "docs/FORMAT-YUIP.md", "docs/BUILD.md",
    "tests/test_capi.c", "tests/test_ed25519.cpp", "tests/test_redteam.cpp",
]
STALE = ["core", "crates", "sandbox", "ui"]  # old layout must not come back


def check_layout() -> None:
    header("cấu trúc monorepo")
    for name in REQUIRED_DIRS:
        (ok if (ROOT / name).is_dir() else bad)(f"thư mục {name}")
    for name in REQUIRED_FILES:
        (ok if (ROOT / name).is_file() else bad)(f"tệp {name}")
    for name in STALE:
        if (ROOT / name).exists():
            bad(f"thư mục cũ '{name}' vẫn còn — nội dung phải nằm trong cấu trúc mới")


# --------------------------------------------------------------------------- 2
def check_policies() -> None:
    header("chính sách hộp cát & schema")
    policies = sorted((ROOT / "vmm-sandbox/policies").glob("*.json"))
    if not policies:
        bad("không có policy nào trong vmm-sandbox/policies")
        return
    for path in policies:
        try:
            data = json.loads(path.read_text(encoding="utf-8"))
        except Exception as exc:  # noqa: BLE001 - report, do not crash
            bad(f"{path.relative_to(ROOT)}: JSON hỏng: {exc}")
            continue
        missing = [k for k in ("app", "tier", "capabilities") if k not in data]
        if missing:
            bad(f"{path.relative_to(ROOT)}: thiếu khoá {missing}")
        elif data["tier"] not in ("native", "isolated", "microvm"):
            bad(f"{path.relative_to(ROOT)}: tier lạ '{data['tier']}'")
        else:
            ok(str(path.relative_to(ROOT)))
    schema = ROOT / "vmm-sandbox/vmm/vm-manifest.schema.json"
    try:
        json.loads(schema.read_text(encoding="utf-8"))
        ok(str(schema.relative_to(ROOT)))
    except Exception as exc:  # noqa: BLE001
        bad(f"{schema.relative_to(ROOT)}: JSON hỏng: {exc}")


# --------------------------------------------------------------------------- 3
CHAR_LITERAL = re.compile(r"'(?:\\.|[^'\\])'")


def strip_rust(src: str) -> str:
    """Remove comments, strings and char literals, keeping line structure.

    Lifetimes (`'a`, `'static`) are *not* char literals, so they are skipped
    without swallowing the following code — that mistake is what makes naive
    bracket checkers useless on Rust.
    """
    out: list[str] = []
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        nxt = src[i + 1] if i + 1 < n else ""
        if c == "/" and nxt == "/":
            while i < n and src[i] != "\n":
                i += 1
            continue
        if c == "/" and nxt == "*":
            depth = 1
            i += 2
            while i < n and depth:
                if src.startswith("/*", i):
                    depth += 1
                    i += 2
                elif src.startswith("*/", i):
                    depth -= 1
                    i += 2
                else:
                    if src[i] == "\n":
                        out.append("\n")
                    i += 1
            continue
        if c == "r" and nxt in ('"', "#"):
            # raw string r"..." or r#"..."#
            j = i + 1
            hashes = 0
            while j < n and src[j] == "#":
                hashes += 1
                j += 1
            if j < n and src[j] == '"':
                terminator = '"' + "#" * hashes
                end = src.find(terminator, j + 1)
                end = n if end == -1 else end + len(terminator)
                out.append(" " * (end - i))
                i = end
                continue
        if c == '"':
            i += 1
            while i < n:
                if src[i] == "\\":
                    i += 2
                    continue
                if src[i] == '"':
                    i += 1
                    break
                if src[i] == "\n":
                    out.append("\n")
                i += 1
            continue
        if c == "'":
            m = CHAR_LITERAL.match(src, i)
            if m:
                i = m.end()
                continue
            i += 1  # lifetime or label: drop the quote only
            continue
        out.append(c)
        i += 1
    return "".join(out)


BRACKETS = {"(": ")", "[": "]", "{": "}"}


SAFE_CRATES = ("kernel-cap", "kernel-services", "vmm-sandbox", "pkg-stripper")

# Những tệp trong khung phần cứng được phép có `unsafe`, kèm lý do. Bất kỳ tệp
# nào khác xuất hiện `unsafe` đều là hồi quy.
FRAME_UNSAFE_FILES = {
    "arch/mod.rs": "lệnh hlt / wfi để dừng máy",
    "arch/x86_64.rs": "cổng nối tiếp (in/out), CPUID, cầu nối 32→64 bit",
    "arch/aarch64.rs": "UART PL011 qua con trỏ MMIO, đọc thanh ghi hệ thống",
    "arch/riscv64.rs": "lời gọi SBI qua ecall, đọc thanh ghi misa",
    "boot.rs": "đọc multiboot info / device tree do firmware cấp",
    "allocator.rs": "triển khai GlobalAlloc",
    "splash.rs": "ghi framebuffer tuyến tính do firmware cấp",
    "main.rs": "thuộc tính #[global_allocator] và #![no_main] đặt ở crate root",
}


def bracket_problems(clean: str) -> list[str]:
    stack: list[tuple[str, int]] = []
    problems: list[str] = []
    line = 1
    for ch in clean:
        if ch == "\n":
            line += 1
        elif ch in BRACKETS:
            stack.append((ch, line))
        elif ch in BRACKETS.values():
            if not stack:
                problems.append(f"dòng {line}: đóng '{ch}' không có mở")
                return problems
            opener, oline = stack.pop()
            if BRACKETS[opener] != ch:
                problems.append(f"dòng {line}: '{ch}' đóng '{opener}' mở ở dòng {oline}")
                return problems
    if stack:
        problems.append(f"còn {len(stack)} ngoặc chưa đóng (gần nhất dòng {stack[-1][1]})")
    return problems


def check_rust() -> None:
    header("an toàn Rust (không unsafe, ngoặc cân)")
    sources = sorted(p for crate in SAFE_CRATES for p in (ROOT / crate).rglob("*.rs"))
    if not sources:
        bad("không tìm thấy tệp .rs nào trong crate nhân")
        return
    unsafe_found = False
    for path in sources:
        rel = path.relative_to(ROOT)
        clean = strip_rust(path.read_text(encoding="utf-8"))
        for match in re.finditer(r"\bunsafe\b", clean):
            line = clean[: match.start()].count("\n") + 1
            bad(f"{rel}:{line}: dùng 'unsafe' — crate nhân phải giữ deny(unsafe_code)")
            unsafe_found = True
        problems = bracket_problems(clean)
        if problems:
            for problem in problems:
                bad(f"{rel}: {problem}")
        else:
            ok(f"ngoặc cân: {rel}")
    if not unsafe_found:
        ok("không có 'unsafe' trong crate nhân")


def check_frame() -> None:
    """Khung phần cứng: `unsafe` chỉ được phép ở những tệp đã được kiểm toán."""
    header("khung phần cứng (unsafe có kiểm soát)")
    frame = ROOT / "kernel-frame"
    if not frame.is_dir():
        bad("thiếu thư mục kernel-frame")
        return
    required = ["Cargo.toml", "src/main.rs", "src/arch/mod.rs", "src/boot.rs",
                "src/console.rs", "src/profiler.rs", "src/cap.rs", "src/splash.rs",
                "src/allocator.rs"]
    for name in required:
        (ok if (frame / name).is_file() else bad)(f"kernel-frame/{name}")

    lib = (frame / "src/main.rs").read_text(encoding="utf-8")
    if "deny(unsafe_code)" in lib.replace(" ", ""):
        bad("kernel-frame/src/main.rs: khung phần cứng không thể deny(unsafe_code) — "
            "hãy để lệnh cấm ở các crate an toàn, không ở khung")
    else:
        ok("khung không tuyên bố deny(unsafe_code) (đúng: unsafe nằm ở đây)")

    total = 0
    rogue: list[str] = []
    for path in sorted((frame / "src").rglob("*.rs")):
        rel = path.relative_to(frame / "src").as_posix()
        clean = strip_rust(path.read_text(encoding="utf-8"))
        problems = bracket_problems(clean)
        if problems:
            for problem in problems:
                bad(f"kernel-frame/src/{rel}: {problem}")
        else:
            ok(f"ngoặc cân: kernel-frame/src/{rel}")
        count = len(re.findall(r"\bunsafe\b", clean))
        total += count
        if count and rel not in FRAME_UNSAFE_FILES:
            rogue.append(rel)
            bad(f"kernel-frame/src/{rel}: có unsafe nhưng không nằm trong danh sách "
                f"kiểm toán ({', '.join(sorted(FRAME_UNSAFE_FILES))})")
    if not rogue:
        ok(f"{total} khối unsafe, tất cả nằm trong tệp đã kiểm toán")

    # Mỗi kiến trúc phải có mặt và mỗi kiến trúc phải in được banner thành công.
    for arch in ("x86_64", "aarch64", "riscv64"):
        path = frame / "src" / "arch" / f"{arch}.rs"
        if not path.is_file():
            bad(f"thiếu nhánh kiến trúc {arch}")
            continue
        text = path.read_text(encoding="utf-8")
        if "kmain" not in text:
            bad(f"kernel-frame/src/arch/{arch}.rs: không gọi vào kmain")
        else:
            ok(f"nhánh {arch} gọi kmain")

    # Lời hứa với CI: banner "yui: ready" phải tồn tại đâu đó trong khung.
    if "yui: ready" not in lib:
        bad("kernel-frame/src/main.rs: thiếu banner 'yui: ready' mà qemu-smoke.sh chờ")
    else:
        ok("banner 'yui: ready' có trong khung")


# --------------------------------------------------------------------------- 4
LINK = re.compile(r"\]\(([^)\s]+)\)")


def doc_files() -> list[pathlib.Path]:
    files = [ROOT / "README.md"]
    files += sorted((ROOT / "docs").glob("*.md"))
    for extra in ("kernel-cap", "kernel-services", "vmm-sandbox", "ai-bridge-yuid",
                  "pkg-stripper", "userspace-vi", "userspace-vi/ui",
                  "ai-bridge-yuid/reference"):
        files += sorted((ROOT / extra).glob("README.md"))
    return [f for f in files if f.exists()]


def check_links() -> None:
    header("liên kết tài liệu")
    docs = doc_files()
    broken = 0
    for doc in docs:
        for target in LINK.findall(doc.read_text(encoding="utf-8")):
            if target.startswith(("http://", "https://", "#", "mailto:")):
                continue
            path = target.split("#")[0]
            if not path:
                continue
            if not (doc.parent / path).resolve().exists():
                bad(f"{doc.relative_to(ROOT)}: liên kết hỏng → {target}")
                broken += 1
    if not broken:
        ok(f"{len(docs)} tệp tài liệu, mọi liên kết nội bộ tồn tại")


# --------------------------------------------------------------------------- 5
PATH_ROOTS = ("docs/", "kernel-cap/", "kernel-services/", "vmm-sandbox/",
              "ai-bridge-yuid/", "pkg-stripper/", "userspace-vi/", "tools/",
              "arch/", "tests/", "scripts/", "include/")
# MCP method names look like paths but are protocol, not files.
PROTOCOL_NAMES = {"tools/list", "tools/call", "ping", "sys/search"}
QUOTED = re.compile(r"`((?:" + "|".join(PATH_ROOTS) + r")[A-Za-z0-9_./-]+)`")


def check_doc_paths() -> None:
    header("đường dẫn trong tài liệu có thật")
    missing = 0
    for doc in [ROOT / "README.md"] + sorted((ROOT / "docs").glob("*.md")):
        for rel in sorted(set(QUOTED.findall(doc.read_text(encoding="utf-8")))):
            if rel in PROTOCOL_NAMES:
                continue
            target = ROOT / rel
            present = target.is_dir() if rel.endswith("/") else target.exists()
            if not present:
                bad(f"{doc.relative_to(ROOT)}: nhắc tới '{rel}' nhưng không tồn tại")
                missing += 1
    if not missing:
        ok("mọi đường dẫn được nêu đều tồn tại")


# --------------------------------------------------------------------------- 6
def check_line_endings() -> None:
    header("định dạng nguồn")
    exts = {".cpp", ".hpp", ".h", ".c", ".rs", ".json", ".md", ".sh", ".py", ".toml", ".ld", ".yml"}
    problems = []
    for path in ROOT.rglob("*"):
        if not path.is_file() or path.suffix not in exts:
            continue
        if any(part in {"build", "target", ".git"} for part in path.parts):
            continue
        if b"\r" in path.read_bytes():
            problems.append(str(path.relative_to(ROOT)))
    if problems:
        for name in problems:
            bad(f"CRLF: {name}")
    else:
        ok("không có CRLF")


def main() -> int:
    if not (ROOT / "docs/PLAN.md").exists():
        print("verify.py: chạy từ trong repo YuiVieOS Beta")
        return 2
    check_layout()
    check_policies()
    check_rust()
    check_frame()
    check_links()
    check_doc_paths()
    check_line_endings()
    print()
    if failures:
        print(f"\033[31m✗ {len(failures)} kiểm tra thất bại\033[0m")
        return 1
    print("\033[32m✓ tất cả kiểm tra tĩnh đều đạt\033[0m")
    return 0


if __name__ == "__main__":
    sys.exit(main())
