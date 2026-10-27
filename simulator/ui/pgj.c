/**
 * pgj.c —— `.pgj` 容器（32 B 头 + 1 张 baseline JPEG）的解析/校验/构造
 *
 * 设计要点见 pgj.h。这里只做三件事：头字段的取值域校验、JPEG marker 扫描
 * （只找 SOF，不解像素）、标准 CRC-32。全程**无动态内存**：设备扫描清单时
 * 每个文件只读 32 B；只有提交/播放前的整文件校验才会线性扫一遍 JPEG marker。
 *
 * 为什么自己扫 marker 而不用 `esp_jpeg_get_image_info()`：
 *   - 本模块要在 PC 上可测，不能依赖 ESP32 ROM；
 *   - 我们只需要 SOF 的宽高和"是不是 baseline"，扫到 SOF 就停（通常在文件前
 *     几百字节），比初始化解码器便宜；
 *   - 顺带能在扫描阶段就明确区分 progressive / 其它 SOF，给出可执行的错误码。
 */
#include "pgj.h"

#include <string.h>

/* ── 小端读写（不假设主机字节序）────────────────────────── */

static uint16_t rd_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void wr_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)(v >> 8);
}

static void wr_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

bool pgj_parse_header(const uint8_t *buf, size_t len, pgj_header_t *out)
{
    if (buf == NULL || out == NULL) return false;
    if (len < PGJ_HEADER_BYTES) return false;
    if (memcmp(buf, "PGJ1", 4) != 0) return false;

    pgj_header_t h;
    h.width        = rd_u16(buf + 4);
    h.height       = rd_u16(buf + 6);
    h.version      = buf[8];
    h.flags        = buf[9];
    h.quality      = rd_u16(buf + 10);
    h.jpeg_bytes   = rd_u32(buf + 12);
    h.crc32        = rd_u32(buf + 16);
    h.created_unix = rd_u32(buf + 20);
    /* 24..31 reserved 有意不校验：留作后续版本扩展，旧固件要能忽略未知位 */

    if (h.version != PGJ_VERSION) return false;
    /* v1 只接受 baseline JPEG；bit0=0 意味着编码器升级了但固件没跟上 */
    if ((h.flags & PGJ_FLAG_BASELINE) == 0) return false;
    if (h.width == 0 || h.width > PGJ_MAX_DIM) return false;
    if (h.height == 0 || h.height > PGJ_MAX_DIM) return false;
    if (h.jpeg_bytes == 0 || h.jpeg_bytes > PGJ_MAX_JPEG_BYTES) return false;
    /* 头注释写的是 1..100（协议 §7 写 10..100）：这里取更宽的 1..100，
     * 因为 quality 仅记录、不参与解码，没必要为它拒掉一张能显示的照片 */
    if (h.quality == 0 || h.quality > 100) return false;

    *out = h;
    return true;
}

/** 是不是 SOFn（帧头）marker。0xC4(DHT)/0xC8(JPG)/0xCC(DAC) 不是。 */
static bool is_sof_marker(uint8_t m)
{
    switch (m) {
    case 0xC0:
    case 0xC1:
    case 0xC2:
    case 0xC3:
    case 0xC5:
    case 0xC6:
    case 0xC7:
    case 0xC9:
    case 0xCA:
    case 0xCB:
    case 0xCD:
    case 0xCE:
    case 0xCF:
        return true;
    default:
        return false;
    }
}

bool pgj_jpeg_info(const uint8_t *jpeg, size_t len, uint16_t *w, uint16_t *h, const char **err)
{
    /* 默认错误码：任何"没读到 SOF"的路径都归为坏 JPEG（截断也走这里） */
    const char *e = "bad_jpeg";

    if (jpeg == NULL) goto fail;
    if (len < 4) goto fail;
    if (jpeg[0] != 0xFF || jpeg[1] != 0xD8) goto fail; /* 缺 SOI */

    size_t pos = 2;
    while (pos < len) {
        if (jpeg[pos] != 0xFF) goto fail;
        /* 允许 0xFF 填充字节（规范允许 marker 前有任意个 0xFF） */
        while (pos < len && jpeg[pos] == 0xFF) pos++;
        if (pos >= len) goto fail;

        uint8_t m = jpeg[pos++];
        if (m == 0x00) goto fail; /* 0xFF00 是熵数据里的字节填充，不该出现在段间 */

        /* 无长度字段的独立 marker：RSTn / TEM */
        if (m == 0x01 || (m >= 0xD0 && m <= 0xD7)) continue;
        if (m == 0xD8) goto fail;                     /* 重复 SOI：结构坏了 */
        if (m == 0xD9) goto fail;                     /* 还没 SOF 就 EOI：截断 */
        if (len - pos < 2) goto fail;

        size_t seg = ((size_t)jpeg[pos] << 8) | (size_t)jpeg[pos + 1];
        if (seg < 2 || seg > len - pos) goto fail;    /* 段长度越界 → 截断/坏文件 */

        if (m == 0xDA) goto fail;                     /* 先 SOS 后 SOF：结构坏了 */
        if (is_sof_marker(m)) {
            if (m == 0xC2) {
                e = "progressive";
                goto fail;
            }
            if (m != 0xC0) {
                e = "unsupported_sof";
                goto fail;
            }
            /* SOF0 段体：精度(1) 高(2) 宽(2) 分量数(1) ... */
            if (seg < 8) goto fail;
            const uint8_t *d = jpeg + pos + 2;
            uint16_t ih = (uint16_t)(((uint16_t)d[1] << 8) | d[2]);
            uint16_t iw = (uint16_t)(((uint16_t)d[3] << 8) | d[4]);
            if (d[5] == 0) goto fail; /* 0 个分量 → 坏文件 */
            /* 高度为 0 在规范里可由 DNL 段补齐，但设备不会见到这种文件，直接拒 */
            if (iw == 0 || ih == 0) goto fail;
            if (w != NULL) *w = iw;
            if (h != NULL) *h = ih;
            if (err != NULL) *err = NULL;
            return true;
        }
        pos += seg;
    }

fail:
    if (err != NULL) *err = e;
    return false;
}

bool pgj_validate_file(const uint8_t *buf, size_t len, pgj_header_t *out, const char **err)
{
    pgj_header_t h;
    const char *e = NULL;

    if (err != NULL) *err = NULL;
    if (buf == NULL) {
        if (err != NULL) *err = "bad_arg";
        return false;
    }
    if (!pgj_parse_header(buf, len, &h)) {
        if (err != NULL) *err = "bad_header";
        return false;
    }
    /* 头里的 jpeg_bytes 必须与真实长度严格自洽：否则"头对、文件半截"会漏过 */
    if (len != (size_t)PGJ_HEADER_BYTES + (size_t)h.jpeg_bytes) {
        if (err != NULL) *err = "bad_size";
        return false;
    }

    uint16_t w = 0, hh = 0;
    if (!pgj_jpeg_info(buf + PGJ_HEADER_BYTES, h.jpeg_bytes, &w, &hh, &e)) {
        if (err != NULL) *err = e;
        return false;
    }
    /* 头里的宽高必须等于 JPEG SOF 里的宽高：App 侧编码参数错会在这里暴露 */
    if (w != h.width || hh != h.height) {
        if (err != NULL) *err = "bad_dim";
        return false;
    }

    if (out != NULL) *out = h;
    return true;
}

/* ── 标准 CRC-32 ────────────────────────────────────────────
 *
 * 用 16 项"半字节查表"（64 B）而不是 256 项全表（1 KB）：设备端静态 RAM 紧，
 * 代价只是每字节多一次查表，对 50 KB 的照片可忽略。
 * 表值是 CRC-32/ISO-HDLC（反射，poly 0xEDB88320）半字节形式。
 */
static const uint32_t s_crc_nibble[16] = {
    0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu,
    0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
    0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu,
    0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu,
};

uint32_t pgj_crc32(const void *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    const uint8_t *p = (const uint8_t *)data;

    if (p == NULL) {
        /* 约定：NULL 视作空数据（空数据 CRC = 0），避免调用方多一个分支 */
        return 0u;
    }
    for (size_t i = 0; i < len; i++) {
        crc = (crc >> 4) ^ s_crc_nibble[(crc ^ p[i]) & 0x0Fu];
        crc = (crc >> 4) ^ s_crc_nibble[(crc ^ (p[i] >> 4)) & 0x0Fu];
    }
    return crc ^ 0xFFFFFFFFu;
}

size_t pgj_build_header(uint8_t out[PGJ_HEADER_BYTES], const pgj_header_t *h)
{
    if (out == NULL || h == NULL) return 0;

    memcpy(out, "PGJ1", 4);
    wr_u16(out + 4, h->width);
    wr_u16(out + 6, h->height);
    out[8]  = h->version;
    out[9]  = h->flags;
    wr_u16(out + 10, h->quality);
    wr_u32(out + 12, h->jpeg_bytes);
    wr_u32(out + 16, h->crc32);
    wr_u32(out + 20, h->created_unix);
    memset(out + 24, 0, 8); /* reserved 必须写 0，保证同一输入的字节级输出稳定 */
    return PGJ_HEADER_BYTES;
}
