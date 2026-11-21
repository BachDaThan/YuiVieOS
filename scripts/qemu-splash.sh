#!/bin/sh
# scripts/qemu-splash.sh — P2.16: màn hình chào phải vẽ ĐÚNG ở ≥ 3 độ phân giải.
#
#   ./scripts/qemu-splash.sh            # mặc định: 800x600 1280x1024 1920x1080 1024x768x24
#   ./scripts/qemu-splash.sh --strict   # thiếu qemu/grub/nhân ⇒ thất bại
#   YUI_SPLASH_RES="1024x768 1920x1080x32" ./scripts/qemu-splash.sh   # WxH[xD], D mặc định 32
#   ./scripts/qemu-splash.sh --uefi --strict   # P2.2/P2.16(a)/P6.4: OVMF + GRUB EFI + GOP
#
# Đường UEFI (--uefi): OVMF (UEFI thật của QEMU, máy q35) → GRUB x86_64-efi →
# `multiboot2` (GRUB thoát boot services rồi nhảy vào _start 32-bit như BIOS)
# → khung hình là của **GOP** do OVMF dựng theo gfxpayload. Nhân phải in
# "firmware: UEFI" (đọc thẻ con trỏ bảng hệ thống EFI do GRUB truyền — bằng
# chứng, không đoán), vẽ đúng màn chào, rồi tới "FromBytes : ok" và
# "yui: boot ok". GOP luôn 32 bit nên mặc định chỉ thử chế độ 32 bit.
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
UEFI=0
for arg in "$@"; do
    case "$arg" in
        --strict) STRICT=1 ;;
        --uefi) UEFI=1 ;;
        *) echo "tham số lạ: $arg" >&2; exit 2 ;;
    esac
done

if [ "$UEFI" -eq 1 ]; then
    RESOLUTIONS="${YUI_SPLASH_RES:-800x600 1280x1024 1920x1080}"
    OUT="build/splash-uefi"
    GRUB_DIR=/usr/lib/grub/x86_64-efi
else
    RESOLUTIONS="${YUI_SPLASH_RES:-800x600 1280x1024 1920x1080 1024x768x24}"
    OUT="build/splash"
    GRUB_DIR=/usr/lib/grub/i386-pc
fi
KERNEL="build/kernel/yui-kernel-x86_64.elf"
TIMEOUT="${YUI_QEMU_TIMEOUT:-120}"

skip() {
    echo "── qemu splash: BỎ QUA — $1"
    [ "$STRICT" -eq 1 ] && { echo "   (--strict: coi như thất bại)" >&2; exit 1; }
    exit 0
}

command -v qemu-system-x86_64 >/dev/null 2>&1 || skip "không có qemu-system-x86_64"
command -v grub-mkrescue >/dev/null 2>&1 || skip "không có grub-mkrescue (gói grub-common + grub-pc-bin)"
command -v xorriso >/dev/null 2>&1 || skip "không có xorriso"
command -v python3 >/dev/null 2>&1 || skip "không có python3"
if [ "$UEFI" -eq 1 ]; then
    [ -d "$GRUB_DIR" ] || skip "không có module GRUB UEFI ($GRUB_DIR, gói grub-efi-amd64-bin)"
    command -v mformat >/dev/null 2>&1 || skip "không có mformat (gói mtools) — grub-mkrescue cần để dựng efi.img"
    # OVMF: ưu tiên cặp CODE + VARS (pflash, kiểu máy thật); không có thì -bios.
    OVMF_CODE=""; OVMF_VARS=""; OVMF_BIOS=""
    for c in /usr/share/OVMF/OVMF_CODE_4M.fd /usr/share/OVMF/OVMF_CODE.fd; do
        [ -f "$c" ] || continue
        v="$(echo "$c" | sed 's/CODE/VARS/')"
        [ -f "$v" ] && { OVMF_CODE="$c"; OVMF_VARS="$v"; break; }
    done
    if [ -z "$OVMF_CODE" ]; then
        for b in /usr/share/ovmf/OVMF.fd /usr/share/qemu/OVMF.fd /usr/share/OVMF/OVMF.fd; do
            [ -f "$b" ] && { OVMF_BIOS="$b"; break; }
        done
    fi
    [ -n "$OVMF_CODE$OVMF_BIOS" ] || skip "không có firmware OVMF (gói ovmf)"
    echo "── UEFI: OVMF ${OVMF_CODE:-$OVMF_BIOS}"
else
    [ -d "$GRUB_DIR" ] || skip "không có module GRUB BIOS ($GRUB_DIR, gói grub-pc-bin)"
fi
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
    echo "── $RES: dựng ISO (GRUB $(basename "$GRUB_DIR") multiboot2, gfxpayload=${W}x${H}x${D})"
    if ! timeout 180 grub-mkrescue -d "$GRUB_DIR" -o "$ISO" "$ISO_DIR" >"$OUT/mkrescue-$RES.log" 2>&1; then
        echo "✗ grub-mkrescue thất bại ($RES):" >&2
        tail -20 "$OUT/mkrescue-$RES.log" >&2
        exit 1
    fi

    if [ "$UEFI" -eq 1 ]; then
        if [ -n "$OVMF_CODE" ]; then
            cp "$OVMF_VARS" "$OUT/vars-$RES.fd"
            FW="-drive if=pflash,format=raw,readonly=on,file=$OVMF_CODE -drive if=pflash,format=raw,file=$OUT/vars-$RES.fd"
        else
            FW="-bios $OVMF_BIOS"
        fi
        echo "── $RES: boot trong QEMU (q35 + OVMF, -vga std)"
        # shellcheck disable=SC2086
        qemu-system-x86_64 -machine q35 -m 1024 -vga std -display none -no-reboot \
            $FW -serial "file:$LOG" -monitor "unix:$SOCK,server=on,wait=off" \
            -cdrom "$ISO" -boot d >"$OUT/qemu-$RES.log" 2>&1 &
    else
        echo "── $RES: boot trong QEMU (-vga std)"
        qemu-system-x86_64 -m 1024 -vga std -display none -no-reboot \
            -serial "file:$LOG" -monitor "unix:$SOCK,server=on,wait=off" \
            -cdrom "$ISO" -boot d >"$OUT/qemu-$RES.log" 2>&1 &
    fi
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
    if [ "$UEFI" -eq 1 ]; then
        # Bằng chứng đường UEFI: nhân tự đọc thẻ EFI (không phải script đoán),
        # và đi hết kmain sau màn chào.
        for want in "firmware: UEFI" "FromBytes : ok" "yui: boot ok"; do
            if grep -q "$want" "$LOG"; then
                echo "   ✓ $RES: serial có \"$want\""
            else
                echo "✗ $RES: serial thiếu \"$want\"" >&2
                FAILED=1
            fi
        done
        if grep -q "RAM       : 0 MiB\|khong doc duoc" "$LOG"; then
            echo "✗ $RES: nhân không đọc được RAM dưới UEFI" >&2
            FAILED=1
        fi
        grep "firmware\|RAM       :\|bootloader" "$LOG" | sed 's/^/   | /'
    fi
done

if [ "$FAILED" -ne 0 ]; then
    echo "✗ qemu splash: có độ phân giải vẽ sai (xem build/splash/)" >&2
    exit 1
fi
if [ "$UEFI" -eq 1 ]; then
    echo "✓ qemu splash UEFI (OVMF + GOP): boot + vẽ đúng ở mọi độ phân giải: $RESOLUTIONS"
else
    echo "✓ qemu splash: vẽ đúng ở mọi độ phân giải: $RESOLUTIONS"
fi
