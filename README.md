# ESP32-S3 车载仪表项目

基于 **微雪 Waveshare ESP32-S3-Touch-AMOLED-1.75-G** 开发板的车载辅助显示设备。
项目定位：独立的车载仪表/信息显示器（非完整 CarPlay），强调稳定、清晰、低延迟、适合驾驶时快速读取。

## 项目状态

当前处于 **里程碑 0：建立工程**（见 `AI_CONTEXT.md` 第 9 节）。

- [x] 目录结构与文档骨架
- [x] macOS 开发环境配置（Arduino IDE 2.3.10 + Arduino-ESP32 Core 3.3.10，见 `docs/setup-macos.md`）
- [x] 官方仓库克隆与版本记录（commit `e4344e7`，见 `reference/` 与 `docs/decisions.md`）
- [x] 01_HelloWorld 编译验证通过（仅编译，未烧录）
- [x] UI 模拟器（SDL2 + LVGL 8.4.0，`simulator/`，见其 README）
- [x] 数字时钟 v2：iPhone 充电息屏风格（模拟器已验证，`design/wireframes/clock.md`）
- [x] 模拟指针时钟 v1：极简风格，刻度环 + 三针 + 橙色秒针（逐点验证，`design/wireframes/analog-clock.md`）
- [x] 页面切换：左右滑动（模拟时钟 ⇄ 数字时钟，`simulator/ui/ui_pages.c`）
- [x] 官方示例 01_HelloWorld 烧录验证（**真机通过**：屏幕显示 + 串口每 200ms 输出）
- [x] 官方示例 03_LVGL_PCF85063_simpleTime 烧录验证（**真机通过**：LVGL 渲染 + RTC 每秒走时）
- [x] 本工程固件烧录运行（**真机通过**：3 页 UI、串口命令、RTC 校时）
- [x] **数据源切换为 iPhone Core Location + BLE**（本机无 LC76G GNSS）：阶段 1 完成——
      固件广播 `CarDashboard`、接收并重组解析 JSON 遥测、线程安全、数据过期置无效；
      **真机+macOS 联调通过**（发现/连接/写入 203 字节/解析 68.5 km/h，见 `docs/ble.md`）
- [ ] 官方示例 03_LVGL_PCF85063_simpleTime 烧录验证（RTC）
- [ ] 官方示例 09_LC76G_I2C 烧录验证（GNSS）
- [x] 速度表盘 V5（驾驶员三态 + 白底 + 速度渐变，像素级验证）
- [x] GNSS 数据链路：NMEA 解析（44 项单测通过）+ 日志回放端到端验证（见 `docs/gnss.md`）
- [ ] GNSS 硬件读取层（I2C，待板子到货）
- [x] 固件工程骨架 `firmware/dashboard/`：官方初始化流程 + 共享 UI/解析代码，
      **编译通过**（Flash 40% / RAM 36%），未烧录（见 firmware/dashboard/README.md）
- [ ] 速度表盘页设计 + 主界面原型

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
