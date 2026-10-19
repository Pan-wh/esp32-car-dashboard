#!/bin/bash
# 重新签名并安装 App 到 iPhone SE —— 用于免费 Apple ID 的 7 天 profile 到期后续期。
#
# 用法：tools/reinstall_app_se.sh
#
# 为什么需要：免费 Apple ID 签发的 provisioning profile **只有 7 天**有效期。
# 到期后 App 图标仍在但点不开（报"未受信任的开发者"），重跑本脚本即可再续 7 天。
# **不会丢数据**：@AppStorage（网关地址/令牌）存在 App 容器里，覆盖安装会保留。
set -e

export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
DEV="${SE_DEVICE_ID:-120BFD47-943E-5534-B2E6-CD666B0470F1}"  # 可用环境变量覆盖
DD=/tmp/dsh-dd-se2
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

echo "=== 目标设备 ==="
xcrun devicectl list devices 2>&1 | grep -E "Name|SE" || true

echo ""
echo "=== 1/2 编译（含自动续签 profile）==="
cd "$ROOT/ios/CarDashboardPhone"
xcodebuild -project CarDashboardPhone.xcodeproj -scheme CarDashboardPhone \
  -sdk iphoneos -destination "id=$DEV" -configuration Debug \
  -derivedDataPath "$DD" -allowProvisioningUpdates build \
  2>&1 | grep -E "error:|BUILD (SUCCEEDED|FAILED)"

echo ""
echo "=== 2/2 安装到手机 ==="
xcrun devicectl device install app --device "$DEV" \
  "$DD/Build/Products/Debug-iphoneos/CarDashboardPhone.app" 2>&1 | tail -5

echo ""
echo "✅ 完成。若首次启动报「未受信任的开发者」："
echo "   手机 → 设置 → 通用 → VPN与设备管理 → 开发者 App → 信任你的 Apple ID"
echo ""
echo "提示：若 xcodebuild 报 'No Accounts'，先打开 Xcode 并在 Settings → Accounts"
echo "     确认 Apple ID 已登录（免费账号偶尔会掉登录）。"
