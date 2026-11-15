#!/usr/bin/env bash
# scripts/build-kernel.sh — build khung nhân (kernel-frame) cho một kiến trúc.
#
#   ./scripts/build-kernel.sh x86_64
#   ./scripts/build-kernel.sh aarch64
#   ./scripts/build-kernel.sh riscv64
#   ./scripts/build-kernel.sh all
#
# Vì sao cần nightly: `#[alloc_error_handler]` (bắt buộc khi no_std + alloc) và
# `-Z build-std` (phải tự biên dịch `core`/`alloc` cho target bare-metal) vẫn là
# tính năng không ổn định. Mọi crate khác của dự án build bằng stable.
#
# Đầu ra: build/kernel/yui-kernel-<arch>.elf
set -euo pipefail
cd "$(dirname "$0")/.."

ARCH="${1:-}"
case "$ARCH" in
    x86_64)  TARGET="x86_64-unknown-none" ;;
    aarch64) TARGET="aarch64-unknown-none" ;;
    riscv64) TARGET="riscv64gc-unknown-none-elf" ;;
    all)
        for arch in x86_64 aarch64 riscv64; do "$0" "$arch"; done
        exit 0
        ;;
    *)
        echo "dùng: $0 {x86_64|aarch64|riscv64|all}" >&2
        exit 2
        ;;
esac

if ! command -v cargo >/dev/null 2>&1; then
    echo "✗ không có cargo — cài Rust: https://rustup.rs" >&2
    exit 1
fi

if ! cargo +nightly --version >/dev/null 2>&1; then
    echo "✗ cần toolchain nightly (xem đầu tệp để biết lý do)" >&2
    echo "  rustup toolchain install nightly --component rust-src" >&2
    exit 1
fi

mkdir -p build/kernel

echo "── build khung nhân cho $ARCH ($TARGET)"
cargo +nightly build \
    --manifest-path kernel-frame/Cargo.toml \
    --target "$TARGET" \
    --release \
    -Z build-std=core,alloc \
    -Z build-std-features=compiler-builtins-mem \
    -Z unstable-options \
    --config "target.$TARGET.rustflags=[\"-C\",\"link-arg=-Tarch/$ARCH/linker.ld\",\"-C\",\"link-arg=-nostartfiles\"]"

SRC="kernel-frame/target/$TARGET/release/yui-frame"
OUT="build/kernel/yui-kernel-$ARCH.elf"
cp "$SRC" "$OUT"
echo "✓ $OUT ($(wc -c <"$OUT") byte)"
