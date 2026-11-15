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
ROOT="$(pwd)"
# TARGET = tên target Rust; LDLINK = thư mục linker script (arm64, không phải aarch64)
case "$ARCH" in
    x86_64)  TARGET="x86_64-unknown-none";        LINK_DIR="x86_64" ;;
    aarch64) TARGET="aarch64-unknown-none";       LINK_DIR="arm64" ;;
    riscv64) TARGET="riscv64gc-unknown-none-elf"; LINK_DIR="riscv64" ;;
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
# RUSTFLAGS thay cho --config: không phải lo quoting TOML, và link-arg chỉ có
# tác dụng ở bước link của binary (các rlib build-std không bị ảnh hưởng).
# relocation-model=static: x86_64-unknown-none mặc định PIC, mà nhân được link
# tại địa chỉ vật lý tuyệt đối (1 MiB) nên PIC không dùng được — rust-lld báo
# "relocation R_X86_64_32 cannot be used against local symbol".
RUSTFLAGS="-C relocation-model=static -C link-arg=-T$ROOT/arch/$LINK_DIR/linker.ld" \
    cargo +nightly build \
    --manifest-path kernel-frame/Cargo.toml \
    --target "$TARGET" \
    --release \
    -Z build-std=core,alloc \
    -Z build-std-features=compiler-builtins-mem

SRC="kernel-frame/target/$TARGET/release/yui-frame"
OUT="build/kernel/yui-kernel-$ARCH.elf"
cp "$SRC" "$OUT"
echo "✓ $OUT ($(wc -c <"$OUT") byte)"
