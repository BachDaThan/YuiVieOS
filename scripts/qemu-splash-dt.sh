#!/bin/sh
# scripts/qemu-splash-dt.sh — P2.16 (c): màn hình chào qua framebuffer DEVICE TREE.
#
#   ./scripts/qemu-splash-dt.sh aarch64            # mọi ca mặc định của kiến trúc
#   ./scripts/qemu-splash-dt.sh riscv64 --strict   # thiếu qemu/dtc/nhân ⇒ thất bại
#
# QEMU `virt` không tự sinh nút simple-framebuffer (ramfb cần firmware), nên ta
# làm đúng việc của U-Boot trên board thật: lấy DTB thật của máy ảo
# (`-machine virt,dumpdtb=`), dùng `dtc` thêm một nút
#   framebuffer@ADDR { compatible = "simple-framebuffer"; reg; width; height;
#                      stride; format; status; }
# trỏ vào một vùng RAM trống, rồi boot nhân với `-dtb`. Nhân đọc nút đó
# (yui_cap::fdt::parse_fdt_framebuffer) và vẽ màn hình chào vào vùng RAM ấy.
# Sau khi cổng nối tiếp báo "ve xong", monitor QEMU `pmemsave` chép nguyên
# vùng nhớ ra tệp và scripts/check_splash_ppm.py --raw kiểm từng điểm ảnh
# (đúng kích thước, đúng thứ tự kênh màu, tiêu đề trùng font phóng k×k…).
#
# Ca âm: nút status = "disabled" ⇒ nhân KHÔNG được vẽ (vùng nhớ phải còn
# nguyên 0); (aarch64) nút trỏ đè lên ảnh nhân ⇒ nhân phải từ chối và vẫn
# boot xong.
#
# Ảnh (đã đổi sang PPM) giữ ở build/splash-dt/*.ppm — CI tải lên làm artifact.
set -eu
cd "$(dirname "$0")/.."

ARCH="${1:-}"
STRICT=0
[ "${2:-}" = "--strict" ] && STRICT=1
case "$ARCH" in
    aarch64)
        QEMU="qemu-system-aarch64"
        MACHINE_ARGS="-cpu cortex-a72 -m 1024"
        BASE=0x48000000
        # WxH:format[:status] — một ca 4K để kiểm k = 2 (VBE của QEMU không dựng được 4K).
        CASES="${YUI_DT_CASES:-800x600:a8r8g8b8 1280x720:x8b8g8r8 1024x768:r8g8b8 3840x2160:x8r8g8b8 800x600:a8r8g8b8:disabled 800x600:a8r8g8b8:overlap}"
        ;;
    riscv64)
        QEMU="qemu-system-riscv64"
        MACHINE_ARGS="-m 1024"
        BASE=0x88000000
        CASES="${YUI_DT_CASES:-800x600:x8r8g8b8 1920x1080:a8b8g8r8 1024x768:r8g8b8 800x600:x8r8g8b8:disabled}"
        ;;
    *)
        echo "dùng: $0 {aarch64|riscv64} [--strict]" >&2
        exit 2
        ;;
esac
KERNEL="build/kernel/yui-kernel-$ARCH.elf"
TIMEOUT="${YUI_QEMU_TIMEOUT:-150}"
OUT="build/splash-dt"

skip() {
    echo "── qemu splash device tree ($ARCH): BỎ QUA — $1"
    [ "$STRICT" -eq 1 ] && { echo "   (--strict: coi như thất bại)" >&2; exit 1; }
    exit 0
}
command -v "$QEMU" >/dev/null 2>&1 || skip "không có $QEMU"
command -v dtc >/dev/null 2>&1 || skip "không có dtc (gói device-tree-compiler)"
command -v python3 >/dev/null 2>&1 || skip "không có python3"
[ -f "$KERNEL" ] || skip "chưa build nhân ($KERNEL) — chạy: ./scripts/build-kernel.sh $ARCH"

mkdir -p "$OUT"
echo "── $ARCH: lấy DTB gốc của máy ảo (dumpdtb)"
# shellcheck disable=SC2086
timeout 60 "$QEMU" -machine "virt,dumpdtb=$OUT/$ARCH-base.dtb" $MACHINE_ARGS -display none \
    -kernel "$KERNEL" >"$OUT/$ARCH-dumpdtb.log" 2>&1 || true
if [ ! -s "$OUT/$ARCH-base.dtb" ]; then
    echo "✗ $ARCH: QEMU không xuất được DTB" >&2
    cat "$OUT/$ARCH-dumpdtb.log" >&2
    exit 1
fi
dtc -q -I dtb -O dts -o "$OUT/$ARCH-base.dts" "$OUT/$ARCH-base.dtb"
grep -q '#address-cells = <0x02>;' "$OUT/$ARCH-base.dts" || {
    echo "✗ $ARCH: DTB gốc không dùng #address-cells = 2 như giả định" >&2
    exit 1
}

FAILED=0
for CASE in $CASES; do
    RES="$(echo "$CASE" | cut -d: -f1)"
    FMT="$(echo "$CASE" | cut -d: -f2)"
    KIND="$(echo "$CASE" | cut -d: -f3)"
    W="${RES%x*}"
    H="${RES#*x}"
    if [ "$FMT" = "r8g8b8" ]; then BPP=24; else BPP=32; fi
    STRIDE=$(( W * BPP / 8 ))
    SIZE=$(( STRIDE * H ))
    ADDR=$BASE
    STATUS=okay
    [ "$KIND" = "disabled" ] && STATUS=disabled
    # Ca "overlap": trỏ vào đầu ảnh nhân aarch64 (linker: 0x41000000).
    [ "$KIND" = "overlap" ] && ADDR=0x41000000
    TAG="$ARCH-${RES}-$FMT${KIND:+-$KIND}"
    DTS="$OUT/$TAG.dts"
    DTB="$OUT/$TAG.dtb"
    LOG="$OUT/$TAG.serial.log"
    RAW="$OUT/$TAG.raw"
    SOCK="$OUT/$TAG.sock"
    rm -f "$DTB" "$LOG" "$RAW" "$SOCK"
    ADDR_HEX=$(printf '%x' $(( ADDR )))
    { cat "$OUT/$ARCH-base.dts"; cat <<EOF

/ {
	framebuffer@$ADDR_HEX {
		compatible = "simple-framebuffer";
		reg = <0x0 0x$ADDR_HEX 0x0 $(printf '0x%x' "$SIZE")>;
		width = <$W>;
		height = <$H>;
		stride = <$STRIDE>;
		format = "$FMT";
		status = "$STATUS";
	};
};
EOF
    } >"$DTS"
    dtc -q -I dts -O dtb -o "$DTB" "$DTS"

    echo "── $TAG: boot với -dtb (simple-framebuffer @ 0x$ADDR_HEX, ${W}x${H} $FMT, status $STATUS)"
    # shellcheck disable=SC2086
    "$QEMU" -M virt $MACHINE_ARGS -display none -no-reboot -dtb "$DTB" \
        -serial "file:$LOG" -monitor "unix:$SOCK,server=on,wait=off" \
        -kernel "$KERNEL" >"$OUT/$TAG.qemu.log" 2>&1 &
    PID=$!

    deadline=$(( $(date +%s) + TIMEOUT ))
    while [ "$(date +%s)" -lt "$deadline" ]; do
        grep -q "Man hinh :" "$LOG" 2>/dev/null && break
        kill -0 "$PID" 2>/dev/null || break
        sleep 1
    done
    # Đợi thêm dòng kết thúc để chắc nhân không chết ngay sau khi vẽ.
    for _ in 1 2 3 4 5; do grep -q "yui: boot ok" "$LOG" 2>/dev/null && break; sleep 1; done
    grep "device tree\|framebuffer\|Man hinh" "$LOG" 2>/dev/null | sed 's/^/   | /' || true

    # Monitor HMP: đọc lời chào, pmemsave, ĐỢI dấu nhắc + tệp đủ byte, rồi quit.
    DUMP_SIZE=$SIZE
    [ "$KIND" = "overlap" ] && DUMP_SIZE=4096
    timeout 90 python3 - "$SOCK" "$RAW" "$ADDR" "$DUMP_SIZE" <<'PY' || echo "   (monitor: python báo lỗi)"
import os, socket, sys, time
sock, raw, addr, size = sys.argv[1], sys.argv[2], int(sys.argv[3], 0), int(sys.argv[4])
s = socket.socket(socket.AF_UNIX)
s.settimeout(10)
for _ in range(50):
    try:
        s.connect(sock)
        break
    except OSError:
        time.sleep(0.2)
else:
    sys.exit("   monitor: không nối được (QEMU đã thoát?)")

def until_prompt(limit=30):
    buf = b""
    end = time.time() + limit
    while b"(qemu)" not in buf and time.time() < end:
        try:
            chunk = s.recv(4096)
        except socket.timeout:
            continue
        if not chunk:
            break
        buf += chunk
    return buf

until_prompt()
s.sendall(("pmemsave %d %d %s\n" % (addr, size, os.path.abspath(raw))).encode())
reply = until_prompt(60)
for _ in range(300):
    if os.path.exists(raw) and os.path.getsize(raw) >= size:
        break
    time.sleep(0.1)
got = os.path.getsize(raw) if os.path.exists(raw) else -1
if got < size:
    print("   monitor: pmemsave chỉ ra %d/%d byte; trả lời: %r" % (got, size, reply[-300:]), file=sys.stderr)
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

    case "$KIND" in
        disabled)
            if grep -q "khong co framebuffer dung duoc (disabled, 1 nut" "$LOG" &&
                grep -q "yui: boot ok" "$LOG" &&
                python3 -c "import sys; d=open(sys.argv[1],'rb').read(); sys.exit(0 if len(d)==int(sys.argv[2]) and not any(d) else 1)" "$RAW" "$SIZE"; then
                echo "✓ $TAG: nút disabled bị bỏ qua, vùng nhớ còn nguyên 0 ($SIZE byte)"
            else
                echo "✗ $TAG: nút disabled vẫn bị dùng hoặc nhân không boot xong" >&2
                sed 's/^/   | /' "$LOG" | tail -20 >&2 || true
                FAILED=1
            fi
            ;;
        overlap)
            if grep -q "framebuffer devicetree: de len anh nhan" "$LOG" &&
                grep -q "yui: boot ok" "$LOG" &&
                ! grep -q "Man hinh : ve xong" "$LOG"; then
                echo "✓ $TAG: framebuffer đè ảnh nhân bị từ chối, nhân boot xong"
            else
                echo "✗ $TAG: nhân không chặn framebuffer đè lên ảnh nhân" >&2
                sed 's/^/   | /' "$LOG" | tail -20 >&2 || true
                FAILED=1
            fi
            ;;
        *)
            if ! grep -q "Man hinh : ve xong" "$LOG" 2>/dev/null; then
                echo "✗ $TAG: nhân không báo vẽ xong màn hình chào" >&2
                sed 's/^/   | /' "$LOG" 2>/dev/null | tail -25 >&2 || true
                sed 's/^/   q /' "$OUT/$TAG.qemu.log" 2>/dev/null | tail -5 >&2 || true
                FAILED=1
            elif [ ! -s "$RAW" ]; then
                echo "✗ $TAG: không chép được vùng nhớ khung hình (pmemsave)" >&2
                sed 's/^/   | /' "$LOG" 2>/dev/null | tail -8 >&2 || true
                sed 's/^/   q /' "$OUT/$TAG.qemu.log" 2>/dev/null | tail -5 >&2 || true
                FAILED=1
            elif python3 scripts/check_splash_ppm.py "$RAW" "$W" "$H" "$LOG" "$BPP" \
                --source=devicetree "--raw=$FMT:$STRIDE" "--save-ppm=$OUT/$TAG.ppm"; then
                :
            else
                FAILED=1
            fi
            ;;
    esac
    rm -f "$RAW"  # bản thô lớn (4K = 33 MB); PPM đã lưu
done

if [ "$FAILED" -ne 0 ]; then
    echo "✗ qemu splash device tree ($ARCH): có ca sai (xem $OUT/)" >&2
    exit 1
fi
echo "✓ qemu splash device tree ($ARCH): đúng mọi ca: $CASES"
