/**
 * ui_speed.c —— 速度表盘页（4 个设计变体，供选型对比）
 *
 * 变体：
 *   0 = V1 数字 + 外环进度弧      （现代电动车风格，最简洁）
 *   1 = V2 指针速度表             （与模拟时钟同语言，机械感）
 *   2 = V3 超大数字 + 底部弧形刻度带（iPhone 极简风延续）
 *   3 = V4 数字 + 分段进度条       （运动风格）
 *
 * 公共信息：大号速度数字 + 单位 km/h + GNSS 定位状态。
 * 无效状态（无定位）：数字显示 "--"，进度/指针归零，状态文字转灰。
 *
 * 数据来源：GNSS 未接入前为模拟值。
 */
#include "ui_speed.h"

#include <math.h>
#include <stdio.h>

LV_FONT_DECLARE(speed_num_150); /* Inter Bold 150px，速度大数字 */
LV_FONT_DECLARE(label_26);      /* Inter Medium 26px，单位/标签/状态 */

/* ---- 设计常量 ---- */
#define C_BG      0x000000 /* 纯黑背景（OLED 省电） */
#define C_MAIN    0xF5F5F7 /* 主数据：白 */
#define C_DIM     0x8A919E /* 次要文字：中灰 */
#define C_TRACK   0x26262A /* 轨道/未点亮段：暗灰 */
#define C_ACCENT  0x0A84FF /* 强调色：进度/指针（蓝） */
#define C_LABEL   0xC8C8CE /* 刻度数字：浅灰 */

#define SPEED_MAX 200.0f /* 量程 km/h */

#define SEG_CNT 24 /* V4 分段数 */

/* ---- 状态 ---- */
static lv_obj_t *s_root;
static int s_variant;
static float s_speed = 88.0f; /* GNSS 接入前模拟值 */
static bool s_valid = true;
static int s_sats = 12;

/* ---- 各变体的动态部件 ---- */
static lv_obj_t *s_big;              /* 大数字（所有变体） */
static lv_obj_t *s_status;           /* GNSS 状态（所有变体） */
static lv_obj_t *s_arc;              /* V1 主弧 / V3 底部弧 */
static lv_obj_t *s_meter;            /* V2 指针表 */
static lv_meter_indicator_t *s_needle;
static lv_obj_t *s_segs[SEG_CNT];    /* V4 分段 */

/* ------------------------------------------------------------------ */
/* 公共部件                                                            */
/* ------------------------------------------------------------------ */

static lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font,
                            uint32_t color, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, text);
    return l;
}

static void make_big_and_unit(lv_obj_t *parent, int big_y, int unit_y)
{
    s_big = make_label(parent, &speed_num_150, C_MAIN, "88");
    lv_obj_align(s_big, LV_ALIGN_CENTER, 0, big_y);

    lv_obj_t *unit = make_label(parent, &label_26, C_DIM, "km/h");
    lv_obj_align(unit, LV_ALIGN_CENTER, 0, unit_y);
}

static void make_status(lv_obj_t *parent, int y)
{
    s_status = make_label(parent, &label_26, C_DIM, "GPS 12 SAT");
    lv_obj_align(s_status, LV_ALIGN_CENTER, 0, y);
}

/* 圆头线帽（与模拟时钟一致的观感），用于指针表 */
static void meter_draw_cb(lv_event_t *e)
{
    lv_obj_draw_part_dsc_t *dsc = lv_event_get_draw_part_dsc(e);
    if (dsc->type == LV_METER_DRAW_PART_NEEDLE_LINE && dsc->line_dsc) {
        dsc->line_dsc->round_start = 1;
        dsc->line_dsc->round_end = 1;
    }
}

/* ------------------------------------------------------------------ */
/* V1：数字 + 外环进度弧（270°，底部开口）                              */
/* ------------------------------------------------------------------ */
static void build_v1(lv_obj_t *p)
{
    s_arc = lv_arc_create(p);
    lv_obj_set_size(s_arc, 430, 430);
    lv_obj_center(s_arc);
    lv_obj_clear_flag(s_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_style(s_arc, NULL, LV_PART_KNOB); /* 去掉拖拽旋钮 */

    lv_arc_set_rotation(s_arc, 135);          /* 起点：左下 45° 处 */
    lv_arc_set_bg_angles(s_arc, 0, 270);      /* 扫过 270°，底部留缺口 */
    lv_arc_set_range(s_arc, 0, (int32_t)SPEED_MAX);
    lv_arc_set_value(s_arc, 0);

    /* 轨道 + 进度样式 */
    lv_obj_set_style_bg_opa(s_arc, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_arc, 12, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_arc, lv_color_hex(C_TRACK), LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(s_arc, true, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_arc, 12, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_arc, lv_color_hex(C_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(s_arc, true, LV_PART_INDICATOR);

    make_big_and_unit(p, -14, 68);
    make_status(p, 150);
}

/* ------------------------------------------------------------------ */
/* V2：指针速度表（0-200，270°），底部加数字读数                        */
/* ------------------------------------------------------------------ */
static void build_v2(lv_obj_t *p)
{
    s_meter = lv_meter_create(p);
    lv_obj_remove_style_all(s_meter);
    lv_obj_set_size(s_meter, 400, 400);
    lv_obj_center(s_meter);
    lv_obj_clear_flag(s_meter, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_meter, meter_draw_cb, LV_EVENT_DRAW_PART_BEGIN, NULL);

    /* 小刻度：41 个（40 段 × 6.75°） */
    lv_meter_scale_t *sc_min = lv_meter_add_scale(s_meter);
    lv_meter_set_scale_ticks(s_meter, sc_min, 41, 2, 10, lv_color_hex(0x5A5A5F));
    lv_meter_set_scale_range(s_meter, sc_min, 0, (int32_t)SPEED_MAX, 270, 135);

    /* 大刻度：7 个 → 每 40 km/h 一根（0/40/80/120/160/200） */
    lv_meter_scale_t *sc_maj = lv_meter_add_scale(s_meter);
    lv_meter_set_scale_ticks(s_meter, sc_maj, 7, 3, 18, lv_color_hex(C_LABEL));
    lv_meter_set_scale_range(s_meter, sc_maj, 0, (int32_t)SPEED_MAX, 270, 135);

    /* 指针：白色圆头，长约 0.60R（r_edge=200） */
    s_needle = lv_meter_add_needle_line(s_meter, sc_min, 6, lv_color_hex(C_MAIN), -(200 - 120));

    /* 刻度数字：沿 135°→405° 均布（LVGL 角度：0°=3 点方向，顺时针） */
    const int vals[] = { 0, 40, 80, 120, 160, 200 };
    for (int i = 0; i < 6; i++) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%d", vals[i]);
        lv_obj_t *n = make_label(p, &label_26, C_LABEL, buf);
        float a = (float)(135 + i * 54) * 3.14159265f / 180.0f; /* 270/5 = 54° 间隔 */
        int32_t x = (int32_t)lroundf(148.0f * cosf(a));
        int32_t y = (int32_t)lroundf(148.0f * sinf(a));
        lv_obj_align(n, LV_ALIGN_CENTER, x, y);
    }

    /* 底部数字读数 + 状态 */
    s_big = make_label(p, &label_26, C_MAIN, "88");
    lv_obj_set_style_text_font(s_big, &speed_num_150, 0);
    lv_obj_align(s_big, LV_ALIGN_CENTER, 0, 118);
    lv_obj_t *unit = make_label(p, &label_26, C_DIM, "km/h");
    lv_obj_align(unit, LV_ALIGN_CENTER, 0, 168);
    make_status(p, -168);
}

/* ------------------------------------------------------------------ */
/* V3：超大数字 + 底部弧形刻度带                                        */
/* ------------------------------------------------------------------ */
static void build_v3(lv_obj_t *p)
{
    /* 底部 80° 弧带（LVGL 角度：90° = 正下方 → 50°..130°） */
    s_arc = lv_arc_create(p);
    lv_obj_set_size(s_arc, 400, 400);
    lv_obj_center(s_arc);
    lv_obj_clear_flag(s_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_style(s_arc, NULL, LV_PART_KNOB);

    lv_arc_set_rotation(s_arc, 50);
    lv_arc_set_bg_angles(s_arc, 0, 80);
    lv_arc_set_range(s_arc, 0, (int32_t)SPEED_MAX);
    lv_arc_set_value(s_arc, 0);

    lv_obj_set_style_bg_opa(s_arc, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_arc, 8, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_arc, lv_color_hex(C_TRACK), LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(s_arc, true, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_arc, 8, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_arc, lv_color_hex(C_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(s_arc, true, LV_PART_INDICATOR);

    /* 弧带两端标注 */
    lv_obj_t *l0 = make_label(p, &label_26, C_DIM, "0");
    lv_obj_align(l0, LV_ALIGN_CENTER, -160, 128);
    lv_obj_t *l1 = make_label(p, &label_26, C_DIM, "200");
    lv_obj_align(l1, LV_ALIGN_CENTER, 160, 128);

    make_big_and_unit(p, -30, 52);
    make_status(p, -168);
}

/* ------------------------------------------------------------------ */
/* V4：数字 + 分段进度条（运动风格）                                    */
/* ------------------------------------------------------------------ */
static void build_v4(lv_obj_t *p)
{
    make_big_and_unit(p, -40, 32);
    make_status(p, -168);

    /* 分段条容器（底部居中） */
    lv_obj_t *bar = lv_obj_create(p);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(bar, LV_ALIGN_CENTER, 0, 140);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(bar, 5, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < SEG_CNT; i++) {
        lv_obj_t *seg = lv_obj_create(bar);
        lv_obj_remove_style_all(seg);
        lv_obj_set_size(seg, 12, 10);
        lv_obj_set_style_radius(seg, 3, 0);
        lv_obj_set_style_bg_color(seg, lv_color_hex(C_TRACK), 0);
        lv_obj_set_style_bg_opa(seg, LV_OPA_COVER, 0);
        lv_obj_clear_flag(seg, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        s_segs[i] = seg;
    }
}

/* ------------------------------------------------------------------ */
/* 状态应用（变体无关）                                                */
/* ------------------------------------------------------------------ */

static void apply_state(void)
{
    char buf[32];

    if (!s_big) return;

    /* 大数字：无效时显示 -- */
    if (s_valid) snprintf(buf, sizeof(buf), "%.0f", s_speed);
    else snprintf(buf, sizeof(buf), "--");
    lv_label_set_text(s_big, buf);

    /* GNSS 状态文字 */
    if (s_status) {
        if (s_valid) snprintf(buf, sizeof(buf), "GPS %d SAT", s_sats);
        else snprintf(buf, sizeof(buf), "NO FIX");
        lv_label_set_text(s_status, buf);
        lv_obj_set_style_text_color(s_status, lv_color_hex(s_valid ? C_DIM : C_TRACK), 0);
    }

    int32_t v = s_valid ? (int32_t)lroundf(s_speed) : 0;

    switch (s_variant) {
    case 0:
    case 2:
        if (s_arc) lv_arc_set_value(s_arc, v);
        break;
    case 1:
        if (s_meter && s_needle) lv_meter_set_indicator_value(s_meter, s_needle, v);
        break;
    case 3: {
        int lit = (int)lroundf((float)SEG_CNT * (s_valid ? s_speed : 0.0f) / SPEED_MAX);
        if (lit < 0) lit = 0;
        if (lit > SEG_CNT) lit = SEG_CNT;
        for (int i = 0; i < SEG_CNT; i++) {
            if (!s_segs[i]) continue;
            lv_obj_set_style_bg_color(s_segs[i],
                lv_color_hex(i < lit ? C_ACCENT : C_TRACK), 0);
        }
        break;
    }
    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* 构建与对外接口                                                      */
/* ------------------------------------------------------------------ */

static void build(lv_obj_t *parent)
{
    /* 状态复位（重建时清空旧指针） */
    s_big = NULL;
    s_status = NULL;
    s_arc = NULL;
    s_meter = NULL;
    s_needle = NULL;
    for (int i = 0; i < SEG_CNT; i++) s_segs[i] = NULL;

    lv_obj_set_style_bg_color(parent, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);

    switch (s_variant) {
    case 1: build_v2(parent); break;
    case 2: build_v3(parent); break;
    case 3: build_v4(parent); break;
    case 0:
    default: build_v1(parent); break;
    }

    apply_state();
}

lv_obj_t *ui_speed_create(lv_obj_t *parent)
{
    s_root = parent;
    build(parent);
    return parent;
}

void ui_speed_set_variant(int variant)
{
    if (variant < 0 || variant >= UI_SPEED_VARIANT_COUNT) variant = 0;
    if (variant == s_variant) return;
    s_variant = variant;
    if (s_root) {
        lv_obj_clean(s_root); /* 删除旧变体内容后重建 */
        build(s_root);
    }
}

int ui_speed_get_variant(void)
{
    return s_variant;
}

void ui_speed_set_speed(float kmh)
{
    s_speed = kmh;
    apply_state();
}

void ui_speed_set_fix(bool valid, int sats)
{
    s_valid = valid;
    s_sats = sats;
    apply_state();
}
