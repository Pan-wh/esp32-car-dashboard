#!/usr/bin/env bash
#
# import_photos.sh —— 把「原始素材」批量转成设备用的 .pgi
#
# 用法：
#   tools/import_photos.sh                       # 默认：/Volumes/NO NAME/{original→photos}
#   tools/import_photos.sh <源目录> <目标目录>
#   tools/import_photos.sh --size 400 --max-mb 8
#   tools/import_photos.sh --fast          # 动图优先：300px + 12fps（帧率约翻倍，代价是画面变小）
#
# 支持：JPG / PNG / GIF / HEIC / MOV / MP4
#   - HEIC  用 macOS 自带 `sips` 转 JPG（Pillow 打不开 HEIC）
#   - 视频  用 tools/extract_frames.swift（AVFoundation 抽帧）→ GIF → .pgi
#
# ## 关于体积上限
#
# 原来写死 8 MB，那是**为已废弃的 Wi-Fi 上传**设的（板子内部堆不够，传大文件必崩）。
# 现在走 SD 卡，**没有任何 8 MB 限制**：
#   - SD 卡可用 5.74 GB；
#   - 设备每次只读**一帧**（≈218 KB），**文件总大小与内存无关**。
#
# 所以默认上限放宽到 **40 MB**（只是防手滑，不是硬约束）。
# 注意：**加大体积只会让动画更长，不会更流畅** ——
# 帧率取决于"每帧读多少像素"，与总帧数无关。想更顺请看 `--fast`。
#
# ## 关于动图卡顿（实测数据）
#
# 设备端每帧耗时的实测拆分：
#     SD 读 217 KB ≈ 93 ms（**瓶颈，占 58%**）
#     解码         ≈ 21 ms
#     全屏 flush   ≈ 43 ms
#     ─────────────────────
#     合计         ≈ 159 ms → **6.3 fps**（真机实测 3~4 fps）
#
# 读的量 = 宽×高，所以**缩小画面是提升帧率最直接的办法**：
#     466×466 → 300×300：读 93→38 ms，整帧 159→104 ms（约 9.6 fps）
#     466×466 → 240×240：读 93→25 ms，整帧 159→ 89 ms（约 11 fps）
#
# `--fast` 就是按这个思路预设的（300px + 12fps）。
# 代价：画面在圆屏中央变小、四周留黑 —— 但动图"动得起来"比"铺满"更重要。
#
set -euo pipefail

# ── 参数 ──
SRC="${1:-/Volumes/NO NAME/original}"
DST="${2:-/Volumes/NO NAME/photos}"
SIZE=466
MAX_MB=40
FPS=8
MAX_FRAMES=35

# 支持 --xxx 形式覆盖
ARGS=()
for a in "$@"; do [[ "$a" == --* ]] && ARGS+=("$a") || true; done
while [[ ${#ARGS[@]} -gt 0 ]]; do
  case "${ARGS[0]}" in
    --size)       SIZE="${ARGS[1]}";       ARGS=("${ARGS[@]:2}") ;;
    --max-mb)     MAX_MB="${ARGS[1]}";     ARGS=("${ARGS[@]:2}") ;;
    --fps)        FPS="${ARGS[1]}";        ARGS=("${ARGS[@]:2}") ;;
    --max-frames) MAX_FRAMES="${ARGS[1]}"; ARGS=("${ARGS[@]:2}") ;;
    --fast)
      # 动图专用：缩小画面 + 提高帧率（见下面的说明）
      SIZE=300; FPS=12; MAX_FRAMES=40
      ARGS=("${ARGS[@]:1}") ;;
    *)            ARGS=("${ARGS[@]:1}") ;;
  esac
done

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PY="${PYTHON:-python3}"
PGI="$ROOT/tools/pgi.py"
SWIFT_EXTRACT="$ROOT/tools/extract_frames.swift"
MAX_BYTES=$((MAX_MB * 1024 * 1024))

# ── 前置检查 ──
[[ -d "$SRC" ]] || { echo "✗ 源目录不存在：$SRC"; exit 1; }
[[ -f "$PGI" ]] || { echo "✗ 找不到 $PGI"; exit 1; }
mkdir -p "$DST"

# 读出 .pgi 的实际帧数（提示里不要写"上限"，那是误导）
actual_frames() {
  "$PY" - "$1" <<'PYEOF2'
import sys, struct
d = open(sys.argv[1], 'rb').read(12)
print(struct.unpack_from("<H", d, 8)[0] if len(d) >= 12 and d[:4] == b'PGI1' else "?")
PYEOF2
}

TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT

echo "源目录  : $SRC"
echo "目标目录: $DST"
echo "边长 $SIZE · 上限 ${MAX_MB}MB（仅防手滑，非硬件限制）· 动图 ${FPS}fps / 最多 ${MAX_FRAMES} 帧"
echo

n=0; ok=0; fail=0

# 按文件名排序，编号前缀保证设备上的轮播顺序稳定
while IFS= read -r -d '' f; do
  base="$(basename "$f")"; stem="${base%.*}"
  ext="${base##*.}"; ext="$(echo "$ext" | tr '[:upper:]' '[:lower:]')"
  n=$((n+1))
  idx="$(printf '%02d' "$n")"
  out="$DST/${idx}_${stem}.pgi"

  case "$ext" in
    jpg|jpeg|png)
      if "$PY" "$PGI" "$f" -o "$out" --size "$SIZE" --max-frames 1 >/dev/null 2>&1; then
        printf "  ✅ %-28s → %s\n" "$base" "$(basename "$out")"; ok=$((ok+1))
      else printf "  ✗ %-28s 转换失败\n" "$base"; fail=$((fail+1)); fi
      ;;
    heic|heif)
      jpg="$TMP/${stem}.jpg"
      if sips -s format jpeg -s formatOptions 90 "$f" --out "$jpg" >/dev/null 2>&1 \
         && "$PY" "$PGI" "$jpg" -o "$out" --size "$SIZE" --max-frames 1 >/dev/null 2>&1; then
        printf "  ✅ %-28s → %s（HEIC 经 sips 转 JPG）\n" "$base" "$(basename "$out")"; ok=$((ok+1))
      else printf "  ✗ %-28s HEIC 转换失败\n" "$base"; fail=$((fail+1)); fi
      ;;
    gif)
      # GIF 帧数未知，先按上限转；超了就减半重试
      frames=$MAX_FRAMES
      while :; do
        if "$PY" "$PGI" "$f" -o "$out" --size "$SIZE" --max-frames "$frames" --fps "$FPS" >/dev/null 2>&1; then
          sz=$(stat -f%z "$out" 2>/dev/null || echo 0)
          if (( sz <= MAX_BYTES )) || (( frames <= 6 )); then break; fi
        fi
        frames=$((frames / 2))
        (( frames < 6 )) && { echo "  ✗ $base 无法压到 ${MAX_MB}MB 内"; fail=$((fail+1)); out=""; break; }
      done
      [[ -n "$out" && -f "$out" ]] && { printf "  ✅ %-28s → %s（%s 帧，%s）\n" "$base" "$(basename "$out")" "$(actual_frames "$out")" "$(du -h "$out" | cut -f1)"; ok=$((ok+1)); }
      ;;
    mov|mp4|m4v)
      frames=$MAX_FRAMES
      while :; do
        rm -rf "$TMP/frames"; 
        if swift "$SWIFT_EXTRACT" "$f" "$TMP/frames" "$frames" "$SIZE" >/dev/null 2>&1 \
           && ls "$TMP/frames"/*.png >/dev/null 2>&1; then
          "$PY" - "$TMP/frames" "$TMP/v.gif" "$SIZE" <<'PYEOF'
import sys, glob, os
from PIL import Image
frames_dir, gif, size = sys.argv[1], sys.argv[2], int(sys.argv[3])
files = sorted(glob.glob(os.path.join(frames_dir, "*.png")))
imgs = []
for f in files:
    im = Image.open(f).convert("RGB")
    w, h = im.size; s = min(w, h)
    im = im.crop(((w-s)//2, (h-s)//2, (w-s)//2+s, (h-s)//2+s)).resize((size, size), Image.LANCZOS)
    imgs.append(im)
if imgs:
    imgs[0].save(gif, save_all=True, append_images=imgs[1:], duration=int(1000/max(1, 8)), loop=0)
PYEOF
          "$PY" "$PGI" "$TMP/v.gif" -o "$out" --size "$SIZE" --max-frames "$frames" --fps "$FPS" >/dev/null 2>&1 || true
          if [[ -f "$out" ]]; then
            sz=$(stat -f%z "$out" 2>/dev/null || echo 0)
            if (( sz <= MAX_BYTES )) || (( frames <= 6 )); then break; fi
          fi
        fi
        frames=$((frames / 2))
        (( frames < 6 )) && { echo "  ✗ $base 抽帧失败或压不到 ${MAX_MB}MB 内"; fail=$((fail+1)); out=""; break; }
      done
      [[ -n "$out" && -f "$out" ]] && { printf "  ✅ %-28s → %s（%s 帧，%s）\n" "$base" "$(basename "$out")" "$(actual_frames "$out")" "$(du -h "$out" | cut -f1)"; ok=$((ok+1)); }
      ;;
    *)
      echo "  · 跳过 $base（不支持的格式）"
      ;;
  esac
done < <(find "$SRC" -maxdepth 1 -type f ! -name '.*' -print0 | sort -z)

echo
echo "完成：成功 $ok 个，失败 $fail 个"
echo
echo "—— 校验（与设备端规则一致：头可解析 + 大小与头自洽）——"
"$PY" - "$DST" <<'PYEOF'
import sys, glob, os, struct
ok = bad = 0
for p in sorted(glob.glob(os.path.join(sys.argv[1], "*.pgi"))):
    d = open(p, 'rb').read(32)
    if len(d) < 32 or d[:4] != b'PGI1':
        print(f"  ✗ {os.path.basename(p)} magic 错"); bad += 1; continue
    w, h, n, fps, _ = struct.unpack_from("<HHHBB", d, 4)
    _, data_off = struct.unpack_from("<II", d, 16) if False else (0, struct.unpack_from("<I", d, 20)[0])
    fsz = os.path.getsize(p)
    expect = data_off + (1024 + w*h) * n
    if fsz == expect:
        kind = "静态" if n == 1 else f"动图{n}帧"
        print(f"  ✅ {os.path.basename(p):44s} {kind:10s} {fsz:>9,} B"); ok += 1
    else:
        print(f"  ✗ {os.path.basename(p)} 大小 {fsz} ≠ 应有 {expect}"); bad += 1
print(f"\n  合计：{ok} 个合法，{bad} 个有问题")
PYEOF
echo
echo "把卡插回仪表 → 划到照片表盘即可看到。"
