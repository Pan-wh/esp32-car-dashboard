# CarDashboardPhone —— iPhone 数据源 App（SwiftUI + Core Location + Core Bluetooth）

阶段 3/4 交付：把 iPhone 变成仪表的定位/时间数据源，经 BLE 下发到 ESP32-S3。

**⚠️ 本目录源码已在本机 `swiftc -typecheck` 通过（含 Swift→C 协议往返验证），
但未在装有 Xcode 的环境编译/真机运行 —— 需你按下面步骤在 Xcode 里构建。**

## 功能范围

- 前台 MVP：连接仪表、获取 Core Location、以 2Hz 发送版本化遥测、显示连接/定位/数据状态（`ContentView`）
- 后台驾驶会话：`开始驾驶`/`结束驾驶`，按 iOS 规则申请后台定位与 BLE 模式（阶段 4）

## 文件

| 文件 | 职责 |
|------|------|
| `CarDashboardPhoneApp.swift` | SwiftUI 入口 |
| `ContentView.swift` | UI：连接/状态/驾驶按钮/调试值 |
| `DriveSessionManager.swift` | 会话编排：定时以 2Hz 发送，不在 UI 帧发送 |
| `LocationService.swift` | Core Location 封装：授权、持续定位、`CLLocation` → 遥测 |
| `BLEClient.swift` | Core Bluetooth Central：扫描 `CarDashboard`、发现特征、写入 |
| `TelemetryPacket.swift` | 遥测 Codable 结构（**字段名与固件严格一致**） |

## 协议

Service `7D2A0001-8F4A-4B7C-9D21-5E6A3B8C1000`；
Telemetry（Write）`7D2A0002-…`；Status（Read/Notify）`7D2A0003-…`。
JSON 字段与语义见 `../../docs/ble.md` 与 `TelemetryPacket.swift` 的 CodingKeys。

## 在 Xcode 中创建工程并接入（手动，唯一未自动化步骤）

1. Xcode → **File → New → Project → iOS → App**，产品名 `CarDashboardPhone`，
   Interface 选 **SwiftUI**，Language **Swift**。
2. 删除模板自带的 `ContentView.swift` 与 `CarDashboardPhoneApp.swift`，
   将本目录 6 个 `.swift` 文件**拖入** project navigator（勾选 Copy if needed）。
3. **Info.plist** 增加以下键（隐私文案，首次弹窗会展示）：

   | Key | Value（示例文案） |
   |-----|------------------|
   | `NSLocationWhenInUseUsageDescription` | 用于在驾驶会话中读取您的车速、航向与位置 |
   | `NSLocationAlwaysAndWhenInUseUsageDescription` | 锁屏/后台驾驶时继续更新车速 |
   | `NSBluetoothAlwaysUsageDescription` | 用于连接车载仪表并发送驾驶数据 |
   | `NSBluetoothPeripheralUsageDescription` | （同上，兼容旧 iOS）|

4. **Target → Signing & Capabilities**：选择你的 Team；添加 **Background Modes**，
   勾选 `Location updates` 与 `Uses Bluetooth LE accessories`（阶段 4 后台必需）。
5. **Build Settings** → Deployment Target 建议 ≥ iOS 16.0（`CLLocation` /
   `CBManager` API 基线；若需 `CLLocation.speedAccuracy` 则 ≥ iOS 17.0，当前代码未用）。
6. 真机运行（Core Location / Core Bluetooth 功能无法在模拟器完整验证）。

## 后台（阶段 4）注意事项

- 后台定位与 BLE 能否持续，取决于权限、系统版本、App 状态；**强制退出 App 后 iOS 不保证继续发送**。
- 代码里 `CLLocationManager.activityType = .otherNavigation`、关闭自动暂停，是为导航场景优化；
  如需后台持续定位，还需 `allowsBackgroundLocationUpdates = true`（阶段 4 接入）。
- App/权限失效时，仪表端不会收到新数据 → 固件按 `BLE_STALE_MS`(2.5s) 自动把速度置无效（已实现）。

## 验证思路

- 先按 `tools/ble_client_test.swift` 或本 App 前台向仪表发一条含 `ts_utc_ms` 的包，
  固件串口应出现 `[BLE] v=1 速度=…` 与 `[同步] …UTC → RTC…`。
- 协议往返已在本机验证：Swift `JSONEncoder` 输出 → 固件 `telemetry.c` 解析结果完全一致
  （`v=1 ts=1791792803000 speed=19.03 course=120.0 location=1 → kmh=68.5`）。
