/**
 * test_nav.c —— 导航卡片纯逻辑单元测试（PC 上运行，不需要硬件/LVGL）
 *
 * 运行：cd tests && make test_nav && ./test_nav
 * 覆盖：距离格式化（各量级/边界/无效）、动作与状态枚举映射、
 *       文案与显示条件、非法值钳制。
 */
#include <stdio.h>
#include <string.h>

#include "nav.h"

static int g_pass, g_fail;

#define CHECK(cond, fmt, ...)                                                  \
    do {                                                                       \
        if (cond) {                                                            \
            g_pass++;                                                          \
        }                                                                      \
        else {                                                                 \
            g_fail++;                                                          \
            printf("  ✗ %s:%d  " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__);  \
        }                                                                      \
    } while (0)

/* 取格式化结果做比较（buf 足够大）*/
static const char *fmt(int meters)
{
    static char b[32];
    nav_format_distance(meters, b, sizeof(b));
    return b;
}

static const char *fmtval(int meters)
{
    static char b[32];
    nav_format_distance_value(meters, b, sizeof(b));
    return b;
}

int main(void)
{
    printf("=== 导航卡片纯逻辑 ===\n");

    /* ---- 距离格式化：米级 ---- */
    printf("--- 距离格式化 ---\n");
    CHECK(strcmp(fmt(0), "0 m") == 0, "0 → \"0 m\"（实际 \"%s\"）", fmt(0));
    CHECK(strcmp(fmt(5), "5 m") == 0, "5 → \"5 m\"");
    CHECK(strcmp(fmt(180), "180 m") == 0, "180 → \"180 m\"");
    CHECK(strcmp(fmt(500), "500 m") == 0, "500 → \"500 m\"");
    CHECK(strcmp(fmt(999), "999 m") == 0, "999 → \"999 m\"（不上取整为 km）");

    /* ---- 公里级（1.0 ~ 9.9）---- */
    CHECK(strcmp(fmt(1000), "1.0 km") == 0, "1000 → \"1.0 km\"（实际 \"%s\"）", fmt(1000));
    CHECK(strcmp(fmt(1250), "1.3 km") == 0, "1250 → \"1.3 km\"（四舍五入，实际 \"%s\"）", fmt(1250));
    CHECK(strcmp(fmt(1240), "1.2 km") == 0, "1240 → \"1.2 km\"（实际 \"%s\"）", fmt(1240));
    CHECK(strcmp(fmt(9949), "9.9 km") == 0, "9949 → \"9.9 km\"（实际 \"%s\"）", fmt(9949));

    /* ---- 十公里以上：整数 km ---- */
    CHECK(strcmp(fmt(10000), "10 km") == 0, "10000 → \"10 km\"（实际 \"%s\"）", fmt(10000));
    CHECK(strcmp(fmt(12500), "13 km") == 0, "12500 → \"13 km\"（四舍五入，实际 \"%s\"）", fmt(12500));
    CHECK(strcmp(fmt(12400), "12 km") == 0, "12400 → \"12 km\"（实际 \"%s\"）", fmt(12400));

    /* ---- 无效 / 越界 ---- */
    CHECK(strcmp(fmt(NAV_DISTANCE_INVALID), "--") == 0, "无效距离 → \"--\"（实际 \"%s\"）",
          fmt(NAV_DISTANCE_INVALID));
    CHECK(strcmp(fmt(-99), "--") == 0, "负距离 → \"--\"");
    CHECK(strcmp(fmt(NAV_DISTANCE_MAX_M + 1), "--") == 0, "超上限 → \"--\"");
    CHECK(strcmp(fmt(NAV_DISTANCE_MAX_M), "600 km") == 0, "恰好等于上限 → 可显示（实际 \"%s\"）",
          fmt(NAV_DISTANCE_MAX_M));

    /* ---- 数值与单位分离（UI 用大号字体显示数值）---- */
    printf("--- 数值/单位分离 ---\n");
    CHECK(strcmp(fmtval(500), "500") == 0, "数值 500（实际 \"%s\"）", fmtval(500));
    CHECK(strcmp(fmtval(1250), "1.3") == 0, "数值 1.3（与合并格式一致，实际 \"%s\"）", fmtval(1250));
    CHECK(strcmp(fmtval(12500), "13") == 0, "数值 13（实际 \"%s\"）", fmtval(12500));
    CHECK(strcmp(fmtval(-1), "--") == 0, "无效数值 → \"--\"");

    CHECK(strcmp(nav_distance_unit(500), "m") == 0, "单位 500 → m");
    CHECK(strcmp(nav_distance_unit(999), "m") == 0, "单位 999 → m");
    CHECK(strcmp(nav_distance_unit(1000), "km") == 0, "单位 1000 → km");
    CHECK(strcmp(nav_distance_unit(NAV_DISTANCE_INVALID), "") == 0,
          "无效距离单位为空（避免显示 \"-- km\" 这类误导）");

    /* ---- 动作枚举 ---- */
    printf("--- 动作与状态 ---\n");
    CHECK(NAV_MANEUVER_COUNT == 8, "共 8 种动作（对齐评估文档 §3.1）");
    CHECK(strcmp(nav_maneuver_name(NAV_MANEUVER_STRAIGHT), "straight") == 0, "直行名");
    CHECK(strcmp(nav_maneuver_name(NAV_MANEUVER_U_TURN), "u_turn") == 0, "掉头名");
    CHECK(strcmp(nav_maneuver_name(NAV_MANEUVER_ARRIVE), "arrive") == 0, "到达名");
    CHECK(strcmp(nav_maneuver_name(NAV_MANEUVER_UNKNOWN), "unknown") == 0, "未知名");
    CHECK(strcmp(nav_maneuver_name(NAV_MANEUVER_KEEP_LEFT), "keep_left") == 0, "靠左名");
    CHECK(strcmp(nav_maneuver_name(NAV_MANEUVER_KEEP_RIGHT), "keep_right") == 0, "靠右名");

    /* 动作钳制：越界一律 UNKNOWN，防止数组越界 */
    CHECK(nav_clamp_maneuver(0) == NAV_MANEUVER_UNKNOWN, "钳制 0");
    CHECK(nav_clamp_maneuver(7) == NAV_MANEUVER_ARRIVE, "钳制 7（到达）");
    CHECK(nav_clamp_maneuver(8) == NAV_MANEUVER_UNKNOWN, "钳制 8（越界）→ UNKNOWN");
    CHECK(nav_clamp_maneuver(-1) == NAV_MANEUVER_UNKNOWN, "钳制 -1 → UNKNOWN");
    CHECK(nav_clamp_maneuver(999) == NAV_MANEUVER_UNKNOWN, "钳制 999 → UNKNOWN");

    /* ---- 状态 ---- */
    CHECK(strcmp(nav_state_name(NAV_STATE_ACTIVE), "active") == 0, "导航中名");
    CHECK(strcmp(nav_state_name(NAV_STATE_STALE), "stale") == 0, "过期名");
    CHECK(strcmp(nav_state_text(NAV_STATE_ACTIVE), "NAV") == 0, "导航中文案");
    CHECK(strcmp(nav_state_text(NAV_STATE_REROUTING), "REROUTE") == 0, "重规划文案");
    CHECK(strcmp(nav_state_text(NAV_STATE_ARRIVED), "ARRIVED") == 0, "到达文案");
    CHECK(strcmp(nav_state_text(NAV_STATE_STALE), "STALE") == 0, "过期文案");
    CHECK(strcmp(nav_state_text(NAV_STATE_IDLE), "NO NAV") == 0, "未开始文案");
    CHECK(strcmp(nav_state_text(NAV_STATE_NO_DATA), "NO DATA") == 0, "无数据文案");

    /* 文案必须是 ASCII（label_26 字体无 CJK 字形，写中文会显示乱码）*/
    for (int i = 0; i < NAV_STATE_COUNT; i++) {
        const char *t = nav_state_text((nav_state_t)i);
        int ascii = 1;
        for (const char *p = t; *p != '\0'; p++) {
            if ((unsigned char)*p > 0x7F) { ascii = 0; break; }
        }
        CHECK(ascii, "状态文案必须为 ASCII（第 %d 项）", i);
    }

    /* 状态钳制：越界保守处理为"无数据"（不假装在导航）*/
    CHECK(nav_clamp_state(NAV_STATE_ACTIVE) == NAV_STATE_ACTIVE, "钳制合法状态");
    CHECK(nav_clamp_state(NAV_STATE_COUNT) == NAV_STATE_NO_DATA, "钳制越界 → NO_DATA");
    CHECK(nav_clamp_state(-5) == NAV_STATE_NO_DATA, "钳制负值 → NO_DATA");

    /* ---- 显示条件：过期/无数据时不显示箭头（不得用旧指引误导）---- */
    printf("--- 显示条件 ---\n");
    CHECK(nav_shows_maneuver(NAV_STATE_ACTIVE), "导航中 → 显示箭头");
    CHECK(nav_shows_maneuver(NAV_STATE_REROUTING), "重规划 → 显示箭头");
    CHECK(!nav_shows_maneuver(NAV_STATE_IDLE), "未开始 → 不显示箭头");
    CHECK(nav_shows_maneuver(NAV_STATE_ARRIVED), "到达 → 显示「目的地」图标");
    CHECK(!nav_shows_maneuver(NAV_STATE_STALE), "过期 → **不显示**箭头（安全）");
    CHECK(!nav_shows_maneuver(NAV_STATE_NO_DATA), "无数据 → **不显示**箭头（安全）");

    /* ---- 缓冲区边界（不得越界写）---- */
    printf("--- 缓冲区边界 ---\n");
    {
        char small[4];
        memset(small, 'X', sizeof(small));
        nav_format_distance(12500, small, sizeof(small));
        CHECK(small[sizeof(small) - 1] == '\0', "小缓冲仍以 NUL 结尾");
        CHECK(strlen(small) <= 3, "小缓冲不越界（内容 \"%s\"）", small);

        char one[1] = { 'X' };
        nav_format_distance(500, one, 1); /* sz=1：只写 NUL */
        CHECK(one[0] == '\0', "sz=1 时写 NUL");

        nav_format_distance(500, NULL, 0);        /* 不应崩溃 */
        nav_format_distance_value(500, NULL, 0);  /* 不应崩溃 */
        CHECK(1, "NULL 入参不崩溃");
    }
    /* ---- 导航开始时"自动跳到导航表盘"的判定（纯函数）---- */
    printf("--- 自动跳转的上升沿判定 ---\n");
    /* 真的"开始"：idle/no_data → active/rerouting */
    CHECK(nav_should_auto_jump(NAV_STATE_IDLE,    NAV_STATE_ACTIVE),    "未开始 → 导航中：应跳");
    CHECK(nav_should_auto_jump(NAV_STATE_NO_DATA, NAV_STATE_ACTIVE),    "无数据 → 导航中：应跳");
    CHECK(nav_should_auto_jump(NAV_STATE_IDLE,    NAV_STATE_REROUTING), "未开始 → 重算中：应跳");
    CHECK(nav_should_auto_jump(NAV_STATE_NO_DATA, NAV_STATE_REROUTING), "无数据 → 重算中：应跳");

    /* 不是"开始"：数据晚到/到达后继续 —— 用户可能已手动划走，不该拽回来 */
    CHECK(!nav_should_auto_jump(NAV_STATE_STALE,  NAV_STATE_ACTIVE),    "过期 → 导航中：**不**跳（数据晚到而已）");
    CHECK(!nav_should_auto_jump(NAV_STATE_ARRIVED, NAV_STATE_ACTIVE),   "已到达 → 导航中：**不**跳");
    CHECK(!nav_should_auto_jump(NAV_STATE_ACTIVE, NAV_STATE_REROUTING), "导航中 → 重算中：**不**跳（同一次导航）");
    CHECK(!nav_should_auto_jump(NAV_STATE_REROUTING, NAV_STATE_ACTIVE),"重算中 → 导航中：**不**跳（同一次导航）");
    CHECK(!nav_should_auto_jump(NAV_STATE_ACTIVE, NAV_STATE_ACTIVE),    "导航中 → 导航中：**不**跳（每几秒一条数据）");

    /* 结束方向不应触发 */
    CHECK(!nav_should_auto_jump(NAV_STATE_ACTIVE, NAV_STATE_IDLE),      "导航中 → 未开始：不跳");
    CHECK(!nav_should_auto_jump(NAV_STATE_ACTIVE, NAV_STATE_ARRIVED),   "导航中 → 已到达：不跳");
    CHECK(!nav_should_auto_jump(NAV_STATE_IDLE,   NAV_STATE_IDLE),      "未开始 → 未开始：不跳");
    CHECK(!nav_should_auto_jump(NAV_STATE_IDLE,   NAV_STATE_STALE),     "未开始 → 过期：不跳");


    printf("结果: %d 通过, %d 失败\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
