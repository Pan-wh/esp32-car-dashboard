/**
 * ui_timesrc.c —— 统一时间源实现
 *
 * 当前：系统时间 / 冻结时间。
 * 真机移植：把 ui_timesrc_now() 内部换成 PCF85063 读取即可，页面代码不用改。
 */
#include "ui_timesrc.h"

#include <stdbool.h>

static bool s_frozen = false;
static struct tm s_fixed;

void ui_timesrc_freeze(int hour, int min, int sec)
{
    if (hour < 0) {
        s_frozen = false;
        return;
    }

    /* 固定一个可读日期：2026-10-10 星期六 */
    s_fixed.tm_year = 2026 - 1900;
    s_fixed.tm_mon = 9;
    s_fixed.tm_mday = 10;
    s_fixed.tm_wday = 6;
    s_fixed.tm_hour = hour;
    s_fixed.tm_min = min;
    s_fixed.tm_sec = sec;
    s_frozen = true;
}

void ui_timesrc_now(struct tm *out)
{
    if (s_frozen) {
        *out = s_fixed;
        return;
    }

    time_t t = time(NULL);
    localtime_r(&t, out);
}
