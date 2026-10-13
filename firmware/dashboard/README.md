# firmware/dashboard —— 固件工程

基于微雪官方示例的初始化流程，运行 `simulator/ui/` 里的共享 UI 与遥测解析代码。

## 当前状态（2026-10-13）

| 项目 | 状态 |
|------|------|
| 编译 | ✅ 通过（arduino-cli + Arduino-ESP32 3.3.10） |
| **烧录 / 真机运行** | ✅ 已烧录运行，长稳 30 分钟无重启、堆零漂移 |
| Flash 占用 | 2,375,619 B（**75%**，应用分区 3 MB） |
| RAM | 全局 38%，运行空闲堆 ~120 KB |
| RTC | ✅ 走时正确；支持 BLE UTC 自动校时 |
| 触摸 | ✅ 100Hz 独立采样 + 按下锁存 + flick 翻页 |
| 数据源 | ✅ iPhone Core Location 经 BLE（本板**无** GNSS） |
| 表盘 | ✅ 五表盘（模拟时钟/数字时钟/速度表/多彩时钟/**导航卡片**），支持 BLE 远程切换 |
| 设备控制 | ✅ 亮度 / 息屏（BLE Command 特征） |

## 目录结构与「共享源码」策略

Arduino 只编译 sketch 目录内的源码，无法直接引用工程外的文件，因此：

- **唯一源码**：`simulator/ui/`（UI 页面、字体、图片、遥测解析、设备控制、表盘注册表）
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
  --fqbn "esp32:esp32:esp32s3:FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,USBMode=hwcdc,CDCOnBoot=cdc,PSRAM=opi" \
  --libraries reference/ESP32-S3-Touch-AMOLED-1.75/examples/arduino/libraries \
  firmware/dashboard
```

烧录（板卡到货后，替换串口）：

```bash
"$CLI" upload \
  --fqbn "esp32:esp32:esp32s3:FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,USBMode=hwcdc,CDCOnBoot=cdc,PSRAM=opi" \
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
4. **数据源为 BLE**：无遥测时 `ui_speed_set_fix(false, 0)` 使速度指针归零并显示链路态（正确降级，不显示旧值）。

## 串口命令（开发便利）

连上 115200 串口后可直接输入命令（CDC 模式，见下方 FQBN 说明）。
**完整命令速查见仓库根 [`PROGRESS.md`](../../PROGRESS.md) 第十一节。**

常用：`ble`（BLE 状态）、`face`（切表盘）、`spd`（诊断速度指针）、
`trip`（行程）、`bright` / `screen`（亮度/息屏）、`time` / `set`（RTC 校时）。

**RTC 上电兜底**：若 RTC 时间早于固件编译时间（说明从未校准或电池耗尽），
自动用**编译时刻**初始化并在串口打印；之后可用上面的 `set` 命令或 **BLE 下发的 UTC**（`ts_utc_ms`）校准。

> 实测记录（2026-10-11）：烧录前 RTC 为官方示例写死的 `2024-09-24 11:09`；
> 本固件启动后被兜底逻辑修正为编译时刻（2026-10-11），再用 `set` 精确校准到
> `2026-10-11 16:11:22`，复查 2 秒后为 16:11:24 ✓

## 当前状态

板子已到货并完成全部验证（详见仓库根 [`PROGRESS.md`](../../PROGRESS.md)）：

- **数据源**：iPhone Core Location 经 BLE 下发（本板**无** LC76G GNSS，GNSS 代码已删除）
- **表盘**：模拟时钟 / 数字时钟 / 速度表，支持 BLE 远程切换
- **控制**：亮度、息屏/亮屏（BLE Command 特征 `7D2A0004`）
- **HUD**：航向 + 行程统计（里程/最高速/均速）
- **性能**：滑页单帧 90–113ms（QSPI 全屏传输 43ms 为硬件下限）
- **占用**：Flash 75%，RAM 38%

### 构建注意

```bash
arduino-cli compile \
  --fqbn "esp32:esp32:esp32s3:FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,USBMode=hwcdc,CDCOnBoot=cdc,PSRAM=opi" \
  --build-property "compiler.cpp.extra_flags=-DESP32QSPI_MAX_PIXELS_AT_ONCE=4096" \
  --libraries ../../reference/ESP32-S3-Touch-AMOLED-1.75/examples/arduino/libraries \
  --upload -p /dev/cu.usbmodem101 .
```

- `PSRAM=opi` **必需**（S3R8 八线 PSRAM，全帧绘制缓冲用）
- `-DESP32QSPI_MAX_PIXELS_AT_ONCE=4096` 为滑页性能优化，不可省
- 跨文件改动后**不要复用 `--build-path`**（可能漏编译，已踩坑）

