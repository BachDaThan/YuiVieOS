#!/usr/bin/env bash
# P1.6 — Smart Stripper trên app THẬT (chạy trên CI; máy bạn cũng chạy được).
#
#   ./scripts/realapp-trim.sh electron   # Electron chính thức + app thử: gọt, bung, CHẠY lại
#   ./scripts/realapp-trim.sh drm        # Google Chrome (kèm Widevine CDM): phải stripped=false, 0 byte sửa
#
# Cần: build/bin/yui-trim (make), curl, unzip, xvfb-run, dpkg-deb, các thư viện
# GTK/NSS mà Chromium cần. Tải về thư mục build/realapp/ (bị .gitignore, KHÔNG
# phân phối lại gì: Stripper chạy cục bộ trên bản người dùng đã có — PLAN §3).
#
# Mỗi dòng kết quả in kèm "::notice" để CI hiện thành annotation.
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$PWD
TRIM=$ROOT/build/bin/yui-trim
WORK=$ROOT/build/realapp
ELECTRON_VERSION=${ELECTRON_VERSION:-33.2.1}
MAX_RATIO=${MAX_RATIO:-65}   # gói .yuip ≤ 65 % cây gốc  ⇔  nhỏ hơn ≥ 35 %
[ -x "$TRIM" ] || { echo "thiếu $TRIM — chạy make trước" >&2; exit 2; }
mkdir -p "$WORK"

note() { echo "$*"; [ -n "${GITHUB_ACTIONS:-}" ] && echo "::notice title=P1.6 $MODE::$*" || true; }
die() { echo "LỖI: $*" >&2; [ -n "${GITHUB_ACTIONS:-}" ] && echo "::error title=P1.6 $MODE::$*"; exit 1; }
bytes() { du -sb "$1" | cut -f1; }
pct() { echo $(( ($1 * 1000 / $2 + 5) / 10 )); }

# So từng tệp thường: đường dẫn, quyền, nội dung (sha256). In số tệp khác.
same_files() {
  local a=$1 b=$2
  diff <(cd "$a" && find . -type f -printf '%P %m\n' | sort) \
       <(cd "$b" && find . -type f -printf '%P %m\n' | sort) > "$WORK/diff-list.txt" || true
  diff <(cd "$a" && find . -type f -print0 | sort -z | xargs -0 sha256sum) \
       <(cd "$b" && find . -type f -print0 | sort -z | xargs -0 sha256sum) > "$WORK/diff-sha.txt" || true
  local n=$(( $(grep -c '^[<>]' "$WORK/diff-list.txt" || true) + $(grep -c '^[<>]' "$WORK/diff-sha.txt" || true) ))
  echo "$n"
}

electron_case() {
  local zip=electron-v$ELECTRON_VERSION-linux-x64.zip
  local base=https://github.com/electron/electron/releases/download/v$ELECTRON_VERSION
  cd "$WORK"
  [ -f "$zip" ] || curl -fsSL --retry 3 -o "$zip" "$base/$zip"
  curl -fsSL --retry 3 -o SHASUMS256.txt "$base/SHASUMS256.txt"
  grep " \*$zip\$" SHASUMS256.txt | sed 's/ \*/  /' | sha256sum -c - >/dev/null || die "SHA-256 của $zip không khớp SHASUMS256.txt"
  rm -rf orig out
  mkdir orig && unzip -q "$zip" -d orig
  # App thử đóng gói đúng cách Electron tìm app: resources/app/ (package.json + main.js)
  # + một gói trong node_modules (trước P1.6 Stripper xoá node_modules ⇒ app hỏng).
  mkdir -p orig/resources/app/node_modules/yui-dep
  cp "$ROOT/tests/data/electron-smoke/package.json" "$ROOT/tests/data/electron-smoke/main.js" orig/resources/app/
  cp "$ROOT/tests/data/electron-smoke/yui-dep.js" orig/resources/app/node_modules/yui-dep/index.js
  echo '{"name":"yui-dep","version":"1.0.0","main":"index.js"}' > orig/resources/app/node_modules/yui-dep/package.json

  run_app() {  # $1 = thư mục app, $2 = tệp kết quả, còn lại = cờ thêm
    local dir=$1 out=$2; shift 2
    rm -f "$out"
    YUI_SMOKE_OUT=$out LANG=en_US.UTF-8 timeout 120 xvfb-run -a "$dir/electron" --no-sandbox --disable-gpu "$@" \
      > "$out.log" 2>&1 || { cat "$out.log" >&2; die "app không chạy được ($dir $*)"; }
    [ -s "$out" ] || { cat "$out.log" >&2; die "app không ghi kết quả ($dir $*)"; }
  }
  run_app orig "$WORK/before.json"
  "$TRIM" orig -o app.yuip | tee trim.log
  grep -q '^stripped=true$' trim.log || die "Electron không có DRM mà Stripper báo stripped=false"
  mkdir out && "$TRIM" --unpack app.yuip -C out >/dev/null
  run_app out "$WORK/after.json"
  run_app out "$WORK/after-vi.json" --lang=vi
  run_app out "$WORK/after-fr.json" --lang=fr   # fr.pak đã bị gọt: Chromium phải lùi về en-US, không sập
  cmp -s before.json after.json || { cat before.json after.json; die "kết quả app trước/sau gọt khác nhau"; }
  grep -q '"locale":"vi"' after-vi.json || { cat after-vi.json; die "--lang=vi sau gọt không ra locale vi (vi.pak mất?)"; }
  grep -q '"text":"Xin chào YuiVieOS|42"' after.json || { cat after.json; die "renderer không chạy đúng"; }

  local tree zipb pack unp
  tree=$(bytes orig); zipb=$(stat -c %s "$zip"); pack=$(stat -c %s app.yuip); unp=$(bytes out)
  local ratio; ratio=$(pct "$pack" "$tree")
  note "Electron $ELECTRON_VERSION + app thử: cây gốc $((tree / 1048576)) MiB → gói .yuip $((pack / 1048576)) MiB (= $ratio % cây gốc, nhỏ hơn $((100 - ratio)) %); bung ra $((unp / 1048576)) MiB (gọt ngôn ngữ ngoài vi/en); zip chính thức $((zipb / 1048576)) MiB (gói = $(pct "$pack" "$zipb") % zip)"
  note "Chạy được sau gọt: $(cat after.json) · --lang=vi ⇒ $(grep -o '"locale":"[^"]*"' after-vi.json) · --lang=fr (đã gọt) ⇒ $(grep -o '"locale":"[^"]*"' after-fr.json)"
  [ "$ratio" -le "$MAX_RATIO" ] || die "gói = $ratio % cây gốc, chưa đạt ≤ $MAX_RATIO %"
}

drm_case() {
  local deb=google-chrome-stable_current_amd64.deb
  cd "$WORK"
  [ -f "$deb" ] || curl -fsSL --retry 3 -o "$deb" "https://dl.google.com/linux/direct/$deb"
  rm -rf chrome-x chrome-out
  dpkg-deb -x "$deb" chrome-x
  local app=chrome-x/opt/google/chrome
  [ -x "$app/chrome" ] || die "không thấy $app/chrome trong gói"
  local ver; ver=$("$app/chrome" --version 2>/dev/null || echo "?")
  "$TRIM" "$app" -o chrome.yuip --strip-docs | tee trim-drm.log
  grep -q '^stripped=false$' trim-drm.log || die "Chrome có Widevine CDM mà Stripper vẫn gọt (stripped=true)"
  grep -q 'Widevine' trim-drm.log || die "lý do không nêu Widevine"
  mkdir chrome-out && "$TRIM" --unpack chrome.yuip -C chrome-out >/dev/null
  local nd; nd=$(same_files "$app" chrome-out)
  local nfiles; nfiles=$(find "$app" -type f | wc -l)
  [ "$nd" = 0 ] || { head -20 diff-list.txt diff-sha.txt >&2; die "$nd khác biệt sau bung — phải 0 byte bị sửa"; }
  local nspecial; nspecial=$(find "$app" ! -type f ! -type d | wc -l)
  local dom; dom=$(timeout 60 chrome-out/chrome --headless=new --no-sandbox --disable-gpu \
      --dump-dom 'data:text/html;charset=utf-8,<h1>Xin chao YuiVieOS</h1>' 2>/dev/null || true)
  echo "$dom" | grep -q 'Xin chao YuiVieOS' || die "Chrome sau bung không chạy được (headless)"
  note "$ver (Widevine CDM): stripped=false · $(grep -c '⚠ bảo vệ' trim-drm.log) dấu hiệu, ví dụ: $(grep -m1 '⚠ bảo vệ' trim-drm.log | sed 's/^ *⚠ bảo vệ: //')"
  note "0 byte bị sửa: $nfiles/$nfiles tệp trùng đường dẫn + quyền + SHA-256 sau bung ($nspecial liên kết/tệp đặc biệt không lưu); bản bung chạy headless được"
}

MODE=${1:-}
case "$MODE" in
  electron) electron_case ;;
  drm) drm_case ;;
  *) echo "dùng: $0 electron|drm" >&2; exit 2 ;;
esac
