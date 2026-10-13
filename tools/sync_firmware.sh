#!/bin/sh
# 把共享源码（simulator/ui/）同步到固件工程目录。
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

# 复制共享源码
for f in "$SRC"/*.c "$SRC"/*.h; do
  cp "$f" "$DST/$(basename "$f")"
done

# 清理**孤儿产物**：曾经同步过、但源目录已删除的文件。
# ⚠️ 真实踩坑：`font_num_dial_38.c` 在源目录删除后，固件目录里的副本仍留着
#    （它被 .gitignore 忽略，git status 看不到），成为无人引用的死文件。
#    此处按"固件目录里的共享层文件必须能在源目录找到同名文件"来判定孤儿。
for f in "$DST"/ui_*.c "$DST"/ui_*.h "$DST"/font_*.c "$DST"/control.c "$DST"/control.h \
         "$DST"/telemetry.c "$DST"/telemetry.h "$DST"/face_report.c "$DST"/face_report.h; do
  [ -e "$f" ] || continue
  base=$(basename "$f")
  if [ ! -e "$SRC/$base" ]; then
    rm -f "$f"
    echo "  [清理孤儿] $base（源目录已无此文件）"
  fi
done

echo "已同步到 $DST："
ls -1 "$DST" | grep -E '^(ui_|font_|control\.|telemetry\.)' | sed 's/^/  /'
echo
echo "接下来（在仓库根目录运行；PSRAM=opi 与 QSPI 分块参数均必需，见 docs/development-guide.md）："
echo "  arduino-cli compile \\"
echo "    --fqbn \"esp32:esp32:esp32s3:FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,USBMode=hwcdc,CDCOnBoot=cdc,PSRAM=opi\" \\"
echo "    --build-property \"compiler.cpp.extra_flags=-DESP32QSPI_MAX_PIXELS_AT_ONCE=4096\" \\"
echo "    --libraries reference/ESP32-S3-Touch-AMOLED-1.75/examples/arduino/libraries \\"
echo "    firmware/dashboard"
