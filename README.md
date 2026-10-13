# ESP32-S3 车载仪表项目

基于 **微雪 Waveshare ESP32-S3-Touch-AMOLED-1.75-G** 开发板的车载辅助显示设备。
项目定位：独立的车载仪表/信息显示器（非完整 CarPlay），强调稳定、清晰、低延迟、适合驾驶时快速读取。

## 项目状态

**当前：数据源为 iPhone Core Location + BLE（本机无 LC76G GNSS）。**

> 📌 **先看 [`PROGRESS.md`](PROGRESS.md)** —— 项目进度总览（单一事实来源）：
> 做到哪一步、还差什么、已知问题、构建命令、串口命令速查。

改造任务书见 [`AI_TASK_IPHONE_BLE_DASHBOARD.md`](AI_TASK_IPHONE_BLE_DASHBOARD.md)，
协议见 [`docs/ble.md`](docs/ble.md)，测试记录见 [`docs/test-results.md`](docs/test-results.md)。

- [x] 目录结构与文档体系（进度见 [`PROGRESS.md`](PROGRESS.md)，测试台账见 [`docs/test-results.md`](docs/test-results.md)）
- [x] macOS 开发环境配置（Arduino IDE 2.3.10 + Arduino-ESP32 Core 3.3.10，见 `docs/setup-macos.md`）
- [x] 官方仓库克隆与版本记录（commit `e4344e7`，见 `reference/` 与 `docs/decisions.md`）
- [x] UI 模拟器（SDL2 + LVGL 8.4.0，`simulator/`，见其 README）
- [x] 数字时钟 v2：iPhone 充电息屏风格（模拟器已验证，`design/wireframes/clock.md`）
- [x] 模拟指针时钟 v1：极简风格，刻度环 + 三针 + 橙色秒针（逐点验证，`design/wireframes/analog-clock.md`）
- [x] 页面切换：左右**滑动**分页（滚动容器 + 吸附，真机通过）
- [x] 官方示例 01_HelloWorld / 03_LVGL_PCF85063_simpleTime 烧录验证（**真机通过**）
- [x] 速度表盘 V5（驾驶员三态 + 白底 + 速度渐变，像素级验证）
- [x] 滑页性能优化：单帧 243ms → **90-113ms**（烘焙表盘 + QSPI 调优 + 字体去抗锯齿）
- [x] 静态表盘烘焙成图片（刻度/数字离线渲染，运行时只留指针）
- [x] **阶段1 BLE 最小验证**（真机+macOS 联调）：广播 `CarDashboard`、接收并**跨 MTU 重组**解析 JSON、
      线程安全、数据过期置无效
- [x] **阶段2 时间同步**：BLE `ts_utc_ms` → 系统时钟 + PCF85063 RTC（UTC+8），屏幕 `TIME SYNC→SYNCED`
- [x] **阶段3 iPhone 前台 App**：`ios/CarDashboardPhone/`（SwiftUI + Core Location + Core Bluetooth）；
      已生成 `.xcodeproj`；**已在 iPhone 12（iOS 27）真机安装运行**，BLE 连接与遥测链路验证通过
- [x] **阶段4 后台代码**：`requestBackground()`（始终授权 + 后台定位，`#if os(iOS)` 门控）；未实机验证
- [x] **阶段5（部分）**：速度页五态 `BT OFF / WAIT GPS / NO FIX / STALE / LIVE`（真机逐态实测）；
      固件连续运行 21min+ 无崩溃、空闲堆平稳
- [x] **真机运行 iPhone App**（iPhone 12 / iOS 27，签名与安装通过）
- [ ] 后台 / 锁屏 / 30min 整链路验收（任务书 §9，需实机驾驶，见 `docs/test-results.md`）
- [x] 设备控制（亮度/息屏）+ 可更换表盘系统（BLE 端到端验证通过）
- [x] 航向显示 + 行程统计（里程/最高速/均速）

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

## 设备功能

| 功能 | 入口 | 说明 |
|------|------|------|
| 亮度调节 | BLE `{"cmd":"brightness","value":0-255}` / 串口 `bright <n>` | 0 视为最低可见亮度（避免全屏黑误判死机）|
| 息屏 / 亮屏 | BLE `{"cmd":"screen","value":"off"\|"on"}` / 串口 `screen on\|off` | 仅关显示，触摸仍可唤醒（由固件决定）|
| 航向显示 | 遥测 `course_deg` 自动驱动 | 速度页底部显示「NE 45°」（选在指针扫不到的扇区）|
| 行程统计 | 串口 `trip` / `trip reset` | 里程、最高速、加权均速；**仅链路 LIVE 且速度有效时累积**（失效不计入）|
| 表盘切换 | BLE `{"cmd":"face","value":N}` | 命令通路已就绪，多表盘实现中（功能③）|
