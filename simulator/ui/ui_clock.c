/**
 * ui_clock.c —— 主时钟表盘 v2：iPhone 充电息屏时钟风格（2026-10-10）
 *
 * 设计规范详见 design/wireframes/clock.md：
 *   - 纯黑背景 #000000（无表环、无装饰，OLED 息屏观感）
 *   - 中文日期在上方（Noto Sans SC 26px，#98989E 弱化）
 *   - 超大极细时间在下（Inter ExtraLight 138px，#F5F5F7）
 *   - 无前导零（如 9:41），无动效
 *
 * 仅 LVGL 8.4 标准 API；数据源暂用系统时间，真机版由 RTC 模块喂给
 * ui_clock_set_time()。
 */
#include "ui_clock.h"

#include <stdio.h>

#include "ui_timesrc.h"

/* ---- 设计常量（与 design/wireframes/clock.md 对应，改动需同步文档）---- */
#define UI_BG_COLOR    0x000000  /* 背景：纯黑（息屏观感） */
#define UI_FG_MAIN     0xF5F5F7  /* 时间：Apple 白 */
#define UI_FG_DIM      0x98989E  /* 日期：Apple 中灰 */

LV_FONT_DECLARE(clock_iphone_138); /* Inter ExtraLight 138px，数字+冒号 */
LV_FONT_DECLARE(date_cjk_26);       /* Noto Sans SC 26px，日期用字形 */

static lv_obj_t *s_label_time;
static lv_obj_t *s_label_date;
static lv_timer_t *s_timer;

/* 预留：真机版由 RTC 模块调用（当前静态稿用系统时钟驱动） */
void ui_clock_set_time(int hour, int minute, int mon, int mday, int wday)
{
    static const char *WD[] = { "日", "一", "二", "三", "四", "五", "六" };
    char buf[40];

    /* 无前导零，如 9:41（iPhone 息屏样式） */
    snprintf(buf, sizeof(buf), "%d:%02d", hour, minute);
    lv_label_set_text(s_label_time, buf);

    snprintf(buf, sizeof(buf), "%d月%d日 星期%s", mon + 1, mday, WD[wday]);
    lv_label_set_text(s_label_date, buf);
}

static void clock_tick_cb(lv_timer_t *timer)
{
    (void)timer;
    struct tm lt;
    ui_timesrc_now(&lt);
    ui_clock_set_time(lt.tm_hour, lt.tm_min, lt.tm_mon, lt.tm_mday, lt.tm_wday);
}

void ui_clock_create(lv_obj_t *parent)
{
    /* 背景：纯黑 */
    lv_obj_set_style_bg_color(parent, lv_color_hex(UI_BG_COLOR), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);

    /* 日期（次要信息，弱化，置于时间上方） */
    s_label_date = lv_label_create(parent);
    lv_obj_set_style_text_font(s_label_date, &date_cjk_26, 0);
    lv_obj_set_style_text_color(s_label_date, lv_color_hex(UI_FG_DIM), 0);
    lv_label_set_text(s_label_date, "10月10日 星期六");
    lv_obj_align(s_label_date, LV_ALIGN_CENTER, 0, -60);

    /* 时间（主数据，超大极细） */
    s_label_time = lv_label_create(parent);
    lv_obj_set_style_text_font(s_label_time, &clock_iphone_138, 0);
    lv_obj_set_style_text_color(s_label_time, lv_color_hex(UI_FG_MAIN), 0);
    lv_label_set_text(s_label_time, "0:00");
    lv_obj_align(s_label_time, LV_ALIGN_CENTER, 0, 16);

    /* 每秒刷新；先立即刷一次 */
    clock_tick_cb(NULL);
    s_timer = lv_timer_create(clock_tick_cb, 1000, NULL);
    (void)s_timer;
}
