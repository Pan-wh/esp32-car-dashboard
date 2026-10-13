#!/bin/sh
# 跑 iOS 全部 XCUITest（自动准备定位场景，避免 LinkDiagnostics 因缺场景而假失败）
#
# 背景：LinkDiagnosticsUITests 依赖模拟器的 Freeway Drive 定位场景，
#       而测试进程内无法调用 simctl，必须由外部先启动。
#       直接跑 `xcodebuild test` 很容易踩这个坑并误判为代码回归。
#
# 用法：tools/run_ios_tests.sh [模拟器名称]
set -e
cd "$(dirname "$0")/.."

# xcode-select 指向 CLT 时 simctl/xcodebuild 不可用，显式指定 Xcode（见 docs/troubleshooting.md）
if [ -z "$DEVELOPER_DIR" ] && [ -d /Applications/Xcode.app/Contents/Developer ]; then
  DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
  export DEVELOPER_DIR
fi
SIM="${1:-iPhone 17}"
UDID=$(xcrun simctl list devices available | grep -m1 "$SIM (" | sed -E 's/.*\(([0-9A-F-]{36})\).*/\1/')
if [ -z "$UDID" ]; then
  echo "找不到模拟器「$SIM」"; exit 1
fi
echo "== 启动模拟器 $SIM ($UDID) =="
xcrun simctl boot "$UDID" 2>/dev/null || true
xcrun simctl bootstatus "$UDID" -b >/dev/null 2>&1 || true

echo "== 启动 Freeway Drive 定位场景（LinkDiagnostics 依赖）=="
xcrun simctl location "$UDID" run "Freeway Drive"

echo "== 运行 XCUITest =="
cd ios/CarDashboardPhone
xcodebuild test -project CarDashboardPhone.xcodeproj -scheme CarDashboardPhone \
  -destination "platform=iOS Simulator,name=$SIM" 2>&1 | \
  grep -E "Test Case|Executed .* tests|error:" | tail -20
