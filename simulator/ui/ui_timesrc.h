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

#ifdef __cplusplus
extern "C" {
#endif


/*
 * 时间提供者：默认为「系统时间」（模拟器）；固件注册 RTC 读取函数即可，
 * 页面代码无需改动。
 */
typedef void (*ui_timesrc_provider_t)(struct tm *out);
void ui_timesrc_set_provider(ui_timesrc_provider_t fn);

/* 当前是否使用了固件注入的外部时间源（真机 RTC）。 */
int ui_timesrc_uses_provider(void);

/* 取当前时间（提供者时间 / 系统时间 / 冻结时间） */
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

#ifdef __cplusplus
}
#endif

#endif /* UI_TIMESRC_H */
