/**
 * pgi.c —— `.pgi` 容器的解析与解码（**纯逻辑，不依赖 LVGL**）
 *
 * 设计要点见 pgi.h。这里只做三件事：校验文件头、定位帧、把帧交给
 * `img_indexed_decode8()`。
 */
#include "pgi.h"
#include "img_indexed.h"

#include <string.h>

/* ── 小端读取（不假设主机字节序）────────────────────────── */

static uint16_t rd_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

/** 一段范围 [off, off+size) 是否完整落在 [0, len) 内（防整数溢出）*/
static bool in_range(uint32_t off, uint32_t size, size_t len)
{
    if (len < PGI_HEADER_BYTES) return false;
    /* 用 64 位比较，避免 off+size 溢出 */
    return ((uint64_t)off + (uint64_t)size) <= (uint64_t)len;
}

bool pgi_parse_header(const uint8_t *src, size_t len, pgi_header_t *out)
{
    if (src == NULL || out == NULL) return false;
    if (len < PGI_HEADER_BYTES) return false;
    if (memcmp(src, "PGI1", 4) != 0) return false;

    pgi_header_t h;
    memset(&h, 0, sizeof(h));
    h.width           = rd_u16(src + 4);
    h.height          = rd_u16(src + 6);
    h.frame_count     = rd_u16(src + 8);
    h.fps             = src[10];
    h.flags           = src[11];
    h.frame_table_off = rd_u32(src + 16);
    h.data_off        = rd_u32(src + 20);

    /* ── 取值域校验（任何一条不过都当"不是合法 pgi"）── */
    if (h.width == 0 || h.height == 0) return false;
    if (h.width > PGI_MAX_DIMENSION || h.height > PGI_MAX_DIMENSION) return false;
    if (h.frame_count == 0) return false;
    if (h.frame_count > PGI_MAX_FRAMES) return false;
    /* v1 不支持 RLE：遇到就明确拒绝，而不是画出花屏 */
    if ((h.flags & PGI_FLAG_RLE) != 0) return false;
    if (h.fps > 60) return false; /* 超过 60fps 无意义，且多半是坏文件 */

    /* 帧索引表必须完整在文件内 */
    uint32_t table_bytes = (uint32_t)h.frame_count * PGI_FRAME_ENTRY_BYTES;
    if (!in_range(h.frame_table_off, table_bytes, len)) return false;
    /* 数据区起点必须在文件内（允许等于 len 之外的空数据由帧级校验兜住）*/
    if (h.data_off > len) return false;
    /* 数据区不得早于文件头 */
    if (h.data_off < PGI_HEADER_BYTES) return false;

    *out = h;
    return true;
}

bool pgi_frame_info(const uint8_t *src, size_t len, const pgi_header_t *h,
                    int index, uint32_t *off, uint32_t *size)
{
    if (src == NULL || h == NULL) return false;
    if (index < 0 || index >= (int)h->frame_count) return false;

    uint32_t entry = h->frame_table_off + (uint32_t)index * PGI_FRAME_ENTRY_BYTES;
    if (!in_range(entry, PGI_FRAME_ENTRY_BYTES, len)) return false;

    uint32_t f_off  = rd_u32(src + entry);
    uint32_t f_size = rd_u32(src + entry + 4);

    /* 每帧至少要有"调色板 + 至少 1 个像素"，且必须完整在文件内 */
    size_t px = (size_t)h->width * (size_t)h->height;
    uint32_t expect = (uint32_t)PGI_PALETTE_BYTES + (uint32_t)px;
    if (f_size < PGI_PALETTE_BYTES + 1u) return false;
    if (f_size < expect) return false; /* 比声明尺寸小 → 坏文件 */
    if (!in_range(f_off, f_size, len)) return false;
    /* 帧不得落在文件头里 */
    if (f_off < PGI_HEADER_BYTES) return false;

    if (off != NULL) *off = f_off;
    if (size != NULL) *size = f_size;
    return true;
}

bool pgi_decode_frame(const uint8_t *src, size_t len, const pgi_header_t *h,
                      int index, uint16_t *dst, size_t dst_capacity)
{
    uint32_t f_off = 0, f_size = 0;
    if (!pgi_frame_info(src, len, h, index, &f_off, &f_size)) return false;

    /*
     * 帧布局 = [调色板 1024 B][索引 w*h] —— **正好**是 img_indexed_decode8
     * 期望的布局，所以这里直接转交，不复制任何转换逻辑。
     */
    size_t px = (size_t)h->width * (size_t)h->height;
    if (dst_capacity < px) return false;

    return img_indexed_decode8(src + f_off, (int)h->width, (int)h->height, dst,
                               dst_capacity);
}

uint32_t pgi_cycle_ms(const pgi_header_t *h)
{
    if (h == NULL) return 0;
    if (h->frame_count <= 1) return 0; /* 静态：由"轮播间隔"决定，不是这里 */
    if (h->fps == 0) return 0;         /* 多帧却没 fps → 视作无效播放 */
    return (uint32_t)h->frame_count * 1000u / (uint32_t)h->fps;
}
