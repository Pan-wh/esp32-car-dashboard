/**
 * ui_nav_map.h —— 导航地图层（深色底图 + 白路线 + 固定车头箭头）
 *
 * ## 它是什么
 *
 * 参考项目那种"真实地图"界的**背景层**：灰色道路 / 灰色建筑轮廓 / 白色路线，
 * 以及永远在屏幕中心、指向屏幕前方的车头箭头。地图整体按**航向朝上**旋转，
 * 随车移动反向平移。
 *
 * ## 数据从哪来
 *
 * 来自固件已解析好的 `nav_map`（见 nav_map.h）与 `nav` 消息里的自车偏移
 * `ox`/`oy`。**本模块不做任何经纬度运算** —— 拿到的是以窗口中心为原点的
 * 局部米坐标，只需"旋转 + 缩放 + 平移"。
 *
 * ## 为什么用一堆 lv_line 而不是 canvas
 *
 * LVGL 8.4 的 `lv_line` **支持 `line_width` 样式**（`lv_obj_init_draw_line_dsc`
 * 会取出来），因此粗白线、粗道路都能画；而 canvas 需要每帧重绘整块 466×466
 * 缓冲区，代价高得多。LVGL 8 没有多边形填充，所以建筑画成**闭合轮廓**
 * （深色地图上的线框风格，与参考项目的观感接近）。
 *
 * ## 性能取舍（重要）
 *
 * 每帧重算 40 个多段线在 LVGL 8.4 上代价不小，而本项目速度表盘的渲染预算
 * 已经很紧。因此：
 *   - 只在**航向变化 ≥ NAV_MAP_REDRAW_DEG** 或**自车位移 ≥ 阈值**时重算；
 *   - 直行时地图基本不动，几乎不产生额外开销。
 */

#ifndef UI_NAV_MAP_H
#define UI_NAV_MAP_H

#include <stdbool.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 航向变化超过这个角度（度）才重算地图坐标 */
#define NAV_MAP_REDRAW_DEG 2.0f
/** 自车位移超过这么多米才重算 */
#define NAV_MAP_REDRAW_M 2.0f
/** 屏幕可见范围对应的实际半径（米）。窗口半径 500m，这里只显示中间一部分。 */
#define NAV_MAP_VIEW_RADIUS_M 300.0f

/**
 * 创建地图层。
 *
 * @param parent 导航页的容器（地图会在**最底层**，之后创建的图标/文字在其上）
 */
void ui_nav_map_init(lv_obj_t *parent);

/**
 * 设置航向（度，0=北，顺时针）。由导航定位的 course 提供。
 * 航向无效（静止/低速时 CoreLocation 不给 course）时传负值，表示"保持上一次"。
 */
void ui_nav_map_set_heading(float deg);

/**
 * 按当前窗口 + 自车偏移 + 航向，必要时重算并重绘地图。
 * 应在导航页可见时、每次主循环调用（内部自带节流）。
 */
void ui_nav_map_update(void);

/** 是否有可画的地图窗口（无窗口时调用方应让地图层隐藏） */
bool ui_nav_map_has_data(void);

/** 强制下次 update 重算（窗口切换后调用，否则要等航向变化才刷新） */
void ui_nav_map_invalidate(void);

/** 诊断：已重算次数 */
uint32_t ui_nav_map_redraw_count(void);

#ifdef __cplusplus
}
#endif

#endif /* UI_NAV_MAP_H */
