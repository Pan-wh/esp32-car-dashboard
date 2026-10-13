/*
 * ble_telemetry.h —— ESP32 BLE Peripheral（GATT Server）+ 遥测接收
 *
 * 固件侧专有模块（依赖 Arduino BLE / Bluedroid），不会被模拟器编译。
 *
 * GATT 设计（AI_TASK_IPHONE_BLE_DASHBOARD.md §4）：
 *   - 设备名：CarDashboard
 *   - Service：          7D2A0001-8F4A-4B7C-9D21-5E6A3B8C1000
 *   - Telemetry（Write）：7D2A0002-8F4A-4B7C-9D21-5E6A3B8C1000
 *   - Status（Read/Notify）：7D2A0003-8F4A-4B7C-9D21-5E6A3B8C1000
 *
 * 线程模型：BLE 写回调运行在 BLE 任务里，主循环经 ble_telemetry_fetch() 取数；
 *           共享状态用 FreeRTOS 互斥量保护，**回调中不触碰 LVGL**。
 */
#ifndef BLE_TELEMETRY_H
#define BLE_TELEMETRY_H

#include <stdint.h>
#include "control.h"
#include "nav_parse.h"
#include "telemetry.h"

/* 初始化 BLE Peripheral 并开始广播；成功返回 true */
bool ble_telemetry_begin(void);

/* 是否有 Central 已连接 */
bool ble_telemetry_connected(void);

/*
 * 非阻塞取最新一条遥测（线程安全）。
 * 有尚未取走的新数据时返回 true 并填 out + *rx_ms（本机毫秒，收到时刻）。
 */
bool ble_telemetry_fetch(telemetry_msg_t *out, uint32_t *rx_ms);

/*
 * 距最后一条有效遥测的毫秒数：
 *   UINT32_MAX        = 从未收到过
 *   BLE_AGE_UNKNOWN   = 暂时取不到状态（并发中），调用方应保持当前状态而非判为超时
 */
#define BLE_AGE_UNKNOWN (UINT32_MAX - 1)
uint32_t ble_telemetry_age_ms(void);

/* 累计统计（排障日志用） */
void ble_telemetry_stats(uint32_t *rx_bytes, uint32_t *ok, uint32_t *parse_err);

/* 更新 Status 特征值（可读 + Notify，供 iPhone 读固件状态） */
void ble_telemetry_set_status(const char *status);

/*
 * 取出待执行的下行控制命令（由主循环调用；BLE 回调只暂存不执行硬件动作）。
 * @return true 表示取到一条命令（已消费）。
 */
bool ble_telemetry_take_command(control_cmd_t *out);

/* 控制命令统计：成功解析 / 拒绝 */
void ble_telemetry_command_stats(uint32_t *ok, uint32_t *err);

/*
 * ---- 导航消息（阶段 2）----
 *
 * 与遥测同一特征下行，用 "t":"nav" 区分。
 * 取用语义与 ble_telemetry_fetch 一致：**取走即清标记**，
 * 保证主循环只在有新数据时更新 UI（避免高频无效重绘）。
 *
 * @return true 表示本次取到一条新导航消息（内容写入 *out）。
 */
bool ble_telemetry_take_nav(nav_msg_t *out);

/* 导航消息累计接收数（含被跳过的畸形包另计）*/
void ble_telemetry_nav_stats(uint32_t *ok);

#endif /* BLE_TELEMETRY_H */