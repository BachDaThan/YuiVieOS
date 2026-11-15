#!/bin/sh
# scripts/qemu-smoke.sh — build nhân, boot trong QEMU và kiểm chứng banner.
#
#   ./scripts/qemu-smoke.sh x86_64            # BIOS Legacy (multiboot 1)
#   ./scripts/qemu-smoke.sh aarch64           # ARM64 virt (PL011)
#   ./scripts/qemu-smoke.sh riscv64           # RISC-V 64 virt (OpenSBI)
#   ./scripts/qemu-smoke.sh x86_64 --build    # build trước rồi mới boot
#   ./scripts/qemu-smoke.sh x86_64 --strict   # thiếu qemu/nhân ⇒ thất bại
#
# Ba dòng bắt buộc phải thấy trên cổng nối tiếp (đây là bằng chứng của P1.2/P1.3):
#   "yui: ready"    — nhân đã vào Rust và in được
#   "Phan loai :"   — Hardware Auto-Profiler đã chạy
#   "Tang 1    :"   — bảng năng lực đã khởi tạo
# Thiếu bất kỳ dòng nào ⇒ exit 1 (kể cả không có --strict), vì khi đã có ảnh
# nhân thì "boot không in gì" luôn là lỗi, không phải "chưa làm".
set -eu
cd "$(dirname "$0")/.."

ARCH=""
BUILD=0
STRICT=0
for arg in "$@"; do
    case "$arg" in
        --strict) STRICT=1 ;;
        --build)  BUILD=1 ;;
        x86_64|aarch64|riscv64) ARCH="$arg" ;;
        *) echo "tham số lạ: $arg" >&2; exit 2 ;;
    esac
done
[ -n "$ARCH" ] || { echo "dùng: $0 {x86_64|aarch64|riscv64} [--build] [--strict]" >&2; exit 2; }

KERNEL="build/kernel/yui-kernel-${ARCH}.elf"
TIMEOUT="${YUI_QEMU_TIMEOUT:-90}"

skip() {
    echo "── qemu $ARCH: BỎ QUA — $1"
    if [ "$STRICT" -eq 1 ]; then
        echo "   (--strict: coi như thất bại)" >&2
        exit 1
    fi
    exit 0
}

if [ "$BUILD" -eq 1 ]; then
    ./scripts/build-kernel.sh "$ARCH"
fi

command -v "qemu-system-$ARCH" >/dev/null 2>&1 || skip "không có qemu-system-$ARCH"
[ -f "$KERNEL" ] || skip "chưa build nhân ($KERNEL) — chạy: ./scripts/build-kernel.sh $ARCH"

LOG="$(mktemp)"
trap 'rm -f "$LOG"' EXIT

case "$ARCH" in
    x86_64)
        qemu-system-x86_64 -m 1024 -serial stdio -display none -no-reboot \
            -kernel "$KERNEL" >"$LOG" 2>&1 &
        ;;
    aarch64)
        qemu-system-aarch64 -M virt -cpu cortex-a72 -m 1024 -serial stdio \
            -display none -no-reboot -kernel "$KERNEL" >"$LOG" 2>&1 &
        ;;
    riscv64)
        qemu-system-riscv64 -M virt -m 1024 -serial stdio -display none \
            -no-reboot -kernel "$KERNEL" >"$LOG" 2>&1 &
        ;;
esac
PID=$!

have_all() {
    grep -q "yui: ready" "$LOG" 2>/dev/null &&
        grep -q "Phan loai" "$LOG" 2>/dev/null &&
        grep -q "Tang 1" "$LOG" 2>/dev/null
}

deadline=$(( $(date +%s) + TIMEOUT ))
while [ "$(date +%s)" -lt "$deadline" ]; do
    if have_all; then
        echo "✓ qemu $ARCH: boot được, profiler chạy, bảng năng lực sống"
        grep -E "Kien truc|RAM|Phan loai|Tu chinh|Tang 1|quyen goc" "$LOG" | sed 's/^/   | /'
        kill "$PID" 2>/dev/null || true
        exit 0
    fi
    kill -0 "$PID" 2>/dev/null || break
    sleep 1
done

echo "✗ qemu $ARCH: không thấy đủ banner khởi động trong ${TIMEOUT}s" >&2
sed 's/^/   | /' "$LOG" | tail -30 >&2
kill "$PID" 2>/dev/null || true
exit 1
