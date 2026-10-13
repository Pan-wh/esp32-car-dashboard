/**
 * img_indexed.c —— 8bpp 索引图解码（纯逻辑，无 LVGL/硬件依赖）
 *
 * 零依赖：只有指针运算与位运算，便于 PC 单测（含越界与非法参数）。
 */
#include "img_indexed.h"

size_t img_indexed_pixels(int w, int h)
{
    if (w <= 0 || h <= 0) return 0;
    return (size_t)w * (size_t)h;
}

size_t img_indexed_src_bytes(int w, int h)
{
    size_t px = img_indexed_pixels(w, h);
    if (px == 0) return 0;
    return IMG_INDEXED_PALETTE_BYTES + px;
}

bool img_indexed_decode8(const uint8_t *src, int w, int h,
                         uint16_t *out, size_t out_capacity)
{
    if (src == NULL || out == NULL) return false;

    size_t px = img_indexed_pixels(w, h);
    if (px == 0) return false;
    if (out_capacity < px) return false;

    const uint8_t *palette = src;
    const uint8_t *indices = src + IMG_INDEXED_PALETTE_BYTES;

    for (size_t i = 0; i < px; i++) {
        const uint8_t *e = palette + (size_t)indices[i] * 4u;
        uint8_t b = e[0];
        uint8_t g = e[1];
        uint8_t r = e[2];
        /* 与 lv_color_make 的 16 位截断逐位一致，保证充气前后不偏色 */
        out[i] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    }
    return true;
}
