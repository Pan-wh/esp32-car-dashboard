/**
 * ui_nav.c —— 导航指引卡片表盘实现（阶段 1：UI 原型）
 *
 * 设计要点：
 *   1. **转向图标用 lv_line 绘制**（而非烘焙图片）：箭头只由 1~2 条折线构成，
 *      运行时绘制开销极小，且省下 8 张图的 Flash（当前已用 77%）。
 *      若日后要更精致的图形，可再用 tools/ 烘焙替换（接口不变）。
 *   2. 所有 setter 都做**变更检测**（项目铁律，见 ui_nav.h 注释）。
 *   3. 过期/无数据时**不显示旧指引**（§6.3 安全原则）——由 nav_shows_maneuver()
 *      决定，UI 只是照做。
 *
 * 布局（466×466 圆屏，圆心 233,233）：
 *   y≈ 66  导航状态（小字）
 *   y≈162  转向图标（160×160 区域）
 *   y≈300  距离数值（150px 大字）
 *   y≈380  距离单位（m / km）
 *   y≈422  当前车速
 */
#include "ui_nav.h"

#include <stdio.h>

#include "nav.h"

LV_FONT_DECLARE(speed_num_150); /* 数字 + . - /（距离数值用）*/
LV_FONT_DECLARE(label_26);      /* 全 ASCII（状态/单位/车速用）*/

/* 黑底 + 高对比（与时钟类表盘一致，OLED 省电）*/
#define C_BG        0x000000
#define C_MAIN      0xF5F5F7 /* 转向图标：近白 */
#define C_DIST      0xFFFFFF /* 距离数值：纯白 */
#define C_DIM       0x8A919E /* 次要文字 */
#define C_ACTIVE    0x34C759 /* 导航中：绿 */
#define C_REROUTE   0xFF9500 /* 重新规划：橙 */
#define C_STALE     0xFF3B30 /* 过期：红 */
#define C_IDLE      0x6A6A70 /* 未开始：灰 */

/* 转向图标区域尺寸（局部坐标系 0..NAV_ICON_BOX）*/
#define NAV_ICON_BOX 160
#define NAV_LINE_W   15

/* ---------------- 状态 ---------------- */
static lv_obj_t *s_box;      /* 图标容器 */
static lv_obj_t *s_shaft;    /* 折线：杆 */
static lv_obj_t *s_head;     /* 折线：箭头 */

static lv_obj_t *s_tgt_out;  /* 到达：外圈 */
static lv_obj_t *s_tgt_in;   /* 到达：内点 */
static lv_obj_t *s_unk_cir;  /* 未知：圆环 */
static lv_obj_t *s_unk_lbl;  /* 未知：问号 */

static lv_obj_t *s_state_lbl;   /* 顶部状态文字标签 */
static lv_obj_t *s_dist;
static lv_obj_t *s_unit;
static lv_obj_t *s_speed;

static int s_maneuver = NAV_MANEUVER_UNKNOWN;
static int s_distance = NAV_DISTANCE_INVALID;
static int s_state = NAV_STATE_NO_DATA;
static float s_speed_kmh = 0.0f;
static bool s_speed_valid = false;

/* 已应用过的值（变更检测用；-1 保证首帧必设）*/
static int s_shown_maneuver = -1;
/* 上次用于图标可见性计算的状态（与 s_shown_state 分开：后者只管文案）*/
static int state_at_last_maneuver = -1;
static int s_shown_state = -1;
static int s_shown_distance = -2;

/* ---------------- 转向图标几何 ----------------
 * 坐标在 160×160 局部空间（x 右 / y 下）。
 * 静态数组：lv_line 只保存指针，生命周期必须覆盖整个使用期。
 */
static const lv_point_t P_SH_STRAIGHT[] = { { 80, 150 }, { 80, 52 } };
static const lv_point_t P_HD_STRAIGHT[] = { { 52, 80 }, { 80, 52 }, { 108, 80 } };

static const lv_point_t P_SH_LEFT[] = { { 112, 150 }, { 112, 86 }, { 52, 86 } };
static const lv_point_t P_HD_LEFT[] = { { 80, 58 }, { 52, 86 }, { 80, 114 } };

static const lv_point_t P_SH_RIGHT[] = { { 48, 150 }, { 48, 86 }, { 108, 86 } };
static const lv_point_t P_HD_RIGHT[] = { { 80, 58 }, { 108, 86 }, { 80, 114 } };

static const lv_point_t P_SH_UTURN[] = { { 62, 150 }, { 62, 70 }, { 98, 70 }, { 98, 96 } };
static const lv_point_t P_HD_UTURN[] = { { 78, 72 }, { 98, 96 }, { 118, 72 } };

static const lv_point_t P_SH_KLEFT[] = { { 96, 150 }, { 96, 74 }, { 62, 74 } };
static const lv_point_t P_HD_KLEFT[] = { { 84, 50 }, { 62, 74 }, { 84, 98 } };

static const lv_point_t P_SH_KRIGHT[] = { { 64, 150 }, { 64, 74 }, { 98, 74 } };
static const lv_point_t P_HD_KRIGHT[] = { { 76, 50 }, { 98, 74 }, { 76, 98 } };

/* 辅助：设置折线点集 */
static void set_line(lv_obj_t *line, const lv_point_t *pts, uint32_t n)
{
    lv_line_set_points(line, pts, (uint16_t)n);
}

/* 显示/隐藏一组的辅助 */
static void show(lv_obj_t *o, bool on)
{
    if (o == NULL) return;
    if (on) lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
    else    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

/* 创建一条折线（父为图标容器）*/
static lv_obj_t *make_line(lv_obj_t *parent)
{
    lv_obj_t *l = lv_line_create(parent);
    lv_obj_remove_style_all(l);
    lv_obj_set_pos(l, 0, 0);
    lv_obj_set_size(l, NAV_ICON_BOX, NAV_ICON_BOX);
    lv_obj_set_style_line_width(l, NAV_LINE_W, 0);
    lv_obj_set_style_line_color(l, lv_color_hex(C_MAIN), 0);
    lv_obj_set_style_line_rounded(l, true, 0);
    return l;
}

/* 创建一个圆（轮廓或实心）*/
static lv_obj_t *make_circle(lv_obj_t *parent, int dia, bool filled)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, dia, dia);
    lv_obj_set_style_radius(c, LV_RADIUS_CIRCLE, 0);
    if (filled) {
        lv_obj_set_style_bg_color(c, lv_color_hex(C_MAIN), 0);
        lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    }
    else {
        lv_obj_set_style_bg_opa(c, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(c, 6, 0);
        lv_obj_set_style_border_color(c, lv_color_hex(C_MAIN), 0);
        lv_obj_set_style_border_opa(c, LV_OPA_COVER, 0);
    }
    lv_obj_center(c);
    return c;
}

/* 状态标签配色 */
static uint32_t state_color(nav_state_t st)
{
    switch (st) {
        case NAV_STATE_ACTIVE:    return C_ACTIVE;
        case NAV_STATE_REROUTING: return C_REROUTE;
        case NAV_STATE_ARRIVED:   return C_ACTIVE;
        case NAV_STATE_STALE:     return C_STALE;
        case NAV_STATE_IDLE:      return C_IDLE;
        default:                  return C_DIM;
    }
}

/* 应用转向图标（含可见性与几何）*/
static void apply_maneuver(void)
{
    nav_state_t st = nav_clamp_state(s_state);
    int eff = s_maneuver;

    /* 已到达：无论上层给什么动作，都显示"目的地"符号 */
    if (st == NAV_STATE_ARRIVED) eff = NAV_MANEUVER_ARRIVE;

    if (!nav_shows_maneuver(st)) {
        show(s_box, false);
        return;
    }
    show(s_box, true);

    bool is_arrive = (eff == NAV_MANEUVER_ARRIVE);
    bool is_unknown = (eff == NAV_MANEUVER_UNKNOWN);

    /* 三组互斥显示 */
    show(s_shaft, !is_arrive && !is_unknown);
    show(s_head,  !is_arrive && !is_unknown);
    show(s_tgt_out, is_arrive);
    show(s_tgt_in,  is_arrive);
    show(s_unk_cir, is_unknown);
    show(s_unk_lbl, is_unknown);

    if (!is_arrive && !is_unknown) {
        switch (eff) {
            case NAV_MANEUVER_STRAIGHT:
                set_line(s_shaft, P_SH_STRAIGHT, 2);
                set_line(s_head, P_HD_STRAIGHT, 3);
                break;
            case NAV_MANEUVER_LEFT:
                set_line(s_shaft, P_SH_LEFT, 3);
                set_line(s_head, P_HD_LEFT, 3);
                break;
            case NAV_MANEUVER_RIGHT:
                set_line(s_shaft, P_SH_RIGHT, 3);
                set_line(s_head, P_HD_RIGHT, 3);
                break;
            case NAV_MANEUVER_U_TURN:
                set_line(s_shaft, P_SH_UTURN, 4);
                set_line(s_head, P_HD_UTURN, 3);
                break;
            case NAV_MANEUVER_KEEP_LEFT:
                set_line(s_shaft, P_SH_KLEFT, 3);
                set_line(s_head, P_HD_KLEFT, 3);
                break;
            default: /* KEEP_RIGHT */
                set_line(s_shaft, P_SH_KRIGHT, 3);
                set_line(s_head, P_HD_KRIGHT, 3);
                break;
        }
    }
}

/* 应用状态（文案 + 配色 + 触发图标刷新）*/
static void apply_state(void)
{
    nav_state_t st = nav_clamp_state(s_state);

    if ((int)st != s_shown_state) {
        s_shown_state = (int)st;
        lv_label_set_text(s_state_lbl, nav_state_text(st));
        lv_obj_set_style_text_color(s_state_lbl, lv_color_hex(state_color(st)), 0);
    }

    /*
     * ⚠️ 图标可见性同时取决于**状态**（如过期/无数据必须隐藏）与**动作**
     * （决定画哪种箭头），所以任一方变化都要重算。
     *
     * 真机前踩坑：早先只在"动作变化"时重算 —— 表盘创建时状态是 NO_DATA
     * （图标区被隐藏），之后状态变为 NAV 而动作未变时，apply_maneuver() 不会
     * 被调用，图标区便**一直隐藏**（表现为"某动作不显示图标"）。
     */
    bool need = false;
    if ((int)st != state_at_last_maneuver) {
        state_at_last_maneuver = (int)st;
        need = true;
    }
    if (s_shown_maneuver != s_maneuver) {
        s_shown_maneuver = s_maneuver;
        need = true;
    }
    if (need) apply_maneuver();

    /* 过期/无数据时距离不显示旧值 */
    if (!nav_shows_maneuver(st)) {
        if (s_shown_distance != NAV_DISTANCE_INVALID) {
            s_shown_distance = NAV_DISTANCE_INVALID;
            lv_label_set_text(s_dist, "--");
            lv_label_set_text(s_unit, "");
        }
    }
}

/* 应用距离（数值大字 + 单位小字）*/
static void apply_distance(void)
{
    nav_state_t st = nav_clamp_state(s_state);
    if (!nav_shows_maneuver(st)) return; /* 由 apply_state 统一置 "--" */

    if (s_shown_distance == s_distance) return;
    s_shown_distance = s_distance;

    char num[16];
    nav_format_distance_value(s_distance, num, sizeof(num));
    lv_label_set_text(s_dist, num);
    lv_label_set_text(s_unit, nav_distance_unit(s_distance));
}

/* 应用车速 */
static void apply_speed(void)
{
    static float shown = -1.0f;
    static bool shown_valid = false;

    if (shown == s_speed_kmh && shown_valid == s_speed_valid) return;
    shown = s_speed_kmh;
    shown_valid = s_speed_valid;

    char b[24];
    if (s_speed_valid) {
        snprintf(b, sizeof(b), "%.0f km/h", (double)s_speed_kmh);
    }
    else {
        snprintf(b, sizeof(b), "-- km/h");
    }
    lv_label_set_text(s_speed, b);
}

/* ---------------- 构建 ---------------- */

lv_obj_t *ui_nav_create(lv_obj_t *parent)
{
    /* 重建时复位缓存，保证首帧把文案/几何都设一遍 */
    s_shown_maneuver = -1;
    s_shown_state = -1;
    s_shown_distance = -2;
    state_at_last_maneuver = -1;

    lv_obj_set_style_bg_color(parent, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);

    /* 导航状态（顶部）*/
    s_state_lbl = lv_label_create(parent);
    lv_obj_set_style_text_font(s_state_lbl, &label_26, 0);
    lv_obj_set_style_text_color(s_state_lbl, lv_color_hex(C_DIM), 0);
    lv_label_set_text(s_state_lbl, nav_state_text(NAV_STATE_NO_DATA));
    lv_obj_align(s_state_lbl, LV_ALIGN_CENTER, 0, -172);

    /* 转向图标容器（透明，仅作局部坐标原点）*/
    s_box = lv_obj_create(parent);
    lv_obj_remove_style_all(s_box);
    lv_obj_set_size(s_box, NAV_ICON_BOX, NAV_ICON_BOX);
    lv_obj_align(s_box, LV_ALIGN_CENTER, 0, -83);
    lv_obj_clear_flag(s_box, LV_OBJ_FLAG_SCROLLABLE);

    s_shaft = make_line(s_box);
    s_head = make_line(s_box);
    set_line(s_shaft, P_SH_STRAIGHT, 2);
    set_line(s_head, P_HD_STRAIGHT, 3);

    s_tgt_out = make_circle(s_box, 112, false);
    s_tgt_in = make_circle(s_box, 40, true);

    s_unk_cir = make_circle(s_box, 112, false);
    s_unk_lbl = lv_label_create(s_box);
    lv_obj_set_style_text_font(s_unk_lbl, &speed_num_150, 0);
    lv_obj_set_style_text_color(s_unk_lbl, lv_color_hex(C_MAIN), 0);
    lv_label_set_text(s_unk_lbl, "?");
    lv_obj_center(s_unk_lbl);

    /* 距离数值（大号）+ 单位 */
    s_dist = lv_label_create(parent);
    lv_obj_set_style_text_font(s_dist, &speed_num_150, 0);
    lv_obj_set_style_text_color(s_dist, lv_color_hex(C_DIST), 0);
    lv_label_set_text(s_dist, "--");
    lv_obj_align(s_dist, LV_ALIGN_CENTER, 0, 67);

    s_unit = lv_label_create(parent);
    lv_obj_set_style_text_font(s_unit, &label_26, 0);
    lv_obj_set_style_text_color(s_unit, lv_color_hex(C_DIM), 0);
    lv_label_set_text(s_unit, "");
    lv_obj_align(s_unit, LV_ALIGN_CENTER, 0, 147);

    /* 当前车速（底部）*/
    s_speed = lv_label_create(parent);
    lv_obj_set_style_text_font(s_speed, &label_26, 0);
    lv_obj_set_style_text_color(s_speed, lv_color_hex(C_DIM), 0);
    lv_label_set_text(s_speed, "-- km/h");
    lv_obj_align(s_speed, LV_ALIGN_CENTER, 0, 189);

    /* 首次应用：把当前数据画出来 */
    apply_state();
    apply_distance();
    apply_speed();

    return parent;
}

/* ---------------- 对外接口（均带变更检测）---------------- */

void ui_nav_set_maneuver(int maneuver)
{
    int v = (int)nav_clamp_maneuver(maneuver);
    if (v == s_maneuver) return;
    s_maneuver = v;
    apply_maneuver();
}

void ui_nav_set_distance(int meters)
{
    if (meters == s_distance) return;
    s_distance = meters;
    apply_distance();
}

void ui_nav_set_state(int state)
{
    int v = (int)nav_clamp_state(state);
    if (v == s_state) return;
    s_state = v;
    apply_state();
    /* 状态从"不显示"变为"显示"时，把距离补上 */
    apply_distance();
}

void ui_nav_set_speed(float kmh, bool valid)
{
    if (kmh == s_speed_kmh && valid == s_speed_valid) return;
    s_speed_kmh = kmh;
    s_speed_valid = valid;
    apply_speed();
}

/* ---------------- 诊断查询 ---------------- */

int ui_nav_get_maneuver(void) { return s_maneuver; }
int ui_nav_get_distance(void) { return s_distance; }
int ui_nav_get_state(void) { return s_state; }
float ui_nav_get_speed(void) { return s_speed_kmh; }
bool ui_nav_get_speed_valid(void) { return s_speed_valid; }
