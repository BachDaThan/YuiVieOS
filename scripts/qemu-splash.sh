#!/bin/sh
# scripts/qemu-splash.sh — P2.16: màn hình chào phải vẽ ĐÚNG ở ≥ 3 độ phân giải.
#
#   ./scripts/qemu-splash.sh            # mặc định: 800x600 1280x1024 1920x1080
#   ./scripts/qemu-splash.sh --strict   # thiếu qemu/grub/nhân ⇒ thất bại
#   YUI_SPLASH_RES="1024x768 1920x1080" ./scripts/qemu-splash.sh
#
# Đường đi (x86_64, BIOS): GRUB2 `multiboot2` + `set gfxpayload=WxHx32` dựng
# framebuffer VBE tuyến tính → nhân đọc thẻ framebuffer (type 8) → splash vẽ
# với hệ số k của yui_cap::display. Sau khi cổng nối tiếp báo "ve xong", ta
# chụp màn hình QEMU (monitor `screendump`) và scripts/check_splash_ppm.py
# kiểm từng điểm ảnh: đúng kích thước, dải đỏ, nền, chữ căn giữa, chữ là khối
# k×k nguyên (không nội suy), bảng thông số có mặt.
#
# Ảnh chụp giữ lại ở build/splash/*.ppm (CI tải lên làm artifact).
set -eu
cd "$(dirname "$0")/.."

STRICT=0
for arg in "$@"; do
    case "$arg" in
        --strict) STRICT=1 ;;
        *) echo "tham số lạ: $arg" >&2; exit 2 ;;
    esac
done

RESOLUTIONS="${YUI_SPLASH_RES:-800x600 1280x1024 1920x1080}"
KERNEL="build/kernel/yui-kernel-x86_64.elf"
TIMEOUT="${YUI_QEMU_TIMEOUT:-120}"
OUT="build/splash"

skip() {
    echo "── qemu splash: BỎ QUA — $1"
    [ "$STRICT" -eq 1 ] && { echo "   (--strict: coi như thất bại)" >&2; exit 1; }
    exit 0
}

command -v qemu-system-x86_64 >/dev/null 2>&1 || skip "không có qemu-system-x86_64"
command -v grub-mkrescue >/dev/null 2>&1 || skip "không có grub-mkrescue (gói grub-common + grub-pc-bin)"
command -v xorriso >/dev/null 2>&1 || skip "không có xorriso"
command -v python3 >/dev/null 2>&1 || skip "không có python3"
[ -d /usr/lib/grub/i386-pc ] || skip "không có module GRUB BIOS (/usr/lib/grub/i386-pc, gói grub-pc-bin)"
[ -f "$KERNEL" ] || skip "chưa build nhân ($KERNEL) — chạy: ./scripts/build-kernel.sh x86_64"

if command -v grub-file >/dev/null 2>&1; then
    if grub-file --is-x86-multiboot2 "$KERNEL"; then
        echo "✓ grub-file: ảnh có header multiboot2 hợp lệ"
    else
        echo "✗ grub-file: ảnh KHÔNG phải multiboot2 hợp lệ" >&2
        exit 1
    fi
fi

mkdir -p "$OUT"
FAILED=0
for RES in $RESOLUTIONS; do
    W="${RES%x*}"
    H="${RES#*x}"
    ISO_DIR="$OUT/iso-$RES"
    ISO="$OUT/yui-$RES.iso"
    LOG="$OUT/serial-$RES.log"
    PPM="$OUT/splash-$RES.ppm"
    SOCK="$OUT/monitor-$RES.sock"
    rm -rf "$ISO_DIR" "$ISO" "$LOG" "$PPM" "$SOCK"
    mkdir -p "$ISO_DIR/boot/grub"
    cp "$KERNEL" "$ISO_DIR/boot/yui.elf"
    cat >"$ISO_DIR/boot/grub/grub.cfg" <<EOF
set timeout=0
set default=0
insmod all_video
menuentry "YuiVieOS ${RES}" {
    set gfxpayload=${RES}x32
    multiboot2 /boot/yui.elf
    boot
}
EOF
    if ! grub-mkrescue -d /usr/lib/grub/i386-pc -o "$ISO" "$ISO_DIR" >"$OUT/mkrescue-$RES.log" 2>&1; then
        echo "✗ grub-mkrescue thất bại ($RES):" >&2
        tail -20 "$OUT/mkrescue-$RES.log" >&2
        exit 1
    fi

    qemu-system-x86_64 -m 1024 -vga std -display none -no-reboot \
        -serial "file:$LOG" -monitor "unix:$SOCK,server=on,wait=off" \
        -cdrom "$ISO" -boot d >"$OUT/qemu-$RES.log" 2>&1 &
    PID=$!

    deadline=$(( $(date +%s) + TIMEOUT ))
    drawn=0
    while [ "$(date +%s)" -lt "$deadline" ]; do
        if grep -q "Man hinh : ve xong" "$LOG" 2>/dev/null; then drawn=1; break; fi
        if grep -q "Man hinh :" "$LOG" 2>/dev/null; then break; fi
        kill -0 "$PID" 2>/dev/null || break
        sleep 1
    done

    if [ "$drawn" -ne 1 ]; then
        echo "✗ $RES: nhân không báo vẽ xong màn hình chào" >&2
        sed 's/^/   | /' "$LOG" 2>/dev/null | tail -25 >&2 || true
        sed 's/^/   q /' "$OUT/qemu-$RES.log" 2>/dev/null | tail -5 >&2 || true
        kill "$PID" 2>/dev/null || true
        FAILED=1
        continue
    fi
    grep "framebuffer\|Man hinh" "$LOG" | sed 's/^/   | /'

    # Chụp màn hình qua monitor, đợi tệp PPM đủ byte rồi tắt QEMU.
    python3 - "$SOCK" "$PPM" "$W" "$H" <<'PY'
import os, socket, sys, time
sock, ppm, w, h = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
s = socket.socket(socket.AF_UNIX)
for _ in range(50):
    try:
        s.connect(sock)
        break
    except OSError:
        time.sleep(0.2)
time.sleep(0.5)  # để QEMU cập nhật bề mặt hiển thị sau lần ghi cuối
s.sendall(("screendump %s\n" % os.path.abspath(ppm)).encode())
want = len("P6\n%d %d\n255\n" % (w, h)) + w * h * 3
for _ in range(100):
    if os.path.exists(ppm) and os.path.getsize(ppm) >= want:
        break
    time.sleep(0.1)
s.sendall(b"quit\n")
s.close()
PY
    wait "$PID" 2>/dev/null || true

    if python3 scripts/check_splash_ppm.py "$PPM" "$W" "$H" "$LOG"; then
        :
    else
        FAILED=1
    fi
done

if [ "$FAILED" -ne 0 ]; then
    echo "✗ qemu splash: có độ phân giải vẽ sai (xem build/splash/)" >&2
    exit 1
fi
echo "✓ qemu splash: vẽ đúng ở mọi độ phân giải: $RESOLUTIONS"
