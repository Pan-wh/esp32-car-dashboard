# 任务模式：只改 UI（外观 / 布局 / 配色 / 字体）

> **适用**：想调整界面的视觉表现，不改任何逻辑、协议、硬件行为。
> **推荐模型**：图形/视觉能力强的模型。
> **预计 Token**：低（只需读 2-4 个文件，见下）

---

## 0. 一句话边界

> **只改 `simulator/ui/ui_*.c` 的绘制代码与样式常量。
> 不改任何 `.h`、不改 `firmware/`、不改 `ios/`、不改业务逻辑文件。**

---

## 1. 🟢 你可以改的文件

### 按需求选（通常只碰其中一个）

| 需求 | 文件 | 行数 |
|------|------|------|
| 速度表（指针、表盘、底部 HUD）| `simulator/ui/ui_speed.c` | 520 |
| 模拟指针时钟 | `simulator/ui/ui_analog.c` | 208 |
| 数字时钟 | `simulator/ui/ui_clock.c` | 99 |
| 页面容器（背景色、翻页动画时长）| `simulator/ui/ui_pages.c` | 253 |

### 需要重新生成资源时（改刻度/数字/字体外观）

| 需求 | 改这个工具然后重新生成 |
|------|---------------------|
| 表盘刻度与数字的外观 | `tools/make_dial_faces.py` |
| 字体（字号/字重）| `tools/make_fonts.sh` |
| 驾驶员插画 | `tools/make_images.py` |

> ⚠️ 生成物 `font_*.c`、`ui_dial_faces.c`、`ui_driver_images.c`
> **不要手改**——它们是工具产物，手改会在下次生成时丢失。

---

## 2. 🔴 你绝对不能改的（越界即失败）

| 禁区 | 具体 |
|------|------|
| **头文件签名** | `simulator/ui/*.h` 里的函数签名、结构体、枚举、`#define` 对外常量 |
| **固件同步产物** | `firmware/dashboard/` 下的 `ui_*.c/h`、`telemetry.*`、`control.*`、`font_*.c` —— 这些是 `sync_firmware.sh` 从 `simulator/ui/` 复制的，改了会被覆盖 |
| **固件本体** | `firmware/dashboard/dashboard.ino`、`ble_telemetry.cpp` |
| **iOS App** | `ios/**` 全部 |
| **业务逻辑** | `simulator/ui/telemetry.c`、`control.c`、`ui_face.c` |
| **测试** | `tests/**`（除非任务明确要求补测试）|
| **构建/工程配置** | `project.yml`、`.gitignore`、`Makefile`（除明确要求）|
| **其它工具** | `tools/` 下非上述三个生成器 |

### 为什么这么严

`simulator/ui/` 是**模拟器与固件共用的唯一源码**。你在这里改的东西，
会同时影响桌面模拟器和真实硬件。头文件则是**模拟器 ↔ 固件 ↔ iOS App 的接口合同**，
改签名会让三处一起编译失败。

---

## 3. 📐 必须遵守的 UI 契约

### 3.1 屏幕与坐标

```c
/* ⚠️ 注意：LCD_WIDTH/HEIGHT 定义在 ui_pages.c 内部（第 41 行），不是共享头文件。
   其它 .c 文件里若要硬编码，直接用字面量 466，或自己定义局部常量。 */
#define LCD_WIDTH  466   /* 圆屏直径 */
#define LCD_HEIGHT 466
/* 圆心 (233, 233)，半径 233 */
```

**圆屏安全区**：内容不要超出半径 233 的圆。四角（距圆心 > 233 的区域）不可见。

| 半径参考 | 用途 |
|---------|------|
| 0 – 139 | 中央插画区（速度表：驾驶员 230×230，最大内容半径 139）|
| 168 | 指针内端半径（`MARKER_INNER_R`）|
| ~204 | 刻度内缘 |
| 233 | 屏幕边缘 |

### 3.2 调色板（改配色优先用这些常量，勿写裸十六进制）

**速度表（`ui_speed.c`，白底表盘）**：
```c
#define C_BG_LIGHT    0xFFFFFF /* 表盘底：纯白 */
#define C_TICK_MIN_L  0x9A9A9E /* 小刻度：中灰 */
#define C_TICK_MAJ_L  0x4A4A50 /* 大刻度：深灰 */
#define C_LABEL_L     0x3A3A40 /* 刻度数字：深灰 */
#define C_TEXT_L      0x1A1A1E /* 主文字：近黑 */
#define C_TEXT_DIM_L  0x6A6A70 /* 次文字：中灰 */
#define C_NEEDLE_L    0x2A2A2E /* 指针：深炭灰 */
#define C_BAD_L       0xB0B0B6 /* 无效/置灰 */
#define C_WARN        0xFF9500 /* 超速提醒：橙 */
```

**黑底页面（时钟类）**：
```c
#define C_BG      0x000000 /* 纯黑背景（OLED 省电）*/
#define C_MAIN    0xF5F5F7 /* 主数据：白 */
#define C_DIM     0x8A919E /* 次要文字：中灰 */
#define C_TRACK   0x26262A /* 轨道/未点亮段：暗灰 */
#define C_ACCENT  0x0A84FF /* 强调色（蓝）*/
#define C_LABEL   0xC8C8CE /* 刻度数字：浅灰 */
```

### 3.3 ⚠️ 性能铁律：setter 必须做变更检测

LVGL 8 的 setter（含 `lv_meter_set_indicator_value`）**无条件 invalidate**，
即使值没变也会把区域标脏。若不检测，空闲时也会以几十 Hz 重绘 → **全局卡顿**
（真机实测：55.5 次/秒 → 修复后 2.6 次/秒）。

**如果你新增任何"被外部周期性调用的 setter"，必须照此模式写**：

```c
void ui_xxx_set_something(float v)
{
    if (v == s_something) return;   /* ★ 变更检测：值相同直接返回 */
    s_something = v;
    apply_state();
}
```

同理，**标签文本**也要避免重复 `lv_label_set_text`：

```c
static int shown = -1;
if ((int)new_state != shown) {
    shown = (int)new_state;
    lv_label_set_text(label, text);   /* 只在文本变化时才设 */
}
```

### 3.4 性能预算（改 UI 时不要突破）

| 项 | 现状 | 说明 |
|----|------|------|
| 滑页单帧 | 90–113 ms | QSPI 传一屏 434KB 就需 43ms，是硬件下限 |
| 全屏传输 | 43 ms | 不要引入需要更频繁全屏刷新的效果 |
| 屏幕 | 466×466 RGB565 | 可用 `lv_color_t` |

**避免的做法**：每帧全屏重绘、大量小对象（每个都有独立重绘开销）、
运行时绘制复杂矢量（刻度已烘焙成图片，勿改回运行时绘制）。

### 3.5 扫表动画接口（速度表）

切到速度表时会自动触发一次自检扫表（0 → 满量程 → 回落）。
动画期间 `s_sweeping` 为真，**会屏蔽数据驱动的 `ui_speed_set_speed()`**。
若你新增了受速度驱动的元素，注意在扫表期间也应跳过更新（保持视觉一致）。

---

## 4. 验证要求（改完必须跑，并贴出结果）

```bash
# 1) 编译（必须过）
cd simulator && make -j8

# 2) UI 像素级回归（10 项，改 UI 必跑）
python3 tools/ui_regression.py

# 3) 全部单测（207 项，确认没碰坏逻辑）
cd tests && make run PYTHON=<python路径>

# 4) 同步到固件 + 编译（确保真机可用）
tools/sync_firmware.sh
arduino-cli compile \
  --fqbn "esp32:esp32:esp32s3:FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,USBMode=hwcdc,CDCOnBoot=cdc,PSRAM=opi" \
  --build-property "compiler.cpp.extra_flags=-DESP32QSPI_MAX_PIXELS_AT_ONCE=4096" \
  --libraries reference/ESP32-S3-Touch-AMOLED-1.75/examples/arduino/libraries \
  firmware/dashboard
```

**截图自检**（推荐，能直接看到效果）：
```bash
cd simulator
./dashboard_sim --offscreen --ms 500 --page 2 --speed 68 --shot /tmp/preview.bmp
```
（`--page 0`=模拟时钟 1=数字时钟 2=速度表；`--speed` 只对速度表有效）

> `<python路径>` 需带 Pillow。本机为
> `/Users/pwh/.dsh/dsh-runtimes/dsh-primary-runtime/dependencies/python/bin/python3`

---

## 5. 你可以安全调用的接口（不要改签名）

这些函数**已经实现且被固件调用**，你只需调用，不要改：

```c
/* 速度表 —— ui_speed.h */
lv_obj_t *ui_speed_create(lv_obj_t *parent);
void ui_speed_set_speed(float kmh);
void ui_speed_set_fix(bool valid, int sats);
void ui_speed_set_driver_state(int state);   /* UI_DRIVER_AUTO/NORMAL/SPIRITED/SPEEDING */
void ui_speed_set_heading(int deg);
void ui_speed_set_trip(float distance_km, float max_kmh);
void ui_speed_set_status(ui_speed_status_t st);
void ui_speed_sweep(void);                   /* 触发扫表动画 */

/* 模拟时钟 —— ui_analog.h */
lv_obj_t *ui_analog_create(lv_obj_t *parent);

/* 数字时钟 —— ui_clock.h */
void ui_clock_create(lv_obj_t *parent);
void ui_clock_set_time_synced(bool synced);

/* 页面 —— ui_pages.h */
#define PAGE_ANIM_MS 130        /* 翻页动画时长，可调 */
void ui_pages_flick(int dir);
```

**五态链路枚举**（状态标签文案/配色用）：
```c
UI_STATUS_CONNECTING  /* BT OFF   */
UI_STATUS_WAIT_LOC    /* WAIT GPS */
UI_STATUS_INVALID     /* NO FIX   */
UI_STATUS_STALE       /* STALE    */
UI_STATUS_OK          /* LIVE     */
```

---

## 6. 产出格式（必须按此回答）

```markdown
## 改动清单
| 文件 | 改动 |
|------|------|
| simulator/ui/ui_speed.c | … |

## 边界检查
- [ ] 未修改任何 .h 的签名/结构体/枚举
- [ ] 未修改 firmware/dashboard/ 下任何文件
- [ ] 未修改 ios/、tests/、tools/（生成器除外）
- [ ] 未引入新依赖
- [ ] 新增的周期性 setter 已做变更检测

## 验证结果
| 步骤 | 结果 |
|------|------|
| `cd simulator && make -j8` | ✅ |
| `python3 tools/ui_regression.py` | ✅ 10 项 |
| `cd tests && make run` | ✅ 207 项 |
| `tools/sync_firmware.sh` | ✅ |
| 固件编译 | ✅ Flash __% |
| 截图预览 | （附截图路径或描述）|

## 未验证 / 需人工确认
- 真机 AMOLED 实际色彩表现（模拟器无法完全代表）
```

---

## 7. 常见需求 → 改哪里（速查）

| 需求 | 具体位置 |
|------|---------|
| 指针颜色/粗细 | `ui_speed.c` 的 `C_NEEDLE_L` 与 `lv_meter_add_needle_line()` 的 width |
| 指针长度 | `MARKER_INNER_R`（内端）、`lv_meter_add_needle_line()` 的 `r_mod`（外端）|
| 超速阈值（表情切换）| `DRIVER_SPIRITED_V` / `DRIVER_SPEEDING_V` |
| 量程 | `SPEED_MAX`（当前 200 km/h）|
| 底部 HUD 文案 | `hud5_text()` |
| 状态标签文案/配色 | `status5_text()` / `status5_color()` |
| 翻页动画时长 | `ui_pages.c` 的 `PAGE_ANIM_MS` |
| 页面背景色 | `ui_pages.c` 的 `UI_BG_COLOR` |
| 时钟刻度/指针 | `ui_analog.c` |
| 数字时钟字号/布局 | `ui_clock.c` |
| 表盘刻度外观 | `tools/make_dial_faces.py` → 重新生成 |

---

## 8. 上下文最小化提示（省 Token）

**只需读你要改的那个文件 + 本文件。** 不要通读整个仓库。

- 改速度表 → 读 `simulator/ui/ui_speed.c`（+ 必要时 `ui_speed.h` 看接口）
- 改时钟 → 读 `ui_analog.c` 或 `ui_clock.c`
- 改翻页/背景 → 读 `ui_pages.c`

**不要读**：`firmware/`、`ios/`、`telemetry.c`、`control.c`、`tests/`、
`reference/`（官方示例，体积大且无关）。
