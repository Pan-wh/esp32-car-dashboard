/**
 * ui_speed.c —— 速度表盘页（4 个设计变体，供选型对比）
 *
 * 变体：
 *   0 = V1 数字 + 外环进度弧      （现代电动车风格，最简洁）
 *   1 = V2 指针速度表             （与模拟时钟同语言，机械感）
 *   2 = V3 超大数字 + 底部弧形刻度带（iPhone 极简风延续）
 *   3 = V4 数字 + 分段进度条       （运动风格）
 *
 * 公共信息：大号速度数字 + 单位 km/h + GNSS 定位状态。
 * 无效状态（无定位）：数字显示 "--"，进度/指针归零，状态文字转灰。
 *
 * 数据来源：GNSS 未接入前为模拟值。
 */
#include "ui_speed.h"

#include <math.h>
#include <string.h>
#include <stdio.h>

#include "ui_timesrc.h"

LV_FONT_DECLARE(speed_num_150); /* Inter Bold 150px，速度大数字 */
LV_FONT_DECLARE(label_26);      /* Inter Medium 26px，单位/标签/状态 */
LV_IMG_DECLARE(dial_speed);     /* 烘焙的静态速度表盘（刻度+渐变+数字），见 tools/make_dial_faces.py */

/* 驾驶员插画（由 tools/make_images.py 生成） */
LV_IMG_DECLARE(driver_normal);
LV_IMG_DECLARE(driver_spirited);
LV_IMG_DECLARE(driver_speeding);

/* ---- 设计常量（V1-V4：黑底主题）---- */
#define C_BG      0x000000 /* 纯黑背景（OLED 省电） */
#define C_MAIN    0xF5F5F7 /* 主数据：白 */
#define C_DIM     0x8A919E /* 次要文字：中灰 */
#define C_TRACK   0x26262A /* 轨道/未点亮段：暗灰 */
#define C_ACCENT  0x0A84FF /* 强调色：进度/指针（蓝） */
#define C_LABEL   0xC8C8CE /* 刻度数字：浅灰 */
#define C_WARN    0xFF9500 /* 超速提醒：橙 */

/* ---- V5：白底表盘主题（用户 2026-10-10 要求，与驾驶员素材白底无缝）---- */
#define C_BG_LIGHT    0xFFFFFF /* 表盘底：纯白 */
#define C_TICK_MIN_L  0x9A9A9E /* 小刻度：中灰 */
#define C_TICK_MAJ_L  0x4A4A50 /* 大刻度：深灰 */
#define C_LABEL_L     0x3A3A40 /* 刻度数字：深灰 */
#define C_TEXT_L      0x1A1A1E /* 主文字：近黑 */
#define C_TEXT_DIM_L  0x6A6A70 /* 次文字：中灰 */
#define C_NEEDLE_L    0x2A2A2E /* 指针：深炭灰 */
#define C_BAD_L       0xB0B0B6 /* 无效/置灰 */

#define SPEED_MAX 200.0f /* 量程 km/h */

/* V5 参数 */
#define DRIVER_IMG_SIZE   230  /* 中央插画边长（美术素材 230×230） */
#define MARKER_INNER_R    168  /* 指针内端半径（插画内容最大半径 ≤ 24+115=139，不冲突） */
#define DRIVER_SPIRITED_V 90.0f  /* 激烈驾驶阈值 km/h（用户确认：0-90 平静） */
#define DRIVER_SPEEDING_V 120.0f /* 超速阈值 km/h（用户确认：>120 咬牙） */


/* ---- 状态 ---- */
static lv_obj_t *s_root;
/*
 * 默认「速度 0 且无效」：这两个值是 GNSS 时代的模拟残留（88 / true），
 * 真机实测踩坑——速度明明无效，指针却停在 88（用户看到"0 速度但指针指 90"）。
 * 数据源改为 iPhone 后，**任何时刻都不应存在"有效但没数据"的状态**，
 * 因此默认必须是「无效」，由 set_fix/set_speed 显式驱动。
 */
static float s_speed = 0.0f;
static bool s_valid = false;
static int s_sats = 12;
static ui_speed_status_t s_status_state = UI_STATUS_CONNECTING;

/* ---- 各变体的动态部件 ---- */
static lv_obj_t *s_meter;            /* V2 指针表 */

/* V5 部件 */
static lv_obj_t *s_driver_img;
static lv_obj_t *s_status5; /* V5 顶部链路状态标签（五态文案） */
static lv_obj_t *s_hud5;    /* V5 底部航向+行程 HUD（功能④） */
static void hud5_refresh(void); /* 定义见下方（build_v5 需先引用） */
static lv_meter_indicator_t *s_needle5; /* V5 指针（与 V2 区分，绘制时外移内端） */
static lv_point_t s_marker_inner;       /* 指针内端缓存（必须静态：存活到绘制调用） */
static int s_driver_state = UI_DRIVER_AUTO;
static int s_heading = 45;        /* 航向（度，正北 0，顺时针） */
static float s_trip_km = 0.0f;    /* 行程里程（km，功能④） */
static float s_trip_max = 0.0f;   /* 行程最高速（km/h） */

/* ------------------------------------------------------------------ */
/* 公共部件                                                            */
/* ------------------------------------------------------------------ */

static lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font,
                            uint32_t color, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, text);
    return l;
}



/* 圆头线帽（与模拟时钟一致的观感），用于指针表 */
static void meter_draw_cb(lv_event_t *e)
{
    lv_obj_draw_part_dsc_t *dsc = lv_event_get_draw_part_dsc(e);
    if (dsc->type == LV_METER_DRAW_PART_NEEDLE_LINE && dsc->line_dsc) {
        dsc->line_dsc->round_start = 1;
        dsc->line_dsc->round_end = 1;
    }
}

/* ------------------------------------------------------------------ */
/* V5 链路状态 → 文案（ASCII，label_26 无 CJK） */
static const char *status5_text(void)
{
    switch (s_status_state) {
        case UI_STATUS_CONNECTING: return "BT OFF";
        case UI_STATUS_WAIT_LOC:   return "WAIT GPS";
        case UI_STATUS_INVALID:    return "NO FIX";
        case UI_STATUS_STALE:      return "STALE";
        case UI_STATUS_OK:         return "LIVE";
        default:                   return "";
    }
}
static uint32_t status5_color(void)
{
    switch (s_status_state) {
        case UI_STATUS_CONNECTING: return C_TEXT_DIM_L; /* 灰  */
        case UI_STATUS_WAIT_LOC:   return C_TEXT_DIM_L; /* 灰  */
        case UI_STATUS_INVALID:    return C_WARN;       /* 橙  */
        case UI_STATUS_STALE:      return 0xFF3B30;     /* 红  */
        case UI_STATUS_OK:         return 0x34C759;     /* 绿  */
        default:                   return C_TEXT_DIM_L;
    }
}
static int driver_state_from_speed(void)
{
    if (!s_valid) return UI_DRIVER_NORMAL;
    if (s_speed > DRIVER_SPEEDING_V) return UI_DRIVER_SPEEDING;
    if (s_speed > DRIVER_SPIRITED_V) return UI_DRIVER_SPIRITED;
    return UI_DRIVER_NORMAL;
}
/* 当前驾驶员状态：显式设定优先，否则按速度自动 */
static int current_driver_state(void)
{
    return (s_driver_state >= 0) ? s_driver_state : driver_state_from_speed();
}
/*
 * 速度 → 颜色（三段渐变）：0 深灰 → 120 橙 → 200 红。
 * 用于刻度着色与刻度数字，速度越高越"战斗"。
 */
static uint32_t speed_color(float v)
{
    if (v < 0) v = 0;
    if (v > SPEED_MAX) v = SPEED_MAX;

    uint8_t r0, g0, b0, r1, g1, b1;
    float t;
    if (v <= DRIVER_SPEEDING_V) {
        r0 = 0x4A; g0 = 0x4A; b0 = 0x50; /* 低速：深灰 */
        r1 = 0xFF; g1 = 0x95; b1 = 0x00; /* 阈值：橙 */
        t = v / DRIVER_SPEEDING_V;
    }
    else {
        r0 = 0xFF; g0 = 0x95; b0 = 0x00;
        r1 = 0xFF; g1 = 0x3B; b1 = 0x30; /* 满量程：红 */
        t = (v - DRIVER_SPEEDING_V) / (SPEED_MAX - DRIVER_SPEEDING_V);
    }
    uint8_t r = (uint8_t)(r0 + (r1 - r0) * t);
    uint8_t g = (uint8_t)(g0 + (g1 - g0) * t);
    uint8_t b = (uint8_t)(b0 + (b1 - b0) * t);
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

/* V1：数字 + 外环进度弧（270°，底部开口）                              */
static void speed5_draw_cb(lv_event_t *e)
{
    lv_obj_draw_part_dsc_t *dsc = lv_event_get_draw_part_dsc(e);
    if (dsc->type != LV_METER_DRAW_PART_NEEDLE_LINE || dsc->line_dsc == NULL) return;

    dsc->line_dsc->round_start = 1;
    dsc->line_dsc->round_end = 1;

    if (dsc->sub_part_ptr != (void *)s_needle5 || dsc->p1 == NULL || dsc->p2 == NULL) return;

    int32_t dx = dsc->p2->x - dsc->p1->x;
    int32_t dy = dsc->p2->y - dsc->p1->y;
    float len = sqrtf((float)(dx * dx) + (float)(dy * dy));
    if (len < 1.0f) return;

    float k = (float)MARKER_INNER_R / len;
    s_marker_inner.x = dsc->p1->x + (lv_coord_t)lroundf((float)dx * k);
    s_marker_inner.y = dsc->p1->y + (lv_coord_t)lroundf((float)dy * k);
    dsc->p1 = &s_marker_inner;
}

static void build_v5(lv_obj_t *p)
{
    /* 白底表盘（与驾驶员素材白底无缝） */
    lv_obj_set_style_bg_color(p, lv_color_hex(C_BG_LIGHT), 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);

    /*
     * 静态表盘（刻度 + 渐变弧 + 刻度数字）烘焙成图片：**必须最先创建（最底层）**，
     * 否则整屏白底图片会盖住驾驶员插画。
     * 原先由 lv_meter 逐刻度绘制 + 2 条渐变 scale_lines + 6 个 lv_label；
     * 实测刻度在整屏重绘（滑页）时约值 50ms，改为图片后只剩一次图片拷贝。
     */
    lv_obj_t *face = lv_img_create(p);
    lv_img_set_src(face, &dial_speed);
    lv_obj_center(face);
    lv_obj_clear_flag(face, LV_OBJ_FLAG_CLICKABLE);

    /* ---- 中央插画（**最先创建 = 最底层**，其白色方角不会盖住刻度与数字）----
     * 素材裁切圆与表盘同心，故底部弧线自然贴合量表内圈。 */
    s_driver_img = lv_img_create(p);
    lv_img_set_src(s_driver_img, &driver_normal);
    lv_obj_center(s_driver_img);
    lv_obj_clear_flag(s_driver_img, LV_OBJ_FLAG_CLICKABLE);

    /* ---- 量表：只保留指针（刻度已烘焙进图片）---- */
    s_meter = lv_meter_create(p);
    lv_obj_remove_style_all(s_meter);
    lv_obj_set_size(s_meter, 440, 440);
    lv_obj_center(s_meter);
    lv_obj_clear_flag(s_meter, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_meter, speed5_draw_cb, LV_EVENT_DRAW_PART_BEGIN, NULL);

    lv_meter_scale_t *sc_min = lv_meter_add_scale(s_meter);
    lv_meter_set_scale_ticks(s_meter, sc_min, 0, 0, 0, lv_color_hex(C_TICK_MIN_L));
    lv_meter_set_scale_range(s_meter, sc_min, 0, (int32_t)SPEED_MAX, 270, 135);

    /* ---- 指针：橙色圆头 ---- */
    s_needle5 = lv_meter_add_needle_line(s_meter, sc_min, 7, lv_color_hex(C_WARN), -(220 - 214));

    /* 刻度数字已烘焙进 dial_speed 图片，无需运行时创建 lv_label */

    /* ---- 顶部链路状态标签（五态，白底可见）----
     * ⚠️ 已知问题（待定稿决策，见 docs/ui-guidelines.md）：
     * 指针扫过半径 168..220，而本标签位于半径约 171..203 的同一环带，
     * 因此行驶速度下指针会与状态文字重叠；且实测 lv_obj_move_foreground()
     * 无法让标签盖住指针（指针由 lv_meter 自身绘制流程输出）。
     * 结论：文字本身可读，但指针穿过时观感不佳，需调整位置或指针长度。 */
    s_status5 = make_label(p, &label_26, status5_color(), status5_text());
    lv_obj_align(s_status5, LV_ALIGN_TOP_MID, 0, 30);

    /* ---- 底部：航向 + 行程（功能④）----
     * 位置选在**指针扫不到**的扇区：量表从 135° 顺时针扫 270° 到 45°，
     * 因此 45°~135°（穿过 6 点方向）永远没有指针；y≈411 也低于中央插画
     * （230×230 → y 118~348），互不遮挡。 */
    s_hud5 = make_label(p, &label_26, C_TEXT_DIM_L, "");
    lv_obj_align(s_hud5, LV_ALIGN_BOTTOM_MID, 0, -18);
    hud5_refresh();
}

static void apply_state(void)
{
    char buf[32];

    int32_t v = s_valid ? (int32_t)lroundf(s_speed) : 0;

    /* V5 定稿：指针 + 驾驶员表情 + 底部辅助信息（无中央大数字） */
    {
        if (s_meter && s_needle5) lv_meter_set_indicator_value(s_meter, s_needle5, v);
        /* 插画：状态三态只在跨过 90/120 阈值时变化，用已显示状态防止重复 set_src */
        static int driver_shown = -2; /* -2 保证首帧必设 */
        if (s_driver_img) {
            int st = current_driver_state();
            if (st != driver_shown) {
                driver_shown = st;
                lv_img_set_src(s_driver_img,
                               st == UI_DRIVER_SPEEDING ? &driver_speeding :
                               st == UI_DRIVER_SPIRITED ? &driver_spirited : &driver_normal);
            }
        }
        /* 状态标签：同文案不重复 set_text（避免每次 apply_state 都 invalidate） */
        static int status_shown = -1;
        if (s_status5) {
            if ((int)s_status_state != status_shown) {
                status_shown = (int)s_status_state;
                lv_label_set_text(s_status5, status5_text());
                lv_obj_set_style_text_color(s_status5, lv_color_hex(status5_color()), 0);
            }
        }
        return;
    }
}

/* ------------------------------------------------------------------ */
/* 构建与对外接口                                                      */
/* ------------------------------------------------------------------ */

static void build(lv_obj_t *parent)
{
    /* 状态复位（重建时清空旧指针） */
    s_meter = NULL;
    s_needle5 = NULL;
    s_driver_img = NULL;
    s_status5 = NULL;

    lv_obj_set_style_bg_color(parent, lv_color_hex(C_BG), 0);
    /* ⚠️ V5 是**白底表盘**设计：这里必须保持不透明白色，
       不能像黑底页面那样改透明（否则会露出 scr 的黑底，表盘变黑）。 */
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);

    /* 定稿设计：V5 驾驶员表盘（旧变体 V1-V4 已删除，见 docs/decisions.md） */
    build_v5(parent);

    apply_state();
}

lv_obj_t *ui_speed_create(lv_obj_t *parent)
{
    s_root = parent;
    build(parent);
    return parent;
}



void ui_speed_set_speed(float kmh)
{
    /*
     * 变更检测（性能关键）：固件 ble_update 以约 50Hz 调用本函数，而
     * lv_meter_set_indicator_value() 在 LVGL 8.4 中**无条件 invalidate**
     * （值相同也会把指针区域标脏）。此前不加检测 → 空闲时也以每秒
     * 几十次的频率重绘整块区域，真机表现为全局卡顿（滑动掉帧、秒针卡住）。
     * EMA 平滑收敛后值不再变化，浮点相等成立，检测即命中。
     */
    if (kmh == s_speed) return;
    s_speed = kmh;
    apply_state();
}

void ui_speed_set_fix(bool valid, int sats)
{
    /* 变更检测：同 set_speed 注释，防无效化风暴 */
    if (valid == s_valid && sats == s_sats) return;
    s_valid = valid;
    s_sats = sats;
    /*
     * 变为无效时把速度也清零：否则「无效 → 再次有效」的瞬间会用旧速度
     * 画一帧（指针闪一下旧值），且诊断接口会显示过期数据。
     */
    if (!valid) s_speed = 0.0f;
    apply_state();
}

void ui_speed_set_driver_state(int state)
{
    int st = (state < -1 || state > UI_DRIVER_SPEEDING) ? UI_DRIVER_AUTO : state;
    if (st == s_driver_state) return; /* 变更检测 */
    s_driver_state = st;
    apply_state();
}

/* 8 方位罗盘名（纯 ASCII，与 label_26 字形集一致） */
static const char *compass_name(int deg)
{
    static const char *names[8] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };
    int oct = ((deg + 22) % 360) / 45; /* 四舍五入到 45° 扇区 */
    if (oct < 0) oct += 8;
    return names[oct % 8];
}

/* 底部 HUD 文案："N 0° · 12.3 km · MAX 87"（无里程时只显示航向） */
static void hud5_text(char *buf, size_t sz)
{
    int h = ((s_heading % 360) + 360) % 360;
    if (s_trip_km > 0.05f) {
        snprintf(buf, sz, "%s %d\xc2\xb0 \xc2\xb7 %.1f km \xc2\xb7 MAX %.0f",
                 compass_name(h), h, (double)s_trip_km, (double)s_trip_max);
    }
    else {
        snprintf(buf, sz, "%s %d\xc2\xb0", compass_name(h), h);
    }
}

/* 底部 HUD 刷新（同文案不重设，避免无谓 invalidate） */
static void hud5_refresh(void)
{
    if (s_hud5 == NULL) return;
    char buf[64];
    hud5_text(buf, sizeof(buf));
    static char shown[64] = "";
    if (strcmp(shown, buf) == 0) return;
    snprintf(shown, sizeof(shown), "%s", buf);
    lv_label_set_text(s_hud5, buf);
}

float ui_speed_get_speed(void) { return s_speed; }
int ui_speed_get_valid(void) { return s_valid ? 1 : 0; }
void *ui_speed_get_needle(void) { return (void *)s_needle5; }

void ui_speed_set_trip(float distance_km, float max_kmh)
{
    if (distance_km == s_trip_km && max_kmh == s_trip_max) return; /* 变更检测 */
    s_trip_km = distance_km;
    s_trip_max = max_kmh;
    hud5_refresh();
}

void ui_speed_set_heading(int deg)
{
    int h = ((deg % 360) + 360) % 360;
    if (h == s_heading) return; /* 变更检测 */
    s_heading = h;
    hud5_refresh();
    apply_state();
}

void ui_speed_set_status(ui_speed_status_t st)
{
    if ((int)st < 0 || st > UI_STATUS_OK) st = UI_STATUS_CONNECTING;
    if (st == s_status_state) return;
    s_status_state = st;
    apply_state();
}
