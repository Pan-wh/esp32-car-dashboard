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

/* 设计变体数量（选型用；定稿后保留选定的一种）
 * 0=V1 数字+外环弧, 1=V2 指针表, 2=V3 超大数字+底部弧带, 3=V4 分段条,
 * 4=V5 驾驶员表盘（外圈刻度 + 指针 + 中央插画 + 底部辅助信息） */
#define UI_SPEED_VARIANT_COUNT 5

/* 驾驶员插画状态 */
#define UI_DRIVER_AUTO     (-1) /* 按速度自动 */
#define UI_DRIVER_NORMAL   0
#define UI_DRIVER_SPIRITED 1
#define UI_DRIVER_SPEEDING 2

/* 创建速度表盘到 parent（页面容器） */
lv_obj_t *ui_speed_create(lv_obj_t *parent);

/* 切换设计变体（0..UI_SPEED_VARIANT_COUNT-1）；已创建时会重建内容 */
void ui_speed_set_variant(int variant);

/* 当前变体序号 */
int ui_speed_get_variant(void);

/* 设置速度（km/h）；valid=false 时显示无效状态（"--"） */
void ui_speed_set_speed(float kmh);

/* 设置 GNSS 定位状态：valid=是否有定位，sats=卫星数 */
void ui_speed_set_fix(bool valid, int sats);

/* 驾驶员插画状态（UI_DRIVER_AUTO 表示按速度自动切换） */
void ui_speed_set_driver_state(int state);

/* 设置航向（度，0=正北顺时针）；GNSS 接入前为模拟值 */
void ui_speed_set_heading(int deg);

#endif /* UI_SPEED_H */
