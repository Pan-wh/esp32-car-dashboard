/**
 * ui_timesrc.c —— 统一时间源实现
 *
 * 当前：系统时间 / 冻结时间。
 * 真机移植：把 ui_timesrc_now() 内部换成 PCF85063 读取即可，页面代码不用改。
 */
#include "ui_timesrc.h"

#include <stdbool.h>
#include <sys/time.h>

static bool s_frozen = false;
static struct tm s_fixed;
static ui_timesrc_provider_t s_provider = NULL;

void ui_timesrc_set_provider(ui_timesrc_provider_t fn)
{
    s_provider = fn;
}

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

    if (s_provider != NULL) { /* 固件：RTC 提供 */
        s_provider(out);
        return;
    }

    time_t t = time(NULL);
    localtime_r(&t, out); /* 模拟器：系统时间 */
}

void ui_timesrc_now_ms(struct tm *out, int *ms)
{
    if (s_frozen) {
        /* 冻结时间用于截图验证：亚秒固定为 0，保证截图可复现 */
        *out = s_fixed;
        *ms = 0;
        return;
    }

    if (s_provider != NULL) {
        /*
         * 固件：PCF85063 只有整秒寄存器，没有亚秒。
         * 当前秒针为跳秒模式（ANALOG_SECOND_SWEEP=0），不需要亚秒；
         * 若日后启用扫秒，在此用 MCU 毫秒计时与 RTC 秒值做插值即可。
         */
        s_provider(out);
        *ms = 0;
        return;
    }

    struct timeval tv;
    gettimeofday(&tv, NULL);
    time_t sec = tv.tv_sec;
    localtime_r(&sec, out);
    *ms = (int)(tv.tv_usec / 1000);
}
