/**
 * ui_photo.c —— 照片表盘的渲染层
 *
 * 只做四件事：显示一帧 RGB565、显示占位提示、按相位做像素位移、显示接收进度。
 *
 * ## 为什么用 `lv_img` + 用户描述符
 *
 * 固件把一帧解到 PSRAM（RGB565），本模块用一个 `lv_img_dsc_t` **指向那块内存**，
 * 无需拷贝。`lv_img_set_src()` 传 `&dsc` 即可。
 *
 * ⚠️ 每次换帧都 `lv_img_set_src(&dsc)` 会触发**整图重绘**（466×466 = 21.7 万像素）。
 * 动图每帧都要重绘是**必然**的 —— 这正是 §十点五 实测里那 43ms flush 的来源。
 *
 * ## 文案与字体
 *
 * 设计稿（`docs/photo-wireless-design.md` §9.4）要求占位文案是中文：
 *   无照片   →「还没有照片 · 在 App 里添加」
 *   无存储卡 →「未检测到存储卡」
 *   接收中   →「接收 62%」
 *
 * 但项目里唯一的 CJK 字体 `date_cjk_26`（`font_date_cjk_26.c`）**只**包含日期用字形
 * （`0123456789月日星期一二三四五六 ·`），没有这些字；而 LVGL 8.4 **没有字体回退**
 * （v9 才有），缺字直接显示成方块。
 *
 * 所以另有一份**按需生成**的 26 px / 4 bpp 字体 `photo_ui_26`
 * （`font_photo_ui_26.c`，生成命令写在那个文件头里）：
 *   `接收 62%` 与中文占位文案用它；`LOAD ERROR` / `LOADING...` 这类纯 ASCII 状态
 * 继续用全 ASCII 的 `label_26`（见 `nav.h` 里关于 CJK 字形的说明）。
 *
 * 为什么不用一份字体包打天下：中文字形代价大（每个 ~340 B），只把**用到的**字
 * 打进 Flash 才划算；这也与 `date_cjk_26` 的做法一致。
 */
#include "ui_photo.h"

#include <stdio.h>
#include <string.h>

/*
 * `label_26`：全 ASCII（LOAD ERROR / LOADING...）；
 * `photo_ui_26`：中文占位文案 + 接收百分比（定义在 font_photo_ui_26.c）。
 */
LV_FONT_DECLARE(label_26);
LV_FONT_DECLARE(photo_ui_26);  /* 定义在 font_photo_ui_26.c（中文占位文案）*/

/* 配色与其它表盘一致：纯黑底（AMOLED 黑不发光，位移露出底色也看不出来）*/
#define C_BG 0x000000
#define C_TEXT 0xFFFFFF
#define C_DIM 0x9AA0A6

/* 接收进度条：细、暗、贴底（低调，不抢画面；只在接收那几秒出现）*/
#define PROG_BAR_W 180
#define PROG_BAR_H 3
#define C_TRACK 0x30343A

static lv_obj_t *s_root;
static lv_obj_t *s_img;
static lv_obj_t *s_placeholder;
static lv_img_dsc_t s_dsc;
static bool s_has_frame;
static int s_shift_phase = -1; /* -1 = 尚未设置 */
static bool s_shift_enable;

/* 接收进度（上传时显示）*/
static lv_obj_t *s_prog_box;
static lv_obj_t *s_prog_fill;
static lv_obj_t *s_prog_label;
static bool s_prog_visible;
static int s_prog_pct = -1;

/** 四档位移（相位 → dx,dy），幅度不超过 UI_PHOTO_SHIFT_MAX */
static void shift_offset(int phase, int *dx, int *dy)
{
    switch (phase & 3) {
        case 0: *dx = 0; *dy = 0; break;
        case 1: *dx = UI_PHOTO_SHIFT_MAX; *dy = 0; break;
        case 2: *dx = 0; *dy = UI_PHOTO_SHIFT_MAX; break;
        default: *dx = -UI_PHOTO_SHIFT_MAX; *dy = -UI_PHOTO_SHIFT_MAX; break;
    }
}

/** 按当前开关与相位把图片挪到位（只在变化时调用）*/
static void apply_shift(void)
{
    if (s_img == NULL) return;

    int dx = 0, dy = 0;
    if (s_shift_enable) shift_offset(s_shift_phase, &dx, &dy);

    /* 居中 + 偏移。图片本身与屏幕同尺寸，偏移后四边会露出底色（黑）。 */
    lv_obj_align(s_img, LV_ALIGN_CENTER, (lv_coord_t)dx, (lv_coord_t)dy);
}

/** 创建接收进度条（底部细条 + 百分比），默认隐藏 */
static void progress_create(lv_obj_t *parent)
{
    /* 外框（轨道）*/
    s_prog_box = lv_obj_create(parent);
    lv_obj_remove_style_all(s_prog_box);
    lv_obj_set_size(s_prog_box, PROG_BAR_W, PROG_BAR_H);
    lv_obj_set_style_bg_color(s_prog_box, lv_color_hex(C_TRACK), 0);
    lv_obj_set_style_bg_opa(s_prog_box, LV_OPA_80, 0);
    lv_obj_clear_flag(s_prog_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(s_prog_box, LV_ALIGN_BOTTOM_MID, 0, -42);

    /* 填充：宽度按百分比设置（左对齐）*/
    s_prog_fill = lv_obj_create(s_prog_box);
    lv_obj_remove_style_all(s_prog_fill);
    lv_obj_set_size(s_prog_fill, 1, PROG_BAR_H);
    lv_obj_set_style_bg_color(s_prog_fill, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_bg_opa(s_prog_fill, LV_OPA_70, 0);
    lv_obj_clear_flag(s_prog_fill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(s_prog_fill, LV_ALIGN_LEFT_MID, 0, 0);

    /* 百分比文字：比正文暗一档，放在进度条下方 */
    s_prog_label = lv_label_create(parent);
    lv_obj_set_style_text_font(s_prog_label, &photo_ui_26, 0);
    lv_obj_set_style_text_color(s_prog_label, lv_color_hex(C_DIM), 0);
    lv_obj_set_style_text_align(s_prog_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_prog_label, LV_ALIGN_BOTTOM_MID, 0, -8);

    /* 进度条/文字**不参与像素位移**（它们是独立的兄弟对象，不是 s_img 的子对象）*/
    lv_obj_add_flag(s_prog_box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_prog_label, LV_OBJ_FLAG_HIDDEN);
    s_prog_visible = false;
    s_prog_pct = -1;
}

lv_obj_t *ui_photo_create(lv_obj_t *parent)
{
    s_root = parent;
    s_img = NULL;
    s_placeholder = NULL;
    s_has_frame = false;
    s_shift_phase = -1;
    s_shift_enable = false;
    s_prog_box = NULL;
    s_prog_fill = NULL;
    s_prog_label = NULL;
    s_prog_visible = false;
    s_prog_pct = -1;
    memset(&s_dsc, 0, sizeof(s_dsc));

    lv_obj_set_style_bg_color(parent, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);

    /* 图片（先创建，占位提示压在它上面）*/
    s_img = lv_img_create(parent);
    lv_obj_align(s_img, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(s_img, LV_OBJ_FLAG_HIDDEN); /* 没有帧时不显示 */
    lv_obj_clear_flag(s_img, LV_OBJ_FLAG_SCROLLABLE);

    /* 占位提示 */
    s_placeholder = lv_label_create(parent);
    lv_obj_set_style_text_font(s_placeholder, &label_26, 0);
    lv_obj_set_style_text_color(s_placeholder, lv_color_hex(C_DIM), 0);
    lv_obj_set_style_text_align(s_placeholder, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_placeholder, LV_ALIGN_CENTER, 0, 0);

    /* 接收进度条（最后创建 → 在最上层，不会被图片盖住）*/
    progress_create(parent);

    ui_photo_set_placeholder(UI_PHOTO_PLACEHOLDER_LOADING);
    return parent;
}

void ui_photo_show_frame(const uint16_t *src, int w, int h)
{
    if (s_img == NULL || src == NULL || w <= 0 || h <= 0) return;

    /* 描述符指向调用方的缓冲，不拷贝 */
    s_dsc.header.always_zero = 0;
    s_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
    s_dsc.header.w = (uint32_t)w;
    s_dsc.header.h = (uint32_t)h;
    s_dsc.data = (const uint8_t *)src;
    s_dsc.data_size = (uint32_t)w * (uint32_t)h * 2u;

    lv_img_set_src(s_img, &s_dsc);
    lv_obj_clear_flag(s_img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_placeholder, LV_OBJ_FLAG_HIDDEN);
    s_has_frame = true;

    /* 新帧尺寸可能与上一帧不同，重新对位（含当前位移相位）*/
    apply_shift();
}

void ui_photo_set_placeholder(ui_photo_placeholder_t kind)
{
    if (s_placeholder == NULL) return;

    const char *text = "";
    bool cjk = false;
    switch (kind) {
        case UI_PHOTO_PLACEHOLDER_NO_SD:
            text = "未检测到存储卡";
            cjk = true;
            break;
        case UI_PHOTO_PLACEHOLDER_EMPTY:
            /*
             * 旧文案是 "Upload in App"（Wi-Fi 上传时代的死路）——
             * 现在加图的入口在手机 App 的照片页，所以文案指向那里。
             */
            text = "还没有照片\n· 在 App 里添加";
            cjk = true;
            break;
        case UI_PHOTO_PLACEHOLDER_ERROR:
            text = "LOAD ERROR";
            break;
        case UI_PHOTO_PLACEHOLDER_LOADING:
        default:
            text = "LOADING...";
            break;
    }

    /* 中文文案必须用内嵌的 photo_ui_26（label_26 没有 CJK 字形，会显示方块）*/
    lv_obj_set_style_text_font(s_placeholder, cjk ? &photo_ui_26 : &label_26, 0);
    lv_label_set_text(s_placeholder, text);
    lv_obj_clear_flag(s_placeholder, LV_OBJ_FLAG_HIDDEN);

    if (s_img != NULL) lv_obj_add_flag(s_img, LV_OBJ_FLAG_HIDDEN);
    s_has_frame = false;
}

void ui_photo_set_shift(bool enable, int phase)
{
    /*
     * 变更检测：固件会按时间高频调用，若每次都 align → 每次都 invalidate →
     * 空闲时也几十 Hz 重绘（项目里已经踩过这个坑，见 ui_nav.h 的注释）。
     */
    if (s_shift_enable == enable && s_shift_phase == (phase & 3)) return;

    s_shift_enable = enable;
    s_shift_phase = phase & 3;
    apply_shift();
}

bool ui_photo_has_frame(void)
{
    return s_has_frame;
}

void ui_photo_set_progress(bool visible, int percent)
{
    if (s_prog_box == NULL || s_prog_fill == NULL || s_prog_label == NULL) return;

    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;

    /* 变更检测：BLE 上传时 5 Hz 调用，百分比没变就不碰 LVGL 对象 */
    if (s_prog_visible == visible && (!visible || s_prog_pct == percent)) return;
    s_prog_visible = visible;
    s_prog_pct = percent;

    if (!visible) {
        lv_obj_add_flag(s_prog_box, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_prog_label, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_obj_clear_flag(s_prog_box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_prog_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_width(s_prog_fill, (lv_coord_t)((PROG_BAR_W * percent) / 100));
    lv_label_set_text_fmt(s_prog_label, "接收 %d%%", percent);
}
