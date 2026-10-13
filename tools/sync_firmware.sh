#!/bin/sh
# 把共享源码（模拟器 ui/ 目录，含 NMEA 解析）同步到固件工程目录。
#
# 为什么用同步而不是直接引用：
#   Arduino 只编译 sketch 目录（含其子目录）里的源码，无法引用工程外的 .c 文件；
#   因此以 simulator/ui/ 为**唯一源码**，构建前同步一份到固件目录（已在 .gitignore 中忽略）。
#
# 用法：tools/sync_firmware.sh
set -e
cd "$(dirname "$0")/.."
ROOT=$(pwd)
DST="$ROOT/firmware/dashboard"
SRC="$ROOT/simulator/ui"

[ -d "$SRC" ] || { echo "找不到共享源码目录: $SRC"; exit 1; }
mkdir -p "$DST"

# 清理上次同步的文件。
# ⚠️ 只删**确实来自 simulator/ui/ 的同名文件**，不要用宽泛通配，
#    否则会误删固件侧手写的文件（例如 ble_telemetry.cpp 是固件专用）。
for f in "$SRC"/*.c "$SRC"/*.h; do
  base=$(basename "$f")
  [ -e "$DST/$base" ] && rm -f "$DST/$base"
done

# 复制共享源码（排除模拟器专用文件）
for f in "$SRC"/*.c "$SRC"/*.h; do
  base=$(basename "$f")
  case "$base" in
    ui_driver_images.c) ;;                 # 图像资源，需要
    *) ;;
  esac
  cp "$f" "$DST/$base"
done

echo "已同步到 $DST："
ls -1 "$DST" | grep -E '^(ui_|font_|control\.|telemetry\.)' | sed 's/^/  /'
echo
echo "接下来（在仓库根目录运行）："
echo "  arduino-cli compile --fqbn \"esp32:esp32:esp32s3:FlashSize=16M,PartitionScheme=app3M_fat9M_16MB\" \\"
echo "    --libraries reference/ESP32-S3-Touch-AMOLED-1.75/examples/arduino/libraries firmware/dashboard"
