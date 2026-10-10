/**
 * ui_clock.c —— 主时钟表盘 v1（静态设计稿，里程碑 2.1）
 *
 * 设计规范详见 design/wireframes/clock.md：
 *   - 深色底 #0B0D10，圆屏 466×466
 *   - 双层表环（外 2px / 内 1px），机械表圈层次感
 *   - 中心大号时间 HH:MM（#F2F4F7），其下日期+星期（#8A919E 弱化）
 *   - 无动效（静态稿；冒号呼吸等待真机帧率评估后再议）
 *
 * 仅 LVGL 8.4 标准 API；数据源暂用系统时间，真机版由 RTC 模块喂给
 * ui_clock_set_time()（预留接口，见下）。
 */
#include "ui_clock.h"

#include <stdio.h>
#include <time.h>

/* ---- 设计常量（与 design/wireframes/clock.md 对应，改动需同步文档）---- */
#define UI_BG_COLOR    0x0B0D10  /* 背景：近黑深蓝灰 */
#define UI_FG_MAIN     0xF2F4F7  /* 主数据：高亮白 */
#define UI_FG_DIM      0x8A919E  /* 次要数据：中灰 */
#define UI_RING_OUTER  0x3A4149  /* 外表环 */
#define UI_RING_INNER  0x2A3138  /* 内表环 */

/* 时间主字体：优先用 lv_font_conv 生成的大号字体（Makefile 自动检测） */
#ifdef CLOCK_FONT_CUSTOM
LV_FONT_DECLARE(clock_font_96);
#define CLOCK_TIME_FONT (&clock_font_96)
#else
#define CLOCK_TIME_FONT (&lv_font_montserrat_48)
#endif

static lv_obj_t *s_label_time;
static lv_obj_t *s_label_date;
static lv_timer_t *s_timer;

/* 预留：真机版由 RTC 模块调用（当前静态稿用系统时钟驱动） */
void ui_clock_set_time(int hour, int minute, int mon, int mday, int wday)
{
    static const char *WD[] = { "SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT" };
    static const char *MO[] = { "JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                                "JUL", "AUG", "SEP", "OCT", "NOV", "DEC" };
    char buf[32];

    snprintf(buf, sizeof(buf), "%02d:%02d", hour, minute);
    lv_label_set_text(s_label_time, buf);

    snprintf(buf, sizeof(buf), "%s %02d · %s", MO[mon], mday, WD[wday]);
    lv_label_set_text(s_label_date, buf);
}

static void clock_tick_cb(lv_timer_t *timer)
{
    (void)timer;
    time_t now = time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);
    ui_clock_set_time(lt.tm_hour, lt.tm_min, lt.tm_mon, lt.tm_mday, lt.tm_wday);
}

/* 通用的“圆形细环”辅助：radius 全圆、透明底、细边框 */
static lv_obj_t *make_ring(lv_obj_t *parent, lv_coord_t size,
                           lv_coord_t border, uint32_t color)
{
    lv_obj_t *ring = lv_obj_create(parent);
    lv_obj_remove_style_all(ring);
    lv_obj_set_size(ring, size, size);
    lv_obj_center(ring);
    lv_obj_clear_flag(ring, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(ring, border, 0);
    lv_obj_set_style_border_color(ring, lv_color_hex(color), 0);
    lv_obj_set_style_border_opa(ring, LV_OPA_COVER, 0);
    return ring;
}

void ui_clock_create(lv_obj_t *parent)
{
    /* 背景 */
    lv_obj_set_style_bg_color(parent, lv_color_hex(UI_BG_COLOR), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);

    /* 表环：外环 450（r=225，屏缘内 8px），内环 436 */
    make_ring(parent, 450, 2, UI_RING_OUTER);
    make_ring(parent, 436, 1, UI_RING_INNER);

    /* 时间（主数据） */
    s_label_time = lv_label_create(parent);
    lv_obj_set_style_text_font(s_label_time, CLOCK_TIME_FONT, 0);
    lv_obj_set_style_text_color(s_label_time, lv_color_hex(UI_FG_MAIN), 0);
    lv_obj_set_style_text_letter_space(s_label_time, 4, 0);
    lv_label_set_text(s_label_time, "00:00");
    lv_obj_align(s_label_time, LV_ALIGN_CENTER, 0, -16);

    /* 日期 + 星期（次要数据，弱化） */
    s_label_date = lv_label_create(parent);
    lv_obj_set_style_text_font(s_label_date, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_label_date, lv_color_hex(UI_FG_DIM), 0);
    lv_obj_set_style_text_letter_space(s_label_date, 1, 0);
    lv_label_set_text(s_label_date, "JAN 01 · MON");
    lv_obj_align(s_label_date, LV_ALIGN_CENTER, 0, 52);

    /* 每秒刷新；先立即刷一次 */
    clock_tick_cb(NULL);
    s_timer = lv_timer_create(clock_tick_cb, 1000, NULL);
    (void)s_timer;
}
