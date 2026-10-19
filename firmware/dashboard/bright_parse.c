/**
 * bright_parse.c —— 亮度曲线包解析实现（纯 C，可 PC 单测）
 *
 * 复用与 telemetry.c / nav_parse.c / control.c 相同的"手写轻量解析"风格：
 * 逐字符扫描、无动态分配、对畸形输入不崩溃。
 *
 * ⚠️ 有意不做的：不做嵌套对象（协议是扁平结构）。
 *    nav_parse.c 里的工具函数是 static，无法跨文件复用 —— 这里按同样的
 *    风格自备一份（代码量很小，比为了复用去改 nav_parse 的可见性风险低）。
 */
#include "bright_parse.h"

#include <string.h>

/* ---------------- 基础扫描工具 ---------------- */

static void skip_ws(const char *s, size_t len, size_t *i)
{
    while (*i < len && (s[*i] == ' ' || s[*i] == '\t' || s[*i] == '\n' || s[*i] == '\r')) {
        (*i)++;
    }
}

/** 解析 JSON 字符串字面量（含转义），内容拷进 out（可为 NULL = 只跳过）*/
static bool scan_string(const char *s, size_t len, size_t *i, char *out, size_t out_sz)
{
    if (*i >= len || s[*i] != '"') return false;
    (*i)++;

    size_t n = 0;
    while (*i < len) {
        char c = s[*i];
        if (c == '\\') {
            (*i)++;
            if (*i >= len) return false;
            c = s[*i];
        }
        else if (c == '"') {
            (*i)++;
            if (out != NULL && out_sz > 0) out[n < out_sz ? n : out_sz - 1] = '\0';
            return true;
        }
        if (out != NULL && out_sz > 0 && n < out_sz - 1) out[n++] = c;
        (*i)++;
    }
    return false; /* 未闭合 */
}

/** 解析整数（支持负号）*/
static bool scan_int(const char *s, size_t len, size_t *i, long *out)
{
    const size_t start = *i;
    bool neg = false;
    if (*i < len && (s[*i] == '-' || s[*i] == '+')) {
        neg = (s[*i] == '-');
        (*i)++;
    }
    const size_t digits = *i;
    long v = 0;
    while (*i < len && s[*i] >= '0' && s[*i] <= '9') {
        v = v * 10 + (s[*i] - '0');
        if (v > 2000000000L) v = 2000000000L; /* 防溢出，钳制即可 */
        (*i)++;
    }
    if (*i == digits) { /* 没有数字 */
        *i = start;
        return false;
    }
    *out = neg ? -v : v;
    return true;
}

/** 跳过任意 JSON 值（数字/字符串/true/false/null）—— 忽略不关心的字段 */
static bool skip_value(const char *s, size_t len, size_t *i)
{
    skip_ws(s, len, i);
    if (*i >= len) return false;

    if (s[*i] == '"') return scan_string(s, len, i, NULL, 0);
    if (s[*i] == '{' || s[*i] == '[') {
        const char open = s[*i];
        const char close = (open == '{') ? '}' : ']';
        int depth = 0;
        while (*i < len) {
            if (s[*i] == open) {
                depth++;
            }
            else if (s[*i] == close) {
                depth--;
                (*i)++;
                if (depth == 0) return true;
                continue;
            }
            (*i)++;
        }
        return false;
    }
    /* 数字 / true / false / null：扫到分隔符为止 */
    while (*i < len && s[*i] != ',' && s[*i] != '}' && s[*i] != ' ' && s[*i] != '\t') {
        (*i)++;
    }
    return true;
}

/* ---------------- 消息类型判定 ---------------- */

bool bright_is_bright_message(const char *json, size_t len)
{
    if (json == NULL || len == 0) return false;

    /* 浅扫描：找 "t" 键且值为 "bright"。遥测包很密，这里要尽量便宜。 */
    char key[16];
    char val[16];
    size_t i = 0;

    skip_ws(json, len, &i);
    if (i >= len || json[i] != '{') return false;
    i++;

    for (;;) {
        skip_ws(json, len, &i);
        if (i >= len) return false;
        if (json[i] == '}') return false; /* 扫完没找到 */

        if (!scan_string(json, len, &i, key, sizeof(key))) return false;
        skip_ws(json, len, &i);
        if (i >= len || json[i] != ':') return false;
        i++;
        skip_ws(json, len, &i);

        if (i < len && json[i] == '"') {
            if (!scan_string(json, len, &i, val, sizeof(val))) return false;
            if (strcmp(key, "t") == 0) {
                return strcmp(val, "bright") == 0; /* 命中类型标记即决定 */
            }
        }
        else {
            if (!skip_value(json, len, &i)) return false;
        }

        skip_ws(json, len, &i);
        if (i < len && json[i] == ',') {
            i++;
            continue;
        }
        return false; /* `}` 或畸形 → 没找到 t */
    }
}

/* ---------------- 主解析 ---------------- */

bool bright_parse(const char *json, size_t len, bright_packet_t *out)
{
    if (json == NULL || len == 0 || out == NULL) return false;

    /*
     * 先在本地结构上解析，**成功后才写回 out** ——
     * 半解析的结果（比如 t 对了但后面畸形）不该污染调用方的配置。
     */
    bright_packet_t pkt;
    memset(&pkt, 0, sizeof(pkt)); /* 所有 has_* = 0 = 未提供 */

    size_t i = 0;
    skip_ws(json, len, &i);
    if (i >= len || json[i] != '{') return false;
    i++;

    bool saw_type = false;

    for (;;) {
        skip_ws(json, len, &i);
        if (i >= len) return false;
        if (json[i] == '}') {
            i++;
            break; /* 正常收尾 */
        }

        char key[16];
        if (!scan_string(json, len, &i, key, sizeof(key))) return false;
        skip_ws(json, len, &i);
        if (i >= len || json[i] != ':') return false;
        i++;
        skip_ws(json, len, &i);

        if (strcmp(key, "t") == 0) {
            char val[16];
            if (i >= len || json[i] != '"') return false;
            if (!scan_string(json, len, &i, val, sizeof(val))) return false;
            if (strcmp(val, "bright") != 0) return false; /* 不是亮度包 */
            saw_type = true;
        }
        else if (strcmp(key, "on") == 0 || strcmp(key, "ds") == 0 ||
                 strcmp(key, "de") == 0 || strcmp(key, "day") == 0 ||
                 strcmp(key, "night") == 0 || strcmp(key, "ramp") == 0 ||
                 strcmp(key, "reset") == 0) {
            long v = 0;
            if (!scan_int(json, len, &i, &v)) {
                /*
                 * 已知字段但不是整数。分两种情况：
                 *
                 *   - **值确实存在**（如 `"day":"x"`、`"day":true`）：
                 *     当作"类型不对"，跳过该值、该字段保持未提供 ——
                 *     不因一个字段写坏就让整包失效（更宽容、更好排查）。
                 *   - **值直接缺失**（如 `"on":}` 或 `"on":,`）：
                 *     这是**语法错误**，必须整包失败。否则半截包会被
                 *     当成合法配置应用，用户改了没生效还查不出原因。
                 *
                 * ⚠️ 不能一律调 skip_value() 了事 —— 它对未知字段很宽容
                 *    （任何非分隔符都吞），会把 `}` 也当成值吃掉，
                 *    于是 `{"on":}` 被误判为合法。这条是自测用例抓到的。
                 */
                size_t probe = i;
                skip_ws(json, len, &probe);
                if (probe >= len || json[probe] == '}' || json[probe] == ',') {
                    return false; /* 值缺失 → 语法错误 */
                }
                if (!skip_value(json, len, &i)) return false;
            }
            else {
                const int iv = (int)v;
                if (strcmp(key, "on") == 0) { pkt.on = iv; pkt.has_on = 1; }
                else if (strcmp(key, "reset") == 0) { pkt.reset = iv; pkt.has_reset = 1; }
                else if (strcmp(key, "ds") == 0) { pkt.ds = iv; pkt.has_ds = 1; }
                else if (strcmp(key, "de") == 0) { pkt.de = iv; pkt.has_de = 1; }
                else if (strcmp(key, "day") == 0) { pkt.day = iv; pkt.has_day = 1; }
                else if (strcmp(key, "night") == 0) { pkt.night = iv; pkt.has_night = 1; }
                else { pkt.ramp = iv; pkt.has_ramp = 1; }
            }
        }
        else {
            /* 未知字段（如未来的 v / 新特性）：跳过，保证向前兼容 */
            if (!skip_value(json, len, &i)) return false;
        }

        skip_ws(json, len, &i);
        if (i < len && json[i] == ',') {
            i++;
            continue;
        }
        if (i < len && json[i] == '}') {
            i++;
            break;
        }
        return false; /* 畸形 */
    }

    if (!saw_type) return false; /* 没有 t："bright" 就不是亮度包 */

    *out = pkt;
    return true;
}

/* ---------------- 应用到配置 ---------------- */

/** 夹取到 [lo, hi] */
static int clamp_range(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

void bright_apply_packet(auto_bright_config_t *cfg, const bright_packet_t *pkt)
{
    if (cfg == NULL || pkt == NULL) return;

    /*
     * 只覆盖**包里确实提供**的字段（-1 = 未提供）。
     *
     * 这样 App 可以只改一个值（比如只调夜间亮度）而不必回传全部字段；
     * 旧 App 连新固件、或新 App 连旧固件时也不会把对方的值清零。
     */
    if (pkt->has_on) cfg->enabled = pkt->on ? 1 : 0;

    /* 时间归一到 [0,1439]：用户可能拖到边界外，或协议里传了负值 */
    if (pkt->has_ds) cfg->day_start_min = clamp_range(pkt->ds, 0, 1439);
    if (pkt->has_de) cfg->day_end_min = clamp_range(pkt->de, 0, 1439);

    /*
     * 亮度夹到 [1,255] —— **不能是 0**：0 会让屏全黑，用户会以为死机
     * （与 control_clamp_brightness 同一约定）。
     */
    if (pkt->has_day) cfg->day_level = clamp_range(pkt->day, 1, 255);
    if (pkt->has_night) cfg->night_level = clamp_range(pkt->night, 1, 255);

    /* 过渡时长夹到 [0, 240]：超过 4 小时已无意义，且防误传超大值 */
    if (pkt->has_ramp) cfg->ramp_min = clamp_range(pkt->ramp, 0, 240);
}
