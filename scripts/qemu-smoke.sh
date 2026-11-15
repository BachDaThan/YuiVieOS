#!/bin/sh
# scripts/qemu-smoke.sh — boot YuiVieOS Beta in QEMU and look for "yui: ready".
#
#   ./scripts/qemu-smoke.sh x86_64            # BIOS Legacy (SeaBIOS)
#   ./scripts/qemu-smoke.sh x86_64 --uefi     # UEFI (OVMF)
#   ./scripts/qemu-smoke.sh aarch64           # ARM64 virt
#   ./scripts/qemu-smoke.sh riscv64           # RISC-V 64 virt (OpenSBI)
#   ./scripts/qemu-smoke.sh --strict ...      # fail instead of skipping
#
# Until Phase P2.2 builds a kernel, the kernel image does not exist: the script
# says so and exits 0 (or 1 with --strict) instead of pretending to test.
set -eu
cd "$(dirname "$0")/.."

ARCH=""
UEFI=0
STRICT=0
for arg in "$@"; do
    case "$arg" in
        --strict) STRICT=1 ;;
        --uefi)   UEFI=1 ;;
        x86_64|aarch64|riscv64) ARCH="$arg" ;;
        *) echo "tham số lạ: $arg" >&2; exit 2 ;;
    esac
done
[ -n "$ARCH" ] || { echo "dùng: $0 {x86_64|aarch64|riscv64} [--uefi] [--strict]" >&2; exit 2; }

KERNEL="build/kernel/yui-kernel-${ARCH}.elf"
TIMEOUT="${YUI_QEMU_TIMEOUT:-60}"

skip() {
    echo "── qemu $ARCH: BỎ QUA — $1"
    if [ "$STRICT" -eq 1 ]; then
        echo "   (--strict: coi như thất bại)" >&2
        exit 1
    fi
    exit 0
}

command -v qemu-system-"$ARCH" >/dev/null 2>&1 || skip "không có qemu-system-$ARCH"
[ -f "$KERNEL" ] || skip "chưa có nhân $KERNEL (Phase P2.2)"

LOG="$(mktemp)"
trap 'rm -f "$LOG"' EXIT

case "$ARCH" in
    x86_64)
        if [ "$UEFI" -eq 1 ]; then
            FW="${OVMF_CODE:-/usr/share/OVMF/OVMF_CODE.fd}"
            [ -f "$FW" ] || skip "không có OVMF ($FW)"
            set -- -bios "$FW"
        else
            set --
        fi
        qemu-system-x86_64 -m 1024 -serial stdio -display none -no-reboot \
            -kernel "$KERNEL" "$@" >"$LOG" 2>&1 &
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

deadline=$(( $(date +%s) + TIMEOUT ))
while [ "$(date +%s)" -lt "$deadline" ]; do
    if grep -q "yui: ready" "$LOG" 2>/dev/null; then
        echo "✓ qemu $ARCH: thấy 'yui: ready'"
        kill "$PID" 2>/dev/null || true
        exit 0
    fi
    kill -0 "$PID" 2>/dev/null || break
    sleep 1
done

echo "✗ qemu $ARCH: không thấy 'yui: ready' trong ${TIMEOUT}s" >&2
sed 's/^/   | /' "$LOG" | tail -20 >&2
kill "$PID" 2>/dev/null || true
exit 1
