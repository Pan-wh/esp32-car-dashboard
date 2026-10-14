/**
 * ui_img_inflate.c —— 索引图充气实现
 *
 * 解码口径必须与 LVGL 完全一致，否则充气前后会**偏色**：
 *   LVGL 读调色板（lv_color32_t：B,G,R,A）后执行
 *       lv_color_make(ch.red, ch.green, ch.blue)
 *   在 LV_COLOR_DEPTH=16 下，ch.red 只有 5 位、green 6 位、blue 5 位，
 *   赋值即截断，最终 16 位取值 = (r>>3)<<11 | (g>>2)<<5 | (b>>3)。
 * 这里逐位做同样的运算，保证与解码器路径**逐像素一致**。
 */
#include "ui_img_inflate.h"

#include "img_indexed.h"

size_t ui_img_inflate_size(const lv_img_dsc_t *dsc)
{
    if (dsc == NULL) return 0;
    if (dsc->header.w <= 0 || dsc->header.h <= 0) return 0;
    return (size_t)dsc->header.w * (size_t)dsc->header.h * 2u;
}

bool ui_img_needs_inflate(const lv_img_dsc_t *dsc)
{
    if (dsc == NULL || dsc->data == NULL) return false;
    return dsc->header.cf == LV_IMG_CF_INDEXED_8BIT;
}

bool ui_img_inflate(lv_img_dsc_t *dsc, void *out_buf, size_t out_bytes)
{
    if (!ui_img_needs_inflate(dsc) || out_buf == NULL) return false;

    size_t need = ui_img_inflate_size(dsc);
    if (need == 0 || out_bytes < need) return false;

    if (!img_indexed_decode8(dsc->data, dsc->header.w, dsc->header.h,
                             (uint16_t *)out_buf, need / 2u)) {
        return false;
    }

    /* 就地改为直通图：之后 LVGL 直接按 RGB565 拷贝，不再查表 */
    dsc->header.cf = LV_IMG_CF_TRUE_COLOR;
    dsc->data_size = (uint32_t)need;
    dsc->data = (const uint8_t *)out_buf;
    return true;
}

int ui_img_inflate_all(lv_img_dsc_t *const *imgs, const char *const *names,
                       int n, void *(*alloc)(size_t))
{
    if (imgs == NULL || alloc == NULL || n <= 0) return 0;

    int ok = 0;
    for (int i = 0; i < n; i++) {
        lv_img_dsc_t *d = imgs[i];
        if (!ui_img_needs_inflate(d)) continue;

        size_t need = ui_img_inflate_size(d);
        if (need == 0) continue;

        void *buf = alloc(need);
        if (buf == NULL) continue; /* 静默降级：保持索引图，功能仍可用 */

        if (!ui_img_inflate(d, buf, need)) {
            continue; /* 解码失败：dsc 未被改写，缓冲交由调用方持有 */
        }
        ok++;
    }
    return ok;
}

bool ui_img_make_bg_transparent(lv_img_dsc_t *dsc)
{
    if (dsc == NULL || dsc->data == NULL) return false;
    if (dsc->header.cf == LV_IMG_CF_TRUE_COLOR_CHROMA_KEYED) return true;
    if (dsc->header.cf != LV_IMG_CF_TRUE_COLOR) return false;
    if (dsc->header.w <= 0 || dsc->header.h <= 0) return false;

    size_t pixels_n = (size_t)dsc->header.w * (size_t)dsc->header.h;
    uint16_t *pixels = (uint16_t *)(uintptr_t)dsc->data;
    /*
     * ⚠️ **假设左上角像素就是背景色**（"纯色背景 + 前景"的图成立）。
     * 已核实当前三张驾驶员图：四角同色（纯白），白色占 23.7% 且环绕前景。
     * **换图时必须确认这一点** —— 若左上角落在前景上，下面就会把前景
     * 某个颜色整片抠成透明（表现为人物出现空洞）。
     */
    const uint16_t bg = pixels[0];
    const lv_color_t key_color = LV_COLOR_CHROMA_KEY;
    const uint16_t key = key_color.full;

    for (size_t i = 0; i < pixels_n; i++) {
        if (pixels[i] == bg) pixels[i] = key;
    }
    dsc->header.cf = LV_IMG_CF_TRUE_COLOR_CHROMA_KEYED;
    return true;
}
