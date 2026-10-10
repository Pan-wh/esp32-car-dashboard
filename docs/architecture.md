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

## 原则

1. 驱动层优先复用官方示例/配套库（`reference/.../examples/arduino/libraries`），只做薄封装。
2. 数据模块与 UI 模块解耦：数据结构定义共享，UI 只读数据。
3. GNSS 无效/低速时向 UI 输出明确状态位，UI 负责呈现无效状态，不伪造数据。
4. 共用 I2C 总线只初始化一次，句柄传递（硬件约束，见 docs/hardware.md）。
