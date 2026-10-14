/**
 * nav_map.c —— 周边地图窗口解析实现（纯 C，可 PC 单测）
 *
 * 与 nav_parse.c / telemetry.c / control.c 相同的"手写轻量解析"风格：
 * 逐字符扫描、无动态分配、对畸形输入不崩溃。
 *
 * ⚠️ 有意不做的事：
 *   - 不解析字符串值（地图消息无字符串字段）→ 遇到字符串只跳过
 *   - 不做嵌套对象（协议是扁平结构；`p` 是"数组的数组"，单独处理）
 *   - **不做坐标换算**：收到什么就是什么（网关已投影成局部米）
 */

#include "nav_map.h"

#include <string.h>

/* ---------------- 状态 ---------------- */

/** 正在累积的窗口 */
static nav_map_window_t s_build;
/** 已完成、可用的窗口 */
static nav_map_window_t s_active;
static bool s_active_valid;
/** 是否正在累积（收到 `mw` 到 `me` 之间） */
static bool s_building;
static uint32_t s_build_start_ms;
static uint32_t s_active_ms; /* 最近一次完成切换的时刻 */
static uint32_t s_now_ms;    /* 由 nav_map_tick 更新，供 handle 使用 */

/* 自车在窗口坐标系中的偏移（米），由 nav 消息的 ox/oy 带来 */
static int s_veh_x;
static int s_veh_y;
static bool s_veh_set;

/* 统计 */
static uint32_t s_msgs;
static uint32_t s_windows;
static uint32_t s_drops;

/* ---------------- 轻量 JSON 扫描 ---------------- */

static void skip_ws(const char *s, size_t len, size_t *i)
{
    while (*i < len) {
        const char c = s[*i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            (*i)++;
        }
        else {
            break;
        }
    }
}

static bool scan_string(const char *s, size_t len, size_t *i, char *out, size_t out_sz)
{
    skip_ws(s, len, i);
    if (*i >= len || s[*i] != '"') return false;
    (*i)++;
    size_t o = 0;
    while (*i < len && s[*i] != '"') {
        if (s[*i] == '\\' && *i + 1 < len) (*i)++; /* 简单转义：跳过被转义字符 */
        if (o + 1 < out_sz) out[o++] = s[*i];
        (*i)++;
    }
    if (*i >= len) return false;
    (*i)++; /* 收尾引号 */
    if (out_sz > 0) out[o] = '\0';
    return true;
}

static bool scan_int(const char *s, size_t len, size_t *i, long *out)
{
    skip_ws(s, len, i);
    if (*i >= len) return false;
    bool neg = false;
    if (s[*i] == '-') {
        neg = true;
        (*i)++;
    }
    if (*i >= len || s[*i] < '0' || s[*i] > '9') return false;
    long v = 0;
    while (*i < len && s[*i] >= '0' && s[*i] <= '9') {
        v = v * 10 + (s[*i] - '0');
        if (v > 100000000L) return false; /* 防溢出 */
        (*i)++;
    }
    *out = neg ? -v : v;
    return true;
}

/** 跳过任意一个 JSON 值（字符串/数字/对象/数组/字面量） */
static bool skip_value(const char *s, size_t len, size_t *i);

static bool skip_container(const char *s, size_t len, size_t *i, char open, char close)
{
    if (*i >= len || s[*i] != open) return false;
    (*i)++;
    int depth = 1;
    while (*i < len && depth > 0) {
        const char c = s[*i];
        if (c == '"') {
            (*i)++;
            while (*i < len && s[*i] != '"') {
                if (s[*i] == '\\' && *i + 1 < len) (*i)++;
                (*i)++;
            }
            if (*i < len) (*i)++;
            continue;
        }
        if (c == open) depth++;
        else if (c == close) depth--;
        (*i)++;
    }
    return depth == 0;
}

static bool skip_value(const char *s, size_t len, size_t *i)
{
    skip_ws(s, len, i);
    if (*i >= len) return false;
    const char c = s[*i];
    if (c == '"') {
        char tmp[2];
        return scan_string(s, len, i, tmp, sizeof(tmp));
    }
    if (c == '{') return skip_container(s, len, i, '{', '}');
    if (c == '[') return skip_container(s, len, i, '[', ']');
    while (*i < len && s[*i] != ',' && s[*i] != '}' && s[*i] != ']') (*i)++;
    return true;
}

/* ---------------- 点数组解析 ---------------- */

/**
 * 解析 `[[x,y],[x,y],…]` 到 `out`。
 *
 * @param max_n 目标缓冲容量
 * @return 实际写入的点数（可能少于 JSON 中的点数 —— 超出的丢弃，不报错）
 */
static int parse_points(const char *s, size_t len, size_t *i,
                        nav_map_pt_t *out, int max_n)
{
    skip_ws(s, len, i);
    if (*i >= len || s[*i] != '[') return 0;
    (*i)++;
    int n = 0;
    for (;;) {
        skip_ws(s, len, i);
        if (*i >= len) break;
        if (s[*i] == ']') {
            (*i)++;
            break;
        }
        if (s[*i] == ',') {
            (*i)++;
            continue;
        }
        if (s[*i] != '[') break; /* 不是点：放弃剩余 */
        (*i)++;

        long x = 0;
        long y = 0;
        if (!scan_int(s, len, i, &x)) break;
        skip_ws(s, len, i);
        if (*i < len && s[*i] == ',') (*i)++;
        if (!scan_int(s, len, i, &y)) break;
        skip_ws(s, len, i);
        if (*i < len && s[*i] == ']') (*i)++;

        if (n < max_n) {
            /* 夹到 int16 范围：协议规定 ±1500m，这里对畸形输入做防御 */
            if (x > 32767) x = 32767;
            if (x < -32768) x = -32768;
            if (y > 32767) y = 32767;
            if (y < -32768) y = -32768;
            out[n].x = (int16_t)x;
            out[n].y = (int16_t)y;
            n++;
        }
        /* 超出容量：继续消费 JSON，但不写入（保持解析器位置正确） */
    }
    return n;
}

/* ---------------- 消息分类 ---------------- */

typedef enum {
    NAV_MAP_MSG_NONE = 0,
    NAV_MAP_MSG_BEGIN, /* mw */
    NAV_MAP_MSG_ROAD,  /* mr */
    NAV_MAP_MSG_BLDG,  /* mb */
    NAV_MAP_MSG_ROUTE, /* mp */
    NAV_MAP_MSG_END,   /* me */
} nav_map_kind_t;

/** 只读前几个字节判断 `"t":"xx"`，避免为分流做完整解析 */
static nav_map_kind_t classify(const char *json, size_t len)
{
    /* 期望形如 {"t":"mr",...}；容忍字段顺序不同，故做一次浅扫 */
    size_t i = 0;
    skip_ws(json, len, &i);
    if (i >= len || json[i] != '{') return NAV_MAP_MSG_NONE;
    i++;
    for (int guard = 0; guard < 8; guard++) {
        skip_ws(json, len, &i);
        if (i >= len) break;
        if (json[i] == '}') break;
        if (json[i] == ',') {
            i++;
            continue;
        }
        char key[16];
        if (!scan_string(json, len, &i, key, sizeof(key))) break;
        skip_ws(json, len, &i);
        if (i < len && json[i] == ':') i++;
        if (strcmp(key, "t") == 0) {
            char val[8];
            if (!scan_string(json, len, &i, val, sizeof(val))) break;
            if (strcmp(val, "mw") == 0) return NAV_MAP_MSG_BEGIN;
            if (strcmp(val, "mr") == 0) return NAV_MAP_MSG_ROAD;
            if (strcmp(val, "mb") == 0) return NAV_MAP_MSG_BLDG;
            if (strcmp(val, "mp") == 0) return NAV_MAP_MSG_ROUTE;
            if (strcmp(val, "me") == 0) return NAV_MAP_MSG_END;
            return NAV_MAP_MSG_NONE; /* 是别的 t（nav/遥测）→ 交给其它解析器 */
        }
        if (!skip_value(json, len, &i)) break;
    }
    return NAV_MAP_MSG_NONE;
}

/** 在 JSON 中找顶层字段 `name` 的冒号位置，返回其值的起点 */
static bool find_field(const char *json, size_t len, const char *name, size_t *val_i)
{
    size_t i = 0;
    skip_ws(json, len, &i);
    if (i >= len || json[i] != '{') return false;
    i++;
    for (int guard = 0; guard < 12; guard++) {
        skip_ws(json, len, &i);
        if (i >= len) return false;
        if (json[i] == '}') return false;
        if (json[i] == ',') {
            i++;
            continue;
        }
        char key[16];
        if (!scan_string(json, len, &i, key, sizeof(key))) return false;
        skip_ws(json, len, &i);
        if (i >= len || json[i] != ':') return false;
        i++;
        if (strcmp(key, name) == 0) {
            *val_i = i;
            return true;
        }
        if (!skip_value(json, len, &i)) return false;
    }
    return false;
}

/* ---------------- 对外接口 ---------------- */

void nav_map_init(void)
{
    memset(&s_build, 0, sizeof(s_build));
    memset(&s_active, 0, sizeof(s_active));
    s_active_valid = false;
    s_building = false;
    s_build_start_ms = 0;
    s_active_ms = 0;
    s_now_ms = 0;
    s_msgs = 0;
    s_windows = 0;
    s_drops = 0;
    s_veh_x = 0;
    s_veh_y = 0;
    s_veh_set = false;
}

void nav_map_set_vehicle_offset(int ox_m, int oy_m)
{
    s_veh_x = ox_m;
    s_veh_y = oy_m;
    s_veh_set = true;
}

bool nav_map_get_vehicle_offset(int *ox_m, int *oy_m)
{
    if (!s_veh_set) return false;
    if (ox_m) *ox_m = s_veh_x;
    if (oy_m) *oy_m = s_veh_y;
    return true;
}

static void begin_window(void)
{
    memset(&s_build, 0, sizeof(s_build));
    s_build.used = true;
    s_building = true;
    s_build_start_ms = s_now_ms;
}

static void end_window(void)
{
    if (!s_building) return;
    s_active = s_build; /* 整体切换：结构赋值，约 2.7KB，主循环内可接受 */
    s_active_valid = true;
    s_active_ms = s_now_ms;
    s_building = false;
    s_windows++;
}

bool nav_map_handle(const char *json, size_t len)
{
    if (json == NULL || len < 7) return false;

    const nav_map_kind_t kind = classify(json, len);
    if (kind == NAV_MAP_MSG_NONE) return false;

    s_msgs++;

    /* `mw` / `me` 没有需要解析的字段 */
    if (kind == NAV_MAP_MSG_BEGIN) {
        begin_window();
        return true;
    }
    if (kind == NAV_MAP_MSG_END) {
        end_window();
        return true;
    }

    /* 没有 `mw` 就打头阵的消息：也接受（容错），但视为一次新累积 */
    if (!s_building) begin_window();

    size_t i = 0;
    if (!find_field(json, len, "p", &i)) return true; /* 认了但不完整：不污染其它解析器 */

    if (kind == NAV_MAP_MSG_ROAD) {
        if (s_build.n_roads >= NAV_MAP_MAX_ROADS) return true; /* 超预算：丢弃该条 */

        long c = 0;
        size_t ci = 0;
        const uint8_t cls = find_field(json, len, "c", &ci) && scan_int(json, len, &ci, &c)
                                ? (uint8_t)((c < 0 || c > 5) ? 5 : c)
                                : 5;

        nav_map_road_t *r = &s_build.roads[s_build.n_roads];
        /* 跨要素总点数也要受限：给本条分配"剩余额度"，避免小预算被前几条吃光 */
        int room = (int)NAV_MAP_MAX_ROAD_PTS;
        for (int k = 0; k < s_build.n_roads; k++) room -= s_build.roads[k].n;
        if (room > NAV_MAP_PTS_PER_FEATURE) room = NAV_MAP_PTS_PER_FEATURE;
        if (room < 2) return true;

        const int n = parse_points(json, len, &i, r->pts, room);
        if (n < 2) return true; /* 少于两点画不出线 */
        r->cls = cls;
        r->n = (uint8_t)n;
        s_build.n_roads++;
        return true;
    }

    if (kind == NAV_MAP_MSG_BLDG) {
        if (s_build.n_bldgs >= NAV_MAP_MAX_BLDGS) return true;
        nav_map_bldg_t *b = &s_build.bldgs[s_build.n_bldgs];
        int room = (int)NAV_MAP_MAX_BLDG_PTS;
        for (int k = 0; k < s_build.n_bldgs; k++) room -= s_build.bldgs[k].n;
        if (room > NAV_MAP_PTS_PER_FEATURE) room = NAV_MAP_PTS_PER_FEATURE;
        if (room < 3) return true; /* 多边形至少三点 */

        const int n = parse_points(json, len, &i, b->pts, room);
        if (n < 3) return true;
        b->n = (uint8_t)n;
        s_build.n_bldgs++;
        return true;
    }

    /* kind == NAV_MAP_MSG_ROUTE */
    {
        int room = (int)NAV_MAP_MAX_ROUTE_PTS - (int)s_build.n_route;
        if (room > NAV_MAP_PTS_PER_FEATURE) room = NAV_MAP_PTS_PER_FEATURE;
        if (room < 2) return true;
        const int n = parse_points(json, len, &i, &s_build.route[s_build.n_route], room);
        if (n < 2) return true;
        s_build.n_route = (uint8_t)(s_build.n_route + n);
        return true;
    }
}

const nav_map_window_t *nav_map_active(void)
{
    if (!s_active_valid) return NULL;
    return &s_active;
}

void nav_map_tick(uint32_t now_ms)
{
    s_now_ms = now_ms;

    /* 累积超时：丢弃半截窗口，避免把混合了两次请求的几何画出来 */
    if (s_building && (uint32_t)(now_ms - s_build_start_ms) > NAV_MAP_BUILD_TIMEOUT_MS) {
        s_building = false;
        s_drops++;
    }

    /* 窗口过期：手机断连或无网时不再收到 `me`，此时应隐藏地图层 */
    if (s_active_valid && (uint32_t)(now_ms - s_active_ms) > NAV_MAP_STALE_MS) {
        s_active_valid = false;
        /*
         * 一并清掉自车偏移：窗口没了，偏移就没有参照系。
         * 否则下一次窗口到来前若先画了地图，会用一个陈旧的偏移把车画偏。
         */
        s_veh_set = false;
    }
}

uint32_t nav_map_msg_count(void)
{
    return s_msgs;
}

uint32_t nav_map_window_count(void)
{
    return s_windows;
}

uint32_t nav_map_drop_count(void)
{
    return s_drops;
}
