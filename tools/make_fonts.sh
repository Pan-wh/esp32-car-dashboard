#!/bin/sh
# 字体管线：可变字体 → 实例化字重 → 子集化 → lv_font_conv 生成 LVGL 字模
#
# 产出（simulator/ui/，同时复制子集 TTF 到 design/lvglpro/fonts/）：
#   font_clock_rounded_132.c Nunito Bold 132px（数字时钟大号时间，圆润饱满）
#   font_clock_color_224.c   Nunito Black 224px（多彩四位时钟，数字独立排版）
#   font_num_dial_38.c       Nunito Bold 38px（模拟时钟表盘数字 1-12，圆润饱满）
#   font_date_cjk_26.c       Noto Sans SC 26px（中文日期）
#
# 依赖：python3 + fonttools（tools/pylibs/）、node + npx lv_font_conv、curl
# 用法：tools/make_fonts.sh
set -e
cd "$(dirname "$0")/.."
ROOT=$(pwd)
PY=${PYTHON:-python3}
export PYTHONPATH="$ROOT/tools/pylibs"

SRC=design/assets/fonts/src
mkdir -p "$SRC"

# ---- 0) 依赖检查：fonttools（纯 Python，装进工作区）----
if ! "$PY" -c "import fontTools" 2>/dev/null; then
  echo "安装 fonttools 到 tools/pylibs/ ..."
  "$PY" -m pip install --no-cache-dir --target tools/pylibs fonttools
  # 移除与沙盒/签名不兼容的编译扩展，回退纯 Python 实现
  find tools/pylibs -name "*.so" -delete || true
fi

# ---- 1) 下载可变字体源（均 OFL，许可见 design/assets/fonts/LICENSE-*.txt）----
[ -f "$SRC/Inter-VF.ttf" ] || curl -sL --max-time 300 -o "$SRC/Inter-VF.ttf" \
  "https://raw.githubusercontent.com/google/fonts/main/ofl/inter/Inter%5Bopsz%2Cwght%5D.ttf"
[ -f "$SRC/Nunito-VF.ttf" ] || curl -sL --max-time 300 -o "$SRC/Nunito-VF.ttf" \
  "https://raw.githubusercontent.com/google/fonts/main/ofl/nunito/Nunito%5Bwght%5D.ttf"
[ -f "$SRC/NotoSansSC-VF.ttf" ] || curl -sL --max-time 300 -o "$SRC/NotoSansSC-VF.ttf" \
  "https://raw.githubusercontent.com/google/fonts/main/ofl/notosanssc/NotoSansSC%5Bwght%5D.ttf"

# ---- 2) 实例化目标字重 ----
# Inter: wght=200 (ExtraLight) + opsz=32（大字号显示优化）
"$PY" -m fontTools.varLib.instancer "$SRC/Inter-VF.ttf" wght=200 opsz=32 \
  -o "$SRC/Inter-ExtraLight.ttf"
# Nunito: wght=700 (Bold)，表盘数字——圆头圆角、笔画饱满（"圆润"观感）
"$PY" -m fontTools.varLib.instancer "$SRC/Nunito-VF.ttf" wght=700 \
  -o "$SRC/Nunito-Bold.ttf"
# Nunito: wght=900 (Black)，多彩时钟需要更接近 Apple Rounded 的饱满造型
"$PY" -m fontTools.varLib.instancer "$SRC/Nunito-VF.ttf" wght=900 \
  -o "$SRC/Nunito-Black.ttf"
# Inter: wght=700 (Bold) + opsz=32，速度大数字（车速需要粗壮醒目）
"$PY" -m fontTools.varLib.instancer "$SRC/Inter-VF.ttf" wght=700 opsz=32 \
  -o "$SRC/Inter-Bold.ttf"
# Inter: wght=500 (Medium) + opsz=32，单位/刻度标签/状态文字
"$PY" -m fontTools.varLib.instancer "$SRC/Inter-VF.ttf" wght=500 opsz=32 \
  -o "$SRC/Inter-Medium.ttf"
# Noto Sans SC: wght=400
"$PY" -m fontTools.varLib.instancer "$SRC/NotoSansSC-VF.ttf" wght=400 \
  -o "$SRC/NotoSansSC-Regular.ttf"

# ---- 3) 子集化（只留需要的字形，TTF 缩到 ~5KB）----
"$PY" -m fontTools.subset "$SRC/Inter-ExtraLight.ttf" \
  --text="0123456789: " --output-file="$SRC/inter-el-sub.ttf"
"$PY" -m fontTools.subset "$SRC/Nunito-Bold.ttf" \
  --text="0123456789" --output-file="$SRC/nunito-num-sub.ttf"
"$PY" -m fontTools.subset "$SRC/Nunito-Bold.ttf" \
  --text="0123456789:" --output-file="$SRC/nunito-clock-sub.ttf"
"$PY" -m fontTools.subset "$SRC/Nunito-Black.ttf" \
  --text="0123456789" --output-file="$SRC/nunito-color-sub.ttf"
"$PY" -m fontTools.subset "$SRC/NotoSansSC-Regular.ttf" \
  --text="0123456789月日星期一二三四五六 ·" --output-file="$SRC/noto-date-sub.ttf"
"$PY" -m fontTools.subset "$SRC/Inter-Bold.ttf" \
  --text="0123456789" --output-file="$SRC/inter-bold-num-sub.ttf"
"$PY" -m fontTools.subset "$SRC/Inter-Medium.ttf" \
  --unicodes="U+0020-007E,U+00B0" --output-file="$SRC/inter-med-ascii-sub.ttf"

# ---- 4) 生成 LVGL 字模 C 文件 ----
cd simulator
# 138px 大号时钟字体用 bpp=1（**不做抗锯齿**）：实测滑页（整屏重绘）时
# 4bpp 抗锯齿需要逐像素混合，是数字时钟页的主要绘制成本之一；
# 138px 字号足够大，去掉抗锯齿后边缘差异仅 2% 像素（>40 灰阶），而字模体积
# 从 194KB 降到 51KB。详见 docs/troubleshooting.md 滑页优化记录。
npx --yes lv_font_conv --font "../$SRC/inter-el-sub.ttf" \
  --size 138 --bpp 1 --format lvgl --no-compress --lv-include lvgl.h \
  -r 0x20-0x3A --lv-font-name clock_iphone_138 -o ui/font_clock_iphone_138.c
npx --yes lv_font_conv --font "../$SRC/nunito-clock-sub.ttf" \
  --size 132 --bpp 2 --format lvgl --no-compress --lv-include lvgl.h \
  -r 0x30-0x3A --lv-font-name clock_rounded_132 -o ui/font_clock_rounded_132.c
npx --yes lv_font_conv --font "../$SRC/nunito-color-sub.ttf" \
  --size 224 --bpp 2 --format lvgl --no-compress --lv-include lvgl.h \
  -r 0x30-0x39 --lv-font-name clock_color_224 -o ui/font_clock_color_224.c
# 注：表盘数字 1-12 已烘焙进 dial_analog 图片（tools/make_dial_faces.py），
# font_num_dial_38 不再需要，故此处不再生成（Nunito 子集仍保留给其他用途）。
npx --yes lv_font_conv --font "../$SRC/noto-date-sub.ttf" \
  --size 26 --bpp 4 --format lvgl --no-compress --lv-include lvgl.h \
  --symbols "0123456789月日星期一二三四五六 ·" --lv-font-name date_cjk_26 -o ui/font_date_cjk_26.c
npx --yes lv_font_conv --font "../$SRC/inter-bold-num-sub.ttf" \
  --size 150 --bpp 4 --format lvgl --no-compress --lv-include lvgl.h \
  -r 0x2D-0x39 --lv-font-name speed_num_150 -o ui/font_speed_num_150.c
npx --yes lv_font_conv --font "../$SRC/inter-med-ascii-sub.ttf" \
  --size 26 --bpp 4 --format lvgl --no-compress --lv-include lvgl.h \
  -r 0x20-0x7E,0xB0 --lv-font-name label_26 -o ui/font_label_26.c
# 中号数字：导航地图模式下的"距离+单位"一行（如 "300 m"）。
# ⚠️ 只取必需字形（数字、减号、空格、k、m），ASCII 全量会让这份 64px 字模涨到 200KB。
npx --yes lv_font_conv --font "../$SRC/inter-bold-num-sub.ttf" \
  --size 64 --bpp 4 --format lvgl --no-compress --lv-include lvgl.h \
  -r 0x20,0x2D-0x39,0x6B,0x6D --lv-font-name speed_num_64 -o ui/font_speed_num_64.c
cd "$ROOT"

# ---- 5) 同步子集 TTF 给 LVGL Pro 草稿工程 ----
cp "$SRC/inter-el-sub.ttf" "$SRC/nunito-num-sub.ttf" "$SRC/nunito-clock-sub.ttf" "$SRC/nunito-color-sub.ttf" "$SRC/noto-date-sub.ttf" \
   "$SRC/inter-bold-num-sub.ttf" "$SRC/inter-med-ascii-sub.ttf" design/lvglpro/fonts/

echo "完成：simulator/ui/font_*.c + design/lvglpro/fonts/*.ttf"
echo "换字重/字号：改第 2 步 wght= 或第 4 步 --size 后重跑本脚本。"
