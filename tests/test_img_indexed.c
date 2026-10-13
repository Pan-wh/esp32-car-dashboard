/**
 * test_img_indexed.c —— 8bpp 索引图解码单元测试（PC 上运行，不需硬件/LVGL）
 *
 * 覆盖：调色板字节序、16 位截断口径、尺寸/容量边界、非法参数、
 *       以及与 LVGL 解码口径的一致性（逐位对齐）。
 */
#include <stdio.h>
#include <string.h>

#include "img_indexed.h"

static int g_pass, g_fail;

#define CHECK(cond, fmt, ...)                                                  \
    do {                                                                       \
        if (cond) { g_pass++; }                                                \
        else {                                                                 \
            g_fail++;                                                          \
            printf("  ✗ %s:%d  " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__);  \
        }                                                                      \
    } while (0)

/* LVGL 在 16 位色深下的取值：lv_color_make 的字段截断 */
static uint16_t lvgl_expected(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

/* 构造一张图的源数据：调色板 256 项 + 索引 */
static void build(uint8_t *src, const uint8_t pal[][3], int n_pal,
                  const uint8_t *idx, int n_px)
{
    memset(src, 0, IMG_INDEXED_PALETTE_BYTES);
    for (int i = 0; i < n_pal; i++) {
        src[i * 4 + 0] = pal[i][2]; /* B */
        src[i * 4 + 1] = pal[i][1]; /* G */
        src[i * 4 + 2] = pal[i][0]; /* R */
        src[i * 4 + 3] = 0xFF;      /* A */
    }
    memcpy(src + IMG_INDEXED_PALETTE_BYTES, idx, (size_t)n_px);
}

int main(void)
{
    printf("=== 索引图解码 ===\n");

    /* ---- 字节序：调色板是 B,G,R,A，解出来必须是正确的红绿蓝 ---- */
    printf("--- 调色板字节序 ---\n");
    {
        static const uint8_t pal[][3] = {
            { 0xFF, 0x00, 0x00 }, /* 纯红 */
            { 0x00, 0xFF, 0x00 }, /* 纯绿 */
            { 0x00, 0x00, 0xFF }, /* 纯蓝 */
            { 0xFF, 0xFF, 0xFF }, /* 白 */
            { 0x00, 0x00, 0x00 }, /* 黑 */
        };
        const uint8_t idx[5] = { 0, 1, 2, 3, 4 };
        uint8_t src[IMG_INDEXED_PALETTE_BYTES + 5];
        uint16_t out[5];
        build(src, pal, 5, idx, 5);

        CHECK(img_indexed_decode8(src, 5, 1, out, 5), "解码成功");
        CHECK(out[0] == lvgl_expected(0xFF, 0x00, 0x00), "索引 0 = 红（实际 0x%04X）", out[0]);
        CHECK(out[1] == lvgl_expected(0x00, 0xFF, 0x00), "索引 1 = 绿（实际 0x%04X）", out[1]);
        CHECK(out[2] == lvgl_expected(0x00, 0x00, 0xFF), "索引 2 = 蓝（实际 0x%04X）", out[2]);
        CHECK(out[3] == lvgl_expected(0xFF, 0xFF, 0xFF), "索引 3 = 白（实际 0x%04X）", out[3]);
        CHECK(out[4] == lvgl_expected(0x00, 0x00, 0x00), "索引 4 = 黑（实际 0x%04X）", out[4]);

        /* 关键：红色若按 R,G,B 误读，会得到蓝色 → 下面这条能捕获 */
        CHECK(out[0] != lvgl_expected(0x00, 0x00, 0xFF), "红≠蓝（字节序未写反）");
    }

    /* ---- 16 位截断口径必须与 LVGL 一致 ---- */
    printf("--- 截断口径 ---\n");
    {
        static const uint8_t pal[][3] = {
            { 0x12, 0x34, 0x56 }, { 0xFF, 0xFF, 0xFF }, { 0x01, 0x02, 0x03 },
        };
        const uint8_t idx[3] = { 0, 1, 2 };
        uint8_t src[IMG_INDEXED_PALETTE_BYTES + 3];
        uint16_t out[3];
        build(src, pal, 3, idx, 3);

        CHECK(img_indexed_decode8(src, 3, 1, out, 3), "解码成功");
        CHECK(out[0] == lvgl_expected(0x12, 0x34, 0x56), "0x123456 截断一致（实际 0x%04X）", out[0]);
        CHECK(out[1] == 0xFFFF, "白 = 0xFFFF");
        CHECK(out[2] == lvgl_expected(0x01, 0x02, 0x03), "暗色截断一致");
    }

    /* ---- 尺寸与容量 ---- */
    printf("--- 尺寸与容量 ---\n");
    CHECK(img_indexed_pixels(466, 466) == 217156, "466×466 = 217156 像素");
    CHECK(img_indexed_src_bytes(466, 466) == 1024 + 217156, "源字节数 = 调色板 + 索引");
    CHECK(img_indexed_pixels(0, 10) == 0, "宽 0 → 0");
    CHECK(img_indexed_pixels(-1, 10) == 0, "负宽 → 0");
    CHECK(img_indexed_pixels(10, 0) == 0, "高 0 → 0");
    CHECK(img_indexed_src_bytes(-5, -5) == 0, "非法尺寸 → 0");

    {
        uint8_t src[IMG_INDEXED_PALETTE_BYTES + 4];
        uint16_t out[4];
        memset(src, 0, sizeof(src));
        CHECK(img_indexed_decode8(src, 2, 2, out, 4), "2×2 恰好放得下");
        CHECK(!img_indexed_decode8(src, 2, 2, out, 3), "缓冲差 1 个像素 → 拒绝");
        CHECK(!img_indexed_decode8(src, 3, 2, out, 4), "缓冲不足（6>4）→ 拒绝");
    }

    /* ---- 非法参数 ---- */
    printf("--- 非法参数 ---\n");
    {
        uint8_t src[IMG_INDEXED_PALETTE_BYTES + 4];
        uint16_t out[4];
        memset(src, 0, sizeof(src));
        CHECK(!img_indexed_decode8(NULL, 2, 2, out, 4), "NULL 源 → 拒绝");
        CHECK(!img_indexed_decode8(src, 2, 2, NULL, 4), "NULL 输出 → 拒绝");
        CHECK(!img_indexed_decode8(src, 0, 2, out, 4), "尺寸 0 → 拒绝");
        CHECK(!img_indexed_decode8(src, -2, 2, out, 4), "负尺寸 → 拒绝");
        CHECK(!img_indexed_decode8(src, 2, 2, out, 0), "容量 0 → 拒绝");
    }

    /* ---- 全 256 索引都能取到（不越界、不串色）---- */
    printf("--- 全索引覆盖 ---\n");
    {
        uint8_t src[IMG_INDEXED_PALETTE_BYTES + 256];
        uint16_t out[256];
        /* 调色板：索引 i 存灰度 i（B=G=R=i），便于校验映射正确 */
        for (int i = 0; i < 256; i++) {
            src[i * 4 + 0] = (uint8_t)i;
            src[i * 4 + 1] = (uint8_t)i;
            src[i * 4 + 2] = (uint8_t)i;
            src[i * 4 + 3] = 0xFF;
        }
        for (int i = 0; i < 256; i++) src[IMG_INDEXED_PALETTE_BYTES + i] = (uint8_t)i;

        CHECK(img_indexed_decode8(src, 256, 1, out, 256), "256 索引解码成功");
        int bad = 0;
        for (int i = 0; i < 256; i++) {
            if (out[i] != lvgl_expected((uint8_t)i, (uint8_t)i, (uint8_t)i)) bad++;
        }
        CHECK(bad == 0, "全部 256 个索引映射正确（错 %d 个）", bad);
        CHECK(out[255] == lvgl_expected(255, 255, 255), "索引 255 正确（未越界读调色板）");
    }

    printf("结果: %d 通过, %d 失败\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
