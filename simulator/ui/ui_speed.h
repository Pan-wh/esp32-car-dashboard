/**
 * ui_speed.h —— 速度表盘页
 *
 * 数据来源：目前为模拟值（GNSS 模块未接入）。接入后由 GNSS 模块调用
 * ui_speed_set_speed() / ui_speed_set_fix() 驱动，页面代码不用改。
 */
#ifndef UI_SPEED_H
#define UI_SPEED_H

#include "lvgl.h"
#include <stdbool.h>

/* 设计变体数量（选型用；定稿后保留选定的一种） */
#define UI_SPEED_VARIANT_COUNT 4

/* 创建速度表盘到 parent（页面容器） */
lv_obj_t *ui_speed_create(lv_obj_t *parent);

/* 切换设计变体（0..UI_SPEED_VARIANT_COUNT-1）；已创建时会重建内容 */
void ui_speed_set_variant(int variant);

/* 设置速度（km/h）；valid=false 时显示无效状态（"--"） */
void ui_speed_set_speed(float kmh);

/* 设置 GNSS 定位状态：valid=是否有定位，sats=卫星数 */
void ui_speed_set_fix(bool valid, int sats);

#endif /* UI_SPEED_H */
