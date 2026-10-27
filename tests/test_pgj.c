/**
 * test_pgj.c —— `.pgj` 容器的 **纯逻辑** 单测（不需要硬件）
 *
 * 覆盖重点：
 *   1. 标准 CRC-32 的已知向量（`"123456789"` → 0xCBF43926，与 zlib/Swift 一致）；
 *   2. 32 B 头的 build → parse 字节级往返；
 *   3. 头字段的每一条拒绝路径（magic / version / flags / 尺寸 / jpeg_bytes / quality）；
 *   4. JPEG marker 扫描：SOF0 接受、progressive 明确拒绝、其它 SOF 拒绝、截断拒绝；
 *   5. 整文件校验：尺寸自洽、头尺寸与 SOF 尺寸一致、坏载荷；
 *   6. 用**真实 16×16 baseline JPEG 夹具**做端到端校验（不是内存里伪造的段）。
 *
 * 夹具 `tests/fixtures/pgj_16x16.jpg` 的生成办法（macOS，无需 Pillow）：
 *
 *     # 1) 用 python3 + zlib 手写一张 16×16 四色块 PNG（红/绿/蓝/白各 8×8）
 *     python3 - <<'PY'
 *     import zlib, struct
 *     w = h = 16
 *     raw = bytearray()
 *     for y in range(h):
 *         raw.append(0)
 *         for x in range(w):
 *             raw += bytes((255,0,0) if (x<8 and y<8) else (0,255,0) if y<8
 *                          else (0,0,255) if x<8 else (255,255,255))
 *     def chunk(t, d):
 *         return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t+d) & 0xFFFFFFFF)
 *     png = b'\x89PNG\r\n\x1a\n'
 *     png += chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
 *     png += chunk(b'IDAT', zlib.compress(bytes(raw), 9))
 *     png += chunk(b'IEND', b'')
 *     open('/tmp/pgj_16x16.png','wb').write(png)
 *     PY
 *     # 2) 交给系统 sips 转 baseline JPEG（默认即 baseline，SOF0）
 *     sips -s format jpeg /tmp/pgj_16x16.png --out tests/fixtures/pgj_16x16.jpg
 *
 * 夹具已入库（839 B），平时不需要重新生成。
 */
#include "../simulator/ui/pgj.h"

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

/* ── 读夹具（与 test_pgi 相同的多重路径，兼容在 tests/ 或仓库根运行）── */

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

static uint8_t *read_fixture(size_t *out_len)
{
    const char *paths[] = {
        "tests/fixtures/pgj_16x16.jpg",
        "../tests/fixtures/pgj_16x16.jpg",
        "fixtures/pgj_16x16.jpg",
    };
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        uint8_t *buf = read_file(paths[i], out_len);
        if (buf != NULL) return buf;
    }
    return NULL;
}

/** 在 JPEG 里定位指定 marker（返回 marker 字节所在下标；找不到返回 (size_t)-1）。
 *  测试里用它来精确改 SOF0 → 0xC2，避免误改熵数据里同值的字节。 */
static size_t find_marker(const uint8_t *d, size_t len, uint8_t want)
{
    if (len < 2 || d[0] != 0xFF || d[1] != 0xD8) return (size_t)-1;
    size_t pos = 2;
    while (pos < len) {
        if (d[pos] != 0xFF) return (size_t)-1;
        while (pos < len && d[pos] == 0xFF) pos++;
        if (pos >= len) return (size_t)-1;
        uint8_t m = d[pos];
        if (m == want) return pos;
        if (m == 0x01 || (m >= 0xD0 && m <= 0xD7)) { pos++; continue; }
        if (m == 0xD9) return (size_t)-1;
        pos++;
        if (len - pos < 2) return (size_t)-1;
        size_t seg = ((size_t)d[pos] << 8) | d[pos + 1];
        if (seg < 2 || seg > len - pos) return (size_t)-1;
        if (m == 0xDA) return (size_t)-1; /* SOS 之后不再有 marker */
        pos += seg;
    }
    return (size_t)-1;
}

/* ── 构造一个合法的 .pgj（用真实 JPEG 载荷）───────────────── */

static uint8_t *build_pgj(const uint8_t *jpeg, size_t jlen, uint16_t iw, uint16_t ih,
                          uint32_t quality, size_t *out_len, pgj_header_t *out_hdr)
{
    size_t total = PGJ_HEADER_BYTES + jlen;
    uint8_t *buf = (uint8_t *)calloc(1, total);
    if (buf == NULL) return NULL;

    memcpy(buf + PGJ_HEADER_BYTES, jpeg, jlen);

    pgj_header_t h;
    memset(&h, 0, sizeof(h));
    h.width        = iw;
    h.height       = ih;
    h.version      = PGJ_VERSION;
    h.flags        = PGJ_FLAG_BASELINE;
    h.quality      = quality;
    h.jpeg_bytes   = (uint32_t)jlen;
    h.created_unix = 1700000000u;

    /* CRC 覆盖整个文件，而 CRC 字段在文件内 → 两遍构造（先把字段写 0 算，再回填） */
    h.crc32 = 0;
    pgj_build_header(buf, &h);
    h.crc32 = pgj_crc32(buf, total);
    pgj_build_header(buf, &h);

    *out_len = total;
    if (out_hdr != NULL) *out_hdr = h;
    return buf;
}

/** 按"crc 字段视为 0"的口径算整文件 CRC（与 App 侧 PGJEncoder 对齐）。 */
static uint32_t file_crc32_canonical(uint8_t *file, size_t total)
{
    uint8_t save[4];
    memcpy(save, file + 16, 4);
    memset(file + 16, 0, 4);
    uint32_t crc = pgj_crc32(file, total);
    memcpy(file + 16, save, 4);
    return crc;
}

/* ── 1. 标准 CRC-32 已知向量 ───────────────────────────────── */

static void test_crc32(void)
{
    CHECK(pgj_crc32("123456789", 9) == 0xCBF43926u, "CRC32(\"123456789\") 必须等于 0xCBF43926");
    CHECK(pgj_crc32("", 0) == 0u, "空数据 CRC32 应为 0");
    CHECK(pgj_crc32(NULL, 0) == 0u, "NULL + len 0 视作空数据");
    CHECK(pgj_crc32("a", 1) == 0xE8B7BE43u, "CRC32(\"a\") 应为 0xE8B7BE43");
    CHECK(pgj_crc32("abc", 3) == 0x352441C2u, "CRC32(\"abc\") 应为 0x352441C2");
    CHECK(pgj_crc32("The quick brown fox jumps over the lazy dog", 43) == 0x414FA339u,
          "CRC32(fox) 应为 0x414FA339");

    /* 分块计算不可用（接口是一次性）→ 至少验证不同长度/内容不碰撞 */
    CHECK(pgj_crc32("abc", 3) != pgj_crc32("abd", 3), "内容不同 CRC 不同");
    CHECK(pgj_crc32("abc", 2) != pgj_crc32("abc", 3), "长度不同 CRC 不同");
}

/* ── 2. 头 build → parse 往返 ──────────────────────────────── */

static void test_header_roundtrip(void)
{
    pgj_header_t h;
    memset(&h, 0, sizeof(h));
    h.width        = 466;
    h.height       = 466;
    h.version      = PGJ_VERSION;
    h.flags        = PGJ_FLAG_BASELINE;
    h.quality      = 80;
    h.jpeg_bytes   = 45210;
    h.crc32        = 0xDEADBEEFu;
    h.created_unix = 1700000000u;

    uint8_t raw[PGJ_HEADER_BYTES];
    CHECK(pgj_build_header(raw, &h) == PGJ_HEADER_BYTES, "build 应写入 32 字节");
    CHECK(memcmp(raw, "PGJ1", 4) == 0, "magic 应为 PGJ1");
    CHECK(raw[8] == 1, "version 字节 = 1");
    CHECK(raw[9] == 0x01, "flags 字节 = bit0");
    for (int i = 24; i < 32; i++) {
        CHECK(raw[i] == 0, "reserved 必须为 0");
    }

    pgj_header_t got;
    CHECK(pgj_parse_header(raw, sizeof(raw), &got), "build 出来的头应能 parse");
    CHECK(got.width == 466 && got.height == 466, "宽高往返一致");
    CHECK(got.version == PGJ_VERSION, "version 往返一致");
    CHECK(got.flags == PGJ_FLAG_BASELINE, "flags 往返一致");
    CHECK(got.quality == 80, "quality 往返一致");
    CHECK(got.jpeg_bytes == 45210, "jpeg_bytes 往返一致");
    CHECK(got.crc32 == 0xDEADBEEFu, "crc32 往返一致");
    CHECK(got.created_unix == 1700000000u, "created_unix 往返一致");

    /* 边界值 */
    h.width = 1; h.height = PGJ_MAX_DIM;
    h.jpeg_bytes = PGJ_MAX_JPEG_BYTES;
    h.quality = 100;
    pgj_build_header(raw, &h);
    CHECK(pgj_parse_header(raw, sizeof(raw), &got), "边界值头应合法");
    CHECK(got.width == 1 && got.height == PGJ_MAX_DIM, "边界尺寸往返一致");

    /* 参数保护 */
    CHECK(pgj_build_header(NULL, &h) == 0, "out=NULL 应返回 0");
    CHECK(pgj_build_header(raw, NULL) == 0, "h=NULL 应返回 0");
}

/* ── 3. 头字段拒绝路径 ─────────────────────────────────────── */

static void make_good_header(uint8_t raw[PGJ_HEADER_BYTES])
{
    pgj_header_t h;
    memset(&h, 0, sizeof(h));
    h.width        = 16;
    h.height       = 16;
    h.version      = PGJ_VERSION;
    h.flags        = PGJ_FLAG_BASELINE;
    h.quality      = 80;
    h.jpeg_bytes   = 800;
    h.created_unix = 1;
    pgj_build_header(raw, &h);
}

static void test_header_rejects(void)
{
    uint8_t raw[PGJ_HEADER_BYTES];
    pgj_header_t got;

    make_good_header(raw);
    CHECK(pgj_parse_header(raw, sizeof(raw), &got), "基准头应合法");

    CHECK(!pgj_parse_header(NULL, sizeof(raw), &got), "buf=NULL 应拒绝");
    CHECK(!pgj_parse_header(raw, sizeof(raw), NULL), "out=NULL 应拒绝");
    CHECK(!pgj_parse_header(raw, PGJ_HEADER_BYTES - 1, &got), "不足 32 字节应拒绝");

    uint8_t bad[PGJ_HEADER_BYTES];

    memcpy(bad, raw, sizeof(bad));
    bad[0] = 'X';
    CHECK(!pgj_parse_header(bad, sizeof(bad), &got), "magic 错应拒绝");

    memcpy(bad, raw, sizeof(bad));
    bad[8] = 2;
    CHECK(!pgj_parse_header(bad, sizeof(bad), &got), "version != 1 应拒绝");

    memcpy(bad, raw, sizeof(bad));
    bad[8] = 0;
    CHECK(!pgj_parse_header(bad, sizeof(bad), &got), "version = 0 应拒绝");

    memcpy(bad, raw, sizeof(bad));
    bad[9] = 0x00;
    CHECK(!pgj_parse_header(bad, sizeof(bad), &got), "flags bit0=0 应拒绝");

    memcpy(bad, raw, sizeof(bad));
    bad[9] = 0x02;
    CHECK(!pgj_parse_header(bad, sizeof(bad), &got), "flags 缺 bit0 应拒绝");

    memcpy(bad, raw, sizeof(bad));
    bad[9] = 0x03; /* 保留位可以存在（向前兼容），bit0 仍为 1 */
    CHECK(pgj_parse_header(bad, sizeof(bad), &got), "flags 其它位应被忽略");

    /* width 小端：0 / 467 */
    memcpy(bad, raw, sizeof(bad));
    bad[4] = 0; bad[5] = 0;
    CHECK(!pgj_parse_header(bad, sizeof(bad), &got), "width=0 应拒绝");
    memcpy(bad, raw, sizeof(bad));
    bad[4] = (uint8_t)((PGJ_MAX_DIM + 1) & 0xFF);
    bad[5] = (uint8_t)((PGJ_MAX_DIM + 1) >> 8);
    CHECK(!pgj_parse_header(bad, sizeof(bad), &got), "width > 466 应拒绝");

    memcpy(bad, raw, sizeof(bad));
    bad[6] = 0; bad[7] = 0;
    CHECK(!pgj_parse_header(bad, sizeof(bad), &got), "height=0 应拒绝");
    memcpy(bad, raw, sizeof(bad));
    bad[6] = (uint8_t)((PGJ_MAX_DIM + 1) & 0xFF);
    bad[7] = (uint8_t)((PGJ_MAX_DIM + 1) >> 8);
    CHECK(!pgj_parse_header(bad, sizeof(bad), &got), "height > 466 应拒绝");

    /* jpeg_bytes = 0 / 超过上限 */
    memcpy(bad, raw, sizeof(bad));
    memset(bad + 12, 0, 4);
    CHECK(!pgj_parse_header(bad, sizeof(bad), &got), "jpeg_bytes=0 应拒绝");
    memcpy(bad, raw, sizeof(bad));
    bad[12] = 0x01; bad[13] = 0x00; bad[14] = 0x08; bad[15] = 0x00; /* 512K+1 */
    CHECK(!pgj_parse_header(bad, sizeof(bad), &got), "jpeg_bytes > 512K 应拒绝");

    /* quality = 0 / 101 */
    memcpy(bad, raw, sizeof(bad));
    bad[10] = 0; bad[11] = 0;
    CHECK(!pgj_parse_header(bad, sizeof(bad), &got), "quality=0 应拒绝");
    memcpy(bad, raw, sizeof(bad));
    bad[10] = 101; bad[11] = 0;
    CHECK(!pgj_parse_header(bad, sizeof(bad), &got), "quality=101 应拒绝");
    memcpy(bad, raw, sizeof(bad));
    bad[10] = 1; bad[11] = 0;
    CHECK(pgj_parse_header(bad, sizeof(bad), &got), "quality=1 应接受（头注释 1..100）");

    /* reserved 非 0 不校验（留给后续版本） */
    memcpy(bad, raw, sizeof(bad));
    memset(bad + 24, 0xAA, 8);
    CHECK(pgj_parse_header(bad, sizeof(bad), &got), "reserved 非 0 应被忽略");
}

/* ── 4. JPEG SOF 扫描（真实夹具）──────────────────────────── */

static void test_jpeg_info_fixture(void)
{
    size_t jlen = 0;
    uint8_t *jpg = read_fixture(&jlen);
    if (jpg == NULL) {
        CHECK(0, "找不到夹具 tests/fixtures/pgj_16x16.jpg");
        return;
    }
    CHECK(jlen > 100 && jlen < 4096, "夹具应是紧凑的小 JPEG");

    uint16_t w = 0, h = 0;
    const char *err = "sentinel";
    CHECK(pgj_jpeg_info(jpg, jlen, &w, &h, &err), "真实 baseline JPEG 应能读出 SOF");
    CHECK(w == 16 && h == 16, "夹具应为 16×16");
    CHECK(err == NULL, "成功时 err 应置 NULL");

    /* 允许 w/h/err 传 NULL */
    CHECK(pgj_jpeg_info(jpg, jlen, NULL, NULL, NULL), "out 参数可为 NULL");
    CHECK(!pgj_jpeg_info(NULL, jlen, &w, &h, &err), "jpeg=NULL 应拒绝");

    /* 改 SOF 里的宽高 → 应读出改后的值（证明读的是 SOF 而不是猜的） */
    uint8_t *copy = (uint8_t *)malloc(jlen);
    if (copy != NULL) {
        memcpy(copy, jpg, jlen);
        size_t sof = find_marker(copy, jlen, 0xC0);
        CHECK(sof != (size_t)-1, "夹具里应能找到 SOF0");
        if (sof != (size_t)-1) {
            /* marker(1) + 段长(2) + 精度(1) → 高在 +4/+5，宽在 +6/+7（大端） */
            copy[sof + 4] = 0; copy[sof + 5] = 20;
            copy[sof + 6] = 0; copy[sof + 7] = 24;
            CHECK(pgj_jpeg_info(copy, jlen, &w, &h, NULL), "改宽高后仍应能解析");
            CHECK(w == 24 && h == 20, "应读出 SOF 里的 24×20");
        }
        free(copy);
    }

    free(jpg);
}

static void test_jpeg_info_errors(void)
{
    size_t jlen = 0;
    uint8_t *jpg = read_fixture(&jlen);
    if (jpg == NULL) {
        CHECK(0, "找不到夹具 tests/fixtures/pgj_16x16.jpg");
        return;
    }
    uint16_t w, h;
    const char *err = NULL;
    size_t sof2 = 0;

    uint8_t *copy = (uint8_t *)malloc(jlen);
    if (copy == NULL) { free(jpg); return; }

    /* 不是 JPEG：空 / 只有 SOI / 首位不是 FF */
    CHECK(!pgj_jpeg_info(jpg, 0, &w, &h, &err), "长度 0 应拒绝");
    CHECK(err != NULL && strcmp(err, "bad_jpeg") == 0, "长度 0 的 err 应为 bad_jpeg");
    CHECK(!pgj_jpeg_info(jpg, 1, &w, &h, &err), "只有 1 字节应拒绝");
    memcpy(copy, jpg, jlen);
    copy[0] = 0x00;
    CHECK(!pgj_jpeg_info(copy, jlen, &w, &h, &err), "缺 SOI 应拒绝");

    /* 截断到 SOF 之前：不断缩短长度，必须一直拒绝（或至少不能返回尺寸） */
    size_t sof = find_marker(jpg, jlen, 0xC0);
    CHECK(sof != (size_t)-1, "夹具里应能找到 SOF0");
    if (sof != (size_t)-1) {
        CHECK(!pgj_jpeg_info(jpg, sof, &w, &h, &err), "截断到 SOF 之前应拒绝");
        CHECK(err != NULL && strcmp(err, "bad_jpeg") == 0, "截断 err 应为 bad_jpeg");
        CHECK(!pgj_jpeg_info(jpg, sof + 1, &w, &h, &err), "截断在 marker 里应拒绝");
        CHECK(!pgj_jpeg_info(jpg, sof + 5, &w, &h, &err), "截断在 SOF 段内应拒绝");
    }

    /* progressive：把 SOF0 marker 改成 0xC2 */
    memcpy(copy, jpg, jlen);
    sof2 = find_marker(copy, jlen, 0xC0);
    copy[sof2] = 0xC2;
    CHECK(!pgj_jpeg_info(copy, jlen, &w, &h, &err), "progressive 应拒绝");
    CHECK(err != NULL && strcmp(err, "progressive") == 0, "progressive 的 err 应为 progressive");

    /* 其它 SOF：0xC1（extended sequential）、0xC3（lossless） */
    memcpy(copy, jpg, jlen);
    sof2 = find_marker(copy, jlen, 0xC0);
    copy[sof2] = 0xC1;
    CHECK(!pgj_jpeg_info(copy, jlen, &w, &h, &err), "SOF1 应拒绝");
    CHECK(err != NULL && strcmp(err, "unsupported_sof") == 0, "SOF1 的 err 应为 unsupported_sof");

    memcpy(copy, jpg, jlen);
    sof2 = find_marker(copy, jlen, 0xC0);
    copy[sof2] = 0xC3;
    CHECK(!pgj_jpeg_info(copy, jlen, &w, &h, &err), "SOF3 应拒绝");

    /* 段长度被改爆 → bad_jpeg */
    memcpy(copy, jpg, jlen);
    sof2 = find_marker(copy, jlen, 0xC0);
    copy[sof2 + 1] = 0xFF; /* 段长高字节 = 0xFF → 越界 */
    CHECK(!pgj_jpeg_info(copy, jlen, &w, &h, &err), "段长越界应拒绝");
    CHECK(err != NULL && strcmp(err, "bad_jpeg") == 0, "段长越界 err 应为 bad_jpeg");

    /* 组件数为 0 → bad_jpeg */
    memcpy(copy, jpg, jlen);
    sof2 = find_marker(copy, jlen, 0xC0);
    /* marker(1) len(2) 段体: prec(+3) h(+4,+5) w(+6,+7) ncomp(+8) */
    copy[sof2 + 8] = 0;
    CHECK(!pgj_jpeg_info(copy, jlen, &w, &h, &err), "分量数 0 应拒绝");

    /* 遇到 SOS 之前没有 SOF：把 SOF 整段"变成"DHT（0xC4），应扫不到 SOF */
    memcpy(copy, jpg, jlen);
    sof2 = find_marker(copy, jlen, 0xC0);
    copy[sof2] = 0xC4;
    CHECK(!pgj_jpeg_info(copy, jlen, &w, &h, &err), "没有 SOF 应拒绝");

    free(copy);
    free(jpg);
}

/* ── 5. 整文件校验 ─────────────────────────────────────────── */

static void test_validate_file(void)
{
    size_t jlen = 0;
    uint8_t *jpg = read_fixture(&jlen);
    if (jpg == NULL) {
        CHECK(0, "找不到夹具 tests/fixtures/pgj_16x16.jpg");
        return;
    }

    size_t total = 0;
    pgj_header_t h;
    uint8_t *file = build_pgj(jpg, jlen, 16, 16, 80, &total, &h);
    if (file == NULL) { free(jpg); return; }

    pgj_header_t got;
    const char *err = "sentinel";
    CHECK(pgj_validate_file(file, total, &got, &err), "合法 .pgj 应通过整文件校验");
    CHECK(err == NULL, "成功时 err 应置 NULL");
    CHECK(got.width == 16 && got.height == 16, "校验输出宽高正确");
    CHECK(got.jpeg_bytes == (uint32_t)jlen, "jpeg_bytes 正确");
    CHECK(got.crc32 == file_crc32_canonical(file, total),
          "头里 CRC32 == 整文件 CRC32（crc 字段置 0 后计算，跨语言口径）");
    CHECK(got.crc32 != pgj_crc32(file, total),
          "不置 0 直接算会不同：证明口径就是\"crc 字段视为 0\"");
    CHECK(got.crc32 != 0, "夹具 CRC 不应碰巧为 0");

    /* out/err 可为 NULL */
    CHECK(pgj_validate_file(file, total, NULL, NULL), "out/err 可为 NULL");
    CHECK(!pgj_validate_file(NULL, total, &got, &err), "buf=NULL 应拒绝");
    CHECK(err != NULL && strcmp(err, "bad_arg") == 0, "NULL buf err 应为 bad_arg");

    /* 头坏了 */
    uint8_t *bad = (uint8_t *)malloc(total);
    if (bad == NULL) { free(file); free(jpg); return; }

    memcpy(bad, file, total);
    bad[0] = 'X';
    CHECK(!pgj_validate_file(bad, total, &got, &err), "magic 坏应拒绝");
    CHECK(err != NULL && strcmp(err, "bad_header") == 0, "头坏 err 应为 bad_header");

    /* 长度与 jpeg_bytes 不自洽 */
    CHECK(!pgj_validate_file(file, total - 1, &got, &err), "少 1 字节应拒绝");
    CHECK(err != NULL && strcmp(err, "bad_size") == 0, "长度不符 err 应为 bad_size");
    uint8_t *bigger = (uint8_t *)malloc(total + 1);
    if (bigger != NULL) {
        memcpy(bigger, file, total);
        bigger[total] = 0;
        CHECK(!pgj_validate_file(bigger, total + 1, &got, &err), "多 1 字节应拒绝");
        CHECK(err != NULL && strcmp(err, "bad_size") == 0, "长度不符 err 应为 bad_size");
        free(bigger);
    }

    /* 头尺寸与 SOF 尺寸不一致：改头里的 width（并重算 CRC 让它"看起来"合法） */
    memcpy(bad, file, total);
    bad[4] = 32; bad[5] = 0;
    {
        pgj_header_t hh;
        CHECK(pgj_parse_header(bad, PGJ_HEADER_BYTES, &hh), "改尺寸后头本身仍合法");
        hh.crc32 = 0;
        pgj_build_header(bad, &hh);
        hh.crc32 = file_crc32_canonical(bad, total);
        pgj_build_header(bad, &hh);
    }
    CHECK(!pgj_validate_file(bad, total, &got, &err), "头尺寸与 SOF 不一致应拒绝");
    CHECK(err != NULL && strcmp(err, "bad_dim") == 0, "尺寸不一致 err 应为 bad_dim");

    /* 载荷是 progressive */
    memcpy(bad, file, total);
    bad[PGJ_HEADER_BYTES + find_marker(bad + PGJ_HEADER_BYTES, jlen, 0xC0)] = 0xC2;
    CHECK(!pgj_validate_file(bad, total, &got, &err), "progressive 载荷应拒绝");
    CHECK(err != NULL && strcmp(err, "progressive") == 0, "progressive err 应透传");

    /* 载荷 SOI 坏了 */
    memcpy(bad, file, total);
    bad[PGJ_HEADER_BYTES] = 0;
    CHECK(!pgj_validate_file(bad, total, &got, &err), "坏 JPEG 载荷应拒绝");
    CHECK(err != NULL && strcmp(err, "bad_jpeg") == 0, "坏载荷 err 应为 bad_jpeg");

    /* 头声明 jpeg_bytes 合法、文件长度也匹配，但删掉载荷中段（结构坏） */
    memcpy(bad, file, total);
    bad[PGJ_HEADER_BYTES + 2] = 0x00; /* APP0 marker 字节被破坏 */
    CHECK(!pgj_validate_file(bad, total, &got, &err), "坏 marker 结构应拒绝");

    free(bad);
    free(file);
    free(jpg);
}

/* ── 6. 夹具端到端：真实 JPEG + 头 + CRC ───────────────────── */

static void test_fixture_end_to_end(void)
{
    size_t jlen = 0;
    uint8_t *jpg = read_fixture(&jlen);
    if (jpg == NULL) {
        CHECK(0, "找不到夹具 tests/fixtures/pgj_16x16.jpg");
        return;
    }
    CHECK(jlen == 839, "夹具应为 839 字节（sips 输出，已入库；改动需重新生成）");

    uint16_t w = 0, h = 0;
    CHECK(pgj_jpeg_info(jpg, jlen, &w, &h, NULL), "夹具 SOF 可读");
    CHECK(w == 16 && h == 16, "夹具尺寸 16×16");

    /* 用 SOF 的实际尺寸构造 .pgj，模拟 App 编码产物 */
    size_t total = 0;
    uint8_t *file = build_pgj(jpg, jlen, w, h, 88, &total, NULL);
    CHECK(file != NULL, "应能构造 .pgj");
    if (file != NULL) {
        CHECK(total == PGJ_HEADER_BYTES + jlen, "文件长度 = 32 + JPEG");
        CHECK(memcmp(file, "PGJ1", 4) == 0, "文件以 PGJ1 magic 开头");
        pgj_header_t got;
        const char *err = NULL;
        CHECK(pgj_validate_file(file, total, &got, &err), "构造的 .pgj 应通过校验");
        CHECK(got.quality == 88, "quality 记录正确");
        free(file);
    }
    free(jpg);
}

/* ── 7. 确定性变异/截断：任何坏字节都不得越界或崩溃 ────────────
 *
 * 这是"扫描阶段挡坏文件"的最后一道保险：设备读到的可能是拔卡中断/写坏的文件。
 * 不要求变异后的结果一定是 false（有些字节改完仍是合法 JPEG），只要求：
 * 不越界、不崩溃、返回值有意义。
 */
static void test_mutation_robustness(void)
{
    size_t jlen = 0;
    uint8_t *jpg = read_fixture(&jlen);
    if (jpg == NULL) {
        CHECK(0, "找不到夹具 tests/fixtures/pgj_16x16.jpg");
        return;
    }

    /* 1) JPEG 单字节变异：覆盖 SOI/APP/DQT/SOF/SOS 各段 */
    static const uint8_t vals[] = { 0x00, 0xFF, 0xC0, 0xC2, 0xDA, 0xD9, 0x80 };
    int survived = 0;
    for (size_t pos = 0; pos < jlen; pos++) {
        uint8_t save = jpg[pos];
        for (size_t v = 0; v < sizeof(vals); v++) {
            jpg[pos] = vals[v];
            uint16_t w = 0, h = 0;
            const char *err = NULL;
            (void)pgj_jpeg_info(jpg, jlen, &w, &h, &err);
            survived++;
        }
        jpg[pos] = save;
    }
    CHECK(survived == (int)(jlen * sizeof(vals)), "JPEG 全字节变异均应安全返回");

    /* 2) 完整文件按每个前缀长度截断：jpeg_info / validate_file 都不得越界 */
    size_t total = 0;
    uint8_t *file = build_pgj(jpg, jlen, 16, 16, 80, &total, NULL);
    if (file == NULL) { free(jpg); return; }
    int prefix_calls = 0;
    for (size_t n = 0; n <= total; n++) {
        uint16_t w = 0, h = 0;
        const char *err = NULL;
        pgj_header_t hdr;
        (void)pgj_jpeg_info(file, n > PGJ_HEADER_BYTES ? n - PGJ_HEADER_BYTES : 0, &w, &h, &err);
        (void)pgj_parse_header(file, n, &hdr);
        (void)pgj_validate_file(file, n, &hdr, &err);
        prefix_calls += 3;
    }
    CHECK(prefix_calls > 0, "所有截断前缀均应安全返回");

    /* 3) 32 B 头逐字节变异：扫描阶段（只看头）不得越界 */
    uint8_t hdr_raw[PGJ_HEADER_BYTES];
    static const uint8_t hdr_vals[] = { 0x00, 0xFF, 0xAA, 0x50 };
    int hdr_calls = 0;
    for (size_t pos = 0; pos < PGJ_HEADER_BYTES; pos++) {
        for (size_t v = 0; v < sizeof(hdr_vals); v++) {
            memcpy(hdr_raw, file, sizeof(hdr_raw));
            hdr_raw[pos] = hdr_vals[v];
            pgj_header_t h;
            (void)pgj_parse_header(hdr_raw, sizeof(hdr_raw), &h);
            (void)pgj_validate_file(file, total, &h, NULL); /* 完整文件仍应安全 */
            hdr_calls += 2;
        }
    }
    CHECK(hdr_calls == (int)(PGJ_HEADER_BYTES * sizeof(hdr_vals) * 2), "头字节变异应安全返回");

    free(file);
    free(jpg);
}

int main(void)
{
    printf("=== test_pgj（.pgj 容器：32B 头 / CRC32 / baseline SOF / 整文件校验）===\n");

    test_crc32();
    test_header_roundtrip();
    test_header_rejects();
    test_jpeg_info_fixture();
    test_jpeg_info_errors();
    test_validate_file();
    test_fixture_end_to_end();
    test_mutation_robustness();

    printf("结果: %d 通过, %d 失败\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
