/**
 * photo_cmd.c —— `PhotoCtl` 紧凑 JSON 命令解析（纯逻辑）
 *
 * 设计要点见 photo_cmd.h。实现与仓库既有 `control.c` 同一路线：**手写扫描，
 * 不引 JSON 库**——命令都是 ≤180 B 的扁平对象，为它拖一个解析器不值得。
 *
 * 与 control.c 的差别（以及为什么）：
 *   - 必须能跳过任意类型的未知字段（App 版本可能比固件新），所以有 `skip_value`；
 *   - begin 的字段多、且类型严格要求是整数（`"kind":"0"` 这种字符串必须拒绝），
 *     因此把"识别 key"和"按类型取值"分开，取错类型就地失败；
 *   - 允许带长度调用（BLE 给的缓冲不一定以 0 结尾），解析完 `}` 后允许空白/NUL 填充。
 *
 * 只做语法与取值域校验：空间、能力位、清单下标夹取等**业务判断**留给运行时。
 */
#include "photo_cmd.h"

#include "pgj.h" /* 只为 PGJ_MAX_DIM：静态照片边长上限（协议 §2 px=466） */

#include <limits.h>
#include <string.h>

/** 手写扫描器状态：指针 + 可用长度 + 当前位置 */
typedef struct {
    const char *s;
    size_t len;
    size_t i;
} scan_t;

static void skip_ws(scan_t *sc)
{
    while (sc->i < sc->len) {
        char c = sc->s[sc->i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            sc->i++;
        }
        else {
            break;
        }
    }
}

/** 输入到头（含"长度内出现 NUL"：BLE 缓冲可能用 0 填充） */
static bool at_end(const scan_t *sc)
{
    return sc->i >= sc->len || sc->s[sc->i] == '\0';
}

static bool expect_char(scan_t *sc, char want)
{
    skip_ws(sc);
    if (at_end(sc) || sc->s[sc->i] != want) return false;
    sc->i++;
    return true;
}

/**
 * 解析 JSON 字符串到 out（可只做语法校验：out=NULL）。
 *
 * 只支持命令里可能出现的短转义（\" \\ \/ b f n r t）；`\u` 直接拒绝——
 * 我们的 key/op/id 都是 ASCII，遇到 \u 说明不是本协议的命令。
 *
 * @param too_long 内容放不进 cap 时置 true（调用方据此拒绝 id 等有长度上限的字段）
 */
static bool parse_string(scan_t *sc, char *out, size_t cap, bool *too_long)
{
    if (too_long != NULL) *too_long = false;
    if (!expect_char(sc, '"')) return false;

    size_t n = 0;
    for (;;) {
        if (at_end(sc)) return false; /* 字符串未闭合 */
        char c = sc->s[sc->i++];
        if (c == '"') break;

        if (c == '\\') {
            if (at_end(sc)) return false;
            char e = sc->s[sc->i++];
            switch (e) {
            case '"': c = '"'; break;
            case '\\': c = '\\'; break;
            case '/': c = '/'; break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'n': c = '\n'; break;
            case 'r': c = '\r'; break;
            case 't': c = '\t'; break;
            default: return false;
            }
        }

        if (out != NULL && cap > 0) {
            if (n + 1 < cap) {
                out[n] = c;
            }
            else if (too_long != NULL) {
                /* 超长：只标记，继续消费到闭引号，保证语法仍被完整校验 */
                *too_long = true;
            }
        }
        n++;
    }

    if (out != NULL && cap > 0) {
        out[(n < cap) ? n : (cap - 1)] = '\0';
    }
    return true;
}

/** 解析 JSON 整数（拒绝小数/指数/前导 +；可带 '-'）。 */
static bool parse_int(scan_t *sc, long long *out)
{
    skip_ws(sc);
    if (at_end(sc)) return false;

    bool neg = false;
    if (sc->s[sc->i] == '-') {
        neg = true;
        sc->i++;
    }
    else if (sc->s[sc->i] == '+') {
        return false; /* 严格 JSON 不允许前导 + */
    }
    if (at_end(sc) || sc->s[sc->i] < '0' || sc->s[sc->i] > '9') return false;

    unsigned long long v = 0;
    bool overflow = false;
    while (!at_end(sc) && sc->s[sc->i] >= '0' && sc->s[sc->i] <= '9') {
        unsigned d = (unsigned)(sc->s[sc->i] - '0');
        if (v > (0xFFFFFFFFFFFFFFFFull - (unsigned long long)d) / 10ull) {
            overflow = true;
        }
        else {
            v = v * 10ull + (unsigned long long)d;
        }
        sc->i++;
    }
    /* 整数字段不接受 1.5 / 1e3：直接拒绝而不是悄悄截断 */
    if (!at_end(sc)) {
        char c = sc->s[sc->i];
        if (c == '.' || c == 'e' || c == 'E') return false;
    }
    if (overflow) return false;
    if (v > 0x7FFFFFFFFFFFFFFFull) return false;

    *out = neg ? -(long long)v : (long long)v;
    return true;
}

static bool skip_literal(scan_t *sc, const char *lit)
{
    size_t n = strlen(lit);
    if (sc->len - sc->i < n) return false; /* 用可用长度判，不依赖结尾 0 */
    if (memcmp(sc->s + sc->i, lit, n) != 0) return false;
    sc->i += n;
    return true;
}

/**
 * 跳过一个值（任意 JSON 类型，嵌套对象/数组也支持）。
 *
 * 为什么需要它：协议规定"未知字段忽略"，而 App 升级后可能多带字段。
 * 不支持跳过就意味着固件比 App 老一点就全盘拒命令。
 * 深度上限 8：正常命令是扁平的，设上限只是防止病态输入把栈打爆。
 */
static bool skip_value(scan_t *sc, int depth)
{
    if (depth > 8) return false;
    skip_ws(sc);
    if (at_end(sc)) return false;

    char c = sc->s[sc->i];
    if (c == '"') return parse_string(sc, NULL, 0, NULL);
    if (c == 't') return skip_literal(sc, "true");
    if (c == 'f') return skip_literal(sc, "false");
    if (c == 'n') return skip_literal(sc, "null");
    if (c == '-' || (c >= '0' && c <= '9')) {
        long long tmp;
        return parse_int(sc, &tmp);
    }

    if (c == '{') {
        sc->i++;
        skip_ws(sc);
        if (at_end(sc)) return false;
        if (sc->s[sc->i] == '}') {
            sc->i++;
            return true;
        }
        for (;;) {
            if (!parse_string(sc, NULL, 0, NULL)) return false;
            if (!expect_char(sc, ':')) return false;
            if (!skip_value(sc, depth + 1)) return false;
            skip_ws(sc);
            if (at_end(sc)) return false;
            if (sc->s[sc->i] == ',') {
                sc->i++;
                continue;
            }
            if (sc->s[sc->i] == '}') {
                sc->i++;
                return true;
            }
            return false;
        }
    }

    if (c == '[') {
        sc->i++;
        skip_ws(sc);
        if (at_end(sc)) return false;
        if (sc->s[sc->i] == ']') {
            sc->i++;
            return true;
        }
        for (;;) {
            if (!skip_value(sc, depth + 1)) return false;
            skip_ws(sc);
            if (at_end(sc)) return false;
            if (sc->s[sc->i] == ',') {
                sc->i++;
                continue;
            }
            if (sc->s[sc->i] == ']') {
                sc->i++;
                return true;
            }
            return false;
        }
    }

    return false;
}

/* 一个已知整数字段的取值范围（含端点） */
typedef struct {
    long long min;
    long long max;
} range_t;

/**
 * 已知整数字段的统一处理：取整数 → 检查范围 → 写目标。
 * 范围检查放在"解析时"而不是"按 op 使用之后"，这样 `{"op":"list","kind":9}`
 * 也会被拒（协议约定未知/脏字段不往 SD/BLE 层传）。
 */
static bool take_int(scan_t *sc, range_t r, long long *dst)
{
    long long v = 0;
    if (!parse_int(sc, &v)) return false;
    if (v < r.min || v > r.max) return false;
    *dst = v;
    return true;
}

bool photo_cmd_parse(const char *json, size_t len, photo_cmd_t *out)
{
    if (json == NULL || out == NULL) return false;
    if (len == 0) len = strlen(json);

    scan_t sc;
    sc.s = json;
    sc.len = len;
    sc.i = 0;

    memset(out, 0, sizeof(*out));
    out->op = PHOTO_OP_NONE;
    out->index = -1; /* -1 = 未指定（运行时按"当前张"处理） */

    if (!expect_char(&sc, '{')) return false;

    char key[16];
    char op[16] = { 0 };
    bool key_too_long = false;
    bool have_t = false, t_ok = false;
    bool have_op = false;
    bool have_kind = false, have_bytes = false;
    bool have_w = false, have_h = false, have_frames = false, have_fps = false;
    bool have_id = false, have_i = false;

    long long kind = 0, bytes = 0, crc = 0, w = 0, h = 0, frames = 0, fps = 0;
    long long index = -1;

    skip_ws(&sc);
    if (at_end(&sc)) return false; /* "{" 之后没了 */

    if (sc.s[sc.i] == '}') {
        sc.i++; /* 空对象：后面统一因缺 t/op 失败 */
    }
    else {
        for (;;) {
            key_too_long = false;
            if (!parse_string(&sc, key, sizeof(key), &key_too_long)) return false;
            if (!expect_char(&sc, ':')) return false;
            skip_ws(&sc);
            if (at_end(&sc)) return false;

            if (key_too_long) {
                /* 已知字段名都 ≤6 字符，超长 key 必然是未知字段 */
                if (!skip_value(&sc, 0)) return false;
            }
            else if (strcmp(key, "t") == 0) {
                char tv[4];
                bool tl = false;
                if (!parse_string(&sc, tv, sizeof(tv), &tl)) return false;
                if (tl) return false;
                have_t = true;
                t_ok = (strcmp(tv, "p") == 0);
            }
            else if (strcmp(key, "op") == 0) {
                bool tl = false;
                if (!parse_string(&sc, op, sizeof(op), &tl)) return false;
                if (tl) return false;
                have_op = true;
            }
            else if (strcmp(key, "kind") == 0) {
                if (!take_int(&sc, (range_t){ 0, 1 }, &kind)) return false;
                have_kind = true;
            }
            else if (strcmp(key, "bytes") == 0) {
                if (!take_int(&sc, (range_t){ 1, 0xFFFFFFFFll }, &bytes)) return false;
                have_bytes = true;
            }
            else if (strcmp(key, "crc") == 0) {
                /* crc 缺失时按 0 处理（头文件"必填校验"未把它列为必填） */
                if (!take_int(&sc, (range_t){ 0, 0xFFFFFFFFll }, &crc)) return false;
            }
            else if (strcmp(key, "w") == 0) {
                if (!take_int(&sc, (range_t){ 1, (long long)PGJ_MAX_DIM }, &w)) return false;
                have_w = true;
            }
            else if (strcmp(key, "h") == 0) {
                if (!take_int(&sc, (range_t){ 1, (long long)PGJ_MAX_DIM }, &h)) return false;
                have_h = true;
            }
            else if (strcmp(key, "frames") == 0) {
                if (!take_int(&sc, (range_t){ 1, 65535 }, &frames)) return false;
                have_frames = true;
            }
            else if (strcmp(key, "fps") == 0) {
                if (!take_int(&sc, (range_t){ 0, 255 }, &fps)) return false;
                have_fps = true;
            }
            else if (strcmp(key, "id") == 0) {
                bool tl = false;
                if (!parse_string(&sc, out->id, sizeof(out->id), &tl)) return false;
                if (tl) return false; /* 超过 63 字符：不可能合法 */
                have_id = true;
            }
            else if (strcmp(key, "i") == 0) {
                if (!take_int(&sc, (range_t){ 0, (long long)INT_MAX }, &index)) return false;
                have_i = true;
            }
            else if (strcmp(key, "slide") == 0) {
                long long v = 0;
                if (!take_int(&sc, (range_t){ 0, 1 }, &v)) return false;
                out->slide = (int)v;
                out->has_slide = true;
            }
            else if (strcmp(key, "shift") == 0) {
                long long v = 0;
                if (!take_int(&sc, (range_t){ 0, 1 }, &v)) return false;
                out->shift = (int)v;
                out->has_shift = true;
            }
            else if (strcmp(key, "intv") == 0) {
                long long v = 0;
                if (!take_int(&sc, (range_t){ 1, 3600 }, &v)) return false;
                out->intv = (int)v;
                out->has_intv = true;
            }
            else if (strcmp(key, "soff_s") == 0) {
                long long v = 0;
                if (!take_int(&sc, (range_t){ 0, 86400 }, &v)) return false;
                out->soff_s = (int)v;
                out->has_soff = true;
            }
            else {
                /* 未知字段：忽略（App 可能比固件新） */
                if (!skip_value(&sc, 0)) return false;
            }

            skip_ws(&sc);
            if (at_end(&sc)) return false;
            if (sc.s[sc.i] == ',') {
                sc.i++;
                skip_ws(&sc);
                if (at_end(&sc)) return false;
                continue;
            }
            if (sc.s[sc.i] == '}') {
                sc.i++;
                break;
            }
            return false;
        }
    }

    /* `}` 之后只允许空白/NUL 填充：BLE 传递的缓冲可能是定长的 */
    while (sc.i < sc.len) {
        char c = sc.s[sc.i++];
        if (c != '\0' && c != ' ' && c != '\t' && c != '\n' && c != '\r') return false;
    }

    /* t 必须存在且为 "p"；op 必须存在 */
    if (!have_t || !t_ok || !have_op) return false;

    if (strcmp(op, "begin") == 0) {
        if (!have_kind || !have_bytes || !have_w || !have_h || !have_frames) return false;
        /* kind=1（动图）必须给 fps>=1；kind=0 的 fps 忽略（协议允许 0/缺省） */
        if (kind == 1 && (!have_fps || fps < 1)) return false;
        out->op = PHOTO_OP_BEGIN;
        out->kind = (uint8_t)kind;
        out->bytes = (uint32_t)bytes;
        out->crc = (uint32_t)crc;
        out->width = (uint16_t)w;
        out->height = (uint16_t)h;
        out->frames = (uint16_t)frames;
        out->fps = (uint8_t)fps;
        return true;
    }
    if (strcmp(op, "end") == 0) {
        out->op = PHOTO_OP_END;
        return true;
    }
    if (strcmp(op, "abort") == 0) {
        out->op = PHOTO_OP_ABORT;
        return true;
    }
    if (strcmp(op, "del") == 0) {
        if (!have_id) return false;
        /* 白名单同时挡住路径穿越与非法扩展名（删除是不可恢复操作） */
        if (!photo_name_valid(out->id)) return false;
        out->op = PHOTO_OP_DEL;
        return true;
    }
    if (strcmp(op, "play") == 0) {
        if (!have_i) return false; /* 必填；-1 由调用方自己传，不由 App 指定 */
        out->op = PHOTO_OP_PLAY;
        out->index = (int)index;
        return true;
    }
    if (strcmp(op, "set") == 0) {
        if (!out->has_slide && !out->has_intv && !out->has_shift && !out->has_soff) {
            return false; /* 一个字段都没有的 set 没有意义，按坏命令拒绝 */
        }
        out->op = PHOTO_OP_SET;
        return true;
    }
    if (strcmp(op, "list") == 0) {
        out->op = PHOTO_OP_LIST;
        return true;
    }
    if (strcmp(op, "bench") == 0) {
        out->op = PHOTO_OP_BENCH;
        return true;
    }
    if (strcmp(op, "format") == 0) {
        out->op = PHOTO_OP_FORMAT;
        return true;
    }

    return false; /* 未知 op → bad_arg（由调用方回 err） */
}
