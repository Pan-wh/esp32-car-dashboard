/**
 * ui_analog.c —— 模拟指针时钟 v1（极简风格）
 *
 * 设计规范详见 design/wireframes/analog-clock.md：
 *   - 纯黑背景（AMOLED 息屏省电）
 *   - 外围 60 个灰色分钟刻度；12 个整点刻度更长更亮
 *   - 白色阿拉伯数字 1-12
 *   - 白色时针（粗短）/ 分针（细长）；橙色秒针（默认**每秒跳动**，可切连续滑动）
 *   - 中心圆点连接三针
 *
 * 实现要点（依据 LVGL 8.4 源码 lv_meter.c 实测语义）：
 *   1. 刻度角度 = i * angle_range / (tick_cnt - 1)，所以要“60 段 × 6°”必须给 61 个刻度；
 *      整点刻度同理用 13 个（首尾在 12 点重叠）。
 *   2. 不调用 lv_meter_set_scale_major_ticks 就完全不会绘制刻度数字
 *      （tick_major_nth=0 → major 恒为 false），数字改由 lv_label 精确摆放。
 *   3. 指针角度 = map(value, min, max, rotation, rotation + angle_range)；
 *      rotation=270 → value 0 指向正上方，顺时针递增。
 *   4. 量表采用“1 小时 = 3600 单位”，使时/分/秒都能用整数精确映射。
 */
#include "ui_analog.h"

#include <math.h>
#include <stdio.h>

#include "ui_timesrc.h"

/* ---- 设计常量（与 design/wireframes/analog-clock.md 对应）---- */
#define UI_BG_COLOR    0x000000  /* 背景：纯黑 */
#define UI_TICK_MINOR  0x5A5A5F  /* 分钟刻度：灰 */
#define UI_TICK_MAJOR  0xC8C8CE  /* 整点刻度：浅灰（更醒目） */
#define UI_NUM_COLOR   0xF5F5F7  /* 数字：白 */
#define UI_HAND_WHITE  0xF5F5F7  /* 时针/分针：白 */
#define UI_HAND_SEC    0xFF9500  /* 秒针：橙 */

#define METER_SIZE     440  /* 表盘直径，r=220（距屏缘 13px） */
#define R_EDGE         220  /* = METER_SIZE / 2，指针长度基准 */
#define TICK_LEN_MIN   8
#define TICK_LEN_MAJ   16
#define NEEDLE_HOUR    106  /* 时针长度 ≈ 0.48R */
#define NEEDLE_MIN     150  /* 分针长度 ≈ 0.68R（数字加大后留出间隙）*/
#define NEEDLE_SEC     166  /* 秒针长度 ≈ 0.75R */
#define NUM_RADIUS     177  /* 数字中心半径（38px 字号）*/
#define NUM_Y_NUDGE    (-2) /* 字体框含降部空间，视觉中心微调 */

LV_FONT_DECLARE(num_dial_38); /* Nunito Bold 38px，表盘数字 0-9 */

/* 秒针模式：1 = 连续滑动扫秒（20Hz）；0 = 每秒跳动（默认，经典石英表观感）
 * 两种模式都已实现并验证，切换本宏即可（详见 design/wireframes/analog-clock.md 第 6 节） */
#define ANALOG_SECOND_SWEEP 0

/* 量表满量程 = 1 小时 = 3600 单位 */
#define SCALE_MAX 3600

typedef struct {
    lv_obj_t *meter;
    lv_meter_indicator_t *ind_hour;
    lv_meter_indicator_t *ind_min;
    lv_meter_indicator_t *ind_sec;
    int32_t last_hour; /* 缓存上次值，避免无谓重绘 */
    int32_t last_min;
} analog_ctx_t;

static analog_ctx_t s_ctx;
static lv_obj_t *s_dot_inner;

static void update_hands(void)
{
    struct tm t;
    int ms;
    ui_timesrc_now_ms(&t, &ms);

    int32_t v_sec;
#if ANALOG_SECOND_SWEEP
    /* 连续滑动：量表 3600 单位 = 60 分钟 → 1 秒 = 60 单位 = 6°，1 毫秒 ≈ 0.06 单位 */
    v_sec = (int32_t)t.tm_sec * 60 + (int32_t)ms * 60 / 1000;
#else
    /* 每秒跳动：位置只取决于整秒 */
    (void)ms;
    v_sec = (int32_t)t.tm_sec * 60;
#endif
    int32_t v_min = (int32_t)t.tm_min * 60 + (int32_t)t.tm_sec;     /* 每分 60 单位 = 6° */
    int32_t v_hour = (int32_t)(t.tm_hour % 12) * 300                /* 每小时 300 单位 = 30° */
                     + (int32_t)t.tm_min * 5 + (int32_t)t.tm_sec / 12;

    /* lv_meter 指针变化只重绘指针扫过的包围盒（源码 inv_line），故仅在变化时更新 */
    if (v_hour != s_ctx.last_hour) {
        lv_meter_set_indicator_value(s_ctx.meter, s_ctx.ind_hour, v_hour);
        s_ctx.last_hour = v_hour;
    }
    if (v_min != s_ctx.last_min) {
        lv_meter_set_indicator_value(s_ctx.meter, s_ctx.ind_min, v_min);
        s_ctx.last_min = v_min;
    }
    lv_meter_set_indicator_value(s_ctx.meter, s_ctx.ind_sec, v_sec);
}

static void analog_tick_cb(lv_timer_t *timer)
{
    (void)timer;
    update_hands();
}

/*
 * 指针圆润化：lv_meter 的线条指针对角是直角，看起来生硬。
 * LVGL 8.4 的 lv_draw_line_dsc_t 支持 round_start / round_end（圆头线帽），
 * 在指针绘制事件里打开即可得到柔性圆头指针。
 */
static void meter_draw_cb(lv_event_t *e)
{
    lv_obj_draw_part_dsc_t *dsc = lv_event_get_draw_part_dsc(e);
    if (dsc->type == LV_METER_DRAW_PART_NEEDLE_LINE && dsc->line_dsc) {
        dsc->line_dsc->round_start = 1;
        dsc->line_dsc->round_end = 1;
    }
}

/* 12 个数字沿圆周定位（0° 在 12 点，顺时针） */
static void create_numerals(lv_obj_t *parent)
{
    for (int h = 1; h <= 12; h++) {
        lv_obj_t *lbl = lv_label_create(parent);
        lv_obj_set_style_text_font(lbl, &num_dial_38, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(UI_NUM_COLOR), 0);

        char buf[4];
        snprintf(buf, sizeof(buf), "%d", h);
        lv_label_set_text(lbl, buf);

        float ang = (float)h * 30.0f * 3.14159265f / 180.0f;
        int32_t x = (int32_t)lroundf(NUM_RADIUS * sinf(ang));
        int32_t y = (int32_t)lroundf(-NUM_RADIUS * cosf(ang)) + NUM_Y_NUDGE;
        lv_obj_align(lbl, LV_ALIGN_CENTER, x, y);
    }
}

/* 中心圆点（连接三针）：白色底 + 橙色内点 */
static void create_center_dot(lv_obj_t *parent)
{
    lv_obj_t *outer = lv_obj_create(parent);
    lv_obj_remove_style_all(outer);
    lv_obj_set_size(outer, 18, 18);
    lv_obj_set_style_radius(outer, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(outer, lv_color_hex(UI_HAND_WHITE), 0);
    lv_obj_set_style_bg_opa(outer, LV_OPA_COVER, 0);
    lv_obj_center(outer);

    s_dot_inner = lv_obj_create(parent);
    lv_obj_remove_style_all(s_dot_inner);
    lv_obj_set_size(s_dot_inner, 8, 8);
    lv_obj_set_style_radius(s_dot_inner, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_dot_inner, lv_color_hex(UI_HAND_SEC), 0);
    lv_obj_set_style_bg_opa(s_dot_inner, LV_OPA_COVER, 0);
    lv_obj_center(s_dot_inner);
}

lv_obj_t *ui_analog_create(lv_obj_t *parent)
{
    lv_obj_set_style_bg_color(parent, lv_color_hex(UI_BG_COLOR), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);

    /* 表盘：lv_meter 负责刻度与指针 */
    s_ctx.meter = lv_meter_create(parent);
    lv_obj_remove_style_all(s_ctx.meter); /* 去掉默认底/边框/内边距；刻度与指针自带宽高 */
    lv_obj_set_size(s_ctx.meter, METER_SIZE, METER_SIZE);
    lv_obj_center(s_ctx.meter);
    lv_obj_clear_flag(s_ctx.meter, LV_OBJ_FLAG_CLICKABLE); /* 触摸手势交给页面容器 */
    lv_obj_set_style_bg_opa(s_ctx.meter, LV_OPA_TRANSP, 0);
    /* 指针圆头（见 meter_draw_cb） */
    lv_obj_add_event_cb(s_ctx.meter, meter_draw_cb, LV_EVENT_DRAW_PART_BEGIN, NULL);

    /* 分钟刻度：61 个 → 60 段 × 6°（无标签） */
    lv_meter_scale_t *sc_min = lv_meter_add_scale(s_ctx.meter);
    lv_meter_set_scale_ticks(s_ctx.meter, sc_min, 61, 2, TICK_LEN_MIN,
                             lv_color_hex(UI_TICK_MINOR));
    lv_meter_set_scale_range(s_ctx.meter, sc_min, 0, SCALE_MAX, 360, 270);

    /* 整点刻度：13 个 → 12 段 × 30°（更长更亮，首尾重叠于 12 点） */
    lv_meter_scale_t *sc_hour = lv_meter_add_scale(s_ctx.meter);
    lv_meter_set_scale_ticks(s_ctx.meter, sc_hour, 13, 3, TICK_LEN_MAJ,
                             lv_color_hex(UI_TICK_MAJOR));
    lv_meter_set_scale_range(s_ctx.meter, sc_hour, 0, SCALE_MAX, 360, 270);

    /* 三根指针：时针粗短、分针细长、秒针橙色最细 */
    s_ctx.ind_hour = lv_meter_add_needle_line(s_ctx.meter, sc_min, 9,
                                              lv_color_hex(UI_HAND_WHITE),
                                              -(R_EDGE - NEEDLE_HOUR));
    s_ctx.ind_min = lv_meter_add_needle_line(s_ctx.meter, sc_min, 5,
                                             lv_color_hex(UI_HAND_WHITE),
                                             -(R_EDGE - NEEDLE_MIN));
    s_ctx.ind_sec = lv_meter_add_needle_line(s_ctx.meter, sc_min, 3,
                                             lv_color_hex(UI_HAND_SEC),
                                             -(R_EDGE - NEEDLE_SEC));

    /* 数字与中心点（在指针之上） */
    create_numerals(parent);
    create_center_dot(parent);

    /* 初始化指针位置并启动刷新定时器 */
    s_ctx.last_hour = -1;
    s_ctx.last_min = -1;
    update_hands();
#if ANALOG_SECOND_SWEEP
    lv_timer_create(analog_tick_cb, 50, NULL);   /* 20 Hz → 每秒 6° 平滑扫过 */
#else
    lv_timer_create(analog_tick_cb, 1000, NULL); /* 1 Hz → 每秒跳一格 */
#endif

    return s_ctx.meter;
}
