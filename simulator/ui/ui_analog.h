/**
 * ui_analog.h —— 模拟指针时钟页
 */
#ifndef UI_ANALOG_H
#define UI_ANALOG_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif


/* 创建模拟时钟到 parent（页面容器）；返回表盘对象 */
lv_obj_t *ui_analog_create(lv_obj_t *parent);

#ifdef __cplusplus
}
#endif

#endif /* UI_ANALOG_H */
