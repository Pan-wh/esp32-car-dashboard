# firmware/dashboard —— 固件工程

基于微雪官方示例的初始化流程，运行 `simulator/ui/` 里的共享 UI 与 GNSS 解析代码。

## 当前状态（2026-10-10）

| 项目 | 状态 |
|------|------|
| 编译 | ✅ 已通过（arduino-cli + Arduino-ESP32 3.3.10） |
| **烧录 / 真机运行** | ✅ **2026-10-11 已烧录并运行**（串口心跳、切页、校时均实测） |
| Flash 占用 | 1,278,544 B（**40%**，应用分区 3 MB） |
| RAM / 空闲堆 | 空闲堆 **246 KB**（运行中实测） |
| RTC | ✅ 实测走时正确（串口每秒输出，跨分钟进位正常） |
| 触摸 | ⏳ 待实测（滑页交互） |
| GNSS 读取层 | ⏳ 未接入（速度页显示 `NO FIX`，属预期） |

## 目录结构与「共享源码」策略

Arduino 只编译 sketch 目录内的源码，无法直接引用工程外的文件，因此：

- **唯一源码**：`simulator/ui/`（UI 页面、字体、图片、NMEA 解析、时间源）
- **同步**：`tools/sync_firmware.sh` 复制到本目录（已在 `.gitignore` 中忽略，避免两份代码漂移）

```bash
tools/sync_firmware.sh        # 每次改完共享代码后执行
```

本目录中**手动维护**的文件只有：`dashboard.ino`、`README.md`。

## 构建

```bash
# 1) 同步共享源码
tools/sync_firmware.sh

# 2) 编译（仓库根目录执行）
CLI="/Applications/Arduino IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli"
"$CLI" compile \
  --fqbn "esp32:esp32:esp32s3:FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,USBMode=hwcdc,CDCOnBoot=cdc" \
  --libraries reference/ESP32-S3-Touch-AMOLED-1.75/examples/arduino/libraries \
  firmware/dashboard
```

烧录（板卡到货后，替换串口）：

```bash
"$CLI" upload \
  --fqbn "esp32:esp32:esp32s3:FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,USBMode=hwcdc,CDCOnBoot=cdc" \
  --libraries reference/ESP32-S3-Touch-AMOLED-1.75/examples/arduino/libraries \
  -p /dev/cu.usbmodemXXXX firmware/dashboard
```

> 分区方案与 Flash 大小必须为 `app3M_fat9M_16MB` + `16M`（官方要求，勿混用）。
>
> **`USBMode=hwcdc,CDCOnBoot=cdc` 必须加**：板卡默认 `CDCOnBoot=default`（Disabled）时，
> Arduino 的 `Serial` 走硬件 UART0（GPIO43/44），**USB 口看不到任何串口日志**。
> 2026-10-10 在真机上实测确认（官方 HelloWorld 加此选项后串口正常输出）。

## 初始化顺序（与官方示例一致）

```
Serial → 显示（Wire + gfx->begin + 亮度） → 触摸（CST9217 @0x5A）
       → RTC（PCF85063） → LVGL（draw_buf/disp/indev/esp_timer 心跳）
       → ui_timesrc_set_provider(RTC) → ui_pages_init() → 默认页 = 速度表盘
```

要点：

1. **LVGL 心跳必须存在**：`esp_timer` 每 2 ms 调 `lv_tick_inc(2)`。缺失会导致所有
   `lv_timer` 不触发（时钟不走、秒针不动）——模拟器上已踩过这个坑，详见
   `docs/troubleshooting.md`。
2. **触摸为轮询实现**（非官方示例的中断置位）：LVGL 的滑动手势需要按住期间的连续坐标，
   轮询更可靠；代价是每帧一次 I2C 读（~33 Hz，可接受）。
3. **RTC 通过 `ui_timesrc_set_provider()` 注入**：UI 页面不关心时间来源；
   RTC 不可用时返回全 0，界面显示无效状态，**不伪造时间**。
4. **GNSS 尚未接入**：`ui_speed_set_fix(false, 0)` 使速度页显示 `NO FIX`（正确降级表现）。

## 串口命令（开发便利）

连上 115200 串口后可直接输入命令（CDC 模式，见下方 FQBN 说明）：

| 命令 | 作用 |
|------|------|
| `help` | 显示帮助 |
| `time` | 显示 RTC 当前时间 |
| `set YYYY-MM-DD HH:MM:SS` | **手动校准 RTC**（例：`set 2026-10-11 16:11:22`） |
| `pages` | 列出页面与当前页 |
| `page N` | 切到第 N 页（0=模拟时钟 1=数字时钟 2=速度表盘） |

**RTC 上电兜底**：若 RTC 时间早于固件编译时间（说明从未校准或电池耗尽），
自动用**编译时刻**初始化并在串口打印；之后可用上面的 `set` 命令或 GNSS UTC 校准。

> 实测记录（2026-10-11）：烧录前 RTC 为官方示例写死的 `2024-09-24 11:09`；
> 本固件启动后被兜底逻辑修正为编译时刻（2026-10-11），再用 `set` 精确校准到
> `2026-10-11 16:11:22`，复查 2 秒后为 16:11:24 ✓

## 待办（板卡到货后）

1. 按官方 `09_LC76G_I2C` 的 I2C 协议实现 LC76G 读取层（0x50 写 / 0x54 读，先读长度再读数据块），
   复用 `gnss_nmea.c` 解析 —— 见 `docs/gnss.md`；
2. 首次上电验证：屏幕显示、触摸响应、RTC 走时；逐项记录串口日志；
3. 先跑通官方 `01_HelloWorld` → `03_LVGL_PCF85063_simpleTime` → `09_LC76G_I2C`，
   再烧本工程（`AI_CONTEXT.md` 里程碑 1 的验收顺序）；
4. 野外实测定速：冷启动首次定位时间、速度跟随延迟、断定位恢复（里程碑 3）。
