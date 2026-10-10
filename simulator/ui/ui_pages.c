/**
 * ui_pages.c —— 页面管理实现
 *
 * 手势说明：LVGL 把 LV_EVENT_GESTURE 发给「按下时命中的可点击对象」。
 * 为了让手势稳定送到页面容器，创建完页面内容后递归清除子对象的
 * CLICKABLE 标志，只保留页面容器可点击。
 */
#include "ui_pages.h"

#include "ui_analog.h"
#include "ui_clock.h"
#include "ui_speed.h"

#define UI_BG_COLOR 0x000000

static lv_obj_t *s_pages[UI_PAGE_COUNT];
static ui_page_t s_current = UI_PAGE_ANALOG;

/* 递归清除子对象的可点击标志（让页面容器成为触摸目标） */
static lv_obj_tree_walk_res_t clear_clickable_cb(lv_obj_t *obj, void *user_data)
{
    (void)user_data;
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    return LV_OBJ_TREE_WALK_NEXT;
}

static void gesture_cb(lv_event_t *e)
{
    (void)e;
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_get_act());

    if (dir == LV_DIR_LEFT) {
        ui_pages_show((ui_page_t)((s_current + 1) % UI_PAGE_COUNT));
    }
    else if (dir == LV_DIR_RIGHT) {
        ui_pages_show((ui_page_t)((s_current + UI_PAGE_COUNT - 1) % UI_PAGE_COUNT));
    }
}

void ui_pages_init(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(UI_BG_COLOR), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    /* 创建页面容器（全屏，透明底，由各页面自己画背景） */
    for (int i = 0; i < UI_PAGE_COUNT; i++) {
        lv_obj_t *page = lv_obj_create(scr);
        lv_obj_remove_style_all(page);
        lv_obj_set_size(page, LV_PCT(100), LV_PCT(100));
        lv_obj_set_pos(page, 0, 0);
        lv_obj_add_flag(page, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(page, gesture_cb, LV_EVENT_GESTURE, NULL);
        s_pages[i] = page;
    }

    /* 页面内容 */
    ui_analog_create(s_pages[UI_PAGE_ANALOG]);
    ui_clock_create(s_pages[UI_PAGE_DIGITAL]);
    ui_speed_create(s_pages[UI_PAGE_SPEED]);

    /* 让页面容器成为触摸目标（手势才能命中容器） */
    for (int i = 0; i < UI_PAGE_COUNT; i++) {
        lv_obj_tree_walk(s_pages[i], clear_clickable_cb, NULL);
        lv_obj_add_flag(s_pages[i], LV_OBJ_FLAG_CLICKABLE);
    }

    ui_pages_show(UI_PAGE_ANALOG);
}

void ui_pages_show(ui_page_t page)
{
    if (page < 0 || page >= UI_PAGE_COUNT) page = UI_PAGE_ANALOG;

    for (int i = 0; i < UI_PAGE_COUNT; i++) {
        if (i == (int)page) lv_obj_clear_flag(s_pages[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_pages[i], LV_OBJ_FLAG_HIDDEN);
    }
    s_current = page;
}

ui_page_t ui_pages_current(void)
{
    return s_current;
}
