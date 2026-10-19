/**
 * ui_photo.c —— 照片表盘的渲染层
 *
 * 只做三件事：显示一帧 RGB565、显示占位提示、按相位做像素位移。
 *
 * ## 为什么用 `lv_img` + 用户描述符
 *
 * 固件把一帧解到 PSRAM（RGB565），本模块用一个 `lv_img_dsc_t` **指向那块内存**，
 * 无需拷贝。`lv_img_set_src()` 传 `&dsc` 即可。
 *
 * ⚠️ 每次换帧都 `lv_img_set_src(&dsc)` 会触发**整图重绘**（466×466 = 21.7 万像素）。
 * 动图每帧都要重绘是**必然**的 —— 这正是 §十点五 实测里那 43ms flush 的来源。
 */
#include "ui_photo.h"

/*
 * ⚠️ `label_26` **没有 CJK 字形**（见 nav.h 的注释），所以本表盘的所有文案
 * 必须是 ASCII —— 否则屏幕上显示成方块。设备的整体风格本来就是 ASCII
 * （导航卡显示 STALE / LIVE），这里保持一致。
 */
LV_FONT_DECLARE(label_26);

#include <string.h>

/* 配色与其它表盘一致：纯黑底（AMOLED 黑不发光，位移露出底色也看不出来）*/
#define C_BG 0x000000
#define C_TEXT 0xFFFFFF
#define C_DIM 0x9AA0A6

static lv_obj_t *s_root;
static lv_obj_t *s_img;
static lv_obj_t *s_placeholder;
static lv_img_dsc_t s_dsc;
static bool s_has_frame;
static int s_shift_phase = -1; /* -1 = 尚未设置 */
static bool s_shift_enable;

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

lv_obj_t *ui_photo_create(lv_obj_t *parent)
{
    s_root = parent;
    s_img = NULL;
    s_placeholder = NULL;
    s_has_frame = false;
    s_shift_phase = -1;
    s_shift_enable = false;
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
    switch (kind) {
        case UI_PHOTO_PLACEHOLDER_NO_SD:   text = "NO SD CARD"; break;
        case UI_PHOTO_PLACEHOLDER_EMPTY:   text = "NO PHOTOS\nUpload in App"; break;
        case UI_PHOTO_PLACEHOLDER_ERROR:   text = "LOAD ERROR"; break;
        case UI_PHOTO_PLACEHOLDER_LOADING: text = "LOADING..."; break;
        default: text = ""; break;
    }
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
