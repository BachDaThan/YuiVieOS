#!/bin/sh
# scripts/verify.sh — wrapper: static checks for YuiVieOS Beta.
# Needs nothing but python3; no cargo, no qemu, no network.
set -u
cd "$(dirname "$0")/.." || exit 2
if command -v python3 >/dev/null 2>&1; then
    exec python3 scripts/verify.py
fi
# Fallback: the minimum that matters when python3 is missing.
fail=0
for d in docs kernel-cap kernel-services vmm-sandbox ai-bridge-yuid \
         pkg-stripper userspace-vi tools arch include tests scripts; do
    [ -d "$d" ] || { echo "FAIL thiếu thư mục $d"; fail=1; }
done
if grep -rn --include='*.rs' '\bunsafe\b' kernel-cap kernel-services \
        | grep -v 'deny(unsafe_code)' >/dev/null 2>&1; then
    echo "FAIL có 'unsafe' trong crate nhân"; fail=1
fi
[ "$fail" -eq 0 ] && echo "✓ kiểm tra tối thiểu đạt (không có python3: bỏ qua liên kết/JSON)" || exit 1
