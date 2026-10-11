/**
 * nmea_replay.h —— NMEA 日志回放（**仅模拟器用**）
 *
 * 作用：把日志文件里的 NMEA 语句按时间回放，喂给 gnss_nmea 解析器，
 * 再把解析结果推给速度页 UI —— 这样没有开发板也能验证整条链路。
 *
 * 真机上不需要本模块：数据来自 I2C/串口（见官方示例 09_LC76G_I2C），
 * 由 app_gnss 层读取后走同一个解析器。
 */
#ifndef NMEA_REPLAY_H
#define NMEA_REPLAY_H

#include <stdbool.h>
#include <stdint.h>

/* 载入日志文件（按 RMC 分组，每组约 1 秒的语句集合）。返回是否成功 */
bool nmea_replay_load(const char *path);

/* 开始回放；speedup = 时间倍速（1.0 = 实时，10 = 十倍速） */
void nmea_replay_start(float speedup);

/* 主循环调用：按经过时间推进回放（到结尾后自动循环） */
void nmea_replay_update(uint32_t now_ms);

/* 是否已载入并处于回放状态 */
bool nmea_replay_active(void);

/* 当前回放到第几组 / 共几组（供状态显示） */
int nmea_replay_group(void);
int nmea_replay_group_count(void);

#endif /* NMEA_REPLAY_H */
