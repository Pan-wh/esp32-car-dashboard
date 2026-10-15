/**
 * test_nav_map.c —— 地图窗口解析单元测试（PC 上运行，不需硬件/LVGL）
 *
 * 覆盖：消息分流（与 nav/遥测区分）、点数组解析、预算限制、
 *       原子切换（mw…me）、累积超时、窗口过期、畸形输入安全。
 */
#include <stdio.h>
#include <string.h>

#include "nav_map.h"

static int g_pass, g_fail;

#define CHECK(cond, fmt, ...)                                                  \
    do {                                                                       \
        if (cond) { g_pass++; }                                                \
        else {                                                                 \
            g_fail++;                                                          \
            printf("  ✗ %s:%d  " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__);  \
        }                                                                      \
    } while (0)

static bool H(const char *j) { return nav_map_handle(j, strlen(j)); }

/** 走完一个最小可用窗口：一条 2 点道路 */
static void feed_min_window(void)
{
    nav_map_init();
    H("{\"t\":\"mw\"}");
    H("{\"t\":\"mr\",\"c\":1,\"p\":[[0,0],[100,0]]}");
    H("{\"t\":\"me\"}");
}

int main(void)
{
    printf("=== 地图窗口解析 ===\n");

    /* ---- 消息分流 ---- */
    printf("--- 消息分流 ---\n");
    nav_map_init();
    CHECK(H("{\"t\":\"mw\"}"), "mw 被认作地图消息");
    CHECK(H("{\"t\":\"me\"}"), "me 被认作地图消息");
    CHECK(H("{\"t\":\"mr\",\"p\":[[0,0],[1,1]]}"), "mr 被认作地图消息");
    CHECK(H("{\"t\":\"mb\",\"p\":[[0,0],[1,0],[1,1]]}"), "mb 被认作地图消息");
    CHECK(H("{\"t\":\"mp\",\"p\":[[0,0],[1,1]]}"), "mp 被认作地图消息");
    CHECK(!H("{\"t\":\"nav\",\"state\":1}"), "nav 不是地图消息（交给导航解析器）");
    CHECK(!H("{\"v\":1,\"speed_mps\":10}"), "遥测包不是地图消息");
    CHECK(!H("{\"t\":\"xx\"}"), "未知 t 不是地图消息");
    CHECK(!H("{}"), "空对象不是地图消息");
    CHECK(!H("not json"), "非 JSON 不是地图消息");
    CHECK(!nav_map_handle(NULL, 0), "NULL 安全");
    CHECK(!H("{"), "截断输入安全");

    /* ---- 基本窗口 ---- */
    printf("--- 基本窗口 ---\n");
    feed_min_window();
    const nav_map_window_t *w = nav_map_active();
    CHECK(w != NULL, "收到 me 后窗口可用");
    CHECK(w && w->n_roads == 1, "道路 1 条（实际 %d）", w ? w->n_roads : -1);
    CHECK(w && w->roads[0].n == 2, "道路点数 2（实际 %d）", w ? w->roads[0].n : -1);
    CHECK(w && w->roads[0].cls == 1, "道路等级 1");
    CHECK(w && w->roads[0].pts[1].x == 100, "第 2 点 x=100");
    CHECK(w && w->roads[0].pts[1].y == 0, "第 2 点 y=0");
    CHECK(nav_map_window_count() == 1, "窗口计数 1");

    /* ---- 原子性：me 之前不切换 ---- */
    printf("--- 原子切换 ---\n");
    feed_min_window();
    H("{\"t\":\"mw\"}");
    H("{\"t\":\"mr\",\"c\":2,\"p\":[[1,1],[2,2]]}");
    const nav_map_window_t *mid = nav_map_active();
    CHECK(mid && mid->n_roads == 1, "累积中不切换：仍是上一条窗口");
    CHECK(mid && mid->roads[0].cls == 1, "累积中：旧窗口内容未变");
    H("{\"t\":\"me\"}");
    const nav_map_window_t *after = nav_map_active();
    CHECK(after && after->roads[0].cls == 2, "me 之后才切换到新窗口");
    CHECK(after && after->roads[0].pts[0].x == 1, "新窗口内容正确");

    /* ---- 多条道路 / 建筑 / 路线 ---- */
    printf("--- 多要素 ----\n");
    nav_map_init();
    H("{\"t\":\"mw\"}");
    for (int k = 0; k < 5; k++) {
        char buf[96];
        snprintf(buf, sizeof(buf), "{\"t\":\"mr\",\"c\":%d,\"p\":[[%d,0],[%d,10]]}", k % 6, k, k + 1);
        H(buf);
    }
    H("{\"t\":\"mb\",\"p\":[[0,0],[10,0],[10,10]]}");
    H("{\"t\":\"mb\",\"p\":[[20,20],[30,20],[30,30]]}");
    H("{\"t\":\"mp\",\"p\":[[0,0],[5,-5],[10,-20]]}");
    H("{\"t\":\"me\"}");
    w = nav_map_active();
    CHECK(w && w->n_roads == 5, "5 条道路（实际 %d）", w ? w->n_roads : -1);
    CHECK(w && w->n_bldgs == 2, "2 栋建筑（实际 %d）", w ? w->n_bldgs : -1);
    CHECK(w && w->n_route == 3, "路线 3 点（实际 %d）", w ? w->n_route : -1);
    CHECK(w && w->bldgs[1].pts[1].x == 30, "建筑点坐标正确");
    CHECK(w && w->route[2].y == -20, "路线负坐标正确");

    /* ---- 负数与极值 ---- */
    printf("--- 数值边界 ---\n");
    nav_map_init();
    H("{\"t\":\"mw\"}");
    H("{\"t\":\"mr\",\"c\":0,\"p\":[[-1500,-1500],[1500,1500]]}");
    H("{\"t\":\"mr\",\"c\":9,\"p\":[[-32768,32767],[0,0]]}");
    H("{\"t\":\"mr\",\"c\":-3,\"p\":[[40000,-40000],[0,0]]}");
    H("{\"t\":\"me\"}");
    w = nav_map_active();
    CHECK(w && w->roads[0].pts[0].x == -1500, "负坐标正确");
    CHECK(w && w->roads[1].cls == 5, "等级越界(9)夹到 5");
    CHECK(w && w->roads[2].cls == 5, "等级为负夹到 5");
    CHECK(w && w->roads[2].pts[0].x == 32767, "超大 x 夹到 32767（不溢出）");
    CHECK(w && w->roads[2].pts[0].y == -32768, "超小 y 夹到 -32768");

    /* ---- 预算限制 ---- */
    printf("--- 预算限制 ---\n");
    nav_map_init();
    H("{\"t\":\"mw\"}");
    for (int k = 0; k < NAV_MAP_MAX_ROADS + 10; k++) {
        H("{\"t\":\"mr\",\"c\":1,\"p\":[[0,0],[1,1]]}");
    }
    H("{\"t\":\"me\"}");
    w = nav_map_active();
    CHECK(w && w->n_roads == NAV_MAP_MAX_ROADS, "道路数被限制在 %d（实际 %d）",
          NAV_MAP_MAX_ROADS, w ? w->n_roads : -1);

    nav_map_init();
    H("{\"t\":\"mw\"}");
    for (int k = 0; k < NAV_MAP_MAX_BLDGS + 10; k++) {
        H("{\"t\":\"mb\",\"p\":[[0,0],[1,0],[1,1]]}");
    }
    H("{\"t\":\"me\"}");
    w = nav_map_active();
    CHECK(w && w->n_bldgs == NAV_MAP_MAX_BLDGS, "建筑数被限制在 %d（实际 %d）",
          NAV_MAP_MAX_BLDGS, w ? w->n_bldgs : -1);

    /* 单条要素点数超上限 → 截断到上限，不越界 */
    nav_map_init();
    H("{\"t\":\"mw\"}");
    {
        char buf[1024];
        int o = snprintf(buf, sizeof(buf), "{\"t\":\"mr\",\"c\":1,\"p\":[");
        for (int k = 0; k < 40; k++) {
            o += snprintf(buf + o, sizeof(buf) - (size_t)o, "%s[%d,%d]", k ? "," : "", k, k);
        }
        snprintf(buf + o, sizeof(buf) - (size_t)o, "]}");
        H(buf);
    }
    H("{\"t\":\"me\"}");
    w = nav_map_active();
    CHECK(w && w->roads[0].n == NAV_MAP_PTS_PER_FEATURE,
          "单条要素点数被截断到 %d（实际 %d）", NAV_MAP_PTS_PER_FEATURE,
          w ? w->roads[0].n : -1);

    /* ---- 畸形输入 ---- */
    printf("--- 畸形输入 ---\n");
    nav_map_init();
    CHECK(H("{\"t\":\"mr\"}"), "缺 p 字段：仍被认作地图消息");
    CHECK(nav_map_active() == NULL, "缺 p 不产生窗口");
    CHECK(H("{\"t\":\"mr\",\"p\":[]}"), "空点数组安全");
    CHECK(H("{\"t\":\"mr\",\"p\":[[0]]}"), "点数不足的点安全");
    CHECK(H("{\"t\":\"mr\",\"p\":[[0,0]]}"), "只有 1 点：画不出线，忽略");
    CHECK(H("{\"t\":\"mr\",\"p\":[[0,0],]}"), "尾部多余逗号安全");
    CHECK(H("{\"t\":\"mr\",\"p\":[[a,b],[1,1]]}"), "非数字坐标安全");
    CHECK(H("{\"t\":\"mr\",\"c\":\"x\",\"p\":[[0,0],[1,1]]}"), "c 不是数字安全");
    CHECK(H("{\"t\":\"mw\"}"), "再来一次 mw 重置累积");
    /*
     * 用"一条有效道路 + 一个坏多边形"来验证坏多边形被忽略。
     * 不能只发坏多边形：那样窗口会变成**空窗口**，而空窗口现在的语义是
     * "清除地图"（见下面的空窗口测试），拿不到 nav_map_active() 来断言。
     */
    H("{\"t\":\"mr\",\"c\":1,\"p\":[[0,0],[10,0]]}");
    H("{\"t\":\"mb\",\"p\":[[0,0],[1,0]]}");
    H("{\"t\":\"me\"}");
    w = nav_map_active();
    CHECK(w && w->n_roads == 1, "有效道路仍在");
    CHECK(w && w->n_bldgs == 0, "只有 2 点的多边形被忽略");

    /* ---- 未知字段（向前兼容） ---- */
    printf("--- 向前兼容 ---\n");
    nav_map_init();
    H("{\"t\":\"mw\",\"future\":123}");
    H("{\"t\":\"mr\",\"c\":3,\"future\":\"x\",\"p\":[[0,0],[9,9]],\"z\":[1,2,3]}");
    H("{\"t\":\"me\",\"extra\":{\"a\":1}}");
    w = nav_map_active();
    CHECK(w && w->n_roads == 1 && w->roads[0].pts[1].x == 9, "未知字段不影响解析");
    CHECK(w && w->roads[0].cls == 3, "c 在未知字段之后仍能解析");

    /* 字段顺序无关 */
    nav_map_init();
    H("{\"p\":[[1,2],[3,4]],\"c\":2,\"t\":\"mr\"}"); /* t 放最后 */
    H("{\"t\":\"me\"}");
    w = nav_map_active();
    CHECK(w && w->n_roads == 1 && w->roads[0].cls == 2 && w->roads[0].pts[0].x == 1,
          "字段顺序无关（t 在最后也能识别）");

    /* ---- 累积超时 ---- */
    printf("--- 超时与过期 ---\n");
    nav_map_init();
    nav_map_tick(1000);
    H("{\"t\":\"mw\"}");
    H("{\"t\":\"mr\",\"c\":1,\"p\":[[0,0],[1,1]]}");
    nav_map_tick(2000);
    CHECK(nav_map_active() == NULL, "未收到 me：不可用");
    nav_map_tick(1000 + NAV_MAP_BUILD_TIMEOUT_MS + 1);
    CHECK(nav_map_drop_count() == 1, "累积超时被丢弃并计数");
    H("{\"t\":\"me\"}");
    CHECK(nav_map_active() == NULL, "超时后的 me 不会凭空造出窗口");

    /* ---- 窗口过期 ---- */
    nav_map_init();
    nav_map_tick(1000);
    H("{\"t\":\"mw\"}");
    H("{\"t\":\"mr\",\"c\":1,\"p\":[[0,0],[1,1]]}");
    H("{\"t\":\"me\"}");
    CHECK(nav_map_active() != NULL, "刚完成：可用");
    nav_map_tick(1000 + 5000);
    CHECK(nav_map_active() != NULL, "5 秒后仍可用");
    nav_map_tick(1000 + 60001);
    CHECK(nav_map_active() == NULL, "超过 60 秒：过期，隐藏地图层");

    /* 过期后新窗口可恢复 */
    H("{\"t\":\"mw\"}");
    H("{\"t\":\"mr\",\"c\":1,\"p\":[[0,0],[2,2]]}");
    H("{\"t\":\"me\"}");
    w = nav_map_active();
    CHECK(w != NULL && w->roads[0].pts[1].x == 2, "过期后新窗口可恢复");

    /* ---- 缺 mw 直接发要素（容错） ---- */
    printf("--- 容错 ---\n");
    nav_map_init();
    H("{\"t\":\"mr\",\"c\":1,\"p\":[[5,5],[6,6]]}");
    H("{\"t\":\"me\"}");
    w = nav_map_active();
    CHECK(w && w->n_roads == 1, "缺 mw 也能成立（容错）");

    /* ---- 空窗口 = 清除地图（手机停止导航时会发 mw+me）---- */
    printf("--- 空窗口语义 ---\n");
    feed_min_window();
    CHECK(nav_map_active() != NULL, "先有一个有效窗口");
    {
        const uint32_t w_before = nav_map_window_count();
        H("{\"t\":\"mw\"}");
        H("{\"t\":\"me\"}");
        CHECK(nav_map_active() == NULL, "空窗口应清除地图（而不是显示一张空白地图）");
        CHECK(nav_map_window_count() == w_before, "空窗口不计入有效窗口数");
        int ox = 0, oy = 0;
        CHECK(!nav_map_get_vehicle_offset(&ox, &oy), "空窗口后自车偏移失效");
    }
    /* 空窗口后仍能正常建立新窗口 */
    feed_min_window();
    CHECK(nav_map_active() != NULL, "空窗口之后仍可建立新窗口");

    printf("\n结果: %d 通过, %d 失败\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
