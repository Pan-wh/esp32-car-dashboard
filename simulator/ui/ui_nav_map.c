/**
 * ui_nav_map.c —— 导航地图层实现
 *
 * 配色参考深色地图：近黑底、灰色路网、更暗的建筑轮廓、纯白路线；
 * 车头箭头固定在屏幕中心，地图绕它旋转（航向朝上）。
 *
 * ## 屏幕坐标变换
 *
 * 输入是"以窗口中心为原点的局部米坐标（x 东、y 北）"以及"自车在该坐标系中
 * 的偏移（ox, oy）"。设航向为 h（度，0=北、顺时针）：
 *
 *     相对自车的东向/北向位移：dx = x - ox，dy = y - oy
 *     航向朝上（把"前方"转到屏幕正上方）：
 *         sx = CX + ( dx*cos h - dy*sin h) * SCALE
 *         sy = CY - ( dx*sin h + dy*cos h) * SCALE
 *
 * 推导：某点相对自车的方位角 b = atan2(dx, dy)，相对航向的夹角 β = b - h；
 * 航向朝上时该点出现在"从正上方顺时针 β"处，于是
 *     sx = CX + d*sin β = CX + (dx*cos h - dy*sin h)
 *     sy = CY - d*cos β = CY - (dx*sin h + dy*cos h)
 */

#include "ui_nav_map.h"

#include <math.h>
#include <string.h>

#include "nav_map.h"

/* ---------------- 配色与几何常量 ---------------- */

#define MAP_COLOR_BG       0x08080A /* 底：近黑（不纯黑，避免与息屏混淆）*/
#define MAP_COLOR_ROAD_HI  0x55555F /* 高速/主干 */
#define MAP_COLOR_ROAD_MID 0x3E3E46 /* 次干/支路 */
#define MAP_COLOR_ROAD_LO  0x2C2C33 /* 辅路/其它 */
#define MAP_COLOR_BLDG     0x25252B /* 建筑轮廓 */
#define MAP_COLOR_ROUTE    0xFFFFFF /* 路线：纯白 */

/* 与 LCD 尺寸一致 */
#define MAP_SCREEN_W 466
#define MAP_SCREEN_H 466
#define MAP_CX       (MAP_SCREEN_W / 2)
#define MAP_CY       (MAP_SCREEN_H / 2)

/** 像素/米：屏幕半径 ÷ 可见半径 */
#define MAP_SCALE ((float)MAP_CX / NAV_MAP_VIEW_RADIUS_M)

/** 各等级道路的线宽（对应网关的道路等级 0..5）*/
static int road_width(uint8_t cls)
{
    switch (cls) {
    case 0: return 7;
    case 1: return 6;
    case 2: return 5;
    case 3: return 3;
    case 4: return 2;
    default: return 2;
    }
}

static uint32_t road_color(uint8_t cls)
{
    if (cls <= 1) return MAP_COLOR_ROAD_HI;
    if (cls <= 3) return MAP_COLOR_ROAD_MID;
    return MAP_COLOR_ROAD_LO;
}

/* ---------------- 状态 ---------------- */

static lv_obj_t *s_root;                                    /* 图层容器 */
static lv_obj_t *s_road_obj[NAV_MAP_MAX_ROADS];
static lv_obj_t *s_bldg_obj[NAV_MAP_MAX_BLDGS];
static lv_obj_t *s_route_obj;
static lv_obj_t *s_arrow;

/** 点缓冲：`lv_line_set_points` 只保存指针，缓冲必须常驻 */
static lv_point_t s_road_pts[NAV_MAP_MAX_ROADS][NAV_MAP_PTS_PER_FEATURE + 2];
static lv_point_t s_bldg_pts[NAV_MAP_MAX_BLDGS][NAV_MAP_PTS_PER_FEATURE + 2];
static lv_point_t s_route_pts[NAV_MAP_MAX_ROUTE_PTS + 2];

/** 固定车头箭头（雪佛龙形，指向屏幕上方；坐标相对屏幕）*/
static lv_point_t s_arrow_pts[5];

static bool s_inited;
static bool s_dirty = true;        /* 需要重算 */
static float s_heading_deg = 0.0f; /* 当前航向 */
static float s_used_heading = 999.0f;
static int s_used_ox = INT32_MIN;
static int s_used_oy = INT32_MIN;
static uint32_t s_redraws;

/* ---------------- 内部工具 ---------------- */

/** 把局部米坐标投影到屏幕（见文件头推导）*/
static void project(int mx, int my, int ox, int oy,
                    float cos_h, float sin_h, lv_point_t *out)
{
    const float dx = (float)(mx - ox);
    const float dy = (float)(my - oy);
    const float rx = dx * cos_h - dy * sin_h;
    const float ry = dx * sin_h + dy * cos_h;
    out->x = (lv_coord_t)lroundf((float)MAP_CX + rx * MAP_SCALE);
    out->y = (lv_coord_t)lroundf((float)MAP_CY - ry * MAP_SCALE);
}

/** 所有对象先隐藏；本次要画的再显示 */
static void hide_all(void)
{
    for (int i = 0; i < NAV_MAP_MAX_ROADS; i++) {
        if (s_road_obj[i]) lv_obj_add_flag(s_road_obj[i], LV_OBJ_FLAG_HIDDEN);
    }
    for (int i = 0; i < NAV_MAP_MAX_BLDGS; i++) {
        if (s_bldg_obj[i]) lv_obj_add_flag(s_bldg_obj[i], LV_OBJ_FLAG_HIDDEN);
    }
    if (s_route_obj) lv_obj_add_flag(s_route_obj, LV_OBJ_FLAG_HIDDEN);
}

/* ---------------- 对外接口 ---------------- */

void ui_nav_map_init(lv_obj_t *parent)
{
    if (s_inited) return;

    s_root = lv_obj_create(parent);
    lv_obj_remove_style_all(s_root);
    lv_obj_set_size(s_root, MAP_SCREEN_W, MAP_SCREEN_H);
    lv_obj_set_pos(s_root, 0, 0);
    lv_obj_set_style_bg_color(s_root, lv_color_hex(MAP_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN); /* 无窗口时隐藏 */

    /*
     * 创建顺序即层级：道路 → 建筑 → 路线 → 箭头。
     * 后创建的在上层，因此路线压住路网、箭头压住一切（与参考项目一致）。
     */
    for (int i = 0; i < NAV_MAP_MAX_ROADS; i++) {
        s_road_obj[i] = lv_line_create(s_root);
        lv_obj_remove_style_all(s_road_obj[i]);
        lv_obj_set_pos(s_road_obj[i], 0, 0);
        lv_obj_add_flag(s_road_obj[i], LV_OBJ_FLAG_HIDDEN);
        lv_line_set_y_invert(s_road_obj[i], false);
    }
    for (int i = 0; i < NAV_MAP_MAX_BLDGS; i++) {
        s_bldg_obj[i] = lv_line_create(s_root);
        lv_obj_remove_style_all(s_bldg_obj[i]);
        lv_obj_set_pos(s_bldg_obj[i], 0, 0);
        lv_obj_set_style_line_color(s_bldg_obj[i], lv_color_hex(MAP_COLOR_BLDG), 0);
        lv_obj_set_style_line_width(s_bldg_obj[i], 2, 0);
        lv_obj_set_style_line_rounded(s_bldg_obj[i], true, 0);
        lv_obj_add_flag(s_bldg_obj[i], LV_OBJ_FLAG_HIDDEN);
        lv_line_set_y_invert(s_bldg_obj[i], false);
    }

    s_route_obj = lv_line_create(s_root);
    lv_obj_remove_style_all(s_route_obj);
    lv_obj_set_pos(s_route_obj, 0, 0);
    lv_obj_set_style_line_color(s_route_obj, lv_color_hex(MAP_COLOR_ROUTE), 0);
    lv_obj_set_style_line_width(s_route_obj, 9, 0);
    lv_obj_set_style_line_rounded(s_route_obj, true, 0);
    lv_obj_add_flag(s_route_obj, LV_OBJ_FLAG_HIDDEN);
    lv_line_set_y_invert(s_route_obj, false);

    /* 车头箭头：雪佛龙（tip → 左 → 内凹 → 右 → 回 tip）*/
    s_arrow_pts[0].x = (lv_coord_t)MAP_CX;
    s_arrow_pts[0].y = (lv_coord_t)(MAP_CY - 24);
    s_arrow_pts[1].x = (lv_coord_t)(MAP_CX - 15);
    s_arrow_pts[1].y = (lv_coord_t)(MAP_CY + 14);
    s_arrow_pts[2].x = (lv_coord_t)MAP_CX;
    s_arrow_pts[2].y = (lv_coord_t)(MAP_CY + 4);
    s_arrow_pts[3].x = (lv_coord_t)(MAP_CX + 15);
    s_arrow_pts[3].y = (lv_coord_t)(MAP_CY + 14);
    s_arrow_pts[4] = s_arrow_pts[0];

    s_arrow = lv_line_create(s_root);
    lv_obj_remove_style_all(s_arrow);
    lv_obj_set_pos(s_arrow, 0, 0);
    lv_obj_set_style_line_color(s_arrow, lv_color_hex(MAP_COLOR_ROUTE), 0);
    lv_obj_set_style_line_width(s_arrow, 4, 0);
    lv_obj_set_style_line_rounded(s_arrow, true, 0);
    lv_line_set_points(s_arrow, s_arrow_pts, 5);

    s_inited = true;
    s_dirty = true;
}

void ui_nav_map_set_heading(float deg)
{
    if (deg < 0.0f || deg >= 360.0f) return; /* 无效航向：保持上一次 */
    s_heading_deg = deg;
}

bool ui_nav_map_has_data(void)
{
    return nav_map_active() != NULL;
}

void ui_nav_map_invalidate(void)
{
    s_dirty = true;
}

uint32_t ui_nav_map_redraw_count(void)
{
    return s_redraws;
}

void ui_nav_map_update(void)
{
    if (!s_inited) return;

    const nav_map_window_t *w = nav_map_active();

    /* 没有窗口：整层隐藏（保留上层导航指引，仍能正常指路）*/
    if (w == NULL) {
        if (!lv_obj_has_flag(s_root, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN);
        }
        s_dirty = true;
        s_used_heading = 999.0f;
        return;
    }

    int ox = 0, oy = 0;
    (void)nav_map_get_vehicle_offset(&ox, &oy);

    /*
     * 节流：航向变化足够大、或自车位移足够大、或窗口刚切换时才重算。
     * 直行时地图几乎不动，这一条把地图层的开销压到接近零 ——
     * 本项目速度表盘的渲染预算已经很紧（见 docs/perf/README.md）。
     */
    const float dh = fabsf(s_heading_deg - s_used_heading);
    const float dm = sqrtf((float)((ox - s_used_ox) * (ox - s_used_ox) +
                                   (oy - s_used_oy) * (oy - s_used_oy)));
    if (!s_dirty && dh < NAV_MAP_REDRAW_DEG && dm < NAV_MAP_REDRAW_M) return;

    s_used_heading = s_heading_deg;
    s_used_ox = ox;
    s_used_oy = oy;
    s_dirty = false;
    s_redraws++;

    /*
     * ⚠️ 必须先全部隐藏：新窗口的道路/建筑可能比上一帧少，
     *    不隐藏的话旧要素会残留（画面里出现"幽灵路"）。
     */
    hide_all();

    const float h_rad = s_heading_deg * (float)M_PI / 180.0f;
    const float cos_h = cosf(h_rad);
    const float sin_h = sinf(h_rad);

    /* ---- 道路 ---- */
    const int n_roads = (w->n_roads < NAV_MAP_MAX_ROADS) ? w->n_roads : NAV_MAP_MAX_ROADS;
    for (int i = 0; i < n_roads; i++) {
        const nav_map_road_t *r = &w->roads[i];
        const int n = (r->n < NAV_MAP_PTS_PER_FEATURE) ? r->n : NAV_MAP_PTS_PER_FEATURE;
        if (n < 2) continue;
        for (int k = 0; k < n; k++) {
            project(r->pts[k].x, r->pts[k].y, ox, oy, cos_h, sin_h, &s_road_pts[i][k]);
        }
        lv_obj_set_style_line_color(s_road_obj[i], lv_color_hex(road_color(r->cls)), 0);
        lv_obj_set_style_line_width(s_road_obj[i], road_width(r->cls), 0);
        lv_line_set_points(s_road_obj[i], s_road_pts[i], (uint16_t)n);
        lv_obj_clear_flag(s_road_obj[i], LV_OBJ_FLAG_HIDDEN);
    }

    /* ---- 建筑（闭合轮廓：末尾补回首点）---- */
    const int n_bldgs = (w->n_bldgs < NAV_MAP_MAX_BLDGS) ? w->n_bldgs : NAV_MAP_MAX_BLDGS;
    for (int i = 0; i < n_bldgs; i++) {
        const nav_map_bldg_t *b = &w->bldgs[i];
        const int n = (b->n < NAV_MAP_PTS_PER_FEATURE) ? b->n : NAV_MAP_PTS_PER_FEATURE;
        if (n < 3) continue;
        for (int k = 0; k < n; k++) {
            project(b->pts[k].x, b->pts[k].y, ox, oy, cos_h, sin_h, &s_bldg_pts[i][k]);
        }
        s_bldg_pts[i][n] = s_bldg_pts[i][0]; /* 闭合 */
        lv_line_set_points(s_bldg_obj[i], s_bldg_pts[i], (uint16_t)(n + 1));
        lv_obj_clear_flag(s_bldg_obj[i], LV_OBJ_FLAG_HIDDEN);
    }

    /* ---- 路线 ---- */
    const int n_route = (w->n_route < NAV_MAP_MAX_ROUTE_PTS) ? w->n_route : NAV_MAP_MAX_ROUTE_PTS;
    if (n_route >= 2) {
        for (int k = 0; k < n_route; k++) {
            project(w->route[k].x, w->route[k].y, ox, oy, cos_h, sin_h, &s_route_pts[k]);
        }
        lv_line_set_points(s_route_obj, s_route_pts, (uint16_t)n_route);
        lv_obj_clear_flag(s_route_obj, LV_OBJ_FLAG_HIDDEN);
    }

    if (lv_obj_has_flag(s_root, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_clear_flag(s_root, LV_OBJ_FLAG_HIDDEN);
    }
}
