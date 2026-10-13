/**
 * ui_color_clock.c - 参考 Apple StandBy 多彩数字风格的四位时钟
 *
 * 四个数字使用固定色位，冒号以两个独立圆点覆盖在中央。时间采用 24 小时制
 * 并保留小时前导零，使构图始终保持四位对称。
 */
#include "ui_color_clock.h"

#include <stdio.h>

#include "ui_timesrc.h"

#define COLOR_BG       0x09090D
#define COLOR_DIGIT_1  0xA291EE
#define COLOR_DIGIT_2  0xF1A9ED
#define COLOR_DIGIT_3  0x7868EF
#define COLOR_DIGIT_4  0x8187EE
#define COLOR_COLON    0xE8DAF9

LV_FONT_DECLARE(clock_color_224);

static lv_obj_t *s_digits[4];
static lv_timer_t *s_timer;
static int s_last_hour = -1;
static int s_last_minute = -1;

static void color_clock_tick_cb(lv_timer_t *timer)
{
    (void)timer;
    struct tm now;
    ui_timesrc_now(&now);

    if (now.tm_hour == s_last_hour && now.tm_min == s_last_minute) return;

    char text[5];
    snprintf(text, sizeof(text), "%02d%02d", now.tm_hour, now.tm_min);
    for (int i = 0; i < 4; i++) {
        char digit[2] = { text[i], '\0' };
        lv_label_set_text(s_digits[i], digit);
    }

    s_last_hour = now.tm_hour;
    s_last_minute = now.tm_min;
}

static void create_colon_dot(lv_obj_t *parent, int y)
{
    lv_obj_t *dot = lv_obj_create(parent);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, 40, 40);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(COLOR_COLON), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_align(dot, LV_ALIGN_CENTER, 0, y);
}

void ui_color_clock_create(lv_obj_t *parent)
{
    static const int16_t x_offset[4] = { -145, -49, 49, 145 };
    static const uint32_t colors[4] = {
        COLOR_DIGIT_1, COLOR_DIGIT_2, COLOR_DIGIT_3, COLOR_DIGIT_4
    };

    lv_obj_set_style_bg_color(parent, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);

    for (int i = 0; i < 4; i++) {
        s_digits[i] = lv_label_create(parent);
        lv_obj_set_style_text_font(s_digits[i], &clock_color_224, 0);
        lv_obj_set_style_text_color(s_digits[i], lv_color_hex(colors[i]), 0);
        lv_label_set_text(s_digits[i], "0");
        lv_obj_align(s_digits[i], LV_ALIGN_CENTER, x_offset[i], -4);
    }

    /* 冒号最后创建，保持参考图中悬浮在数字接缝之上的层级。 */
    create_colon_dot(parent, -45);
    create_colon_dot(parent, 45);

    s_last_hour = -1;
    s_last_minute = -1;
    color_clock_tick_cb(NULL);
    s_timer = lv_timer_create(color_clock_tick_cb, 1000, NULL);
    (void)s_timer;
}
