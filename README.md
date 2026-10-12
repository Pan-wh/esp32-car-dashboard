# ESP32-S3 车载仪表项目

基于 **微雪 Waveshare ESP32-S3-Touch-AMOLED-1.75-G** 开发板的车载辅助显示设备。
项目定位：独立的车载仪表/信息显示器（非完整 CarPlay），强调稳定、清晰、低延迟、适合驾驶时快速读取。

## 项目状态

**当前：数据源为 iPhone Core Location + BLE（本机无 LC76G GNSS）。**
改造任务书见 `AI_TASK_IPHONE_BLE_DASHBOARD.md`，协议见 `docs/ble.md`，测试记录见 `docs/test-results.md`。

- [x] 目录结构与文档骨架
- [x] macOS 开发环境配置（Arduino IDE 2.3.10 + Arduino-ESP32 Core 3.3.10，见 `docs/setup-macos.md`）
- [x] 官方仓库克隆与版本记录（commit `e4344e7`，见 `reference/` 与 `docs/decisions.md`）
- [x] UI 模拟器（SDL2 + LVGL 8.4.0，`simulator/`，见其 README）
- [x] 数字时钟 v2：iPhone 充电息屏风格（模拟器已验证，`design/wireframes/clock.md`）
- [x] 模拟指针时钟 v1：极简风格，刻度环 + 三针 + 橙色秒针（逐点验证，`design/wireframes/analog-clock.md`）
- [x] 页面切换：左右**滑动**分页（滚动容器 + 吸附，真机通过）
- [x] 官方示例 01_HelloWorld / 03_LVGL_PCF85063_simpleTime 烧录验证（**真机通过**）
- [x] 速度表盘 V5（驾驶员三态 + 白底 + 速度渐变，像素级验证）
- [x] **阶段1 BLE 最小验证**（真机+macOS 联调）：广播 `CarDashboard`、接收并**跨 MTU 重组**解析 JSON、
      线程安全、数据过期置无效
- [x] **阶段2 时间同步**：BLE `ts_utc_ms` → 系统时钟 + PCF85063 RTC（UTC+8），屏幕 `TIME SYNC→SYNCED`
- [x] **阶段3 iPhone 前台 App**：`ios/CarDashboardPhone/`（SwiftUI + Core Location + Core Bluetooth）；
      已生成 `.xcodeproj`，**对 iOS 26.5 SDK/Swift 6.3 编译+链接通过**（模拟器版与设备版）；未真机运行
- [x] **阶段4 后台代码**：`requestBackground()`（始终授权 + 后台定位，`#if os(iOS)` 门控）；未实机验证
- [x] **阶段5（部分）**：速度页五态 `BT OFF / WAIT GPS / NO FIX / STALE / LIVE`（真机逐态实测）；
      固件连续运行 21min+ 无崩溃、空闲堆平稳
- [ ] **真机运行 iPhone App**（需 Xcode 下载 iOS 平台组件 + 签名 + 连接 iPhone SE 3）
- [ ] 后台 / 锁屏 / CarPlay / 30min 整链路验收（任务书 §9，需实机驾驶，见 `docs/test-results.md`）
- [x] GNSS NMEA 解析器与 44 项单测（本机无 GPS 模块，作为**休眠资产**保留，当前非活动数据源）

## 目录结构

```
esp32-car-dashboard/
├── README.md            # 本文件
├── AI_CONTEXT.md        # 提供给 AI 的长期项目上下文（务必先读）
├── docs/                # 硬件、环境、架构、UI 规范、决策与排障记录
├── design/              # 线框图与设计资源（含许可说明）
├── firmware/dashboard/  # 实际固件工程（Arduino）
├── reference/           # 微雪官方仓库（gitignore，按需克隆，独立参考依赖）
├── tests/               # 可自动化测试
└── tools/               # 辅助脚本
```

## 开发环境

| 工具 | 版本 | 说明 |
|------|------|------|
| macOS | Apple Silicon / pwh 主机 | 开发主机 |
| Arduino IDE | 2.3.10 | 主构建/烧录工具 |
| Arduino-ESP32 Core | 3.3.10（espressif） | 官方仓库要求的版本 |
| Git | 2.39.2 | 版本管理 |
| VS Code | 已安装 | 代码编辑/浏览 |

详细安装与烧录步骤见 [`docs/setup-macos.md`](docs/setup-macos.md)。

## 关键约定

- 开发规则、优先级、里程碑定义见 `AI_CONTEXT.md`。
- 硬件引脚与总线信息以官方文档为准，见 `docs/hardware.md`。
- 关键技术决策及原因记录在 `docs/decisions.md`。
- 每个可运行阶段通过编译+硬件验证后建立 Git commit。

## 官方资料

- 官方仓库：<https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75>
- 官方文档：<https://docs.waveshare.net/ESP32-S3-Touch-AMOLED-1.75/>
- 官方快速开始：<https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75/blob/main/docs/getting-started_ZH.md>
