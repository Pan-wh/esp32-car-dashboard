# 任务模式：改 iPhone App

> **适用**：调整 App 界面、定位策略、发送节奏、控制项。
> **推荐模型**：Swift / SwiftUI 能力。
> **预计 Token**：低（7 个 Swift 文件，共约 730 行）

---

## 0. 一句话边界

> **只改 `ios/CarDashboardPhone/CarDashboardPhone/*.swift`。
> 若改动涉及 JSON 字段，必须与固件解析保持一致（见 §4）。**

---

## 1. 🟢 你可以改的文件

全部在 `ios/CarDashboardPhone/CarDashboardPhone/`：

| 文件 | 行数 | 职责 | 典型需求 |
|------|------|------|---------|
| `ContentView.swift` | 165 | SwiftUI 表单界面 | 加设置项、调布局、改文案 |
| `BLEClient.swift` | 213 | BLE 连接/发送/控制命令 | 加特征、改重连策略 |
| `LocationService.swift` | 104 | 定位采集 | 改精度/后台模式/新鲜度 |
| `TelemetryQuality.swift` | 45 | **数据质量门槛** | 改速度有效性判定 |
| `TelemetryPacket.swift` | 79 | JSON 打包 | **改字段（须同步固件）** |
| `DriveSessionManager.swift` | 116 | 会话 + 2Hz 发送 | 改发送节奏 |

**工程配置**（改了才需要重跑 `xcodegen generate`）：
- `ios/CarDashboardPhone/project.yml` —— App 名称、签名 Team、Info.plist 项

---

## 2. 🔴 你不能改的

| 禁区 | 原因 |
|------|------|
| `simulator/ui/**`、`firmware/dashboard/**` | 仪表侧，不在本任务 |
| `ios/.../CarDashboardPhoneApp.swift` | 仅 11 行的入口，通常无需动 |
| `ios/.../CarDashboardPhoneUITests/**` | 除非任务明确要求补测试 |
| `tools/**`、`tests/**`、`docs/**` | 工具与文档 |

---

## 3. 📐 必须遵守的契约

### 3.1 ⚠️ SwiftUI 刷新陷阱（已踩坑）

视图观察的是 `DriveSessionManager`（`@StateObject`）。
其**子对象**（`ble` / `location`）的 `@Published` 变化**必须显式转发**，
否则界面不刷新：

```swift
private var cancellables = Set<AnyCancellable>()

private func forwardChanges() {
    ble.objectWillChange
        .sink { [weak self] _ in self?.objectWillChange.send() }
        .store(in: &cancellables)
    location.objectWillChange
        .sink { [weak self] _ in self?.objectWillChange.send() }
        .store(in: &cancellables)
}
```

> 真实症状：强退重开后「BLE 状态一直初始化中」，点一下别的按钮才刷新。

### 3.2 ⚠️ 定位失效必须即刻生效（安全相关）

任务书 §6.3 要求：**数据无效时不得用旧值冒充当前值**。
`lastLocation` 是缓存值，权限被拒/定位停止后它**仍然是旧值**，必须主动失效：

```swift
case .denied, .restricted:
    manager.stopUpdatingLocation()
    lastLocation = nil            // ★ 必须清空
```

且 `makePacket()` 要校验新鲜度（当前实现为 3 秒阈值）：

```swift
guard Date().timeIntervalSince(loc.timestamp) <= 3.0 else { return nil }
```

> 真实缺陷：曾因未清空缓存，权限改成「永不」后**仍以 2Hz 发送过期坐标**。

### 3.3 数据质量门槛（`TelemetryQuality.swift`）

```swift
maxHorizontalAccuracyMeters = 50    // 水平精度上限
maxSpeedAccuracyMPS = 2.0           // 测速精度上限（iOS 17+）
minMeaningfulSpeedMPS = 0.0         // speed<0 表示"未知"，不要放宽！
```

> ⚠️ **`speed < 0` 是 iOS 表示"速度未知"的标准信号**（室内/冷启动），
> 不要为了让数值好看而放宽此判定——那会让仪表显示假速度。

### 3.4 发送节奏

`DriveSessionManager.sendInterval = 0.5`（2Hz 定时器）。
但**实际发送速率取决于定位回调频率**（`makePacket` 无新鲜位置则不发送）：
- 静止时 iOS 降频 → 实际约 1–1.4Hz（**正常**，非缺陷）
- 行驶时恢复 ≈2Hz

> ⚠️ 不要改成"每个定位回调都立刻发"——iOS 会突发补发多个缓存点，导致瞬时刷屏。

### 3.5 BLE 特征 UUID（不要改）

| 用途 | UUID |
|------|------|
| Service | `7D2A0001-8F4A-4B7C-9D21-5E6A3B8C1000` |
| Telemetry（写）| `7D2A0002-…` |
| Status（读/通知）| `7D2A0003-…` |
| Command（写）| `7D2A0004-…` |
| 设备名 | `CarDashboard` |

---

## 4. ⚠️ 改 JSON 字段必须两边同步

`TelemetryPacket.swift` 的**编码 key** 必须与
`simulator/ui/telemetry.c` 的**解析 key** 完全一致。

```swift
// TelemetryPacket.swift
case speedMps = "speed_mps"
```
```c
/* simulator/ui/telemetry.c —— 解析同名 key */
```

**当前字段**：`v` `ts_utc_ms` `speed_mps` `speed_accuracy_mps`
`latitude` `longitude` `horizontal_accuracy_m` `course_deg`
`speed_valid` `location_valid`

**改字段的正确流程**：
1. 改 `TelemetryPacket.swift`
2. **同时**改 `simulator/ui/telemetry.c`（+ `telemetry.h`）—— 这属于跨任务，需说明
3. `cd tests && make run`（115 项 telemetry 单测会验证解析）
4. 新字段若仪表需要显示，还需改固件与 UI（**在产出中提议，不要直接改**）

> 若只改 App 侧不改固件，仪表会解析失败。**不确定时应保持字段不变。**

---

## 5. 验证要求

```bash
# 1) 编译（模拟器最快）
cd ios/CarDashboardPhone
xcodebuild -project CarDashboardPhone.xcodeproj -scheme CarDashboardPhone \
  -destination 'platform=iOS Simulator,name=iPhone 17' build

# 2) 全部 XCUITest（5 项，自动起定位场景）
tools/run_ios_tests.sh
```

> ⚠️ **不要直接跑 `xcodebuild test`**：`LinkDiagnosticsUITests` 依赖模拟器
> Freeway Drive 定位场景，测试进程内无法调用 simctl，会报 3 项**假失败**
> （已踩坑，极易误判为回归）。

**装了 `project.yml` 的改动**还要：
```bash
xcodegen generate
```

**装真机**（需人工执行，AI 通常无设备访问）：
```bash
export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
xcrun devicectl list devices                        # 查 UDID
xcodebuild -project CarDashboardPhone.xcodeproj -scheme CarDashboardPhone \
  -destination 'platform=iOS,id=<UDID>' build
xcrun devicectl device install app --device <UDID> <构建产物.app>
```

---

## 6. 产出格式

```markdown
## 改动清单
| 文件 | 改动 |
|------|------|
| ios/.../ContentView.swift | 新增「屏幕常亮」开关 |

## 边界检查
- [ ] 未修改 simulator/ui/、firmware/
- [ ] 若改了 JSON 字段，已同步说明需改 telemetry.c（或未改字段）
- [ ] 子对象 @Published 变化已转发（若新增了子对象）
- [ ] 未放宽 speed<0 的判定
- [ ] 未改动 BLE UUID

## 验证结果
| 步骤 | 结果 |
|------|------|
| xcodebuild（模拟器）| ✅ |
| tools/run_ios_tests.sh | ✅ 5 项 |

## 需人工完成
- [ ] 真机安装验证（需设备连接）
- [ ] 若改了 JSON 字段：固件侧 telemetry.c 同步
```

---

## 7. 省 Token 提示

**只需读你要改的那个 Swift 文件 + 本文件。**

- 改界面 → `ContentView.swift`
- 改定位 → `LocationService.swift`（+ `TelemetryQuality.swift`）
- 改发送 → `DriveSessionManager.swift`
- 改 BLE → `BLEClient.swift`

**不要读**：`simulator/`、`firmware/`、`reference/`、`docs/`（除本文件）、
`.xcodeproj/`（xcodegen 生成）。
