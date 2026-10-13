/**
 * nav_parse.c —— 导航消息 JSON 解析实现（纯 C，可 PC 单测）
 *
 * 复用与 telemetry.c / control.c 相同的"手写轻量解析"风格：
 * 逐字符扫描、无动态分配、对畸形输入不崩溃。
 *
 * ⚠️ 有意不做的事：
 *   - 不解析字符串值（阶段 2 不显示路名，无字符串字段）→ 遇到字符串只跳过
 *   - 不做嵌套对象（协议是扁平结构）
 */
#include "nav_parse.h"

#include <string.h>

/* ---------------- 基础扫描工具 ---------------- */

static void skip_ws(const char *s, size_t len, size_t *i)
{
    while (*i < len && (s[*i] == ' ' || s[*i] == '\t' || s[*i] == '\n' || s[*i] == '\r')) {
        (*i)++;
    }
}

/* 解析 JSON 字符串字面量（含转义），把内容拷进 out（可为 NULL = 只跳过）*/
static bool scan_string(const char *s, size_t len, size_t *i, char *out, size_t out_sz)
{
    if (*i >= len || s[*i] != '"') return false;
    (*i)++;

    size_t n = 0;
    while (*i < len) {
        char c = s[*i];
        if (c == '\\') {
            /* 转义：简化处理——原样保留被转义字符（够用于类型标记比较）*/
            (*i)++;
            if (*i >= len) return false;
            c = s[*i];
        }
        else if (c == '"') {
            (*i)++;
            if (out != NULL && out_sz > 0) out[n < out_sz - 1 ? n : out_sz - 1] = '\0';
            return true;
        }
        if (out != NULL && out_sz > 0 && n < out_sz - 1) out[n++] = c;
        (*i)++;
    }
    return false; /* 未闭合 */
}

/* 解析整数（支持负号）。仅接受整数字面量。 */
static bool scan_int(const char *s, size_t len, size_t *i, long *out)
{
    size_t start = *i;
    bool neg = false;
    if (*i < len && (s[*i] == '-' || s[*i] == '+')) {
        neg = (s[*i] == '-');
        (*i)++;
    }
    size_t digits = *i;
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

/* 跳过任意 JSON 值（数字/字符串/true/false/null），用于忽略不关心的字段 */
static bool skip_value(const char *s, size_t len, size_t *i)
{
    skip_ws(s, len, i);
    if (*i >= len) return false;

    if (s[*i] == '"') return scan_string(s, len, i, NULL, 0);
    if (s[*i] == '{' || s[*i] == '[') {
        /* 简化：跳过成对括号（协议无嵌套，够用）*/
        char open = s[*i], close = (open == '{') ? '}' : ']';
        int depth = 0;
        while (*i < len) {
            if (s[*i] == open) depth++;
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

bool nav_is_nav_message(const char *json, size_t len)
{
    if (json == NULL || len == 0) return false;

    /* 浅扫描：找 "t" 键且值为 "nav"。遥测包很密，这里要尽量便宜。 */
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
                return strcmp(val, "nav") == 0; /* 命中类型标记即决定 */
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
        if (i < len && json[i] == '}') return false;
        return false;
    }
}

/* ---------------- 主解析 ---------------- */

bool nav_parse(const char *json, size_t len, nav_msg_t *out)
{
    if (json == NULL || out == NULL || len == 0) return false;

    /*
     * ⚠️ 先在**局部变量**里解析，全部成功后再一次性写回 out。
     *
     * 为什么必须这样：早先的实现在函数开头就重置 out，中途遇到畸形输入
     * 直接 return false —— 调用方拿到的是**被清空的结构**。后果是一个畸形包
     * 会让仪表瞬间从"导航中"掉回"无数据"（闪一下），并且旧指引被抹掉。
     * 正确语义是：**解析失败 = 什么都不做**（原子性）。
     */
    nav_msg_t r;
    r.has_state = false;
    r.has_maneuver = false;
    r.has_dist = false;
    r.state = NAV_STATE_NO_DATA;
    r.maneuver = NAV_MANEUVER_UNKNOWN;
    r.distance_m = NAV_DISTANCE_INVALID;

    size_t i = 0;
    skip_ws(json, len, &i);
    if (i >= len || json[i] != '{') return false;
    i++;

    bool is_nav = false;
    long v = 0;

    for (;;) {
        skip_ws(json, len, &i);
        if (i >= len) return false;

        if (json[i] == '}') { /* 对象结束 */
            i++;
            break;
        }

        char key[24];
        if (!scan_string(json, len, &i, key, sizeof(key))) return false;

        skip_ws(json, len, &i);
        if (i >= len || json[i] != ':') return false;
        i++;
        skip_ws(json, len, &i);

        /* ---- 类型标记 ---- */
        if (strcmp(key, "t") == 0 && i < len && json[i] == '"') {
            char tv[16];
            if (!scan_string(json, len, &i, tv, sizeof(tv))) return false;
            is_nav = (strcmp(tv, "nav") == 0);
        }
        /* ---- 状态 ---- */
        else if (strcmp(key, "state") == 0 && i < len && json[i] != '"' && json[i] != 'n') {
            if (!scan_int(json, len, &i, &v)) return false;
            r.has_state = true;
            r.state = nav_clamp_state((int)v); /* 越界 → NO_DATA（保守）*/
        }
        /* ---- 动作 ---- */
        else if (strcmp(key, "maneuver") == 0 && i < len && json[i] != '"' && json[i] != 'n') {
            if (!scan_int(json, len, &i, &v)) return false;
            r.has_maneuver = true;
            r.maneuver = nav_clamp_maneuver((int)v); /* 越界 → UNKNOWN */
        }
        /* ---- 距离 ---- */
        else if (strcmp(key, "dist_m") == 0) {
            if (i < len && json[i] == 'n') {
                /* null：**明确无效**（与"字段缺失"语义不同）*/
                if (!skip_value(json, len, &i)) return false;
                r.has_dist = true;
                r.distance_m = NAV_DISTANCE_INVALID;
            }
            else if (i < len && json[i] != '"') {
                if (!scan_int(json, len, &i, &v)) return false;
                r.has_dist = true;
                /* 超范围 → 无效（不截断成误导性的小数值）*/
                r.distance_m = (v < 0 || v > NAV_DISTANCE_MAX_M)
                                   ? NAV_DISTANCE_INVALID
                                   : (int)v;
            }
            else {
                if (!skip_value(json, len, &i)) return false;
            }
        }
        /* ---- 其它字段：忽略（向前兼容，未来加字段不会解析失败）---- */
        else {
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
        return false; /* 语法错误 */
    }

    if (!is_nav) return false; /* 不是导航消息 */

    /* 至少要有一个有效字段，否则视为无效包（避免空包把状态清掉）*/
    if (!(r.has_state || r.has_maneuver || r.has_dist)) return false;

    *out = r; /* 成功：原子提交 */
    return true;
}
