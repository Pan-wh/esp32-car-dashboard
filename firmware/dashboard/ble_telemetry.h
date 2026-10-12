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

/* 距最后一条有效遥测的毫秒数；从未收到过返回 UINT32_MAX */
uint32_t ble_telemetry_age_ms(void);

/* 累计统计（排障日志用） */
void ble_telemetry_stats(uint32_t *rx_bytes, uint32_t *ok, uint32_t *parse_err);

/* 更新 Status 特征值（可读 + Notify，供 iPhone 读固件状态） */
void ble_telemetry_set_status(const char *status);

#endif /* BLE_TELEMETRY_H */