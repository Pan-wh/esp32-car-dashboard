# 软件架构与模块边界

> 状态：**已实现**（阶段 0-6 完成，含照片表盘）。进度与测试状态见 [`../PROGRESS.md`](../PROGRESS.md)
> 与 [`test-results.md`](test-results.md)。
> **要动手改代码？见 [`development-guide.md`](development-guide.md)（按意图查改哪里）。**

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

## 当前实现（2026-10-22）

```
simulator/ui/                 ← 唯一源码（模拟器与固件共用）
├── ui_pages.[ch]             页面管理（左右滑动切换）+ 表盘注册
├── ui_face.[ch]              表盘注册表（分组 / 启用掩码 / NVS）
├── face_report.[ch]          表盘列表上报（紧凑 JSON，6 个 ≈173 字节）
├── ui_clock.[ch]             数字时钟（iPhone 息屏风格）
├── ui_analog.[ch]            模拟指针时钟（lv_meter，跳秒/扫秒开关）
├── ui_color_clock.[ch]       彩色数字时钟
├── ui_speed.[ch]             速度表盘（驾驶员三态阈值 80/110 km/h）
├── ui_nav.c / ui_nav_map.c   导航卡片 + 地图窗口渲染
├── ui_photo.[ch]             照片表盘（仅"怎么显示"）
├── pgi.[ch]                  .pgi 容器（头/帧表/解码）
├── photo_slide.[ch]          轮播状态机（静态/动图混排、三开关、息屏）
├── img_indexed.[ch]          8bpp 索引图解码（烘焙图 + .pgi 共用）
├── ui_img_inflate.[ch]       索引图 → PSRAM RGB565 充气
├── ui_timesrc.[ch]           统一时间源（系统时间 / RTC / 冻结时间）
├── telemetry.[ch]            BLE 遥测解析/换算/平滑/合理性校验/行程统计
├── control.[ch]              设备控制命令解析（亮度/息屏/表盘/方向/照片）
├── nav.[ch] / nav_parse.[ch] 导航纯逻辑 / 消息解析
├── auto_bright.[ch] / bright_parse.[ch]  亮度时段曲线 / 曲线包解析
├── ui_dial_faces.c           烘焙表盘图片（tools/make_dial_faces.py 生成）
├── ui_driver_images.c        驾驶员三态图像（tools/make_images.py 生成）
└── font_*.c                  字模（tools/make_fonts.sh 生成）

simulator/                    ← 仅模拟器
└── main.c                    SDL2 绑定（显示/触摸/圆遮罩/截图/命令行）

firmware/dashboard/           ← 固件（Arduino）
├── dashboard.ino             官方初始化流程 + 装配共享代码（BLE/SD/照片）
├── ble_telemetry.cpp         BLE Peripheral + 跨 MTU 重组
└── （共享源码由 tools/sync_firmware.sh 同步，gitignore）

tests/                        ← PC 上运行的逻辑测试（C 12 套 + Swift 11 套 + Node）
tools/                        ← 资源与流程脚本
├── make_fonts.sh             字体管线（VF→实例化→子集化→字模）
├── make_images.py            PNG→LVGL 数组 + 临时底图
├── make_dial_faces.py        表盘底图烘焙
├── pgi.py                    PNG/GIF/视频 → .pgi
├── import_photos.sh          批量导入素材（HEIC/视频/EXIF/--fast）
├── ui_regression.py          UI 像素级回归（33 项）
└── sync_firmware.sh          共享源码同步到固件
```

**共享策略**：`simulator/ui/` 为唯一源码；模拟器直接编译它，固件由
`tools/sync_firmware.sh` 同步副本（Arduino 无法引用工程外源码）。
共享模块均为平台无关 C，头文件带 `extern "C"` 保护，可被 C++ 的 `.ino` 调用。

## 原则

1. 驱动层优先复用官方示例/配套库（`reference/.../examples/arduino/libraries`），只做薄封装。
2. 数据模块与 UI 模块解耦：数据结构定义共享，UI 只读数据。
3. **定位/速度无效时向 UI 输出明确状态位**（数据源现为 iPhone Core Location，
   见 docs/hardware.md），UI 负责呈现无效状态，**不伪造数据**。
4. 共用 I2C 总线只初始化一次，句柄传递（硬件约束，见 docs/hardware.md）。

## 可测核心逻辑（共享纯 C 层）

以下逻辑刻意放在 `simulator/ui/`（平台无关纯 C），由 `tests/` 在 PC 上单元测试，
固件与模拟器共用同一份实现，避免"固件一份、测试一份"的双份漂移：

| 模块 | 职责 | 测试数 |
|------|------|--------|
| `telemetry.c` | BLE 遥测解析/换算/平滑/合理性校验/状态机/行程统计 | 107 |
| `control.c` | 设备控制命令解析（亮度/息屏/表盘/方向/演示/照片）| 58 |
| `ui_face.c` | 表盘注册表（注册/查找/翻页语义/启用掩码）| 67 |
| `face_report.c` | 表盘列表上报（紧凑格式，含 <184 字节回归）| 93 |
| `nav.c` / `nav_parse.c` / `nav_map.c` | 导航逻辑 / 消息解析 / 地图窗口 | 76 / 92 / 66 |
| `pgi.c` / `img_indexed.c` | `.pgi` 容器 / 8bpp 索引图解码 | 92 / 28 |
| `photo_slide.c` | 照片轮播状态机 | 54 |
| `auto_bright.c` / `bright_parse.c` | 亮度曲线 / 曲线包解析 | 50 / 62 |

iOS 侧纯逻辑（Foundation/CoreLocation，**可在 macOS 本机直接跑**，无需 iOS 运行时）：
`tools/test_ios_logic.swift` 等 —— 协议字段名契约、nil 省略、质量门槛边界、
负速度不外发、精度差仍带经纬度、静止(0)为合法速度 | 59

UI 像素级回归（工具）：`tools/ui_regression.py` —— 渲染各页面/状态并断言设计不变量
（圆屏遮罩、时钟黑底与橙色秒针、V5 白底、五态配色、状态文字安全区留边）| 33

一键运行全部：`cd tests && make run`（C 845 + Swift 599 + Node 98 + UI 33 = **1575 项，全绿**；2026-10-22 实测）。

> UI 回归需要带 Pillow 的解释器；缺省会**优雅跳过**并提示
> `PYTHON=<带Pillow的python3> make run`。

固件侧 `ble_update()` 直接调用状态机（不再内联判断）；枚举取值与
`ui_speed_status_t` 一一对应，改动需同步（已在两处标注）。

---

> **合规基线（全项目适用）**：本项目仅用于**个人开发、学习与技术验证** ——
> **不装车、不商业化、不分发**。项目使用高德开放平台 Web 服务，其
> **服务协议 §3.4 禁止用于车载场景**；若将来需要装车、分发或商业化，
> 必须先取得书面授权或更换数据源。详见 [`nav-backend.md`](nav-backend.md) §0。
