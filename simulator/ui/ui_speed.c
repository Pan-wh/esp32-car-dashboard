/**
 * ui_speed.c —— V5 驾驶员速度表盘
 *
 * 视觉层级：驾驶员插画和橙色平滑指针为主；底部白色圆形子仪表仅显示
 * 数字速度与链路状态。无效状态显示 "--"，指针归零。
 *
 * 数据来源：GNSS 未接入前为模拟值。
 */
#include "ui_speed.h"

#include "ui_baked_images.h"
#include "ui_img_inflate.h"

#include <math.h>
#include <string.h>
#include <stdio.h>

#include "ui_timesrc.h"

LV_FONT_DECLARE(label_26);      /* Inter Medium 26px，单位/标签/状态 */

/* 驾驶员插画（由 tools/make_images.py 生成） */

#define C_BG      0x000000 /* 页面容器底色 */
#define C_WARN    0xFF9500 /* 指针 / 告警：橙 */

/* ---- V5：白底表盘主题（用户 2026-10-10 要求，与驾驶员素材白底无缝）---- */
#define C_BG_LIGHT    0xFFFFFF /* 表盘底：纯白 */
#define C_TICK_MIN_L  0x9A9A9E /* 小刻度：中灰 */
#define C_LABEL_L     0x3A3A40 /* 刻度数字：深灰 */
#define C_TEXT_DIM_L  0x6A6A70 /* 次文字：中灰 */
#define C_BAD_L       0xB0B0B6 /* 无效/置灰 */

#define SPEED_MAX 200.0f /* 量程 km/h */

/* V5 参数 */
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
static lv_obj_t *s_status5;    /* 子仪表底部链路状态文案 */
static lv_obj_t *s_status_dot; /* 链路状态色点 */
static lv_obj_t *s_hud5;       /* 子仪表中央数字速度 */
static void hud5_refresh(void); /* 定义见下方（build_v5 需先引用） */
static lv_meter_indicator_t *s_needle5; /* V5 指针（与 V2 区分，绘制时外移内端） */
static lv_point_t s_marker_inner;       /* 指针内端缓存（必须静态：存活到绘制调用） */
static int s_driver_state = UI_DRIVER_AUTO;
static bool s_sweeping = false;   /* 扫表动画进行中（屏蔽数据驱动，见 ui_speed_sweep）*/
static int s_heading = 45;        /* 航向（度，正北 0，顺时针） */
static float s_trip_km = 0.0f;    /* 行程里程（km，功能④） */
static float s_trip_max = 0.0f;   /* 行程最高速（km/h） */
static int s_driver_shown = -2;
static int s_status_shown = -1;
static char s_hud_shown[16] = "";

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

/* ------------------------------------------------------------------ */
/* V5 链路状态 → 文案（ASCII，内置 12px 字体即可，无新增字体资源） */
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

    /*
     * 三张图已在启动时由索引色充气到 PSRAM。把纯白底原地改为色键透明，
     * 让下层 0/200 刻度自然露出；前景像素、Flash 与 PSRAM 占用均不变。
     */
    ui_img_make_bg_transparent(&driver_normal);
    ui_img_make_bg_transparent(&driver_spirited);
    ui_img_make_bg_transparent(&driver_speeding);

    /* 中央插画下移 10px；原生 300px 图不缩放，避免增加采样开销或画质损失。 */
    s_driver_img = lv_img_create(p);
    lv_img_set_src(s_driver_img, &driver_normal);
    lv_obj_align(s_driver_img, LV_ALIGN_CENTER, 0, 10);
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

    /* 底部白色圆形子仪表：只用图元和现有字体，不新增位图/字体占用。 */
    lv_obj_t *subdial = lv_obj_create(p);
    lv_obj_remove_style_all(subdial);
    lv_obj_set_size(subdial, 86, 86);
    lv_obj_set_style_radius(subdial, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(subdial, lv_color_hex(C_BG_LIGHT), 0);
    lv_obj_set_style_bg_opa(subdial, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(subdial, lv_color_hex(0xD7D7DC), 0);
    lv_obj_set_style_border_width(subdial, 2, 0);
    lv_obj_align(subdial, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_clear_flag(subdial, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    s_hud5 = make_label(subdial, &label_26, C_LABEL_L, "--");
    lv_obj_align(s_hud5, LV_ALIGN_CENTER, 0, -14);

    lv_obj_t *unit = make_label(subdial, &lv_font_montserrat_12, C_BAD_L, "km/h");
    lv_obj_align(unit, LV_ALIGN_CENTER, 0, 5);

    lv_obj_t *divider = lv_obj_create(subdial);
    lv_obj_remove_style_all(divider);
    lv_obj_set_size(divider, 44, 1);
    lv_obj_set_style_bg_color(divider, lv_color_hex(0xE2E2E5), 0);
    lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, 0);
    lv_obj_align(divider, LV_ALIGN_CENTER, 0, 15);

    s_status_dot = lv_obj_create(subdial);
    lv_obj_remove_style_all(s_status_dot);
    lv_obj_set_size(s_status_dot, 7, 7);
    lv_obj_set_style_radius(s_status_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_status_dot, lv_color_hex(status5_color()), 0);
    lv_obj_set_style_bg_opa(s_status_dot, LV_OPA_COVER, 0);
    lv_obj_align(s_status_dot, LV_ALIGN_CENTER, -31, 29);

    s_status5 = make_label(subdial, &lv_font_montserrat_12, C_TEXT_DIM_L, status5_text());
    lv_obj_align(s_status5, LV_ALIGN_CENTER, 5, 29);
    hud5_refresh();
}

static void apply_state(void)
{
    int32_t v = s_valid ? (int32_t)lroundf(s_speed) : 0;

    /* V5 定稿：指针 + 驾驶员表情 + 底部辅助信息（无中央大数字） */
    {
        if (s_meter && s_needle5) lv_meter_set_indicator_value(s_meter, s_needle5, v);
        /* 插画：状态三态只在跨过 90/120 阈值时变化，用已显示状态防止重复 set_src */
        if (s_driver_img) {
            int st = current_driver_state();
            if (st != s_driver_shown) {
                s_driver_shown = st;
                lv_img_set_src(s_driver_img,
                               st == UI_DRIVER_SPEEDING ? &driver_speeding :
                               st == UI_DRIVER_SPIRITED ? &driver_spirited : &driver_normal);
            }
        }
        /* 状态标签：同文案不重复 set_text（避免每次 apply_state 都 invalidate） */
        if (s_status5) {
            if ((int)s_status_state != s_status_shown) {
                s_status_shown = (int)s_status_state;
                lv_label_set_text(s_status5, status5_text());
                if (s_status_dot) {
                    lv_obj_set_style_bg_color(s_status_dot,
                                              lv_color_hex(status5_color()), 0);
                }
            }
        }
        /* 子仪表数字速度跟随同一平滑值，内部有同文案去重。 */
        hud5_refresh();
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
    s_status_dot = NULL;
    s_hud5 = NULL;
    s_driver_shown = -2;
    s_status_shown = -1;
    s_hud_shown[0] = '\0';

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
    if (s_sweeping) return; /* 扫表动画期间不打断指针 */
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

/* 子仪表只显示平滑后的整数速度；状态与单位由独立对象显示。 */
static void hud5_text(char *buf, size_t sz)
{
    if (s_valid) {
        snprintf(buf, sz, "%.0f", (double)s_speed);
    }
    else {
        snprintf(buf, sz, "--");
    }
}

/*
 * ---- 开机扫表动画（#2b 用户需求：切到速度表盘时指针自检扫一遍）----
 *
 * 行为：指针 0 → 满量程 → 回落到当前真实速度，类似汽车启动自检。
 * 实现要点：
 *   1. 两段动画（上升 ease_out + 回落 ease_in_out），用第一阶段 ready 回调
 *      启动第二阶段，避免依赖延迟启动 API 的版本差异；
 *   2. 动画期间置 s_sweeping，**屏蔽**数据驱动的 set_speed，
 *      否则 2Hz 遥测会把扫表打断（指针扫一半就被拉回真实值）；
 *   3. 动画结束后恢复数据驱动（apply_state 用最新 s_speed 落位）。
 */
#define SWEEP_UP_MS   420 /* 0 → 满量程 */
#define SWEEP_DOWN_MS 460 /* 满量程 → 当前值 */

static lv_anim_t s_sweep_anim;
static lv_timer_t *s_sweep_timer = NULL;

/* 扫表结束：解除屏蔽并落位到最新真实速度 */
static void sweep_end_timer_cb(lv_timer_t *tm)
{
    (void)tm;
    s_sweeping = false;
    s_sweep_timer = NULL;
    apply_state();
}

static void sweep_anim_cb(void *var, int32_t v)
{
    (void)var;
    if (s_meter && s_needle5) lv_meter_set_indicator_value(s_meter, s_needle5, v);
}

/* 第一阶段结束：启动回落段 */
static void sweep_up_done_cb(lv_anim_t *a)
{
    (void)a;
    int32_t target = s_valid ? (int32_t)lroundf(s_speed) : 0;

    lv_anim_init(&s_sweep_anim);
    lv_anim_set_var(&s_sweep_anim, NULL);
    lv_anim_set_exec_cb(&s_sweep_anim, sweep_anim_cb);
    lv_anim_set_values(&s_sweep_anim, (int32_t)SPEED_MAX, target);
    lv_anim_set_time(&s_sweep_anim, SWEEP_DOWN_MS);
    lv_anim_set_path_cb(&s_sweep_anim, lv_anim_path_ease_in_out);
    lv_anim_set_ready_cb(&s_sweep_anim, NULL);
    lv_anim_start(&s_sweep_anim);
}

void ui_speed_sweep(void)
{
    if (s_meter == NULL || s_needle5 == NULL) return;

    /*
     * ⚠️ 必须按 exec_cb 匹配，**不能**传 &s_sweep_anim。
     *
     * LVGL 的 lv_anim_del(var, cb) 匹配条件是：
     *     (a->var == var || var == NULL) && (a->exec_cb == cb || cb == NULL)
     * 而本动画的 var 被设成了 NULL（lv_anim_set_var(&s_sweep_anim, NULL)），
     * 所以传 &s_sweep_anim 时 a->var(NULL) != var(&s_sweep_anim) → **一个都删不掉**，
     * 扫表动画会一直残留并继续访问 s_meter/s_needle5。
     * 传 var=NULL、cb=sweep_anim_cb 才能按回调精确匹配到所有扫表动画。
     */
    lv_anim_del(NULL, (lv_anim_exec_xcb_t)sweep_anim_cb);
    if (s_sweep_timer != NULL) { /* 连点切换表盘时避免叠加多个定时器 */
        lv_timer_del(s_sweep_timer);
        s_sweep_timer = NULL;
    }
    s_sweeping = true; /* 动画期间屏蔽数据驱动 */

    lv_anim_init(&s_sweep_anim);
    lv_anim_set_var(&s_sweep_anim, NULL);
    lv_anim_set_exec_cb(&s_sweep_anim, sweep_anim_cb);
    lv_anim_set_values(&s_sweep_anim, 0, (int32_t)SPEED_MAX);
    lv_anim_set_time(&s_sweep_anim, SWEEP_UP_MS);
    lv_anim_set_path_cb(&s_sweep_anim, lv_anim_path_ease_out);
    lv_anim_set_ready_cb(&s_sweep_anim, sweep_up_done_cb);
    lv_anim_start(&s_sweep_anim);

    /*
     * 扫表结束后解除屏蔽：用一次性 lv_timer（总时长 = 上升 + 回落 + 余量）。
     * 比依赖动画 ready 链更直观，也便于单点取消。
     */
    s_sweep_timer = lv_timer_create(sweep_end_timer_cb, SWEEP_UP_MS + SWEEP_DOWN_MS + 60, NULL);
    lv_timer_set_repeat_count(s_sweep_timer, 1);
}

/* 子仪表速度刷新（同文案不重设，避免无谓 invalidate） */
static void hud5_refresh(void)
{
    if (s_hud5 == NULL) return;
    char buf[16];
    hud5_text(buf, sizeof(buf));
    if (strcmp(s_hud_shown, buf) == 0) return;
    snprintf(s_hud_shown, sizeof(s_hud_shown), "%s", buf);
    lv_label_set_text(s_hud5, buf);
}

void ui_speed_set_trip(float distance_km, float max_kmh)
{
    if (distance_km == s_trip_km && max_kmh == s_trip_max) return; /* 变更检测 */
    s_trip_km = distance_km;
    s_trip_max = max_kmh;
}

void ui_speed_set_heading(int deg)
{
    int h = ((deg % 360) + 360) % 360;
    if (h == s_heading) return; /* 变更检测 */
    s_heading = h;
}

void ui_speed_set_status(ui_speed_status_t st)
{
    if ((int)st < 0 || st > UI_STATUS_OK) st = UI_STATUS_CONNECTING;
    if (st == s_status_state) return;
    s_status_state = st;
    apply_state();
}
