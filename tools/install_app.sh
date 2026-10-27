#!/usr/bin/env bash
#
# install_app.sh —— 一条命令把 iOS App 装到已连接的 iPhone
#
# 解决什么问题：
#   **免费 Apple ID 的描述文件只有 7 天有效期**，过期后 iPhone 会说
#   "App 不再可用"。到期后重跑一次本脚本即可（会自动续期描述文件）。
#
# 用法：
#   tools/install_app.sh              # 自动挑第一个在线设备
#   tools/install_app.sh <设备ID>     # 指定设备（xcrun devicectl list devices 里查）
#   tools/install_app.sh --list       # 只列出设备
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
PROJ_DIR="$ROOT/ios/CarDashboardPhone"
DD="$ROOT/../build-artifacts/dd-install"

# ── 只列设备 ──
if [[ "${1:-}" == "--list" ]]; then
  echo "已连接的 iOS 设备："
  xcrun devicectl list devices 2>/dev/null | grep -iE "available|unavailable" | sed 's/^/  /'
  exit 0
fi

# ── 选设备 ──
DEV="${1:-}"
if [[ -z "$DEV" ]]; then
  echo "→ 未指定设备，自动挑选…"
  # 用 UUID 正则提取（设备名可能含空格，按列取不可靠）
  DEV="$(xcrun devicectl list devices 2>/dev/null \
        | grep "available" | grep -iE "iphone" \
        | head -1 \
        | grep -oE '[A-F0-9]{8}-[A-F0-9]{4}-[A-F0-9]{4}-[A-F0-9]{4}-[A-F0-9]{12}' \
        | head -1)" || true
  if [[ -z "${DEV:-}" ]]; then
    echo "❌ 没有在线的 iPhone。请插上手机、解锁、并信任这台电脑。"
    echo "   查看设备：tools/install_app.sh --list"
    exit 1
  fi
fi
echo "目标设备: $DEV"

# ── 自动识别签名 Team ──
# project.yml 里 DEVELOPMENT_TEAM 故意留空（避免把个人 Team ID 提交进仓库），
# 这里从本机 Apple Development 证书的 OU 自动取出 Team ID；
# 也可用 `DEVELOPMENT_TEAM=XXXX tools/install_app.sh` 显式覆盖。
if [[ -z "${DEVELOPMENT_TEAM:-}" ]]; then
  CERT="$(security find-identity -v -p codesigning 2>/dev/null \
          | sed -n 's/.*"\(Apple Development: .*\)"/\1/p' | head -1)"
  if [[ -n "${CERT:-}" ]]; then
    DEVELOPMENT_TEAM="$(security find-certificate -c "$CERT" -p 2>/dev/null \
      | openssl x509 -noout -subject 2>/dev/null \
      | sed -n 's/.*OU=\([A-Z0-9]*\).*/\1/p' | head -1)"
  fi
fi
if [[ -z "${DEVELOPMENT_TEAM:-}" ]]; then
  echo "❌ 找不到可用的 Apple Development 证书/Team。"
  echo "   请先在 Xcode → Settings → Accounts 登录 Apple ID，"
  echo "   或显式指定：DEVELOPMENT_TEAM=<TeamID> tools/install_app.sh"
  exit 1
fi
echo "签名 Team: $DEVELOPMENT_TEAM"
echo

cd "$PROJ_DIR"

echo "→ 构建（-allowProvisioningUpdates 会自动续期描述文件）…"
if ! xcodebuild -project CarDashboardPhone.xcodeproj -scheme CarDashboardPhone \
      -configuration Debug -destination "platform=iOS,id=$DEV" \
      -derivedDataPath "$DD" -allowProvisioningUpdates \
      DEVELOPMENT_TEAM="$DEVELOPMENT_TEAM" build 2>&1 \
      | grep -E "error:|BUILD SUCCEEDED|BUILD FAILED" | head -5; then
  :
fi

APP="$DD/Build/Products/Debug-iphoneos/CarDashboardPhone.app"
if [[ ! -d "$APP" ]]; then
  echo
  echo "❌ 构建产物不存在，说明构建失败。"
  echo "   最常见原因：Xcode 里的 Apple 账号登录失效"
  echo "   → 打开 Xcode → Settings → Accounts，重新登录后再跑本脚本。"
  exit 1
fi

echo
echo "→ 安装到设备…"
xcrun devicectl device install app --device "$DEV" "$APP" 2>&1 \
  | grep -E "App installed|error" | head -3

echo
echo "✅ 完成。若 iPhone 仍提示「不再可用」，请："
echo "   1) 重启手机后重试；"
echo "   2) 或在 Xcode → Settings → Accounts 里重新登录 Apple ID。"
