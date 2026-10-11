# 常见问题和排查记录

> 状态：骨架。每次实际排障后在此追加记录（日期 + 现象 + 原因 + 解决）。

## 官方排障入口

`reference/ESP32-S3-Touch-AMOLED-1.75/docs/troubleshooting_ZH.md`（含烧录与运行故障）

## 环境类（本机已遇到）

### 2026-10-10 写 ~/Library/Arduino15 被沙盒拒绝

- 现象：直接 bash 写 `~/Library/Arduino15/arduino-cli.yaml` 报 `Operation not permitted`。
- 原因：AI 会话文件沙盒仅允许写项目工作区。
- 解决：经授权的操作完成写入与核心安装；后续手动操作不受影响。

## 2026-10-10 烧录后 USB 串口无任何输出（真机实测）

- **现象**：官方 `01_HelloWorld` 烧录成功、屏幕有显示，但打开串口监视器（`/dev/cu.usbmodem101`）
  完全收不到数据（esptool 能正常通信、烧录正常）。
- **原因**：ESP32-S3 板卡选项默认 `USB CDC On Boot = Disabled`（`CDCOnBoot=default`）。
  此时 Arduino 的 `Serial` 指向**硬件 UART0（GPIO43/44）**，而不是原生 USB CDC；
  本板没有 USB-UART 桥接芯片，因此 USB 口只能烧录、看不到应用日志。
- **解决**：编译时显式打开 USB CDC：
  ```
  --fqbn "esp32:esp32:esp32s3:FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,USBMode=hwcdc,CDCOnBoot=cdc"
  ```
- **验证结果**：重新烧录后 8 秒收到 40 行 `loop`（每 200 ms 一次，与 HelloWorld 程序一致）✓
- **对本项目的影响**：固件与所有官方示例的调试都必须带该选项，否则看不到 `Serial` 日志
  （我们的 `dashboard.ino` 依赖 Serial 输出启动信息与心跳）。

## 2026-10-11 RTC 显示 2024-09-24（不是故障）

- **现象**：烧录官方 `03_LVGL_PCF85063_simpleTime` 后，RTC 时间恒为 `2024-09-24 11:09`。
- **原因**：**官方示例自己写死了时间** —— `rtc.setDateTime(2024, 9, 24, 11, 9, 41)`
  在每次上电时执行（示例源码第 78-87 行）。RTC 芯片本身走时正常（跨分钟进位正确）。
- **处理**：本工程固件改为「RTC 早于编译时间才用编译时刻兜底」，并提供串口
  `set YYYY-MM-DD HH:MM:SS` 手动校准；后续接入 GNSS 后用 RMC 的 UTC 时间自动校准。
- **验证**：烧录本工程固件后 RTC 显示 2026-10-11，串口 `set` 校准到 16:11:22，
  复查 16:11:24（正常走时）✓

## 记录模板

```
### 日期 - 简述
- 现象：
- 复现步骤：
- 原因：
- 解决：
- 验证结果：
```

## 2026-10-10 模拟器里时钟不走 / 秒针不动

- **现象**：模拟器界面能正常显示，但数字时钟不刷新、模拟时钟秒针静止不动。
- **复现步骤**：`./dashboard_sim` 打开窗口观察数十秒。
- **原因**：`simulator/main.c` 主循环**从未调用 `lv_tick_inc()`**，且 `lv_conf.h` 未启用
  `LV_TICK_CUSTOM`（默认 0，即需要手动喂心跳）。结果 LVGL 内部时间恒为 0，
  `lv_timer_handler()` 认为没有时间流逝 → **所有 `lv_timer` 都不触发**。
  界面只在初始化时渲染了一次，所以看起来"卡住"。
- **解决**：在主循环中按经过的真实毫秒调用 `lv_tick_inc(delta)`：
  ```c
  Uint32 now_tick = SDL_GetTicks();
  if (now_tick != last_tick) { lv_tick_inc(now_tick - last_tick); last_tick = now_tick; }
  lv_timer_handler();
  ```
- **验证结果**：间隔约 4 秒截取两张图，橙色秒针角度从 144.2° 变为 168.1°（差 24° ≈ 6°/s
  的每秒一跳）→ 确认定时器已正常工作。

### 真机注意事项（重要）

ESP32 上同样需要**稳定的 LVGL 心跳源**，通常由硬件定时器周期性调用 `lv_tick_inc()`。
微雪官方 Arduino 示例已包含该机制（`EXAMPLE_LVGL_TICK_PERIOD_MS 2` + 定时器回调），
因此**移植 UI 代码到固件时必须沿用官方示例的 tick 配置**，否则会出现与本条完全相同的
"界面静止"现象。这是本坑对固件阶段的直接启示。
