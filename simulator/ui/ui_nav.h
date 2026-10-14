/**
 * ui_nav.h —— 导航指引卡片表盘（阶段 1：UI 原型）
 *
 * 显示（对齐《导航卡片可行性评估》§3，用户决策 **不显示路名**）：
 *   导航状态 + 大号转向图标 + 距下一转弯距离 + 当前车速
 *
 * 数据来源在阶段 1 是**模拟的**（串口/模拟器喂入）；阶段 2 再接入 BLE。
 * 本模块只负责"怎么显示"，不关心数据从哪来。
 */
#ifndef UI_NAV_H
#define UI_NAV_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 构建表盘内容（注册表回调签名一致：注册时转型为 void(*)(void*)）*/
lv_obj_t *ui_nav_create(lv_obj_t *parent);

/*
 * ---- 数据设置（全部带变更检测，可安全地高频调用）----
 *
 * ⚠️ 项目约定：LVGL 8 的 setter 无条件 invalidate，若外部周期性调用
 *    而不做变更检测，空闲时也会几十 Hz 重绘 → 全局卡顿（已踩坑）。
 */
void ui_nav_set_maneuver(int maneuver);        /* nav_maneuver_t */
void ui_nav_set_distance(int meters);          /* NAV_DISTANCE_INVALID = 无效 */
void ui_nav_set_state(int state);              /* nav_state_t */
void ui_nav_set_speed(float kmh, bool valid);  /* 当前车速 */

/**
 * 告知"当前是否有地图窗口"，用于切换两套布局。
 *
 * 有地图时把大图标与 150px 距离收起来，改用紧凑布局，把画面主体让给地图；
 * 无地图（未连接/无网/窗口过期）时回到原来的大字卡片 —— **保证导航指引
 * 在没有底图时依然清晰可读**，这是刻意的降级策略。
 *
 * 高频调用安全：内部只在模式变化时才动布局。
 */
void ui_nav_set_map_mode(bool on);

/* ---- 诊断查询（串口 nav 命令用）---- */
int ui_nav_get_maneuver(void);
int ui_nav_get_distance(void);
int ui_nav_get_state(void);
float ui_nav_get_speed(void);
bool ui_nav_get_speed_valid(void);

#ifdef __cplusplus
}
#endif

#endif /* UI_NAV_H */
