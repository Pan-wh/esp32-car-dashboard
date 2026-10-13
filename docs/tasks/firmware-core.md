# 任务模式：改固件主流程 / 硬件

> **适用**：改初始化流程、主循环调度、硬件外设（屏幕/触摸/RTC/IMU）、BLE 传输层。
> **推荐模型**：嵌入式能力，**风险最高，改动需最谨慎**。
> **预计 Token**：中高（`dashboard.ino` 约 1300 行）

---

## 0. 一句话边界

> **只改 `firmware/dashboard/dashboard.ino` 与 `ble_telemetry.cpp`。
> 不要改固件目录里的 `ui_*.c` / `telemetry.*` / `control.*`（那是同步产物）。**

---

## 1. 固件工程结构

```
firmware/dashboard/
├── dashboard.ino            ← 固件本体（可改）
├── ble_telemetry.cpp/.h     ← BLE 传输层（可改）
│
├── ui_*.c/h                 ┐
├── telemetry.c/h            │ 同步产物！
├── control.c/h              │ 由 tools/sync_firmware.sh 从
├── font_*.c                 │ simulator/ui/ 复制而来（已 gitignore）
├── ui_dial_faces.c          │ ❌ 改这里 = 改错地方 + git 看不到
└── ui_driver_images.c       ┘
```

**判断方法**：若同名文件在 `simulator/ui/` 也存在 → 它是同步产物，**不要改**。

---

## 2. 🟢 你可以改的

### `dashboard.ino` 区域导航

| 区域 | 内容 | 典型需求 |
|------|------|---------|
| `setup()` | 官方初始化（屏幕/触摸/RTC/IMU）+ BLE 装配 | 加外设初始化 |
| `loop()` | 主循环调度 | 调整任务频率/顺序 |
| `lvgl_init()` | LVGL 缓冲配置（PSRAM 全帧）| 缓冲策略 |
| `ble_update()` | 状态机 + 指针驱动（50Hz 限频）| 数据→UI 的映射 |
| `ble_poll()` | 读取 BLE 暂存数据 | 消费节奏 |
| `control_poll()` | 下行控制命令处理 | 新增控制项 |
| `handle_serial()` | 串口命令 | 加调试命令 |
| `apply_brightness()` / `apply_screen()` | 硬件副作用 | 亮度/息屏实现 |
| 触摸采样 | 100Hz esp_timer + 按下锁存 | 触摸行为 |

### `ble_telemetry.cpp`
- UUID、特征属性、GATT 事件回调
- 线程安全（互斥锁）
- 数据暂存与取出

---

## 3. 🔴 你不能改的

| 禁区 | 原因 |
|------|------|
| 固件目录里的 `ui_*.c/h`、`telemetry.*`、`control.*`、`font_*.c` | **同步产物**，改 `simulator/ui/` 才对 |
| `simulator/ui/**` | 共享层，另开任务 |
| `ios/**` | App，另开任务 |
| `tools/`、`tests/`、`docs/` | 工具与文档 |

---

## 4. 📐 必须遵守的契约（每条都有踩坑背景）

### 4.1 ⚠️ 不要动 `lv_disp_drv_register()`

**真实事故**：一次自动化补丁误删了 `lv_disp_drv_register(&disp_drv);`，
导致 `lv_scr_act()` 返回 NULL，进而 `obj->spec_attr`（偏移 8）被解引用
→ **启动 panic 循环，且需 BOOT+RESET 才能恢复**。

初始化顺序（`setup()` 内）不要调整：
```
屏幕初始化 → lv_init() → lv_disp_drv_register() → PSRAM 缓冲配置 → UI 构建
```

### 4.2 ⚠️ BLE 回调里不能碰硬件

Bluedroid 任务不能操作显示。回调**只解析+存变量**，副作用放主循环：

```c
/* ble_telemetry.cpp */
static void on_write(...) {
    control_parse(data, len, &s_pending_cmd);   /* ✅ 只存 */
    /* ❌ 不要 lv_*() / 不要操作 GPIO */
}
```
```c
/* dashboard.ino 主循环 */
void loop() {
    control_cmd_t c;
    if (ble_telemetry_take_command(&c)) { apply_brightness(...); }  /* ✅ 这里做 */
}
```

### 4.3 ⚠️ 主循环不能阻塞

`loop()` 每轮必须快速返回（当前约 199 次/秒）。**不要**在循环里 `delay()`、
忙等、或做长耗时操作——那会：
- 饿死 LVGL 的 `lv_timer_handler()` → 时钟不走、动画卡住
- 触摸采样失效

耗时操作应拆分为状态机分帧执行。

### 4.4 ⚠️ 触摸采样是独立的 100Hz 中断源

触摸用 `esp_timer` 以 **100Hz** 独立采样（不是每帧读 I2C），
并有**按下锁存**保证 LVGL 不会漏掉按下事件。

**背景**：曾因每帧（200ms）才读一次触摸，快速滑动整个"按下→抬起"
落在一次渲染内 → LVGL 感知不到触摸（用户报"操作无反应"）。
不要改回"每帧读触摸"。

### 4.5 ⚠️ UI setter 已有变更检测，不要绕过

`ui_speed_set_*` 等 setter 内部做了变更检测（LVGL 8 的 setter 无条件 invalidate）。
调用它们**不会**造成无效化风暴。但**新增**周期性调用时要注意：

```c
/* ✅ 安全：setter 内部有检测 */
ui_speed_set_heading(deg);
```
若新增直接操作 LVGL 对象的周期代码，**必须自己比较新旧值**。

### 4.6 保持数据失效语义
数据过期/断连时**必须**让指针归零，不得残留旧值（§6.3）：
```c
else {
    telemetry_ramp_reset(&s_ramp);
    ui_speed_set_fix(false, 0);    /* ★ 归零，不显示旧值 */
}
```

### 4.7 不要引入新库
Arduino 只编译 sketch 目录，且 Flash 已用 75%（上限 3MB）。
新增依赖需评估体积。

### 4.8 时间同步语义
已改为「**首次校时 + 仅偏差 >30s 才重校**」。
不要改回"收到就校时"——那会因秒/毫秒精度不匹配导致**频繁重写 RTC**
（真机表现为时钟界面隔几秒重置一下）。

---

## 5. 构建与烧录

```bash
# ⚠️ 必须先同步（若改过 simulator/ui/）
tools/sync_firmware.sh

# ⚠️ 不要复用 --build-path（跨文件改动会漏编译，已踩坑）
arduino-cli compile \
  --fqbn "esp32:esp32:esp32s3:FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,USBMode=hwcdc,CDCOnBoot=cdc,PSRAM=opi" \
  --build-path /tmp/build_$(date +%s) \
  --build-property "compiler.cpp.extra_flags=-DESP32QSPI_MAX_PIXELS_AT_ONCE=4096" \
  --libraries reference/ESP32-S3-Touch-AMOLED-1.75/examples/arduino/libraries \
  --upload -p /dev/cu.usbmodem101 \
  firmware/dashboard
```

**编译参数不可省**：
| 参数 | 原因 |
|------|------|
| `PSRAM=opi` | **必需** —— S3R8 八线 PSRAM，全帧绘制缓冲依赖它 |
| `-DESP32QSPI_MAX_PIXELS_AT_ONCE=4096` | 滑页性能优化，省了会掉帧 |
| `PartitionScheme=app3M_fat9M_16MB` | 应用 3MB + 9.9MB 未用分区 |

**占用基线**：Flash 75%、RAM 38%（改动后应贴近，明显上涨要说明原因）。

---

## 6. 验证要求

```bash
# 1) 编译 + 烧录（上面命令）

# 2) 串口冒烟（必需）
#    连 115200 后输入：
help        # 确认命令都在
ble         # 链路/遥测统计
mem         # 内存（确认无异常）
face        # 表盘切换正常
spd 60      # 诊断：强制速度 60，看指针
trip        # 行程统计

# 3) 心跳观察（30 秒，确认无崩溃、循环速率正常）
#    预期：循环 ~199/s，空闲堆 >120KB，无 reboot

# 4) 共享层单测（若同时改了共享层）
cd tests && make run PYTHON=<python路径>
```

**冒烟必须确认**：
- 屏幕正常点亮显示
- 触摸能翻页
- 串口无 panic / 无重启（`ble` 会显示复位原因）
- 空闲堆无异常下降

---

## 7. 产出格式

```markdown
## 改动清单
| 文件 | 改动 |
|------|------|
| firmware/dashboard/dashboard.ino | loop() 新增 XX 轮询 |

## 边界检查
- [ ] 未改固件目录里的同步产物（ui_*/telemetry.*/control.*/font_*）
- [ ] 未调整 lv_disp_drv_register() 相关初始化顺序
- [ ] BLE 回调中未操作硬件
- [ ] 主循环未引入阻塞
- [ ] 未改回"每帧读触摸"
- [ ] 数据失效仍归零
- [ ] 未引入新库

## 验证结果
| 步骤 | 结果 |
|------|------|
| 编译 | ✅ Flash __% / RAM __% |
| 烧录 | ✅ |
| 串口冒烟 | ✅（help/ble/mem/face 正常）|
| 心跳 30s | ✅ 循环 __/s，堆 __ KB，无重启 |

## 未验证 / 需人工确认
- 真机长时间稳定性（需 30 分钟长稳，见 test-results.md #12）
```

---

## 8. 故障恢复提示

若烧录后**启动 panic 循环**（串口刷错误、屏幕不亮）：
1. 按住 **BOOT** 再按 **RESET**，松开 RESET 后松开 BOOT → 进下载模式
2. 重新烧录上一版可用固件

> 不要试图用 esptool 反复重试同步——若板子未进下载模式会一直失败。

---

## 9. 省 Token 提示

**只需读**：
- 本文件
- `firmware/dashboard/dashboard.ino` 的**目标函数**（不要通读 1300 行）
- 需要时：`simulator/ui/<module>.h`（看接口签名）

**不要读**：`simulator/ui/ui_*.c`（绘制实现）、`ios/`、`reference/`（官方示例）。
