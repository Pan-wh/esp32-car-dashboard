# CarDashboardPhone —— iPhone 数据源 App（SwiftUI + Core Location + Core Bluetooth）

阶段 3/4 交付：把 iPhone 变成仪表的定位/时间数据源，经 BLE 下发到 ESP32-S3。

**状态**：已在 Xcode 26.5 完成模拟器 + 真机构建/运行；照片 BLE 直传已通过 iPhone 12 ↔ ESP32-S3
真机验收（详见 `docs/photo-implementation.md`）。

## 功能范围

- 前台 MVP：连接仪表、获取 Core Location、以 2Hz 发送版本化遥测、显示连接/定位/数据状态（`ContentView`）
- 后台驾驶会话：`开始驾驶`/`结束驾驶`，按 iOS 规则申请后台定位与 BLE 模式（阶段 4）
- 照片表盘：在 App 里选图/视频 → 圆屏裁切 → **BLE 直传**到仪表（详见下文「照片」）

## 连接策略（自动连接 / 手动断开）

**蓝牙就绪即自动连接仪表，掉线后自动重连** —— 用户不需要手动点「连接仪表」。

同时保留**手动断开**，且断开是"粘性"的：

| 动作 | 行为 |
|------|------|
| App 启动（蓝牙就绪）| 自动扫描并连接 `CarDashboard`，无需点击 |
| 连接中掉线 / 连接失败 | 自动重连（仪表重新上电后会自愈）|
| 点「断开连接」| 断开，并**停止自动连接** —— 不会被自动连回去 |
| 断开后点「连接仪表」| 恢复连接，并**重新打开**自动连接 |

- **校时策略（2026-10-27 回退）**：连接只建立链路，**不再自动写 RTC**；点「开始驾驶」后
  随遥测包的 `ts_utc_ms` 校时。原因是连接校时在真机上会导致 RTC 被反复写入、时钟跳变；
  若以后要恢复“连接即校时”，需要先解决 RTC/I2C 写入的稳定性并做跳变验证。

设计的核心是区分**「当前没连着」（`isConnected`）**与**「用户想不想连着」（`autoConnectEnabled`）**：
只有后者能决定"要不要自动连"。若用 `isConnected` 判断，断开按钮会立刻被自动重连覆盖而失效。

决策逻辑抽成了纯函数 `BLEConnectionPolicy`（不依赖 CoreBluetooth），
因此"用户断开优先于蓝牙就绪"这类只能在真机上暴露的组合可以在 PC 上穷举验证：

```bash
cd tests && make test-ble-policy   # 19 项断言，含 16 种状态组合的穷举不变量检查
```

界面状态文案随之明确区分两种"未连接"：

- `正在连接…` —— 过渡态，用户等着即可
- `已断开` —— 稳定态，用户需要点「连接仪表」

## 界面结构（五个标签页）

按"**配置**"与"**观察**"分页 —— 此前所有内容挤在一页，开车时要看的
速度/航向被亮度、表盘开关等配置项挤到下面，每次都得滚动。

| 标签 | 内容 | 定位 |
|------|------|------|
| **仪表** | 权限说明 · 仪表连接（含**开始驾驶**）· 屏幕显示 · 表盘 | 配置：让仪表按你要的方式工作 |
| **数据** | 发送状态 · 实时数据（速度/航向/精度）· **诊断详情**（默认折叠）| 观察：手机看到了什么、发出去了什么 |
| **导航** | 进度说明（预留）| 后续：路线进度、下一转向、偏航状态 |
| **照片** | 仪表状态 · 上传队列 · 设备相册 · 播放设置 · 高级 | 把照片通过蓝牙发到仪表 |
| **设置** | 导航网关、令牌等开发期配置 | 一次性配置 |

几处刻意的设计：

- **「开始驾驶」放在「仪表」页**：它是**动作**不是数据，与"连接"同页才能形成
  *连接 → 开始驾驶* 的顺畅流程；发送统计属于"观察"，留在数据页。
- **连接状态在三个页面的导航栏都显示**（绿/橙圆点）：
  数据是否有效完全取决于连接，若只在「仪表」页显示，用户在数据页看到
  速度不动时无法判断是"没定位"还是"没连上"。
- **诊断信息默认折叠**：质量门槛、原始值、经纬度等是排查用的，开车时只看速度/航向。
- **导航页如实标注进度**（已完成的打勾、未完成的空圈），不假装能用。

## 照片（BLE 直传）

照片页（`Photo/PhotoTabView.swift`）把「选图 → 裁切 → 编码 → 传输」整条链做在 App 里，
**不再需要拔 SD 卡，也不再走 Wi-Fi**（Wi-Fi 上传因仪表内部 RAM 不足已废弃）。

```
PhotosPicker 多选（图片/视频/GIF）
  → 默认居中方形裁切 466×466（可点开单张用 PhotoCropView 重新构图）
  → PhotoEncoder 出 baseline JPEG（q80，约 40–60 KB）
  → PGJEncoder 打包成 .pgj（32 B 头 + CRC32）写到 tmp/photo-upload/
  → BLEClient 分片 + 窗口流控上传（iOS 上逐片 `WRITE` 带确认；进度按设备已落盘字节算）
  → 设备校验 + 原子提交，App 刷新设备相册
```

几条设计约束（细节见 `../../docs/photo-wireless-design.md` §10、`../../docs/photo-protocol.md`）：

- **能力分档**：LITE 档只支持静态照片；视频只能「取一帧」。只有固件支持动图、
  设备实测动图 ≥8 fps、且本次连接实测上传 ≥30 KB/s 时，才出现「生成动图」入口。
  不可用时**显示具体原因**，不把入口藏起来了事。
- **状态以设备为准**：照片清单、当前播放张、轮播/位移/息屏设置都来自设备上报
  （`PhotoInfo` / `PhotoState` / 清单分页），App 只读+改，不做乐观本地值。
- **失败可恢复**：编码产物先落沙盒临时文件，断线重连按设备已落盘偏移续传；
  校验失败可重试；失败原因用用户能看懂的话显示。
- **缩略图缓存**：上传时顺手生成 96×96 JPEG，存在 Application Support
  （`PhotoThumbnailCache`，按设备照片 id 命名）；设备清单不传画面，读不到就显示占位图。
- **旧固件降级**：设备没有照片服务时整页给「如何用 SD 卡导入」的说明，
  不显示点不动的传输按钮。
- **主操作**：照片页在状态卡下有醒目的「添加照片」卡片（比右上角 ＋ 更易发现）；
  空相册同样引导到该入口。
- **格式化 SD 卡**（高级）：二次确认后让设备清空卡内全部文件并重建目录；
  卡的文件系统损坏时会先由 SD 驱动重建。执行中显示进度，成功后自动刷新清单；
  正在上传时禁用（固件也会返回 `busy`）。

构建/更新工程：改了文件后无需改工程配置（`project.yml` 按目录收源码），
但新增文件后要 `xcodegen generate` 重新生成 `.xcodeproj`。

## 文件

| 文件 | 职责 |
|------|------|
| `CarDashboardPhoneApp.swift` | SwiftUI 入口 |
| `ContentView.swift` | UI：**五个标签页**（仪表 / 数据 / 导航 / 照片 / 设置）|
| `DeviceFaces.swift` | 设备上报表盘列表的解析、分组与本地化（含图标映射）|
| `DriveSessionManager.swift` | 会话编排：定时以 2Hz 发送，不在 UI 帧发送 |
| `LocationService.swift` | Core Location 封装：授权、持续定位、`CLLocation` → 遥测 |
| `BLEClient.swift` | Core Bluetooth Central：扫描 `CarDashboard`、发现特征、写入 |
| `BLEConnectionPolicy.swift` | **自动连接决策**（纯函数，不依赖 CoreBluetooth）：何时扫描、断开是否重连、状态文案 |
| `TelemetryPacket.swift` | 遥测 Codable 结构（**字段名与固件严格一致**） |
| `Photo/PhotoTabView.swift` | 照片页：添加照片 / 上传队列 / 设备相册 / 播放设置 / 格式化 SD 卡（BLE 直传） |
| `Photo/PhotoEncoder.swift` | 裁切后的 CGImage → 466×466 baseline JPEG；视频取帧/抽帧（动图） |
| `Photo/PGJEncoder.swift` | JPEG → `.pgj`（32 B 头 + 整文件 CRC32）并写临时文件（Foundation-only） |
| `Photo/PgiEncoder.swift` | FULL 档动图 `.pgi` 打包（调色板 + 索引帧） |
| `Photo/PhotoCropView.swift` | 单张的圆屏裁切编辑（捏合缩放/拖动/旋转 90°） |
| `Photo/PhotoThumbnailCache.swift` | 设备照片列表的 96×96 缩略图缓存（Application Support） |
| `Photo/DevicePhotos.swift` | 设备照片能力/状态/清单/上传队列模型与解析（Foundation-only） |
| `Photo/PhotoProtocol.swift` | 照片服务 BLE 协议：UUID、PGJ 头、CRC32、命令/事件编解码（Foundation-only） |
| `Photo/PhotoTransfer.swift` | 上传状态机：分片 / 窗口 credit / 续传 / 看门狗（纯逻辑，可注入） |

## 协议

Service `7D2A0001-8F4A-4B7C-9D21-5E6A3B8C1000`；
Telemetry（Write）`7D2A0002-…`；Status（Read/Notify）`7D2A0003-…`。
JSON 字段与语义见 `../../docs/ble.md` 与 `TelemetryPacket.swift` 的 CodingKeys。

照片服务（`7D2A0010-…`）另有 5 个特征（Ctl/Data/Evt/Info/State），
字段与 `.pgj` 文件格式见 `../../docs/photo-protocol.md`（冻结契约）。

## 在 Xcode 中运行（**已生成可直接打开的工程**）

`CarDashboardPhone.xcodeproj` 已随仓库提交，**直接双击打开**即可（无需 xcodegen、无需手动建工程）：

1. 打开 `CarDashboardPhone.xcodeproj` → 选 target `CarDashboardPhone` → **Signing & Capabilities**
   选择你的 Team（Personal Team 即可）。
2. 若 Xcode 提示缺少 iOS 平台组件：**Xcode → Settings → Components** 下载 iOS 模拟器/设备支持
   （本机 Xcode 26.5 只有 SDK，未下载平台运行时，故命令行 `xcodebuild` 无可用 destination）。
3. 连接 iPhone SE 3（iOS 17.0.2），选中设备后 Run。

> 工程文件由 `project.yml`（XcodeGen）生成，改工程配置请改 `project.yml` 后执行
> `brew install xcodegen && xcodegen generate`，避免手改 pbxproj。
> Info.plist 的定位/蓝牙隐私键与 `UIBackgroundModes` 已由 `project.yml` 自动生成。

### 已验证的编译状态

| 项目 | 结果 |
|------|------|
| 6 个源文件 `swiftc -typecheck`（macOS SDK） | ✅ 通过 |
| **对 iOS 26.5 SDK / Swift 6.3 `-typecheck`（target ios17.0）** | ✅ 通过 |
| **模拟器版编译+链接（arm64-apple-ios17.0-simulator）** | ✅ 产出可执行文件 |
| **设备版编译+链接（arm64-apple-ios17.0）** | ✅ 通过 |
| Info.plist 隐私键 + 后台模式 | ✅ 已生成并核对 |
| **xcodebuild 完整构建（模拟器）** | ✅ BUILD SUCCEEDED |
| **App 启动运行无崩溃 + UI 渲染** | ✅ 截图像素验证 |
| **XCUITest 自动化（会话开/关 + 定位→编码→计数）** | ✅ 2 项通过（Freeway Drive 场景）|
| 真机运行 / 后台 / 锁屏 / CarPlay | ✅ **已真机验证**（iPhone 12 / iOS 27）见 `docs/test-results.md` |
| **照片直传（选图 → 上传 → 设备提交 → 相册/播放）** | ✅ 真机验收通过（2026-10-27，69,845 B） |
| **格式化 SD 卡（二次确认 → 进度 → 刷新）** | ✅ 真机验收通过（清空/重建坏卡） |

### 装到真机 / 每周重签

个人免费 Apple ID 的描述文件只有 **7 天**有效期，过期后 App 会提示“不再可用”，
重装一次即可恢复（数据保留）。一条命令完成构建 + 安装 + 续期：

```bash
tools/install_app.sh                 # 自动挑第一个在线设备
tools/install_app.sh <设备UUID>      # 指定设备（xcrun devicectl list devices）
```

脚本会从本机 Apple Development 证书自动识别 Team（`project.yml` 里刻意留空，避免提交个人 Team ID）；
也可 `DEVELOPMENT_TEAM=XXXX tools/install_app.sh` 显式覆盖。

## 后台（阶段 4）注意事项

- 后台定位与 BLE 能否持续，取决于权限、系统版本、App 状态；**强制退出 App 后 iOS 不保证继续发送**。
- 代码里 `CLLocationManager.activityType = .otherNavigation`、关闭自动暂停，是为导航场景优化；
  `requestBackground()` 会申请「始终」授权并开启 `allowsBackgroundLocationUpdates`。
- App/权限失效时，仪表端不会收到新数据 → 固件按 `BLE_STALE_MS`(2.5s) 自动把速度置无效（已实现）。

## 验证思路

- 先按 `tools/ble_client_test.swift` 或本 App 前台向仪表发一条含 `ts_utc_ms` 的包，
  固件串口应出现 `[BLE] v=1 速度=…` 与 `[同步] …UTC → RTC…`。
- 协议往返已在本机验证：Swift `JSONEncoder` 输出 → 固件 `telemetry.c` 解析结果完全一致
  （`v=1 ts=1791792803000 speed=19.03 course=120.0 location=1 → kmh=68.5`）。

## 仪表控制（App 端）

「仪表控制」分区（需仪表已连接）：

| 控件 | 作用 | BLE 命令 |
|------|------|---------|
| 表盘分段选择器 | 模拟时钟 / 数字时钟 / 速度表 | `{"cmd":"face","value":N}` |
| 亮度滑块 | 1-255（拖动结束才发送，避免高频写）| `{"cmd":"brightness","value":N}` |
| 屏幕点亮开关 | 息屏 / 亮屏 | `{"cmd":"screen","value":"on"\|"off"}` |

未连接时操作会给出明确提示（`lastNotice`），不会「点了没反应」。

XCUITest 覆盖：`DeviceControlUITests` 验证分区可达、分段控件含表盘选项、
滑块与开关可交互且不崩溃（模拟器无 BLE 外设，故只验证 UI 路径）。
