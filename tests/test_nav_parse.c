/**
 * test_nav_parse.c —— 导航消息解析单元测试（PC 上运行，不需硬件/LVGL）
 *
 * 覆盖：类型分流（与遥测包区分）、字段解析、缺失/null/越界/畸形输入、
 *       向前兼容（未知字段不影响解析）、缓冲区安全。
 */
#include <stdio.h>
#include <string.h>

#include "nav_parse.h"

static int g_pass, g_fail;

#define CHECK(cond, fmt, ...)                                                  \
    do {                                                                       \
        if (cond) { g_pass++; }                                                \
        else {                                                                 \
            g_fail++;                                                          \
            printf("  ✗ %s:%d  " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__);  \
        }                                                                      \
    } while (0)

static bool PARSE(const char *j, nav_msg_t *m) { return nav_parse(j, strlen(j), m); }
static bool ISNAV(const char *j) { return nav_is_nav_message(j, strlen(j)); }

int main(void)
{
    nav_msg_t m;
    printf("=== 导航消息解析 ===\n");

    /* ---- 类型分流 ---- */
    printf("--- 类型分流 ---\n");
    CHECK(ISNAV("{\"t\":\"nav\",\"state\":1}"), "识别导航消息");
    CHECK(!ISNAV("{\"v\":1,\"speed_mps\":10}"), "遥测包不是导航消息");
    CHECK(!ISNAV("{\"t\":\"tel\",\"state\":1}"), "t=tel 不是导航消息");
    CHECK(!ISNAV("{}"), "空对象不是导航消息");
    CHECK(!ISNAV("not json"), "非 JSON 不是导航消息");
    CHECK(!nav_is_nav_message(NULL, 0), "NULL 不是导航消息");
    CHECK(ISNAV("{\"state\":1,\"t\":\"nav\"}"), "t 在后面也能识别（顺序无关）");

    /* ---- 基本解析 ---- */
    printf("--- 基本解析 ---\n");
    CHECK(PARSE("{\"t\":\"nav\",\"state\":1,\"maneuver\":2,\"dist_m\":500}", &m), "完整消息可解析");
    CHECK(m.has_state && m.state == NAV_STATE_ACTIVE, "状态=导航中");
    CHECK(m.has_maneuver && m.maneuver == NAV_MANEUVER_LEFT, "动作=左转");
    CHECK(m.has_dist && m.distance_m == 500, "距离=500");

    /* 非导航消息必须被拒（否则遥测包会被当成导航）*/
    CHECK(!PARSE("{\"v\":1,\"speed_mps\":10}", &m), "遥测包被拒");
    CHECK(!PARSE("{\"t\":\"tel\",\"state\":1}", &m), "t=tel 被拒");

    /* ---- 部分字段（缺失 ≠ 无效）---- */
    printf("--- 部分字段 ---\n");
    CHECK(PARSE("{\"t\":\"nav\",\"state\":3}", &m), "只有 state 也可解析");
    CHECK(m.has_state && m.state == NAV_STATE_ARRIVED, "状态=已到达");
    CHECK(!m.has_maneuver, "未提供动作 → has_maneuver=false（保留旧值）");
    CHECK(!m.has_dist, "未提供距离 → has_dist=false（保留旧值）");

    CHECK(PARSE("{\"t\":\"nav\",\"maneuver\":4}", &m), "只有 maneuver");
    CHECK(m.has_maneuver && m.maneuver == NAV_MANEUVER_U_TURN, "动作=掉头");

    CHECK(PARSE("{\"t\":\"nav\",\"dist_m\":0}", &m), "只有 dist_m=0");
    CHECK(m.has_dist && m.distance_m == 0, "距离 0 有效（即将转向）");

    /* 一个字段都没有的导航包 → 拒绝（避免空包清状态）*/
    CHECK(!PARSE("{\"t\":\"nav\"}", &m), "空导航包被拒");
    CHECK(!PARSE("{\"t\":\"nav\",\"unknown\":5}", &m), "只有未知字段被拒");

    /* ---- null 与类型错误 ---- */
    printf("--- null / 类型错误 ---\n");
    CHECK(PARSE("{\"t\":\"nav\",\"dist_m\":null}", &m), "dist_m=null 可解析");
    CHECK(m.has_dist && m.distance_m == NAV_DISTANCE_INVALID,
          "null 表示**明确无效**（与缺失不同）");

    CHECK(PARSE("{\"t\":\"nav\",\"state\":1,\"dist_m\":\"500\"}", &m), "距离为字符串时仍可解析");
    CHECK(!m.has_dist, "字符串型距离 → 未提供（不误解析）");

    /* ---- 越界值：钳制而非崩溃 ---- */
    printf("--- 越界钳制 ---\n");
    CHECK(PARSE("{\"t\":\"nav\",\"state\":99}", &m), "越界 state 可解析");
    CHECK(m.state == NAV_STATE_NO_DATA, "越界 state → NO_DATA（保守：不假装在导航）");

    CHECK(PARSE("{\"t\":\"nav\",\"state\":-5}", &m), "负 state 可解析");
    CHECK(m.state == NAV_STATE_NO_DATA, "负 state → NO_DATA");

    CHECK(PARSE("{\"t\":\"nav\",\"maneuver\":99}", &m), "越界 maneuver 可解析");
    CHECK(m.maneuver == NAV_MANEUVER_UNKNOWN, "越界 maneuver → UNKNOWN");

    CHECK(PARSE("{\"t\":\"nav\",\"dist_m\":-1}", &m), "负距离可解析");
    CHECK(m.has_dist && m.distance_m == NAV_DISTANCE_INVALID, "负距离 → 无效");

    CHECK(PARSE("{\"t\":\"nav\",\"dist_m\":999999}", &m), "超大距离可解析");
    CHECK(m.distance_m == NAV_DISTANCE_INVALID, "超上限 → 无效（不截断成误导值）");

    CHECK(PARSE("{\"t\":\"nav\",\"dist_m\":600000}", &m), "恰好等于上限");
    CHECK(m.distance_m == 600000, "上限值有效");

    /* ---- 向前兼容：未知字段不影响已知字段 ---- */
    printf("--- 向前兼容 ---\n");
    CHECK(PARSE("{\"t\":\"nav\",\"state\":1,\"future_field\":123,\"dist_m\":250}", &m),
          "含未知字段仍可解析（未来加字段不破坏）");
    CHECK(m.state == NAV_STATE_ACTIVE && m.distance_m == 250, "已知字段正确");
    CHECK(PARSE("{\"t\":\"nav\",\"extra\":{\"nested\":1},\"state\":2}", &m),
          "含嵌套对象仍可解析");
    CHECK(m.state == NAV_STATE_REROUTING, "嵌套对象后的字段正确");

    /* ---- 空白与格式宽容 ---- */
    printf("--- 格式宽容 ---\n");
    CHECK(PARSE("  {  \"t\" : \"nav\" ,  \"state\" : 1  }  ", &m), "多余空白可解析");
    CHECK(m.state == NAV_STATE_ACTIVE, "空白不影响取值");
    CHECK(PARSE("{ \"t\":\"nav\", \"state\":4 }", &m), "带空格可解析");
    CHECK(m.state == NAV_STATE_STALE, "状态=过期");

    /* ---- 畸形输入不得崩溃 ---- */
    printf("--- 畸形输入 ---\n");
    CHECK(!PARSE("{\"t\":\"nav\",\"state\":", &m), "缺值 → 拒绝");
    CHECK(!PARSE("{\"t\":\"nav\",\"state\" 1}", &m), "缺冒号 → 拒绝");
    CHECK(!PARSE("{\"t\":\"nav\"", &m), "未闭合 → 拒绝");
    CHECK(!PARSE("", &m), "空串 → 拒绝");
    CHECK(!PARSE("{", &m), "只有左括号 → 拒绝");
    CHECK(!nav_parse(NULL, 0, &m), "NULL 输入 → 拒绝");
    CHECK(!nav_parse("{\"t\":\"nav\",\"state\":1}", 5, &m), "截断输入 → 拒绝");
    CHECK(!nav_parse("{\"t\":\"nav\",\"state\":1}", 22, NULL), "NULL 输出 → 拒绝");

    /* 截断到一半的包不应改写输出（先设置哨兵）*/
    m.has_state = true;
    m.state = NAV_STATE_ACTIVE;
    (void)nav_parse("{\"t\":\"nav\",\"state\":", 19, &m);
    CHECK(m.has_state && m.state == NAV_STATE_ACTIVE, "解析失败时输出未被破坏");

    /* ---- 全部状态/动作枚举都能解析（防漏配）---- */
    printf("--- 枚举全覆盖 ---\n");
    for (int st = 0; st < NAV_STATE_COUNT; st++) {
        char buf[64];
        snprintf(buf, sizeof(buf), "{\"t\":\"nav\",\"state\":%d}", st);
        CHECK(PARSE(buf, &m) && m.state == (nav_state_t)st, "状态 %d 可解析", st);
    }
    for (int mv = 0; mv < NAV_MANEUVER_COUNT; mv++) {
        char buf[64];
        snprintf(buf, sizeof(buf), "{\"t\":\"nav\",\"maneuver\":%d}", mv);
        CHECK(PARSE(buf, &m) && m.maneuver == (nav_maneuver_t)mv, "动作 %d 可解析", mv);
    }

    /* ---- 与真实遥测包共存（同一特征下发，必须能区分）---- */
    printf("--- 与遥测包共存 ---\n");
    {
        const char *tel = "{\"v\":1,\"ts_utc_ms\":1791868128536,\"speed_mps\":12.5,"
                          "\"latitude\":37.3341,\"longitude\":-122.0482,"
                          "\"speed_valid\":true,\"location_valid\":true}";
        CHECK(!nav_parse(tel, strlen(tel), &m), "真实遥测包不被当作导航消息");
        CHECK(!nav_is_nav_message(tel, strlen(tel)), "分流函数也正确排除遥测包");
    }

    /* ---- 自车偏移 ox/oy（地图窗口用）---- */
    printf("--- 自车偏移 ox/oy ---\n");
    CHECK(PARSE("{\"t\":\"nav\",\"state\":1,\"ox\":13,\"oy\":-42}", &m), "带偏移的导航包可解析");
    CHECK(m.has_offset, "偏移被标记有效");
    CHECK(m.offset_x_m == 13, "ox=13（实际 %d）", m.offset_x_m);
    CHECK(m.offset_y_m == -42, "oy=-42（实际 %d）", m.offset_y_m);

    CHECK(PARSE("{\"t\":\"nav\",\"state\":1}", &m), "不带偏移也能解析");
    CHECK(!m.has_offset, "缺偏移时 has_offset=false");

    /* 只带偏移、不带指引：也算有效包（换窗口后手机可能只补发一次偏移）*/
    CHECK(PARSE("{\"t\":\"nav\",\"ox\":1,\"oy\":2}", &m), "只有偏移也是有效包");
    CHECK(m.has_offset && m.offset_x_m == 1 && m.offset_y_m == 2, "只有偏移时数值正确");
    CHECK(!m.has_state && !m.has_maneuver && !m.has_dist, "只有偏移时不清掉指引");

    /* 必须成对：只给一个不算有效偏移 */
    CHECK(PARSE("{\"t\":\"nav\",\"state\":1,\"ox\":5}", &m), "只给 ox 仍可解析（state 有效）");
    CHECK(!m.has_offset, "只给 ox：偏移无效（必须成对）");

    /* null → 明确表示本次无偏移 */
    CHECK(PARSE("{\"t\":\"nav\",\"state\":1,\"ox\":null,\"oy\":null}", &m), "null 偏移可解析");
    CHECK(!m.has_offset, "null 偏移 → has_offset=false");

    /* 越界值不夹到边界（避免把车画到屏幕边上）*/
    CHECK(PARSE("{\"t\":\"nav\",\"state\":1,\"ox\":40000,\"oy\":0}", &m), "超大偏移可解析");
    CHECK(!m.has_offset, "超大偏移视为无效，不夹到边界");

    /* 字段顺序无关 */
    CHECK(PARSE("{\"oy\":-7,\"ox\":8,\"t\":\"nav\",\"state\":1}", &m), "偏移字段顺序无关");
    CHECK(m.has_offset && m.offset_x_m == 8 && m.offset_y_m == -7, "顺序无关时数值正确");

    /* 偏移不干扰原有字段 */
    CHECK(PARSE("{\"t\":\"nav\",\"state\":1,\"maneuver\":2,\"dist_m\":500,\"ox\":3,\"oy\":4}", &m),
          "偏移与原有字段共存");
    CHECK(m.has_state && m.state == NAV_STATE_ACTIVE && m.has_maneuver && m.maneuver == NAV_MANEUVER_LEFT,
          "原有字段仍正确");
    CHECK(m.has_dist && m.distance_m == 500 && m.has_offset && m.offset_x_m == 3, "所有字段都对");

    printf("结果: %d 通过, %d 失败\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
