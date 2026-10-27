/**
 * jpeg_decode.h —— 用 ESP32-S3 ROM 里的 TJpgDec（经 esp_jpeg）把 JPEG 解成 RGB565
 *
 * 背景：S3 的 ROM 自带 `jd_prepare/jd_decomp`，Arduino-ESP32 核心已预编译
 * `esp_jpeg`（`libespressif__esp_jpeg.a`，include/链接参数都在默认路径），
 * 因此**零安装、Flash 增量 ≈0~2 KB**。代价是单帧 466×466 约 200~260 ms（待真机 S3 标定）。
 *
 * 只给**静态照片**（`.pgj`）用；动图仍走 8bpp 索引（`img_indexed`，21 ms）。
 */
#ifndef JPEG_DECODE_H
#define JPEG_DECODE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 解码一张 baseline JPEG 到 RGB565（打包成 LVGL 的 `uint16_t` 像素）。
 *
 * @param jpg      完整 JPEG（整个文件需在内存；`.pgj` 的载荷）
 * @param len      JPEG 长度
 * @param out      输出缓冲（PSRAM），容量 out_px 个像素
 * @param out_px   缓冲能放多少像素（466×466 = 217156）
 * @param out_w/out_h 成功时写实际尺寸
 * @return false：解码失败 / 尺寸超出缓冲 / 非 baseline
 *
 * ⚠️ 阻塞调用（200~260 ms）。只允许在"切到某张静态图"时调用。
 * ⚠️ RGB565 字节序：esp_jpeg 默认输出大端，LVGL 要小端；本封装默认做 swizzle，
 *    真机上若颜色异常，改 `jpeg_decode_set_swap()` 即可（不改变调用方）。
 */
bool jpeg_decode_rgb565(const uint8_t *jpg, size_t len, uint16_t *out, size_t out_px,
                        uint16_t *out_w, uint16_t *out_h);

/** 最近一次解码耗时（ms；0 = 还没解过）。给 PhotoInfo.bench.dec_ms 用。 */
uint32_t jpeg_decode_last_ms(void);

/** 读取 JPEG 尺寸（不解像素；用于提交校验）。 */
bool jpeg_decode_probe(const uint8_t *jpg, size_t len, uint16_t *w, uint16_t *h);

/** 字节序开关（默认 true）。仅供真机标定用。 */
void jpeg_decode_set_swap(bool swap);

#ifdef __cplusplus
}
#endif

#endif /* JPEG_DECODE_H */
