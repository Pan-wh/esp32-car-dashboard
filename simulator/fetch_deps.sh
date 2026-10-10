#!/bin/sh
# 获取 LVGL v8.4.0 源码到 simulator/deps/lvgl（与官方配套库版本一致）
set -e
cd "$(dirname "$0")"

if [ -d deps/lvgl ]; then
  echo "deps/lvgl 已存在，跳过（如需重下先删除该目录）"
  exit 0
fi

mkdir -p deps
echo "下载 LVGL v8.4.0 ..."
curl -sL --max-time 300 -o /tmp/lvgl-8.4.0.tar.gz \
  https://github.com/lvgl/lvgl/archive/refs/tags/v8.4.0.tar.gz
tar xzf /tmp/lvgl-8.4.0.tar.gz -C deps
mv deps/lvgl-8.4.0 deps/lvgl
rm /tmp/lvgl-8.4.0.tar.gz

echo "完成：deps/lvgl（版本 8.4.0，与 reference 官方库一致）"
