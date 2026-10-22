#!/usr/bin/env bash
#
# fetch_reference.sh —— 拉取编译固件必需的厂商资料
#
# 为什么需要这一步：`reference/` 在 .gitignore 里（体积大、且是第三方代码），
# **新克隆的仓库里没有它**，而编译命令要用
#     --libraries reference/ESP32-S3-Touch-AMOLED-1.75/examples/arduino/libraries
# 所以**不跑这个脚本，编译一定失败**。
#
# 用法：
#   tools/fetch_reference.sh          # 拉取（已存在则只做版本校验）
#   tools/fetch_reference.sh --force  # 强制重新拉取
#
set -euo pipefail

REPO_URL="https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75.git"
PINNED_COMMIT="e4344e7"        # 与 docs/decisions.md 记录一致
DEST_DIR="reference"
DEST="$DEST_DIR/ESP32-S3-Touch-AMOLED-1.75"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

FORCE=0
[[ "${1:-}" == "--force" ]] && FORCE=1

echo "厂商仓库: $REPO_URL"
echo "固定版本: $PINNED_COMMIT"
echo

if [[ -d "$DEST/.git" && $FORCE -eq 0 ]]; then
  echo "→ 已存在，校验版本…"
  CUR="$(git -C "$DEST" rev-parse --short HEAD 2>/dev/null || true)"
  CUR="${CUR:-unknown}"
  if [[ "$CUR" == "$PINNED_COMMIT"* ]]; then
    echo "  ✅ 版本正确（${CUR}）"
  else
    echo "  ⚠️ 当前 $CUR ≠ 固定版本 $PINNED_COMMIT"
    echo "     （通常仍可编译；如遇问题运行 tools/fetch_reference.sh --force）"
  fi
else
  [[ $FORCE -eq 1 && -d "$DEST" ]] && { echo "→ --force：删除旧目录"; rm -rf "$DEST"; }
  mkdir -p "$DEST_DIR"
  echo "→ 克隆（浅克隆，只取目标版本）…"
  git clone --quiet "$REPO_URL" "$DEST"
  if git -C "$DEST" rev-parse --verify --quiet "$PINNED_COMMIT" >/dev/null; then
    git -C "$DEST" checkout --quiet "$PINNED_COMMIT"
    echo "  ✅ 已检出固定版本 $PINNED_COMMIT"
  else
    echo "  ⚠️ 仓库里找不到 ${PINNED_COMMIT}，保持默认分支 HEAD"
  fi
fi

echo
echo "→ 校验编译所需的内容"
LIBS="$DEST/examples/arduino/libraries"
FAIL=0
for d in lvgl ESP32_IO_Expander GFX_Library_for_Arduino Mylibrary SensorLib XPowersLib esp-lib-utils; do
  if [[ -d "$LIBS/$d" ]]; then
    echo "  ✅ $d"
  else
    echo "  ✗ 缺少 $d"; FAIL=1
  fi
done
[[ -f "$LIBS/lv_conf.h" ]] && echo "  ✅ lv_conf.h" || { echo "  ✗ 缺少 lv_conf.h"; FAIL=1; }

echo
if [[ $FAIL -ne 0 ]]; then
  echo "❌ reference 内容不完整，编译会失败。"
  exit 1
fi
echo "✅ 就绪。下一步："
echo "   tools/sync_firmware.sh        # 把 simulator/ui 同步进固件目录"
echo "   （然后按 docs/ai-bootstrap.md 的编译命令构建）"
