# 软件架构与模块边界

> 状态：骨架（里程碑 0）。在里程碑 2 进入 UI 开发时细化。

## 目标

在官方驱动基础上分层封装，避免所有逻辑堆在一个文件（AI_CONTEXT.md 规则 8）。

## 计划模块（firmware/dashboard/）

```
firmware/dashboard/
├── dashboard.ino          # 入口：初始化顺序 + 主循环
├── app_display.*          # 屏幕初始化、LVGL 移植（封装官方驱动）
├── app_ui_*               # 各页面（时钟/车速/状态）与组件规范
├── app_rtc.*              # PCF85063 时间读取
├── app_gnss.*             # LC76G 定位/速度/航向数据解析
├── app_motion.*           # QMI8658 运动状态（辅助平滑）
├── app_storage.*          # microSD 轨迹记录（里程碑 4）
└── app_settings.*         # 设置项持久化
```

## 当前实现（2026-10-10）

```
simulator/ui/                 ← 唯一源码（模拟器与固件共用）
├── ui_pages.[ch]             页面管理（左右滑动切换）+ 点击目标修正
├── ui_clock.[ch]             数字时钟（iPhone 息屏风格）
├── ui_analog.[ch]            模拟指针时钟（lv_meter，跳秒/扫秒开关）
├── ui_speed.[ch]             速度表盘（V1-V4 变体 + V5 驾驶员表盘）
├── ui_timesrc.[ch]           统一时间源（系统时间 / RTC 提供者 / 冻结时间）
├── gnss_nmea.[ch]            NMEA 解析（纯 C，无 LVGL/硬件依赖）
├── ui_driver_images.c        驾驶员三态图像（由 tools/make_images.py 生成）
└── font_*.c                  字模（由 tools/make_fonts.sh 生成）

simulator/                    ← 仅模拟器
├── main.c                    SDL2 绑定（显示/触摸/圆遮罩/截图/命令行）
└── nmea_replay.[ch]          NMEA 日志回放（无硬件联调）

firmware/dashboard/           ← 固件（Arduino）
├── dashboard.ino             官方初始化流程 + 装配共享代码
└── （共享源码由 tools/sync_firmware.sh 同步，gitignore）

tests/                        ← PC 上运行的逻辑测试
└── test_nmea.c               NMEA 解析 44 项断言

tools/                        ← 资源与流程脚本
├── make_fonts.sh             字体管线（VF→实例化→子集化→字模）
├── make_images.py            PNG→LVGL 数组 + 临时底图
├── cut_driver_sheet.py       组图切图
├── prep_driver_png.py        单张素材预处理
├── make_nmea_track.py        合成 NMEA 轨迹
└── sync_firmware.sh          共享源码同步到固件
```

**共享策略**：`simulator/ui/` 为唯一源码；模拟器直接编译它，固件由
`tools/sync_firmware.sh` 同步副本（Arduino 无法引用工程外源码）。
共享模块均为平台无关 C，头文件带 `extern "C"` 保护，可被 C++ 的 `.ino` 调用。

## 原则

1. 驱动层优先复用官方示例/配套库（`reference/.../examples/arduino/libraries`），只做薄封装。
2. 数据模块与 UI 模块解耦：数据结构定义共享，UI 只读数据。
3. GNSS 无效/低速时向 UI 输出明确状态位，UI 负责呈现无效状态，不伪造数据。
4. 共用 I2C 总线只初始化一次，句柄传递（硬件约束，见 docs/hardware.md）。

## 可测核心逻辑（共享纯 C 层）

以下逻辑刻意放在 `simulator/ui/`（平台无关纯 C），由 `tests/` 在 PC 上单元测试，
固件与模拟器共用同一份实现，避免"固件一份、测试一份"的双份漂移：

| 模块 | 职责 | 测试数 |
|------|------|--------|
| `gnss_nmea.c` | NMEA 解析（休眠资产） | 44 |
| `telemetry.c` 解析 | BLE 遥测 JSON 解析 | 46+ |
| `telemetry.c` 校验 | `telemetry_plausible()` 超范围/自相矛盾 | 15 |
| `telemetry.c` 平滑 | `telemetry_smooth_step()` 限滞后平滑 | 9 |
| `telemetry.c` 状态机 | `telemetry_link_state()` §6.4 五态 + 并发 HOLD | 11 |

固件侧 `ble_update()` 直接调用状态机（不再内联判断）；枚举取值与
`ui_speed_status_t` 一一对应，改动需同步（已在两处标注）。
