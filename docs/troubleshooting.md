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

## 2026-10-11 真机滑动手势不切页（三个连环坑，全部已修）

**现象**：真机上左右滑动无法切换页面；串口诊断显示 LVGL **确实识别出了手势**
（LEFT=6 RIGHT=3），但页面不切换；速度页显示也与模拟器不同（另见下方第 4 条）。

**排查手段**（可复用）：固件内置串口命令 `mem` / `dump` / `touch`，
配合模拟器的 `--gesture-test`（自动注入拖拽并打印 LVGL 输入内部状态与命中测试）。

### 坑 1：`lv_obj` 默认给子对象加 `GESTURE_BUBBLE`

`lv_obj.c` 构造函数：`if(parent) obj->flags |= LV_OBJ_FLAG_GESTURE_BUBBLE;`
手势检测会沿父链上移目标（`lv_indev.c: while(has_flag(GESTURE_BUBBLE)) gesture_obj = parent`），
于是手势被转发给**屏幕**，页面容器的回调永远不执行。
→ 解决：页面/子对象统一清掉 `GESTURE_BUBBLE`（连同 `SCROLLABLE` 等一组默认标志）。

### 坑 2：`SCROLLABLE` 会吞掉手势

`lv_indev.c` 的 `indev_gesture()` 开头即 `if(scroll_obj) return;` ——
**滚动与手势互斥**；而 `lv_obj_remove_style_all()` 只清样式、**不清标志**，
基类默认带 `SCROLLABLE`，于是拖拽被判为"滚动"、手势不发。

### 坑 3：隐藏"被按住的页面"会连环触发手势

即便修好 1、2，自己用 `LV_EVENT_GESTURE` 切页仍有问题：切页要隐藏当前页，
而隐藏被按住的页面会让 LVGL 重置输入状态，同一次拖拽反复判定手势
（实测一次拖拽触发 3 次：0→2→1→0，**转回原点**，看起来就是"滑不动"）。

**最终方案**：改用 LVGL 标准的**横向滚动容器 + 吸附分页**（`lv_tileview` 同款机制），
不再自己处理手势：容器尺寸 = **视口大小**，页面在其中并排摆放并超出边界，
`SCROLLABLE + SCROLL_SNAP_CENTER + 子对象 SNAPPABLE`，滚动结束用 `scroll_x` 反推当前页。

> 附带坑：容器若做成 `页数 × 屏宽`，则内容刚好填满容器 → **没有可滚动区域** → 依然滑不动。

### 坑 4：速度页与模拟器不一致（变体默认值）

`ui_speed.c` 的 `s_variant` 默认是 0（V1），而模拟器一直用 `--variant 4` 启动，
固件从未设置变体 → 真机显示 V1（数字+外环弧）而非 V5（驾驶员表盘）。
→ 解决：默认值改为 4（V5 定稿设计）。**排查用 `dump` 打印元素树**即可发现
（V1 只有 arc+label，V5 应有 img 300×300 + meter + 6 个刻度数字）。

**验证**：模拟器 `--gesture-test`：从第 1 页右拖 400px → 第 0 页；左拖 400px → 第 1 页 ✓

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
