#!/bin/sh
# scripts/qemu-splash.sh — P2.16: màn hình chào phải vẽ ĐÚNG ở ≥ 3 độ phân giải.
#
#   ./scripts/qemu-splash.sh            # mặc định: 800x600 1280x1024 1920x1080 1024x768x24
#   ./scripts/qemu-splash.sh --strict   # thiếu qemu/grub/nhân ⇒ thất bại
#   YUI_SPLASH_RES="1024x768 1920x1080x32" ./scripts/qemu-splash.sh   # WxH[xD], D mặc định 32
#
# Đường đi (x86_64, BIOS): GRUB2 `multiboot2` + `set gfxpayload=WxHxD` dựng
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

RESOLUTIONS="${YUI_SPLASH_RES:-800x600 1280x1024 1920x1080 1024x768x24}"
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
    W="$(echo "$RES" | cut -dx -f1)"
    H="$(echo "$RES" | cut -dx -f2)"
    D="$(echo "$RES" | cut -dx -f3)"
    [ -n "$D" ] || D=32
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
menuentry "YuiVieOS ${W}x${H}x${D}" {
    multiboot2 /boot/yui.elf
    # SAU lệnh multiboot2: khi đọc header, GRUB đặt đè gfxpayload theo thẻ
    # framebuffer của nhân ("auto" vì nhân không ghi độ phân giải ưu tiên).
    # Chế độ chỉ được dựng lúc 'boot', nên giá trị đặt ở đây mới có hiệu lực.
    set gfxpayload=${W}x${H}x${D}
    boot
}
EOF
    echo "── $RES: dựng ISO (GRUB multiboot2, gfxpayload=${W}x${H}x${D})"
    if ! timeout 180 grub-mkrescue -d /usr/lib/grub/i386-pc -o "$ISO" "$ISO_DIR" >"$OUT/mkrescue-$RES.log" 2>&1; then
        echo "✗ grub-mkrescue thất bại ($RES):" >&2
        tail -20 "$OUT/mkrescue-$RES.log" >&2
        exit 1
    fi

    echo "── $RES: boot trong QEMU (-vga std)"
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

    # Chụp màn hình qua monitor HMP: đọc lời chào, gửi screendump, ĐỢI dấu
    # nhắc trả về (đóng socket sớm thì QEMU có thể bỏ lệnh còn trong bộ đệm),
    # đợi tệp PPM đủ byte, rồi `quit`. Mọi bước đều có hạn giờ.
    echo "── $RES: chụp màn hình"
    timeout 60 python3 - "$SOCK" "$PPM" "$W" "$H" <<'PY' || echo "   (chụp màn hình: python báo lỗi, xem kiểm tra ảnh bên dưới)"
import os, socket, sys, time
sock, ppm, w, h = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
s = socket.socket(socket.AF_UNIX)
s.settimeout(10)
for _ in range(50):
    try:
        s.connect(sock)
        break
    except OSError:
        time.sleep(0.2)
else:
    sys.exit("không nối được monitor QEMU")

def until_prompt():
    buf = b""
    end = time.time() + 15
    while b"(qemu)" not in buf and time.time() < end:
        try:
            chunk = s.recv(4096)
        except socket.timeout:
            break
        if not chunk:
            break
        buf += chunk
    return buf

until_prompt()
time.sleep(0.5)  # để QEMU cập nhật bề mặt hiển thị sau lần ghi cuối
s.sendall(("screendump %s\n" % os.path.abspath(ppm)).encode())
until_prompt()
want = len("P6\n%d %d\n255\n" % (w, h)) + w * h * 3
for _ in range(100):
    if os.path.exists(ppm) and os.path.getsize(ppm) >= want:
        break
    time.sleep(0.1)
s.sendall(b"quit\n")
end = time.time() + 10
while time.time() < end:
    try:
        if not s.recv(4096):
            break
    except OSError:
        break
s.close()
PY
    for _ in 1 2 3 4 5 6 7 8 9 10; do kill -0 "$PID" 2>/dev/null || break; sleep 1; done
    kill -9 "$PID" 2>/dev/null || true
    wait "$PID" 2>/dev/null || true

    if python3 scripts/check_splash_ppm.py "$PPM" "$W" "$H" "$LOG" "$D"; then
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
