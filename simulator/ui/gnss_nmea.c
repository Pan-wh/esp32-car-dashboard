/**
 * gnss_nmea.c —— NMEA 0183 解析实现
 *
 * 只做「字节流 → 结构化数据」，不依赖 LVGL / 硬件，便于单元测试与真机复用。
 */
#include "gnss_nmea.h"

#include <stdlib.h>
#include <string.h>

#define FIELD_MAX 24 /* 最多切分的字段数（GSA 字段较多） */
#define KNOT_TO_KMH 1.852f

/* ---------------- 基础工具 ---------------- */

static void reset_data(gnss_data_t *d)
{
    memset(d, 0, sizeof(*d));
    d->fix = GNSS_FIX_NONE;
}

void gnss_init(gnss_ctx_t *ctx)
{
    if (ctx == NULL) return;
    memset(ctx, 0, sizeof(*ctx));
    reset_data(&ctx->data);
}

void gnss_set_time(gnss_ctx_t *ctx, uint32_t now_ms)
{
    if (ctx == NULL) return;
    ctx->now_ms = now_ms;
}

/* 解析 * 后的两位十六进制校验和；返回 -1 表示格式错误 */
static int parse_checksum(const char *p)
{
    int v = 0;
    for (int i = 0; i < 2; i++) {
        char c = p[i];
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else return -1;
        v = (v << 4) | d;
    }
    return v;
}

/*
 * 校验并取出语句主体（$ 与 * 之间）。
 * 输入 line 可为 "$GNRMC,...*hh" 形式（允许没有 $ 前缀）；
 * body 为输出缓冲（调用方提供，长度 >= GNSS_LINE_MAX）。
 * 返回 true 表示校验通过。
 */
static bool extract_body(const char *line, char *body, size_t body_sz)
{
    if (line == NULL || body == NULL) return false;

    const char *start = line;
    if (*start == '$' || *start == '!') start++; /* ! 为 AIS，这里容错处理 */

    const char *star = strchr(start, '*');
    if (star == NULL) {
        /* 无校验和字段：仍接受（部分设备可配），但不计入校验错误 */
        size_t n = strlen(start);
        if (n == 0 || n >= body_sz) return false;
        memcpy(body, start, n + 1);
        /* 去掉行尾 CR/LF */
        for (size_t i = 0; i < n; i++) {
            if (body[i] == '\r' || body[i] == '\n') { body[i] = '\0'; break; }
        }
        return true;
    }

    int want = parse_checksum(star + 1);
    if (want < 0) return false;

    uint8_t cs = 0;
    for (const char *p = start; p < star; p++) cs ^= (uint8_t)(*p);
    if (cs != (uint8_t)want) return false;

    size_t n = (size_t)(star - start);
    if (n == 0 || n >= body_sz) return false;
    memcpy(body, start, n);
    body[n] = '\0';
    return true;
}

/* 按 ',' 切分（就地修改 body）；返回字段数 */
static int split_fields(char *body, char **fields, int max_fields)
{
    int n = 0;
    char *p = body;
    fields[n++] = p;
    while (*p != '\0' && n < max_fields) {
        if (*p == ',') {
            *p = '\0';
            fields[n++] = p + 1;
        }
        p++;
    }
    return n;
}

static bool get_float(const char *s, float *out)
{
    if (s == NULL || *s == '\0') return false;
    char *end = NULL;
    float v = strtof(s, &end);
    if (end == s) return false;
    *out = v;
    return true;
}

static bool get_int(const char *s, int *out)
{
    if (s == NULL || *s == '\0') return false;
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (end == s) return false;
    *out = (int)v;
    return true;
}

/* "hhmmss.ss" → 时/分/秒 */
static void parse_utc(const char *s, gnss_data_t *d)
{
    if (s == NULL || strlen(s) < 6) return;
    int h = (s[0] - '0') * 10 + (s[1] - '0');
    int m = (s[2] - '0') * 10 + (s[3] - '0');
    int sec = (s[4] - '0') * 10 + (s[5] - '0');
    if (h >= 0 && h < 24) d->utc_h = h;
    if (m >= 0 && m < 60) d->utc_m = m;
    if (sec >= 0 && sec < 61) d->utc_s = sec;
}

/* "ddmmyy" → 日/月/年(20xx) */
static void parse_date(const char *s, gnss_data_t *d)
{
    if (s == NULL || strlen(s) < 6) return;
    int dd = (s[0] - '0') * 10 + (s[1] - '0');
    int mm = (s[2] - '0') * 10 + (s[3] - '0');
    int yy = (s[4] - '0') * 10 + (s[5] - '0');
    if (dd >= 1 && dd <= 31) d->day = dd;
    if (mm >= 1 && mm <= 12) d->month = mm;
    /*
     * NMEA 只有两位年份，存在世纪歧义。按业界惯例处理：
     * yy >= 80 视为 19xx（GPS 时代自 1980 起），否则 20xx。
     * 例：191194 → 1994-11-19；010120 → 2020-01-01。
     */
    d->year = (yy >= 80) ? (1900 + yy) : (2000 + yy);
}

/* 语句类型 = 地址后 3 位（兼容 GP/GN/GL/GA/GB talker） */
static bool type_is(const char *addr, const char *type3)
{
    size_t n = strlen(addr);
    if (n < 3) return false;
    return strcmp(addr + n - 3, type3) == 0;
}

/* ---------------- 各语句解析 ---------------- */

static void parse_rmc(gnss_ctx_t *ctx, char **f, int n)
{
    gnss_data_t *d = &ctx->data;

    parse_utc(f[1], d);

    /* 字段 2：A = 有效，V = 无效（未定位） */
    bool ok = (f[2] != NULL && f[2][0] == 'A');
    d->valid = ok;
    ctx->has_rmc = true;

    float knots = 0.0f, course = 0.0f;
    if (get_float(f[7], &knots)) d->speed_kmh = knots * KNOT_TO_KMH;
    if (get_float(f[8], &course)) d->course_deg = course;

    if (n > 9) parse_date(f[9], d);
}

static void parse_gga(gnss_ctx_t *ctx, char **f, int n)
{
    gnss_data_t *d = &ctx->data;

    parse_utc(f[1], d);

    int q = 0;
    if (get_int(f[6], &q)) d->gga_quality = q;

    int sats = 0;
    if (get_int(f[7], &sats)) d->sats_used = sats;

    float v = 0.0f;
    if (get_float(f[8], &v)) d->hdop = v;
    if (get_float(f[9], &v)) d->altitude_m = v;

    /* 没有 RMC 时，用 GGA 质量判断定位有效性 */
    if (!ctx->has_rmc) d->valid = (d->gga_quality > 0);
    ctx->has_gga = true;
    (void)n;
}

static void parse_gsa(gnss_ctx_t *ctx, char **f, int n)
{
    gnss_data_t *d = &ctx->data;

    int t = 0;
    if (get_int(f[2], &t)) {
        if (t == 3) d->fix = GNSS_FIX_3D;
        else if (t == 2) d->fix = GNSS_FIX_2D;
        else d->fix = GNSS_FIX_NONE;
    }

    float v = 0.0f;
    if (n > 16 && get_float(f[16], &v)) d->hdop = v;
    (void)n;
}

static void parse_vtg(gnss_ctx_t *ctx, char **f, int n)
{
    gnss_data_t *d = &ctx->data;

    float v = 0.0f;
    if (get_float(f[1], &v)) d->course_deg = v;         /* 真航向 */
    if (n > 7 && get_float(f[7], &v)) d->speed_kmh = v; /* km/h 字段 */
    else if (get_float(f[5], &v)) d->speed_kmh = v * KNOT_TO_KMH; /* 兜底：节 */
}

static void parse_gsv(gnss_ctx_t *ctx, char **f, int n)
{
    int view = 0;
    if (n > 3 && get_int(f[3], &view)) {
        if (view > ctx->data.sats_view) ctx->data.sats_view = view;
    }
}

/* ---------------- 对外接口 ---------------- */

bool gnss_parse_line(gnss_ctx_t *ctx, const char *line)
{
    if (ctx == NULL || line == NULL) return false;

    char body[GNSS_LINE_MAX];
    if (!extract_body(line, body, sizeof(body))) {
        /* 有 '*' 但校验失败 → 计入错误；无 '*' 且为空则忽略 */
        if (strchr(line, '*') != NULL) ctx->checksum_err++;
        return false;
    }

    char *f[FIELD_MAX];
    int n = split_fields(body, f, FIELD_MAX);
    if (n < 1 || strlen(f[0]) < 3) return false;

    const char *addr = f[0];
    bool handled = false;

    if (type_is(addr, "RMC") && n > 8) {
        parse_rmc(ctx, f, n);
        handled = true;
    }
    else if (type_is(addr, "GGA") && n > 9) {
        parse_gga(ctx, f, n);
        handled = true;
    }
    else if (type_is(addr, "GSA") && n > 2) {
        parse_gsa(ctx, f, n);
        handled = true;
    }
    else if (type_is(addr, "VTG") && n > 7) {
        parse_vtg(ctx, f, n);
        handled = true;
    }
    else if (type_is(addr, "GSV") && n > 3) {
        parse_gsv(ctx, f, n);
        handled = true;
    }

    if (handled) {
        ctx->sentences++;
        ctx->data.last_rx_ms = ctx->now_ms;
    }
    else {
        ctx->unknown++;
    }
    return handled;
}

void gnss_feed(gnss_ctx_t *ctx, const char *buf, size_t len)
{
    if (ctx == NULL || buf == NULL) return;

    for (size_t i = 0; i < len; i++) {
        char c = buf[i];

        if (c == '\n') {
            if (ctx->line_len > 0) {
                ctx->line[ctx->line_len] = '\0';
                gnss_parse_line(ctx, ctx->line);
                ctx->line_len = 0;
            }
            continue;
        }

        /* 新语句开始：丢弃前面未结束的残行 */
        if (c == '$') {
            if (ctx->line_len > 0) ctx->overflow++; /* 残行说明数据不完整 */
            ctx->line_len = 0;
        }

        if (c == '\r') continue;

        if (ctx->line_len + 1 < sizeof(ctx->line)) {
            ctx->line[ctx->line_len++] = c;
        }
        else {
            ctx->line_len = 0;
            ctx->overflow++;
        }
    }
}

uint32_t gnss_age_ms(const gnss_ctx_t *ctx)
{
    if (ctx == NULL || ctx->sentences == 0) return UINT32_MAX;
    return ctx->now_ms - ctx->data.last_rx_ms;
}

bool gnss_is_usable(const gnss_ctx_t *ctx)
{
    if (ctx == NULL) return false;
    if (!ctx->data.valid) return false;
    return gnss_age_ms(ctx) <= GNSS_STALE_MS;
}
