/**
 * ui_clock.h —— 主时钟表盘（与固件共享的 UI 代码）
 *
 * 仅使用 LVGL 8.4 标准 API，不含任何 PC/设备平台代码，
 * 移植到 Arduino 固件时把本目录下的 ui 源文件一并加入工程即可。
 */
#ifndef UI_CLOCK_H
#define UI_CLOCK_H

#include "lvgl.h"

/* 创建主时钟表盘；parent 通常传 lv_scr_act() */
void ui_clock_create(lv_obj_t *parent);

#endif /* UI_CLOCK_H */
