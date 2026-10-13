/**
 * ui_analog.c —— Apple StandBy 小组件风格模拟时钟
 *
 * 设计规范详见 design/wireframes/analog-clock.md：
 *   - 纯黑背景（AMOLED 息屏省电）
 *   - 外围 60 个短刻度；12/3/6/9 对应刻度为加粗橙色
 *   - 12/3/6/9 为主数字，其余八个数字缩小
 *   - 指针使用上一版几何：白色时分针 + 橙色细秒针
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

#include "ui_baked_images.h"

#include <math.h>
#include <stdio.h>

#include "ui_timesrc.h"

/* ---- 设计常量（与 design/wireframes/analog-clock.md 对应）---- */
#define UI_BG_COLOR    0x000000  /* 背景：纯黑 */
#define UI_TICK_MINOR  0x5E5E63  /* 分钟刻度：灰 */
#define UI_TICK_MAJOR  0xE5E5EA  /* 整点刻度：浅灰（更醒目） */
#define UI_NUM_COLOR   0xF5F5F7  /* 数字：白 */
#define UI_HAND_WHITE  0xF5F5F7  /* 时针/分针：白 */
#define UI_HAND_SEC    0xFF9500  /* 秒针：橙 */

#define METER_SIZE     432  /* 表盘直径，r=216（与当前数字/表圈保持一致） */
#define R_EDGE         216  /* = METER_SIZE / 2，指针长度基准 */
#define TICK_LEN_MIN   7
#define TICK_LEN_MAJ   12
#define NEEDLE_HOUR    106  /* 还原上一版时针长度 */
#define NEEDLE_MIN     150  /* 还原上一版分针长度 */
#define NEEDLE_SEC     166  /* 还原上一版秒针长度 */
#define NUM_RADIUS     160  /* 数字整体向圆心收，建立刻度与数字间留白 */
#define NUM_Y_NUDGE    (-2) /* 字体框含降部空间，视觉中心微调 */

/* 烘焙好的静态表盘（刻度 + 数字 1-12）声明见 ui_baked_images.h */

/* 秒针模式：1 = 连续线性扫秒；0 = 每秒跳动（当前）。
 * 两种模式共用同一时间源，切换本宏即可（详见 design/wireframes/analog-clock.md 第 6 节）。 */
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
    struct tm rtc_time;
    uint32_t rtc_read_tick;
    int rtc_time_valid;
} analog_ctx_t;

static analog_ctx_t s_ctx;
static lv_obj_t *s_dot_inner;

static void update_hands(void)
{
    struct tm t;
    int ms = 0;
    uint32_t tick = lv_tick_get();
    int rtc_provider = ui_timesrc_uses_provider();

    if (rtc_provider) {
        /*
         * PCF85063 没有亚秒寄存器。时分针每秒从 RTC 校准一次；秒针仅作为
         * 连续动画，由 MCU 单调时钟驱动，避免每 50ms 读取一次 I2C RTC。
         */
        if (!s_ctx.rtc_time_valid ||
            (uint32_t)(tick - s_ctx.rtc_read_tick) >= 1000U) {
            ui_timesrc_now(&s_ctx.rtc_time);
            s_ctx.rtc_read_tick = tick;
            s_ctx.rtc_time_valid = 1;
        }
        t = s_ctx.rtc_time;
    } else {
        /* 模拟器继续使用系统亚秒；冻结时间仍保持确定性截图。 */
        ui_timesrc_now_ms(&t, &ms);
    }

    int32_t v_sec;
#if ANALOG_SECOND_SWEEP
    if (rtc_provider) {
        /* 秒针精度不参与报时：独立做 60 秒一圈的约 30Hz 连续动画。 */
        uint32_t phase_ms = tick % 60000U;
        v_sec = (int32_t)(phase_ms * SCALE_MAX / 60000U);
    } else {
        /* 模拟器使用真实时间：1 秒 = 60 单位 = 6°。 */
        v_sec = (int32_t)t.tm_sec * 60 + (int32_t)ms * 60 / 1000;
    }
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

/*
 * 数字 1-12 已烘焙进 dial_analog 图片（原 create_numerals 已移除，
 * 12 个 lv_label 的绘制开销随之消失；字体 num_dial_38 已随之删除）。
 */

/* 还原上一版中心轴：18px 白底 + 8px 橙色轴心。 */
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
    /* 页面背景透明：整屏黑色底色由 scr 提供，避免滑页时每页各做一次
       整屏不透明填充（434K 像素 × 页数），可显著降低整屏重绘成本 */
    lv_obj_set_style_bg_opa(parent, LV_OPA_TRANSP, 0);

    /*
     * 静态表盘：刻度与数字烘焙成图片（最底层）。
     * 原先由 lv_meter 逐刻度绘制 + 12 个 lv_label 数字；实测 74 个刻度
     * 在整屏重绘（滑页）时约值 50ms，改为图片后该成本变成一次图片拷贝。
     * 外观与设计一致（几何常量与下方常量区相同），且离线超采样渲染更平滑。
     */
    lv_obj_t *face = lv_img_create(parent);
    lv_img_set_src(face, &dial_analog);
    lv_obj_center(face);
    lv_obj_clear_flag(face, LV_OBJ_FLAG_CLICKABLE);

    /* 指针：只留三条指针的 lv_meter（无刻度） */
    s_ctx.meter = lv_meter_create(parent);
    lv_obj_remove_style_all(s_ctx.meter); /* 去掉默认底/边框/内边距；刻度与指针自带宽高 */
    lv_obj_set_size(s_ctx.meter, METER_SIZE, METER_SIZE);
    lv_obj_center(s_ctx.meter);
    lv_obj_clear_flag(s_ctx.meter, LV_OBJ_FLAG_CLICKABLE); /* 触摸手势交给页面容器 */
    lv_obj_set_style_bg_opa(s_ctx.meter, LV_OPA_TRANSP, 0);
    /* 指针圆头（见 meter_draw_cb） */
    lv_obj_add_event_cb(s_ctx.meter, meter_draw_cb, LV_EVENT_DRAW_PART_BEGIN, NULL);

    /*
     * 刻度已烘焙进表盘图片，这里不再添加刻度表（tick 数为 0 → 不绘制）。
     * 保留 scale 是因为指针必须挂在某个 scale 上。
     */
    lv_meter_scale_t *sc_min = lv_meter_add_scale(s_ctx.meter);
    lv_meter_set_scale_ticks(s_ctx.meter, sc_min, 0, 0, 0, lv_color_hex(UI_TICK_MINOR));
    lv_meter_set_scale_range(s_ctx.meter, sc_min, 0, SCALE_MAX, 360, 270);

    lv_meter_scale_t *sc_hour = lv_meter_add_scale(s_ctx.meter);
    lv_meter_set_scale_ticks(s_ctx.meter, sc_hour, 0, 0, 0,
                             lv_color_hex(UI_TICK_MAJOR));
    lv_meter_set_scale_range(s_ctx.meter, sc_hour, 0, SCALE_MAX, 360, 270);

    /* 还原上一版绘制顺序与线宽：时针 9、分针 5、秒针 3。 */
    s_ctx.ind_hour = lv_meter_add_needle_line(s_ctx.meter, sc_min, 9,
                                              lv_color_hex(UI_HAND_WHITE),
                                              -(R_EDGE - NEEDLE_HOUR));
    s_ctx.ind_min = lv_meter_add_needle_line(s_ctx.meter, sc_min, 5,
                                             lv_color_hex(UI_HAND_WHITE),
                                             -(R_EDGE - NEEDLE_MIN));
    s_ctx.ind_sec = lv_meter_add_needle_line(s_ctx.meter, sc_min, 3,
                                             lv_color_hex(UI_HAND_SEC),
                                             -(R_EDGE - NEEDLE_SEC));

    /* 中心点（在指针之上）；数字已烘焙进表盘图片，无需运行时创建 */
    create_center_dot(parent);

    /* 初始化指针位置并启动刷新定时器 */
    s_ctx.last_hour = -1;
    s_ctx.last_min = -1;
    s_ctx.rtc_time_valid = 0;
    update_hands();
#if ANALOG_SECOND_SWEEP
    lv_timer_create(analog_tick_cb, 33, NULL);   /* 约 30 Hz → 每秒 6° 平滑扫过 */
#else
    lv_timer_create(analog_tick_cb, 1000, NULL); /* 1 Hz → 每秒跳一格 */
#endif

    return s_ctx.meter;
}
