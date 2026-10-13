/**
 * ui_pages.h —— 页面管理（模拟时钟 / 数字时钟切换）
 *
 * 交互：左右滑动切换页面（LV_EVENT_GESTURE）。
 * 页面用隐藏/显示切换（隐藏页不重绘，开销可忽略）。
 */
#ifndef UI_PAGES_H
#define UI_PAGES_H

#include <stdbool.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif


typedef enum {
    UI_PAGE_ANALOG = 0, /* 模拟指针时钟 */
    UI_PAGE_DIGITAL,    /* 数字时钟（iPhone 息屏风格） */
    UI_PAGE_SPEED,      /* 速度表盘（数据源：iPhone BLE 遥测）*/
    UI_PAGE_COLOR_CLOCK,/* 多彩四位数字时钟 */
    UI_PAGE_NAV,        /* 导航指引卡片（阶段 1：UI 原型）*/
    UI_PAGE_COUNT
} ui_page_t;

/* 创建全部页面并显示默认页 */
void ui_pages_init(void);

/* 切换到指定页 */
void ui_pages_show(ui_page_t page);

/*
 * 快速划动（flick）翻页：dir = +1 下一页，-1 上一页。
 *
 * 为什么需要它：一帧渲染约 110ms，LVGL 的输入读取被拖慢；用户快速一划时
 * 「按下→移动→抬起」可能只被 LVGL 采到**一个**坐标点，位移量恒为 0，
 * 于是原生拖拽滚动完全不动（用户实测「滑一页后要等一会才能再滑」）。
 * 固件侧的 100Hz 触摸采样能拿到完整轨迹，因此由它判定 flick 并直接翻页。
 *
 * 若容器已被原生滚动带走（缓慢拖拽的情形），本函数不动作，避免翻两页。
 */
void ui_pages_flick(int dir);

/*
 * 若目标表盘是速度表（UI_PAGE_SPEED），触发一次自检扫表（#2b）。
 * 集中在此判断，避免"什么算速度表"的知识散落到固件各调用点。
 */
void ui_face_sweep_if_speed(int face_id);

/* 松手后按最近页吸附（无 flick 的缓慢拖拽用），同样走 130ms 短动画 */
void ui_pages_settle(void);

/* 当前页 */
ui_page_t ui_pages_current(void);

/* 取页面容器对象（供固件挂诊断/扩展回调用；找不到返回 NULL） */
lv_obj_t *ui_pages_obj(ui_page_t page);

/*
 * 表盘切换（功能③）：按 face id 切换，返回是否成功。
 * face id 与 ui_page_t 一一对应（注册顺序一致）；越界返回 false 且不动作。
 */
bool ui_pages_set_face(int face_id);

/* 当前表盘 id（= 当前页） */
int ui_pages_current_face(void);

/* 当前表盘名（注册表里的 name；无则返回 "?"） */
const char *ui_pages_current_face_name(void);

#ifdef __cplusplus
}
#endif

#endif /* UI_PAGES_H */
