# ESP32-S3 车载仪表项目

基于 **微雪 Waveshare ESP32-S3-Touch-AMOLED-1.75-G** 开发板的车载辅助显示设备。
项目定位：独立的车载仪表/信息显示器（非完整 CarPlay），强调稳定、清晰、低延迟、适合驾驶时快速读取。

## 项目状态

当前处于 **里程碑 0：建立工程**（见 `AI_CONTEXT.md` 第 9 节）。

- [x] 目录结构与文档骨架
- [x] macOS 开发环境配置（Arduino IDE 2.3.10 + Arduino-ESP32 Core，见 `docs/setup-macos.md`）
- [x] 官方仓库克隆与版本记录（见 `reference/` 与 `docs/setup-macos.md`）
- [ ] 官方示例 01_HelloWorld 烧录验证（屏幕）
- [ ] 官方示例 03_LVGL_PCF85063_simpleTime 烧录验证（RTC）
- [ ] 官方示例 09_LC76G_I2C 烧录验证（GNSS）
- [ ] 主界面原型（时钟/车速表盘）

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
