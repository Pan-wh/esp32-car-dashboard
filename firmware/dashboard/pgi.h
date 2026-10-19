/**
 * pgi.h —— `.pgi`（Photo/Graphic Indexed）容器：**照片表盘的图片格式**
 *
 * 纯逻辑，不依赖 LVGL，可在 PC 上单测。
 *
 * ## 为什么自己定一个格式
 *
 * 设备端**不解** GIF / PNG / JPEG：
 *   - ESP32-S3 无硬件 JPEG 解码，软解 466×466 要 50~100 ms → 动图不可行；
 *   - GIF 要 LZW + 多帧，PNG 要 zlib/inflate，都太重。
 *
 * 而项目**已有 8bpp 索引图解码**（`img_indexed_decode8`，28 项单测）——
 * 所以 `.pgi` 的每一帧**就是** `img_indexed` 能直接吃的字节布局：
 *
 *     [调色板 256 × 4 B（B,G,R,A）][索引 w×h 字节]
 *
 * 这样两件事同时成立：
 *   1. **零重复逻辑** —— 本模块直接调 `img_indexed_decode8()`，不会与它漂移；
 *   2. **天然支持逐帧调色板** —— GIF 的典型特征就是每帧可以有自己的调色板，
 *      把调色板放在每帧前面即可（代价 1 KB/帧，可忽略）。
 *
 * ## 文件布局（全部小端）
 *
 * ```
 * 偏移  长度  字段
 *   0     4   magic "PGI1"
 *   4     2   width            u16
 *   6     2   height           u16
 *   8     2   frame_count      u16（>= 1）
 *  10     1   fps              u8（0 = 静态/单帧）
 *  11     1   flags            u8（bit0 = 帧数据为 RLE；**v1 必须为 0**）
 *  12     2   reserved         u16 = 0
 *  14     2   reserved         u16 = 0
 *  16     4   frame_table_off  u32（帧索引表偏移，相对文件头）
 *  20     4   data_off         u32（帧数据区起始）
 *  24     4   reserved         u32 = 0
 *  28     4   reserved         u32 = 0
 * ---- 32 字节文件头结束 ----
 *  帧索引表：frame_count × 8 B
 *      每项： off u32（该帧**调色板**起始，相对文件头）| size u32（= 1024 + w*h）
 *  帧数据：每帧如上（调色板 + 索引）
 * ```
 *
 * ## 与"静态 / 动图"的关系
 *
 * **帧数就是类型**：`frame_count == 1` → 静态照片；`> 1` → 动图。
 * 不需要额外的"这是静态还是动图"标记 —— 表盘只按帧数决定播放方式。
 */
#ifndef PGI_H
#define PGI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 文件头固定长度 */
#define PGI_HEADER_BYTES 32u
/** 每帧自带的调色板长度（256 × 4 B，与 img_indexed 一致）*/
#define PGI_PALETTE_BYTES 1024u
/** 每帧索引表项长度 */
#define PGI_FRAME_ENTRY_BYTES 8u

/** 支持的最大帧数（防止异常文件把内存算爆；24 帧 @12fps ≈ 2 秒动图）*/
#define PGI_MAX_FRAMES 512
/** 支持的单边最大像素（本项目屏是 466；留一倍余量）*/
#define PGI_MAX_DIMENSION 1024
/** flags 里 v1 唯一会拒绝的位：RLE 压缩 */
#define PGI_FLAG_RLE 0x01u

/** 解析后的文件头 */
typedef struct {
    uint16_t width;
    uint16_t height;
    uint16_t frame_count;
    uint8_t  fps;          /* 0 = 静态 */
    uint8_t  flags;
    uint32_t frame_table_off;
    uint32_t data_off;
} pgi_header_t;

/**
 * 解析文件头并做**结构与取值域**校验。
 *
 * 校验项：magic、宽高非 0 且不超过 `PGI_MAX_DIMENSION`、帧数 1..`PGI_MAX_FRAMES`、
 * `flags` 不含 RLE、帧索引表与数据区都在文件范围内。
 *
 * @return true = 可用。失败时 `out` 内容未定义。
 */
bool pgi_parse_header(const uint8_t *src, size_t len, pgi_header_t *out);

/**
 * 取第 `index` 帧的调色板偏移与长度。
 *
 * @param off  输出：该帧起始（调色板）在文件中的偏移
 * @param size 输出：该帧总长度（应为 `PGI_PALETTE_BYTES + w*h`）
 * @return false = 下标越界、表项损坏或超出文件范围
 */
bool pgi_frame_info(const uint8_t *src, size_t len, const pgi_header_t *h,
                    int index, uint32_t *off, uint32_t *size);

/**
 * 解码第 `index` 帧到 RGB565。
 *
 * **内部直接调用 `img_indexed_decode8()`** —— 与现有图片管线逐像素一致，
 * 不存在第二份转换逻辑。
 *
 * @param dst          输出缓冲（`width*height` 个 uint16）
 * @param dst_capacity 缓冲可容纳的像素数
 */
bool pgi_decode_frame(const uint8_t *src, size_t len, const pgi_header_t *h,
                      int index, uint16_t *dst, size_t dst_capacity);

/** 该帧是否是动图帧（`frame_count > 1`）*/
static inline bool pgi_is_animated(const pgi_header_t *h)
{
    return h != NULL && h->frame_count > 1;
}

/** 播放一整轮需要多久（毫秒）。静态或 fps 为 0 时返回 0。 */
uint32_t pgi_cycle_ms(const pgi_header_t *h);

#ifdef __cplusplus
}
#endif

#endif /* PGI_H */
