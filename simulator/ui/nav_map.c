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
/**
 * **最近一次收到地图消息的时刻**（不是"窗口开始时刻"）。
 *
 * ⚠️ 这个区别是踩坑换来的：超时若按"窗口开始到现在"，那么**慢但在推进**的
 *    传输会被误判为中断。实测 iPhone 等应答逐条发送时，一个 50 条消息的窗口
 *    要接近 3 秒 —— 正好踩中超时，`s_building` 被清成 false，下一条消息于是
 *    走进"没有 mw"的容错分支调用 begin_window()，**把已收到的整批道路清空**。
 *    症状极其迷惑：条数统计显示道路收到过、且因"满"拒绝了后续几条，
 *    但提交时窗口里道路恒为 0。
 *    改为按"无消息"计时后，只要还在收数据就永远不会超时。
 */
static uint32_t s_build_last_ms;
static uint32_t s_active_ms; /* 最近一次完成切换的时刻 */
static uint32_t s_now_ms;    /* 由 nav_map_tick 更新，供 handle 使用 */

/* 自车在窗口坐标系中的偏移（米），由 nav 消息的 ox/oy 带来 */
static int s_veh_x;
static int s_veh_y;
static bool s_veh_set;

/* 统计 */
static uint32_t s_msgs;
/* 分类计数：排查"某一类消息收不到"时是关键线索（实测踩过：道路恒为 0）*/
static uint32_t s_cnt_begin, s_cnt_road, s_cnt_bldg, s_cnt_route, s_cnt_end;
static uint32_t s_rej_road_full, s_rej_road_pts, s_rej_bldg_full, s_rej_bldg_pts;

/*
 * 最近收到的消息**类型顺序**（环形，最多 64 条）。
 *
 * 为什么需要它：定位"某一类要素在窗口里恒为 0"时，光看各类**条数**不够 ——
 * 条数正常但窗口内容为空，说明该要素在 `mw` 之前就到了（被 begin_window 清掉）。
 * 顺序记录能一眼看出这类乱序问题。
 * 取值：0=无 1=mw 2=mr 3=mb 4=mp 5=me
 */
static uint8_t s_order[64];
static uint8_t s_order_n;
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
    s_build_last_ms = 0;
    s_active_ms = 0;
    s_now_ms = 0;
    s_msgs = 0;
    s_order_n = 0;
    s_windows = 0;
    s_drops = 0;
    s_cnt_begin = s_cnt_road = s_cnt_bldg = s_cnt_route = s_cnt_end = 0;
    s_rej_road_full = s_rej_road_pts = s_rej_bldg_full = s_rej_bldg_pts = 0;
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
    s_build_last_ms = s_now_ms;
}

/* 提交瞬间的内容快照（诊断用；排查完可删）*/
uint32_t g_nav_map_dbg_build[3];
uint32_t g_nav_map_dbg_commit[3];

static void end_window(void)
{
    if (!s_building) return;
    s_building = false;
    g_nav_map_dbg_build[0] = s_build.n_roads;
    g_nav_map_dbg_build[1] = s_build.n_bldgs;
    g_nav_map_dbg_build[2] = s_build.n_route;

    /*
     * 空窗口（一个要素都没有）视为**清除地图**，而不是"一张空白地图"。
     *
     * 为什么：手机停止导航时会发一对 `mw`+`me` 明确要求清空（见 NavSession.stop）。
     * 若把空窗口当成有效地图，地图层会显示为一片纯黑，同时把导航指引挤到
     * 紧凑布局 —— 用户看到的是"界面坏了"，而不是"回到了无地图状态"。
     * 按"无地图"处理则会自动回退到原来的大字卡片，语义正确。
     */
    const bool empty = (s_build.n_roads == 0 && s_build.n_bldgs == 0 && s_build.n_route == 0);
    if (empty) {
        s_active_valid = false;
        s_veh_set = false; /* 窗口没了，偏移失去参照系 */
        s_drops++;         /* 计入"丢弃"便于诊断：这不是一次有效的窗口切换 */
        return;
    }

    s_active = s_build; /* 整体切换：结构赋值，约 2.7KB，主循环内可接受 */
    s_active_valid = true;
    s_active_ms = s_now_ms;
    s_windows++;
    g_nav_map_dbg_commit[0] = s_active.n_roads;
    g_nav_map_dbg_commit[1] = s_active.n_bldgs;
    g_nav_map_dbg_commit[2] = s_active.n_route;
}

bool nav_map_handle(const char *json, size_t len)
{
    if (json == NULL || len < 7) return false;

    const nav_map_kind_t kind = classify(json, len);
    if (kind == NAV_MAP_MSG_NONE) return false;

    s_msgs++;
    s_build_last_ms = s_now_ms; /* 有消息进来就不算超时（见 s_build_last_ms 的说明）*/
    if (s_order_n < sizeof(s_order)) s_order[s_order_n++] = (uint8_t)kind;

    /* `mw` / `me` 没有需要解析的字段 */
    if (kind == NAV_MAP_MSG_BEGIN) {
        s_cnt_begin++;
        begin_window();
        return true;
    }
    if (kind == NAV_MAP_MSG_END) {
        s_cnt_end++;
        end_window();
        return true;
    }

    /* 没有 `mw` 就打头阵的消息：也接受（容错），但视为一次新累积 */
    if (!s_building) begin_window();

    size_t i = 0;
    if (!find_field(json, len, "p", &i)) return true; /* 认了但不完整：不污染其它解析器 */

    if (kind == NAV_MAP_MSG_ROAD) {
        s_cnt_road++;
        if (s_build.n_roads >= NAV_MAP_MAX_ROADS) { s_rej_road_full++; return true; }

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
        if (room < 2) { s_rej_road_pts++; return true; }

        const int n = parse_points(json, len, &i, r->pts, room);
        if (n < 2) { s_rej_road_pts++; return true; } /* 少于两点画不出线 */
        r->cls = cls;
        r->n = (uint8_t)n;
        s_build.n_roads++;
        return true;
    }

    if (kind == NAV_MAP_MSG_BLDG) {
        s_cnt_bldg++;
        if (s_build.n_bldgs >= NAV_MAP_MAX_BLDGS) { s_rej_bldg_full++; return true; }
        nav_map_bldg_t *b = &s_build.bldgs[s_build.n_bldgs];
        int room = (int)NAV_MAP_MAX_BLDG_PTS;
        for (int k = 0; k < s_build.n_bldgs; k++) room -= s_build.bldgs[k].n;
        if (room > NAV_MAP_PTS_PER_FEATURE) room = NAV_MAP_PTS_PER_FEATURE;
        if (room < 3) { s_rej_bldg_pts++; return true; } /* 多边形至少三点 */

        const int n = parse_points(json, len, &i, b->pts, room);
        if (n < 3) { s_rej_bldg_pts++; return true; }
        b->n = (uint8_t)n;
        s_build.n_bldgs++;
        return true;
    }

    /* kind == NAV_MAP_MSG_ROUTE */
    s_cnt_route++;
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

    /*
     * 累积超时：**以"最后一条消息"为基准**，只有真的收不到数据了才丢弃。
     * 见 s_build_last_ms 的说明 —— 按"窗口开始时刻"计时会在慢速传输中途
     * 误判超时，进而清空半截窗口。
     */
    if (s_building && (uint32_t)(now_ms - s_build_last_ms) > NAV_MAP_BUILD_TIMEOUT_MS) {
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

/** 分类计数（排查用）：依次写入 begin/road/bldg/route/end 的条数 */
void nav_map_counts(uint32_t *begin, uint32_t *road, uint32_t *bldg,
                    uint32_t *route, uint32_t *end)
{
    if (begin) *begin = s_cnt_begin;
    if (road) *road = s_cnt_road;
    if (bldg) *bldg = s_cnt_bldg;
    if (route) *route = s_cnt_route;
    if (end) *end = s_cnt_end;
}

/** 被拒条数（排查用）：道路满/道路点数不足/建筑满/建筑点数不足 */
void nav_map_rejects(uint32_t *road_full, uint32_t *road_pts,
                     uint32_t *bldg_full, uint32_t *bldg_pts)
{
    if (road_full) *road_full = s_rej_road_full;
    if (road_pts) *road_pts = s_rej_road_pts;
    if (bldg_full) *bldg_full = s_rej_bldg_full;
    if (bldg_pts) *bldg_pts = s_rej_bldg_pts;
}

/** 最近收到的消息类型顺序（排查乱序用）。返回写入的条数。 */
uint32_t nav_map_order(const uint8_t **out)
{
    if (out) *out = s_order;
    return s_order_n;
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
