/*
 * telemetry.c —— iPhone BLE 遥测协议解析（平台无关纯 C）
 *
 * 一个面向**受控协议**的最小 JSON 对象解析器：
 *   - 只解析一层扁平对象（{"key":value,...}），不需要通用 JSON 库；
 *   - 支持 number / true / false / null / 字符串值（字符串仅跳过，本协议不用）；
 *   - 未知键忽略，缺省字段以 has_* 标志区分。
 *
 * 注意：本解析器针对 AI_TASK §4.1 的受控 schema 设计，不承诺解析任意 JSON；
 *       输入来自我们自己的 iOS 端，字段名与两端保持一致。
 */
#include "telemetry.h"

#include <stdlib.h>
#include <string.h>

/* ---------------- 词法辅助 ---------------- */

static void skip_ws(const char *s, size_t len, size_t *i)
{
    while (*i < len && (s[*i] == ' ' || s[*i] == '\t' || s[*i] == '\r' || s[*i] == '\n')) {
        (*i)++;
    }
}

/* 解析并返回一个字符串键（"..."，不支持反斜杠转义键名） */
static bool parse_key(const char *s, size_t len, size_t *i, char *key, size_t key_sz)
{
    if (*i >= len || s[*i] != '"') return false;
    (*i)++;

    size_t k = 0;
    while (*i < len && s[*i] != '"') {
        if (k + 1 < key_sz) key[k++] = s[*i];
        (*i)++;
    }
    if (*i >= len) return false; /* 未闭合 */
    key[k] = '\0';
    (*i)++; /* 跳过闭引号 */
    return true;
}

/* 解析数字为 double（整数/小数/科学计数法；支持负号；不处理前导 +，JSON 不允许） */
static bool parse_number(const char *s, size_t len, size_t *i, double *out)
{
    size_t start = *i;
    if (*i < len && s[*i] == '-') (*i)++;

    bool any_digit = false;
    while (*i < len && s[*i] >= '0' && s[*i] <= '9') { (*i)++; any_digit = true; }
    if (*i < len && s[*i] == '.') {
        (*i)++;
        while (*i < len && s[*i] >= '0' && s[*i] <= '9') { (*i)++; any_digit = true; }
    }
    if (!any_digit) return false;

    if (*i < len && (s[*i] == 'e' || s[*i] == 'E')) {
        (*i)++;
        if (*i < len && (s[*i] == '+' || s[*i] == '-')) (*i)++;
        size_t es = *i;
        while (*i < len && s[*i] >= '0' && s[*i] <= '9') (*i)++;
        if (*i == es) return false; /* 指数后无数字 */
    }

    /* 已精确框定数字区间，交给 strtod 精确转换 */
    size_t n = *i - start;
    char tmp[64];
    if (n >= sizeof(tmp)) n = sizeof(tmp) - 1;
    memcpy(tmp, s + start, n);
    tmp[n] = '\0';
    *out = strtod(tmp, NULL);
    return true;
}

/* 匹配字面量 true/false/null */
static bool parse_literal(const char *s, size_t len, size_t *i, const char *lit, bool *matched)
{
    size_t ln = strlen(lit);
    if (*i + ln <= len && strncmp(s + *i, lit, ln) == 0) {
        (*i) += ln;
        if (matched != NULL) *matched = true;
        return true;
    }
    return false;
}

/* 跳过字符串值（含 \" 转义）；返回 false 表示格式错误/未闭合 */
static bool parse_string(const char *s, size_t len, size_t *i)
{
    if (*i >= len || s[*i] != '"') return false;
    (*i)++;
    while (*i < len) {
        if (s[*i] == '\\') { (*i) += 2; continue; }
        if (s[*i] == '"') { (*i)++; return true; }
        (*i)++;
    }
    return false; /* 未闭合 */
}

/* 值类型 */
typedef enum { VAL_NUMBER, VAL_BOOL, VAL_NULL, VAL_STRING, VAL_OTHER } val_kind_t;

/*
 * 解析一个值。number 时填 *num；bool 时填 *b；string/null 只推进游标。
 * VAL_OTHER 表示嵌套对象/数组被整体跳过。
 */
static bool parse_value(const char *s, size_t len, size_t *i, val_kind_t *kind,
                        double *num, bool *b)
{
    skip_ws(s, len, i);
    if (*i >= len) return false;

    char c = s[*i];

    if (c == '{' || c == '[') {
        /* 本协议无嵌套；整体跳过以容错 */
        (*i)++;
        int depth = 1;
        while (*i < len && depth > 0) {
            if (s[*i] == '"') {
                if (!parse_string(s, len, i)) return false;
                continue;
            }
            if (s[*i] == '{' || s[*i] == '[') depth++;
            else if (s[*i] == '}' || s[*i] == ']') depth--;
            (*i)++;
        }
        if (depth != 0) return false;
        *kind = VAL_OTHER;
        return true;
    }

    if (c == '"') {
        if (!parse_string(s, len, i)) return false;
        *kind = VAL_STRING;
        return true;
    }

    if (c == 't' || c == 'f') {
        bool matched = false;
        if (parse_literal(s, len, i, "true", &matched) && matched) {
            *kind = VAL_BOOL; *b = true; return true;
        }
        matched = false;
        if (parse_literal(s, len, i, "false", &matched) && matched) {
            *kind = VAL_BOOL; *b = false; return true;
        }
        return false;
    }

    if (c == 'n') {
        if (parse_literal(s, len, i, "null", NULL)) { *kind = VAL_NULL; return true; }
        return false;
    }

    if (c == '-' || (c >= '0' && c <= '9')) {
        if (!parse_number(s, len, i, num)) return false;
        *kind = VAL_NUMBER;
        return true;
    }

    return false;
}

/* 把已解析的 key/value 填入目标结构 */
static void assign_field(telemetry_msg_t *out, const char *key,
                         val_kind_t kind, double num, bool b)
{
    if (strcmp(key, "v") == 0) {
        if (kind == VAL_NUMBER) out->v = (int)num;
    }
    else if (strcmp(key, "ts_utc_ms") == 0) {
        if (kind == VAL_NUMBER) { out->has_ts = true; out->ts_utc_ms = (int64_t)num; }
    }
    else if (strcmp(key, "speed_mps") == 0) {
        if (kind == VAL_NUMBER) out->speed_mps = (float)num;
    }
    else if (strcmp(key, "speed_accuracy_mps") == 0) {
        if (kind == VAL_NUMBER) { out->has_speed_acc = true; out->speed_accuracy_mps = (float)num; }
    }
    else if (strcmp(key, "latitude") == 0) {
        if (kind == VAL_NUMBER) { out->has_lat = true; out->latitude = num; }
    }
    else if (strcmp(key, "longitude") == 0) {
        if (kind == VAL_NUMBER) { out->has_lon = true; out->longitude = num; }
    }
    else if (strcmp(key, "horizontal_accuracy_m") == 0) {
        if (kind == VAL_NUMBER) { out->has_hacc = true; out->horizontal_accuracy_m = (float)num; }
    }
    else if (strcmp(key, "course_deg") == 0) {
        if (kind == VAL_NUMBER) { out->has_course = true; out->course_deg = (float)num; }
    }
    else if (strcmp(key, "location_valid") == 0) {
        if (kind == VAL_BOOL) out->location_valid = b;
    }
    else if (strcmp(key, "speed_valid") == 0) {
        if (kind == VAL_BOOL) out->speed_valid = b;
    }
    /* 未知键或类型不符：忽略 */
}

bool telemetry_parse(const char *json, size_t len, telemetry_msg_t *out)
{
    if (out == NULL) return false;
    memset(out, 0, sizeof(*out));

    if (json == NULL) return false;

    size_t i = 0;
    skip_ws(json, len, &i);
    if (i >= len || json[i] != '{') return false;
    i++;

    for (;;) {
        skip_ws(json, len, &i);
        if (i >= len) return false;

        if (json[i] == '}') return true; /* 空对象或正常结束 */

        /* key */
        char key[40];
        if (!parse_key(json, len, &i, key, sizeof(key))) return false;
        skip_ws(json, len, &i);
        if (i >= len || json[i] != ':') return false;
        i++;

        /* value */
        val_kind_t kind = VAL_OTHER;
        double num = 0.0;
        bool b = false;
        if (!parse_value(json, len, &i, &kind, &num, &b)) return false;
        assign_field(out, key, kind, num, b);

        /* 分隔符 */
        skip_ws(json, len, &i);
        if (i >= len) return false;
        if (json[i] == ',') {
            i++;
            skip_ws(json, len, &i);
            if (i < len && json[i] == '}') return false; /* 尾逗号非法 */
            continue;
        }
        if (json[i] == '}') return true;
        return false;
    }
}

float telemetry_speed_kmh(const telemetry_msg_t *m)
{
    if (m == NULL) return 0.0f;
    return m->speed_mps * 3.6f;
}

bool telemetry_plausible(const telemetry_msg_t *m)
{
    if (m == NULL) return false;

    /* 速度：负值允许（iOS 用负表示无效，由 speed_valid 表达），但绝对值不得超范围 */
    if (m->has_speed_acc || m->speed_valid) {
        float kmh = m->speed_mps * 3.6f;
        if (kmh < -1.0f || kmh > TELEMETRY_SPEED_MAX_KMH) return false;
    }

    /* 航向：0..360（含边界容差） */
    if (m->has_course && (m->course_deg < -0.5f || m->course_deg > 360.5f)) return false;

    /* 经纬度：地理范围 */
    if (m->has_lat && (m->latitude < -90.0 || m->latitude > 90.0)) return false;
    if (m->has_lon && (m->longitude < -180.0 || m->longitude > 180.0)) return false;

    /* 精度值不得为负（-1 表示未知，允许） */
    if (m->has_hacc && m->horizontal_accuracy_m < -1.0f) return false;
    if (m->has_speed_acc && m->speed_accuracy_mps < -1.0f) return false;

    /* 定位有效但完全没有位置信息 → 自相矛盾 */
    if (m->location_valid && !m->has_lat && !m->has_lon) return false;

    return true;
}