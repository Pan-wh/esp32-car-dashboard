/**
 * ui_timesrc.h —— 统一时间源
 *
 * 所有页面通过本模块取时间，便于：
 *   1. 真机上换成 RTC（PCF85063）驱动；
 *   2. 模拟器/截图时冻结到指定时刻，做确定性验证与设计比对。
 */
#ifndef UI_TIMESRC_H
#define UI_TIMESRC_H

#include <time.h>

/* 取当前时间（系统时间，或冻结后的固定时间） */
void ui_timesrc_now(struct tm *out);

/*
 * 取当前时间 + 亚秒毫秒（0-999），用于秒针连续滑动（扫秒）。
 *
 * 真机注意：PCF85063 只有整秒寄存器，没有亚秒。
 * 移植时应在 RTC 秒值变化时记录 MCU 毫秒计时作为基准，
 * 用 (now_ms - mark_ms) 推算亚秒偏移（此处接口不变）。
 */
void ui_timesrc_now_ms(struct tm *out, int *ms);

/*
 * 冻结时间到指定时刻，用于截图验证与设计比对。
 * hour < 0 表示解除冻结，恢复系统时间。
 */
void ui_timesrc_freeze(int hour, int min, int sec);

#endif /* UI_TIMESRC_H */
