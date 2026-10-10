# 常见问题和排查记录

> 状态：骨架。每次实际排障后在此追加记录（日期 + 现象 + 原因 + 解决）。

## 官方排障入口

`reference/ESP32-S3-Touch-AMOLED-1.75/docs/troubleshooting_ZH.md`（含烧录与运行故障）

## 环境类（本机已遇到）

### 2026-10-10 写 ~/Library/Arduino15 被沙盒拒绝

- 现象：直接 bash 写 `~/Library/Arduino15/arduino-cli.yaml` 报 `Operation not permitted`。
- 原因：AI 会话文件沙盒仅允许写项目工作区。
- 解决：经授权的操作完成写入与核心安装；后续手动操作不受影响。

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
