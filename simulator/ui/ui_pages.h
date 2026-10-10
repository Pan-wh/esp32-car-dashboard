/**
 * ui_pages.h —— 页面管理（模拟时钟 / 数字时钟切换）
 *
 * 交互：左右滑动切换页面（LV_EVENT_GESTURE）。
 * 页面用隐藏/显示切换（隐藏页不重绘，开销可忽略）。
 */
#ifndef UI_PAGES_H
#define UI_PAGES_H

#include "lvgl.h"

typedef enum {
    UI_PAGE_ANALOG = 0, /* 模拟指针时钟 */
    UI_PAGE_DIGITAL,    /* 数字时钟（iPhone 息屏风格） */
    UI_PAGE_SPEED,      /* 速度表盘（GNSS） */
    UI_PAGE_COUNT
} ui_page_t;

/* 创建全部页面并显示默认页 */
void ui_pages_init(void);

/* 切换到指定页 */
void ui_pages_show(ui_page_t page);

/* 当前页 */
ui_page_t ui_pages_current(void);

#endif /* UI_PAGES_H */
