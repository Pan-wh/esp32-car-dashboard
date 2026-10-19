/**
 * test_pgi.c —— `.pgi` 容器的 **纯逻辑** 单测（不需要硬件）
 *
 * 覆盖重点：
 *   1. **逐像素一致性**：解码结果必须与 `img_indexed_decode8()` **完全相同**
 *      —— 这是本模块存在的意义（不产生第二份颜色转换逻辑）；
 *   2. 逐帧调色板：不同帧可以有不同的调色板 → 两帧解出来的像素不同；
 *   3. 文件头校验的每一条拒绝路径（magic / 尺寸 / 帧数 / RLE / fps / 越界）；
 *   4. 帧索引表的每一条拒绝路径（下标越界 / 长度不足 / 超出文件）。
 *
 * 用内存里现场构造的 `.pgi`，不依赖磁盘文件；另有一节用 `tools/pgi.py`
 * 生成的夹具做**跨语言**校验（见 test_pgi_tool_fixture）。
 */
#include "../simulator/ui/img_indexed.h"
#include "../simulator/ui/pgi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_pass, g_fail;

#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        if (cond) {                                                            \
            g_pass++;                                                          \
        } else {                                                               \
            g_fail++;                                                          \
            printf("  ✗ %s:%d  %s\n", __FILE__, __LINE__, msg);                \
        }                                                                      \
    } while (0)

/* ── 构造工具 ─────────────────────────────────────────────── */

static void wr_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
}

static void wr_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

/**
 * 构造一个合法的 `.pgi`。
 *
 * 每帧用调用方给的调色板基色 + 帧序号做偏移，使得**不同帧的像素必然不同**
 * （用于验证"逐帧调色板真的生效"）。
 *
 * @param frame_colors 每帧的基色（R,G,B），长度 = frame_count
 * @return 新分配的缓冲；`*out_len` 返回长度
 */
static uint8_t *build_pgi(uint16_t w, uint16_t h, uint16_t frame_count, uint8_t fps,
                          const uint8_t (*frame_colors)[3], size_t *out_len)
{
    uint32_t frame_bytes = PGI_PALETTE_BYTES + (uint32_t)w * h;
    uint32_t table_off   = PGI_HEADER_BYTES;
    uint32_t data_off    = table_off + (uint32_t)frame_count * PGI_FRAME_ENTRY_BYTES;
    size_t total         = data_off + (size_t)frame_bytes * frame_count;

    uint8_t *buf = (uint8_t *)calloc(1, total);
    if (buf == NULL) return NULL;

    memcpy(buf, "PGI1", 4);
    wr_u16(buf + 4, w);
    wr_u16(buf + 6, h);
    wr_u16(buf + 8, frame_count);
    buf[10] = fps;
    buf[11] = 0; /* flags：无 RLE */
    wr_u32(buf + 16, table_off);
    wr_u32(buf + 20, data_off);

    for (uint16_t f = 0; f < frame_count; f++) {
        uint32_t off = data_off + (uint32_t)f * frame_bytes;

        /* 帧索引表项 */
        wr_u32(buf + table_off + (uint32_t)f * PGI_FRAME_ENTRY_BYTES, off);
        wr_u32(buf + table_off + (uint32_t)f * PGI_FRAME_ENTRY_BYTES + 4, frame_bytes);

        /* 调色板：基色依次变化，保证各帧不同 */
        for (int i = 0; i < 256; i++) {
            uint8_t *e = buf + off + (size_t)i * 4u;
            e[0] = (uint8_t)((frame_colors ? frame_colors[f][2] : 0) + i); /* B */
            e[1] = (uint8_t)((frame_colors ? frame_colors[f][1] : 0) + i); /* G */
            e[2] = (uint8_t)((frame_colors ? frame_colors[f][0] : 0) + i); /* R */
            e[3] = 0xFF;                                                   /* A */
        }
        /* 索引：第 i 个像素用索引 i%256，保证可预测 */
        uint8_t *idx = buf + off + PGI_PALETTE_BYTES;
        for (uint32_t i = 0; i < (uint32_t)w * h; i++) idx[i] = (uint8_t)(i % 256u);
    }

    *out_len = total;
    return buf;
}

/* ── 1. 文件头校验：合法路径 ───────────────────────────────── */

static void test_header_valid(void)
{
    uint8_t colors[2][3] = { { 10, 20, 30 }, { 40, 50, 60 } };
    size_t len = 0;
    uint8_t *buf = build_pgi(8, 4, 2, 10, colors, &len);
    CHECK(buf != NULL, "构造 pgi 失败");

    pgi_header_t h;
    CHECK(pgi_parse_header(buf, len, &h), "合法文件应解析成功");
    CHECK(h.width == 8 && h.height == 4, "宽高应正确");
    CHECK(h.frame_count == 2, "帧数应正确");
    CHECK(h.fps == 10, "fps 应正确");
    CHECK(h.flags == 0, "flags 应为 0");
    CHECK(h.frame_table_off == PGI_HEADER_BYTES, "帧索引表应在文件头之后");
    CHECK(pgi_is_animated(&h), "2 帧应判定为动图");

    free(buf);
}

/* ── 2. 文件头校验：每一条拒绝路径 ─────────────────────────── */

static void test_header_rejects(void)
{
    uint8_t colors[1][3] = { { 1, 2, 3 } };
    size_t len = 0;
    uint8_t *base = build_pgi(4, 4, 1, 0, colors, &len);
    CHECK(base != NULL, "构造 pgi 失败");

    uint8_t *buf = (uint8_t *)malloc(len);
    pgi_header_t h;

    /* magic 错 */
    memcpy(buf, base, len);
    memcpy(buf, "XGI1", 4);
    CHECK(!pgi_parse_header(buf, len, &h), "magic 错应拒绝");

    /* 缓冲区太短 */
    CHECK(!pgi_parse_header(base, PGI_HEADER_BYTES - 1, &h), "短于文件头应拒绝");

    /* 宽为 0 */
    memcpy(buf, base, len);
    wr_u16(buf + 4, 0);
    CHECK(!pgi_parse_header(buf, len, &h), "宽为 0 应拒绝");

    /* 高为 0 */
    memcpy(buf, base, len);
    wr_u16(buf + 6, 0);
    CHECK(!pgi_parse_header(buf, len, &h), "高为 0 应拒绝");

    /* 尺寸超上限 */
    memcpy(buf, base, len);
    wr_u16(buf + 4, (uint16_t)(PGI_MAX_DIMENSION + 1));
    CHECK(!pgi_parse_header(buf, len, &h), "尺寸超上限应拒绝");

    /* 帧数为 0 */
    memcpy(buf, base, len);
    wr_u16(buf + 8, 0);
    CHECK(!pgi_parse_header(buf, len, &h), "帧数为 0 应拒绝");

    /* 帧数超上限 */
    memcpy(buf, base, len);
    wr_u16(buf + 8, (uint16_t)(PGI_MAX_FRAMES + 1));
    CHECK(!pgi_parse_header(buf, len, &h), "帧数超上限应拒绝");

    /* flags 声明 RLE（v1 不支持）*/
    memcpy(buf, base, len);
    buf[11] = PGI_FLAG_RLE;
    CHECK(!pgi_parse_header(buf, len, &h), "声明 RLE 应拒绝");

    /* fps 过大 */
    memcpy(buf, base, len);
    buf[10] = 61;
    CHECK(!pgi_parse_header(buf, len, &h), "fps>60 应拒绝");

    /* 帧索引表超出文件 */
    memcpy(buf, base, len);
    wr_u32(buf + 16, (uint32_t)len + 100);
    CHECK(!pgi_parse_header(buf, len, &h), "帧索引表越界应拒绝");

    /* 数据区早于文件头 */
    memcpy(buf, base, len);
    wr_u32(buf + 20, PGI_HEADER_BYTES - 1);
    CHECK(!pgi_parse_header(buf, len, &h), "数据区早于文件头应拒绝");

    free(buf);
    free(base);
}

/* ── 3. 帧索引：下标与长度校验 ─────────────────────────────── */

static void test_frame_info(void)
{
    uint8_t colors[3][3] = { { 5, 5, 5 }, { 9, 9, 9 }, { 13, 13, 13 } };
    size_t len = 0;
    uint8_t *buf = build_pgi(6, 5, 3, 12, colors, &len);
    CHECK(buf != NULL, "构造 pgi 失败");

    pgi_header_t h;
    CHECK(pgi_parse_header(buf, len, &h), "应解析成功");

    uint32_t off = 0, size = 0;
    for (int i = 0; i < 3; i++) {
        CHECK(pgi_frame_info(buf, len, &h, i, &off, &size), "合法下标应可取帧");
        CHECK(size == PGI_PALETTE_BYTES + 6u * 5u, "帧长度应为 调色板+像素");
        CHECK(off >= PGI_HEADER_BYTES, "帧偏移应在文件头之后");
    }

    /* 下标越界 */
    CHECK(!pgi_frame_info(buf, len, &h, -1, &off, &size), "负下标应拒绝");
    CHECK(!pgi_frame_info(buf, len, &h, 3, &off, &size), "下标==帧数应拒绝");

    /* 表项声明长度不足（改小第 0 帧的 size）*/
    uint8_t *bad = (uint8_t *)malloc(len);
    memcpy(bad, buf, len);
    wr_u32(bad + h.frame_table_off + 4, PGI_PALETTE_BYTES); /* 比 调色板+像素 小 */
    CHECK(!pgi_frame_info(bad, len, &h, 0, &off, &size), "帧长度不足应拒绝");

    /* 表项偏移越界 */
    memcpy(bad, buf, len);
    wr_u32(bad + h.frame_table_off, (uint32_t)len - 10);
    CHECK(!pgi_frame_info(bad, len, &h, 0, &off, &size), "帧偏移越界应拒绝");

    free(bad);
    free(buf);
}

/* ── 4. **逐像素一致性**（本模块最重要的不变量）───────────── */

static void test_decode_matches_img_indexed(void)
{
    uint8_t colors[2][3] = { { 200, 30, 40 }, { 10, 220, 90 } };
    size_t len = 0;
    uint8_t *buf = build_pgi(16, 9, 2, 8, colors, &len);
    CHECK(buf != NULL, "构造 pgi 失败");

    pgi_header_t h;
    CHECK(pgi_parse_header(buf, len, &h), "应解析成功");

    size_t px = (size_t)h.width * h.height;
    uint16_t *dst = (uint16_t *)malloc(px * sizeof(uint16_t));
    CHECK(dst != NULL, "分配失败");

    for (int f = 0; f < 2; f++) {
        uint32_t off = 0, size = 0;
        CHECK(pgi_frame_info(buf, len, &h, f, &off, &size), "应能取帧");

        /* pgi 解码 */
        memset(dst, 0, px * sizeof(uint16_t));
        CHECK(pgi_decode_frame(buf, len, &h, f, dst, px), "pgi 解码应成功");

        /* 同一份字节直接交给 img_indexed */
        uint16_t *ref = (uint16_t *)calloc(px, sizeof(uint16_t));
        CHECK(img_indexed_decode8(buf + off, (int)h.width, (int)h.height, ref, px),
              "img_indexed 解码应成功");

        int diff = 0;
        for (size_t i = 0; i < px; i++) {
            if (dst[i] != ref[i]) diff++;
        }
        CHECK(diff == 0, "pgi 解码结果必须与 img_indexed 逐像素一致");
        free(ref);
    }

    free(dst);
    free(buf);
}

/* ── 5. 逐帧调色板：不同帧必须解出不同像素 ────────────────── */

static void test_per_frame_palette(void)
{
    /*
     * 两帧基色必须差到"截断后仍不同"：RGB565 里 R/B 各只保留 5 位
     * （v>>3），G 保留 6 位（v>>2）。若两帧只差 1~7，截断后会撞进同一个桶，
     * 于是"像素相同"—— 那是**测试构造**的问题，不是解码器的问题。
     * 差 64 → 右移 3 位后差 8 档，保证永远不同。
     */
    uint8_t colors[2][3] = { { 0, 0, 0 }, { 64, 64, 64 } };
    size_t len = 0;
    uint8_t *buf = build_pgi(8, 8, 2, 10, colors, &len);
    CHECK(buf != NULL, "构造 pgi 失败");

    pgi_header_t h;
    CHECK(pgi_parse_header(buf, len, &h), "应解析成功");

    size_t px = (size_t)h.width * h.height;
    uint16_t *a = (uint16_t *)calloc(px, sizeof(uint16_t));
    uint16_t *b = (uint16_t *)calloc(px, sizeof(uint16_t));
    CHECK(pgi_decode_frame(buf, len, &h, 0, a, px), "帧0 解码应成功");
    CHECK(pgi_decode_frame(buf, len, &h, 1, b, px), "帧1 解码应成功");

    int same = 0;
    for (size_t i = 0; i < px; i++) {
        if (a[i] == b[i]) same++;
    }
    CHECK(same == 0, "两帧调色板不同 → 像素必须全部不同");

    free(a);
    free(b);
    free(buf);
}

/* ── 6. 解码参数保护 ──────────────────────────────────────── */

static void test_decode_guards(void)
{
    uint8_t colors[1][3] = { { 7, 8, 9 } };
    size_t len = 0;
    uint8_t *buf = build_pgi(5, 5, 1, 0, colors, &len);
    CHECK(buf != NULL, "构造 pgi 失败");

    pgi_header_t h;
    CHECK(pgi_parse_header(buf, len, &h), "应解析成功");

    size_t px = 25;
    uint16_t *dst = (uint16_t *)malloc(px * sizeof(uint16_t));

    CHECK(!pgi_decode_frame(buf, len, &h, 0, dst, px - 1), "缓冲不足应拒绝");
    CHECK(!pgi_decode_frame(buf, len, &h, 0, NULL, px), "dst=NULL 应拒绝");
    CHECK(!pgi_decode_frame(buf, len, NULL, 0, dst, px), "h=NULL 应拒绝");
    CHECK(!pgi_decode_frame(buf, len, &h, 1, dst, px), "下标越界应拒绝");

    free(dst);
    free(buf);
}

/* ── 7. 静态/动图判定与整轮时长 ────────────────────────────── */

static void test_cycle_ms(void)
{
    uint8_t colors[25][3];
    memset(colors, 0, sizeof(colors));

    size_t len = 0;
    uint8_t *still = build_pgi(2, 2, 1, 0, colors, &len);
    CHECK(still != NULL, "构造失败");
    pgi_header_t h;
    CHECK(pgi_parse_header(still, len, &h), "单帧应解析成功");
    CHECK(!pgi_is_animated(&h), "单帧不是动图");
    CHECK(pgi_cycle_ms(&h) == 0, "静态的整轮时长应为 0（由轮播间隔决定）");
    free(still);

    uint8_t *anim = build_pgi(2, 2, 24, 12, colors, &len);
    CHECK(anim != NULL, "构造失败");
    CHECK(pgi_parse_header(anim, len, &h), "24 帧应解析成功");
    CHECK(pgi_is_animated(&h), "24 帧是动图");
    /* 24 帧 @12fps = 2000 ms */
    CHECK(pgi_cycle_ms(&h) == 2000, "24 帧@12fps 应为 2000ms");
    free(anim);

    /* 多帧但 fps=0 → 无法播放 */
    uint8_t *bad_fps = build_pgi(2, 2, 3, 0, colors, &len);
    CHECK(pgi_parse_header(bad_fps, len, &h), "fps=0 的多帧文件头本身合法");
    CHECK(pgi_cycle_ms(&h) == 0, "fps=0 的动图时长应为 0（调用方据此跳过）");
    free(bad_fps);
}

/* ── 8. **跨语言夹具**：读 tools/pgi.py 生成的真实文件 ──────────
 *
 * 这一条是"格式定义"的真正校验：Python 生产者与 C 消费者必须对同一份字节
 * 得出同一个结果。夹具 `tests/fixtures/pgi_4color.pgi` 是
 * `tools/pgi.py` 把一张 8×8 四色块 PNG（左上红/右上绿/左下蓝/右下白）转出来的。
 *
 * 期望值（RGB565：R 5 位 / G 6 位 / B 5 位，截断与 img_indexed 一致）：
 *   红 (255,0,0)   → 0xF800
 *   绿 (0,255,0)   → 0x07E0
 *   蓝 (0,0,255)   → 0x001F
 *   白 (255,255,255) → 0xFFFF
 */
static uint8_t *read_file(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long n = ftell(f);
    if (n <= 0) { fclose(f); return NULL; }
    rewind(f);
    uint8_t *buf = (uint8_t *)malloc((size_t)n);
    if (buf == NULL) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    if (got != (size_t)n) { free(buf); return NULL; }
    *out_len = (size_t)n;
    return buf;
}

static void test_tool_fixture(void)
{
    const char *paths[] = {
        "tests/fixtures/pgi_4color.pgi",
        "../tests/fixtures/pgi_4color.pgi",
        "fixtures/pgi_4color.pgi",
    };
    size_t len = 0;
    uint8_t *buf = NULL;
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]) && buf == NULL; i++) {
        buf = read_file(paths[i], &len);
    }
    if (buf == NULL) {
        /* 夹具缺失时明确失败，避免"静默跳过"看起来像通过 */
        CHECK(0, "找不到夹具 tests/fixtures/pgi_4color.pgi（应由 tools/pgi.py 生成）");
        return;
    }
    CHECK(len == 1128, "夹具长度应为 1128（32 头 + 8 表 + 1024 调色板 + 64 像素）");

    pgi_header_t h;
    CHECK(pgi_parse_header(buf, len, &h), "工具生成的 .pgi 应能被解析");
    CHECK(h.width == 8 && h.height == 8, "夹具应为 8×8");
    CHECK(h.frame_count == 1, "夹具是单帧（静态）");
    CHECK(h.fps == 0, "静态文件 fps 应为 0");
    CHECK(h.flags == 0, "工具不产生 RLE");

    uint16_t px[64];
    CHECK(pgi_decode_frame(buf, len, &h, 0, px, 64), "夹具应能解码");

    /* 四个象限各取一个代表点 */
    CHECK(px[0 * 8 + 0] == 0xF800, "左上应为红 0xF800");
    CHECK(px[0 * 8 + 7] == 0x07E0, "右上应为绿 0x07E0");
    CHECK(px[7 * 8 + 0] == 0x001F, "左下应为蓝 0x001F");
    CHECK(px[7 * 8 + 7] == 0xFFFF, "右下应为白 0xFFFF");

    free(buf);
}

/* ── 9. **跨语言夹具（动图）**：3 帧、每帧一种纯色 ──────────────
 *
 * `tests/fixtures/pgi_3frame.pgi` 由 tools/pgi.py 从一张 3 帧 GIF
 * （红 → 绿 → 蓝，100ms/帧）转出。用来验证：
 *   - 多帧索引表偏移正确（每帧各自的调色板都被读到）；
 *   - fps 从帧延时推导正确（100ms → 10fps）；
 *   - 整轮时长 = 3 帧 / 10fps = 300ms。
 */
static void test_tool_fixture_animation(void)
{
    const char *paths[] = {
        "tests/fixtures/pgi_3frame.pgi",
        "../tests/fixtures/pgi_3frame.pgi",
        "fixtures/pgi_3frame.pgi",
    };
    size_t len = 0;
    uint8_t *buf = NULL;
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]) && buf == NULL; i++) {
        buf = read_file(paths[i], &len);
    }
    if (buf == NULL) {
        CHECK(0, "找不到夹具 tests/fixtures/pgi_3frame.pgi");
        return;
    }

    pgi_header_t h;
    CHECK(pgi_parse_header(buf, len, &h), "动图夹具应能解析");
    CHECK(h.frame_count == 3, "夹具应为 3 帧");
    CHECK(h.fps == 10, "100ms/帧 应推出 10fps");
    CHECK(pgi_is_animated(&h), "3 帧应判定为动图");
    CHECK(pgi_cycle_ms(&h) == 300, "3 帧@10fps 整轮应为 300ms");

    uint16_t px[64];
    const uint16_t want[3] = { 0xF800, 0x07E0, 0x001F }; /* 红 绿 蓝 */
    for (int f = 0; f < 3; f++) {
        CHECK(pgi_decode_frame(buf, len, &h, f, px, 64), "每帧都应能解码");
        CHECK(px[0] == want[f], "帧色应依次为 红/绿/蓝");
        CHECK(px[63] == want[f], "整帧应为纯色");
    }

    free(buf);
}

int main(void)
{
    printf("=== test_pgi（.pgi 容器：解析 / 帧索引 / 解码一致性）===\n");

    test_header_valid();
    test_header_rejects();
    test_frame_info();
    test_decode_matches_img_indexed();
    test_per_frame_palette();
    test_decode_guards();
    test_cycle_ms();
    test_tool_fixture();
    test_tool_fixture_animation();

    printf("结果: %d 通过, %d 失败\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
