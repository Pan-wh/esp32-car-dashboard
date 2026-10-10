# 技术决策记录

记录本项目关键决策及原因。每条决策注明日期、依据与影响。

## D-001 主构建路线：Arduino IDE + Arduino-ESP32 Core

- **日期**：2026-10-10（里程碑 0）
- **决策**：以 Arduino IDE 2.3.10 作为主构建/烧录工具，Arduino-ESP32 Core **3.3.10**（espressif）作为平台。
- **原因**：
  1. `AI_CONTEXT.md` 建议初期降低环境复杂度，先跑通官方示例。
  2. 官方仓库（commit `e4344e7`）的 Arduino 示例明确验证于 Core 3.3.10。
  3. 微雪官方 Arduino 示例覆盖屏幕/触摸/RTC/IMU/GNSS/SD 全部所需外设，可直接增量开发。
- **影响**：不引入 ESP-IDF、PlatformIO 等第二套构建体系；若日后切换框架，先评估迁移成本再行动。
- **状态**：已定。

## D-002 官方仓库作为独立参考依赖

- **日期**：2026-10-10
- **决策**：微雪官方仓库克隆至 `reference/ESP32-S3-Touch-AMOLED-1.75`，**不复制进固件目录**；版本记录于本文件与 `docs/setup-macos.md`。
- **原因**：保持业务固件干净；官方仓库更新时可单独同步对比。
- **版本**：commit `e4344e70c2fa78a13e8a06566507f1ba8af6672a`（2026-08-21，"Refresh ESP-IDF v5.5 compatibility matrix (#21)"）。
- **状态**：已定。`.gitignore` 已忽略 `reference/`，换新机器按 `docs/setup-macos.md` 重新克隆。

## D-003 编译配置：16MB Flash + app3M_fat9M_16MB 分区

- **日期**：2026-10-10
- **决策**：FQBN 统一为
  `esp32:esp32:esp32s3:FlashSize=16M,PartitionScheme=app3M_fat9M_16MB`
- **原因**：官方快速开始明确要求该组合（Flash 16MB、分区 `app3M_fat9M_16MB`）；与官方 Release 固件布局一致，避免混用布局导致启动问题。
- **状态**：已定。所有示例验证与业务固件均使用此配置。

## D-004 配套库使用方式：随仓库 `examples/arduino/libraries`

- **日期**：2026-10-10
- **决策**：编译示例/固件时用 `--libraries reference/ESP32-S3-Touch-AMOLED-1.75/examples/arduino/libraries` 指定随仓库配套库，保证版本配套。
- **原因**：官方文档明确建议；避免与全局库副本版本冲突（若 IDE 内有冲突的全局副本，删除或降低其优先级）。
- **状态**：已定。后续业务固件如需拆分依赖，再评估将配套库复制到独立目录并记录。

## D-005 UI 框架：LVGL

- **日期**：2026-10-10（初定）
- **决策**：图形界面使用 LVGL（版本随官方配套库，不单独升级）。
- **原因**：官方示例均基于 LVGL，触摸、圆形屏适配已有现成代码；单独升级 LVGL 版本会带来与板级驱动的不兼容风险。
- **状态**：初定，随里程碑 2 落实。

## 待决策事项

- 里程碑 5 的地图/导航技术方案（对比后再定，不提前实施）。
- 轨迹存储格式（GPX / 自定义 CSV，里程碑 4 时评估）。
