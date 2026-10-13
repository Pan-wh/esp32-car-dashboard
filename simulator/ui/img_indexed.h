/**
 * img_indexed.h —— 8bpp 索引图解码（**纯逻辑，不依赖 LVGL**，可 PC 单测）
 *
 * 数据布局（与 LVGL 8 对 `LV_IMG_SRC_VARIABLE` 的约定一致，已对源码核实）：
 *
 *     [调色板 256 × 4 字节][像素索引 w×h 字节]
 *
 * 调色板每项字节序为 **B, G, R, A**（`lv_color32_t` 的字段顺序）。
 *
 * 与 LVGL 的分工：本模块只做"字节 → RGB565 数值"的纯计算，
 * 便于在 PC 上单测边界与越界；`lv_img_dsc_t` 的改造见 ui_img_inflate.h。
 */
#ifndef IMG_INDEXED_H
#define IMG_INDEXED_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 调色板恒为 256 项（LVGL 对索引 8 位图固定读 256 项），每项 4 字节 */
#define IMG_INDEXED_PALETTE_ENTRIES 256
#define IMG_INDEXED_PALETTE_BYTES   (IMG_INDEXED_PALETTE_ENTRIES * 4)

/** 解码所需的输出像素个数；参数非法返回 0。 */
size_t img_indexed_pixels(int w, int h);

/** 解码所需的源数据字节数（调色板 + 索引）；参数非法返回 0。 */
size_t img_indexed_src_bytes(int w, int h);

/**
 * 8bpp 索引图 → RGB565 像素。
 *
 * 取值口径与 LVGL 完全一致（`lv_color_make` 在 16 位色深下的截断）：
 *     v = (r >> 3) << 11 | (g >> 2) << 5 | (b >> 3)
 * 保证"充气"前后渲染**逐像素相同**。
 *
 * @param src           源数据（调色板 + 索引）
 * @param w,h           图像尺寸
 * @param out           输出（每像素 2 字节，写入 uint16_t）
 * @param out_capacity  输出**像素个数**上限
 * @return false 表示参数非法或输出缓冲不足
 */
bool img_indexed_decode8(const uint8_t *src, int w, int h,
                         uint16_t *out, size_t out_capacity);

#ifdef __cplusplus
}
#endif

#endif /* IMG_INDEXED_H */
