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

/*
 * lv_obj 基类构造时会给**每个有父对象的对象**默认加上一组标志（见 lv_obj.c）：
 *   GESTURE_BUBBLE / SCROLL_CHAIN / SCROLLABLE / SCROLL_ELASTIC /
 *   SCROLL_MOMENTUM / SCROLL_WITH_ARROW / SNAPPABLE / PRESS_LOCK / CLICK_FOCUSABLE
 *
 * 其中 **GESTURE_BUBBLE 是滑页失效的真正原因**：手势检测会把目标沿父链上移
 * （lv_indev.c: while(has_flag(GESTURE_BUBBLE)) gesture_obj = parent），
 * 于是手势被转发给屏幕、页面容器的回调永远不会被调用。
 * （真机现象：屏幕级回调收到手势、页面不切换；模拟器自动化测试同样复现。）
 */
#define UI_UNWANTED_FLAGS (LV_OBJ_FLAG_GESTURE_BUBBLE | LV_OBJ_FLAG_EVENT_BUBBLE | \
                           LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_CHAIN | \
                           LV_OBJ_FLAG_SCROLL_ELASTIC | LV_OBJ_FLAG_SCROLL_MOMENTUM | \
                           LV_OBJ_FLAG_SCROLL_WITH_ARROW | LV_OBJ_FLAG_SNAPPABLE | \
                           LV_OBJ_FLAG_PRESS_LOCK | LV_OBJ_FLAG_CLICK_FOCUSABLE)

static lv_obj_t *s_pages[UI_PAGE_COUNT];
static ui_page_t s_current = UI_PAGE_ANALOG;
static lv_obj_t *s_cont = NULL;   /* 横向滚动容器 */
static bool s_ready = false;      /* 首次显示不做滑动动画 */

/* 页面定义（与真机/模拟器屏幕一致；改屏需同步） */
#ifndef LCD_WIDTH
#define LCD_WIDTH 466
#define LCD_HEIGHT 466
#endif

/* 滚动停止后，按 scroll_x 反推当前页（用户滑动切页时同步状态） */
static void scroll_end_cb(lv_event_t *e)
{
    (void)e;
    if (s_cont == NULL) return;

    lv_coord_t x = lv_obj_get_scroll_x(s_cont);
    int page = (int)((x + LCD_WIDTH / 2) / LCD_WIDTH); /* 四舍五入到最近页 */
    if (page < 0) page = 0;
    if (page >= UI_PAGE_COUNT) page = UI_PAGE_COUNT - 1;

    if (page != (int)s_current) {
        s_current = (ui_page_t)page;
    }
}

/*
 * 递归清除子对象的输入相关标志。
 *
 * 注意：lv_obj 基类默认带 CLICKABLE 与 **SCROLLABLE** 标志，而 remove_style_all()
 * 只清样式、不清标志。LVGL 中「滚动」与「手势」互斥（见 lv_indev.c indev_gesture：
 * 若 scroll_obj 非空则直接 return，不发送 LV_EVENT_GESTURE），
 * 因此页面与子对象必须清掉 SCROLLABLE / SCROLL_CHAIN，否则滑动手势会被吞掉、
 * 无法切页（真机实测踩过：手势事件全部落到屏幕而非页面容器）。
 */
static lv_obj_tree_walk_res_t clear_input_flags_cb(lv_obj_t *obj, void *user_data)
{
    (void)user_data;
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE | UI_UNWANTED_FLAGS);
    return LV_OBJ_TREE_WALK_NEXT;
}

/* 横向滚动容器 + 吸附分页（等价于 lv_tileview 的机制）
 *
 * 为什么不用 LV_EVENT_GESTURE 自己切页：
 *   1. lv_obj 基类默认给子对象加 GESTURE_BUBBLE，手势会被转发给父对象；
 *   2. 即便清掉该标志，切页时把被按住的页面隐藏会触发 LVGL 输入重置，
 *      同一次拖拽会连环触发多次手势（实测 0→2→1→0，看起来“滑不动”）。
 * 滚动分页没有上述问题，且自带滑动动画与吸附，是 LVGL 的标准做法。
 */

void ui_pages_init(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(UI_BG_COLOR), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_CLICKABLE | UI_UNWANTED_FLAGS);

    /*
     * 横向容器：尺寸 = **视口**（屏宽 × 屏高），页面在容器内并排摆放并超出边界，
     * 这样内容大于容器才会产生可滚动区域。
     * （坑：若把容器做成 页数×屏宽，则内容刚好填满容器 → 无滚动区域 → 滑不动。）
     */
    s_cont = lv_obj_create(scr);
    lv_obj_remove_style_all(s_cont);
    lv_obj_set_size(s_cont, LCD_WIDTH, LCD_HEIGHT);
    lv_obj_set_pos(s_cont, 0, 0);
    lv_obj_clear_flag(s_cont, UI_UNWANTED_FLAGS);
    lv_obj_add_flag(s_cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_cont, LV_DIR_HOR);              /* 只允许横向翻页 */
    lv_obj_set_scroll_snap_x(s_cont, LV_SCROLL_SNAP_CENTER);/* 松手吸附到整页 */
    lv_obj_set_scrollbar_mode(s_cont, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(s_cont, scroll_end_cb, LV_EVENT_SCROLL_END, NULL);

    /* 页面：等宽并排 */
    for (int i = 0; i < UI_PAGE_COUNT; i++) {
        lv_obj_t *page = lv_obj_create(s_cont);
        lv_obj_remove_style_all(page);
        lv_obj_set_size(page, LCD_WIDTH, LCD_HEIGHT);
        lv_obj_set_pos(page, i * LCD_WIDTH, 0);
        lv_obj_add_flag(page, LV_OBJ_FLAG_CLICKABLE); /* 作为按压目标 */
        lv_obj_clear_flag(page, UI_UNWANTED_FLAGS);
        lv_obj_add_flag(page, LV_OBJ_FLAG_SCROLL_CHAIN); /* 滚动传递给容器 */
        lv_obj_add_flag(page, LV_OBJ_FLAG_SNAPPABLE);    /* 参与吸附 */
        s_pages[i] = page;
    }

    /* 页面内容 */
    ui_analog_create(s_pages[UI_PAGE_ANALOG]);
    ui_clock_create(s_pages[UI_PAGE_DIGITAL]);
    ui_speed_create(s_pages[UI_PAGE_SPEED]);

    /* 子对象不再参与输入（只让页面容器接收按压） */
    for (int i = 0; i < UI_PAGE_COUNT; i++) {
        lv_obj_tree_walk(s_pages[i], clear_input_flags_cb, NULL);
        lv_obj_add_flag(s_pages[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(s_pages[i], LV_OBJ_FLAG_SCROLL_CHAIN | LV_OBJ_FLAG_SNAPPABLE);
    }

    s_current = UI_PAGE_ANALOG;
    ui_pages_show(UI_PAGE_ANALOG);
}

void ui_pages_flick(int dir)
{
    if (s_cont == NULL || dir == 0) return;

    /*
     * 注意：这里**不再**检查"容器是否已被原生拖拽带走"。
     * 实测（2026-10-13 真机日志）用户每次快滑都会把容器拖走几十到几百像素，
     * 若因此拒绝翻页，就会出现「滑一下要等一会才能再滑」的现象——
     * 快滑是明确的用户意图，一律按当前页翻一页。
     */
    int next = (int)s_current + dir;
    if (next < 0 || next >= UI_PAGE_COUNT) return; /* 到头不循环 */
    ui_pages_show((ui_page_t)next);
}

/* 翻页动画时长（ms）：LVGL 内置吸附按像素速度算，200-400ms 偏钝，这里固定短动画 */
#define PAGE_ANIM_MS 130

static void page_scroll_anim_cb(void *obj, int32_t v)
{
    lv_obj_scroll_to_x((lv_obj_t *)obj, (lv_coord_t)v, LV_ANIM_OFF);
}

/* 松手后按"最近的页"吸附（缓慢拖拽、未达 flick 阈值时使用） */
void ui_pages_settle(void)
{
    if (s_cont == NULL) return;
    lv_coord_t x = lv_obj_get_scroll_x(s_cont);
    int page = (int)((x + LCD_WIDTH / 2) / LCD_WIDTH); /* 四舍五入到最近页 */
    if (page < 0) page = 0;
    if (page >= UI_PAGE_COUNT) page = UI_PAGE_COUNT - 1;
    ui_pages_show((ui_page_t)page);
}

void ui_pages_show(ui_page_t page)
{
    if (page < 0 || page >= UI_PAGE_COUNT) page = UI_PAGE_ANALOG;

    if (!s_ready || s_cont == NULL) {
        lv_obj_scroll_to_view(s_pages[page], LV_ANIM_OFF);
        s_current = page;
        return;
    }

    /* 目标位置：第 page 页在容器中的横向偏移 */
    lv_coord_t target = (lv_coord_t)((int)page * LCD_WIDTH);
    lv_coord_t from = lv_obj_get_scroll_x(s_cont);

    /*
     * 先清掉容器上正在跑的滚动动画：LVGL 原生吸附用内部 scroll_x_anim，
     * 与我们的 page_scroll_anim_cb 是两个不同的 exec_cb，若不清理会同时跑、
     * 互相抢 scroll_x（表现为抖动或翻不过去）。
     */
    lv_anim_del(s_cont, NULL);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_cont);
    lv_anim_set_values(&a, from, target);
    lv_anim_set_time(&a, PAGE_ANIM_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, page_scroll_anim_cb);
    lv_anim_start(&a);

    s_current = page;
    s_ready = true;
}

ui_page_t ui_pages_current(void)
{
    return s_current;
}

lv_obj_t *ui_pages_obj(ui_page_t page)
{
    if (page < 0 || page >= UI_PAGE_COUNT) return NULL;
    return s_pages[page];
}
