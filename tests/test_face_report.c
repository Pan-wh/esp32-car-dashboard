/**
 * test_face_report.c —— 表盘列表上报测试（PC 上运行，不需要硬件）
 *
 * 运行：cd tests && make test_face_report && ./test_face_report
 *
 * 覆盖：JSON 结构、字段完整性、分组名、启用状态反映、转义、截断、
 *       边界（无表盘 / 缓冲过小 / 空指针）。
 */
#include <stdio.h>
#include <string.h>

#include "face_report.h"
#include "ui_face.h"

static int g_pass, g_fail;

#define CHECK(cond, fmt, ...)                                                  \
    do {                                                                       \
        if (cond) { g_pass++; }                                                \
        else {                                                                 \
            g_fail++;                                                          \
            printf("  ✗ %s:%d  " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__); \
        }                                                                      \
    } while (0)

/* 本文件用的简单断言（CHECK 宏已覆盖大部分；这个用于纯条件） */
#define check_true(cond, msg) CHECK(cond, "%s", msg)

/* 按固件真实顺序注册 5 个表盘（与 ui_pages.c 一致） */
static void D(void *p) { (void)p; }

static void register_five(void)
{
    ui_face_reset();
    ui_face_register(&(ui_face_desc_t){ "analog", "Analog", D, UI_FACE_GROUP_CLOCK });
    ui_face_register(&(ui_face_desc_t){ "digital", "Digital", D, UI_FACE_GROUP_CLOCK });
    ui_face_register(&(ui_face_desc_t){ "speed", "Speed", D, UI_FACE_GROUP_SPEED });
    ui_face_register(&(ui_face_desc_t){ "color", "Color", D, UI_FACE_GROUP_CLOCK });
    ui_face_register(&(ui_face_desc_t){ "nav", "Nav", D, UI_FACE_GROUP_NAV });
}

/** 粗略统计子串出现次数 */
static int count_substr(const char *hay, const char *needle)
{
    int n = 0;
    for (const char *p = hay; (p = strstr(p, needle)) != NULL; p += strlen(needle)) n++;
    return n;
}

int main(void)
{
    char buf[FACE_REPORT_BUF_SIZE];

    printf("=== 上报结构 ===\n");
    register_five();
    int len = face_report_build(buf, sizeof(buf));
    CHECK(len > 0, "应生成非空报告（len=%d）", len);
    CHECK((size_t)len == strlen(buf), "返回长度应等于字符串长度");
    CHECK(len <= FACE_REPORT_MAX_PAYLOAD, "载荷应在单次上限内（%d ≤ %d）", len,
          FACE_REPORT_MAX_PAYLOAD);

    /* 顶层字段 */
    CHECK(strstr(buf, "\"t\":\"faces\"") != NULL, "应带 t=faces 判别字段");
    CHECK(strstr(buf, "\"v\":1") != NULL, "应带协议版本");
    CHECK(strstr(buf, "\"count\":5") != NULL, "应带表盘数");
    CHECK(strstr(buf, "\"mask\":") != NULL, "应带启用掩码");
    CHECK(strstr(buf, "truncated") == NULL, "5 个表盘不应截断");

    /* 每个表盘都有完整字段 */
    CHECK(count_substr(buf, "\"id\":") == 5, "应有 5 个 id 字段，实际 %d",
          count_substr(buf, "\"id\":"));
    CHECK(count_substr(buf, "\"name\":") == 5, "应有 5 个 name");
    CHECK(count_substr(buf, "\"label\":") == 5, "应有 5 个 label");
    CHECK(count_substr(buf, "\"group\":") == 5, "应有 5 个 group");
    CHECK(count_substr(buf, "\"on\":") == 5, "应有 5 个 on");

    /* 顺序与 name 必须与注册顺序一致（App 依赖 name 做本地化） */
    const char *want_order[] = { "analog", "digital", "speed", "color", "nav" };
    const char *cursor = buf;
    for (int i = 0; i < 5; i++) {
        char needle[32];
        snprintf(needle, sizeof(needle), "{\"id\":%d,\"name\":\"%s\"", i, want_order[i]);
        const char *p = strstr(cursor, needle);
        CHECK(p != NULL, "第 %d 项应为 id=%d name=%s", i, i, want_order[i]);
        if (p) cursor = p + strlen(needle);
    }

    printf("=== 当前表盘（cur）===\n");
    /* 未设置时为 -1 */
    check_true(strstr(buf, "\"cur\":-1") != NULL, "未设置时应为 -1");
    /* 设置后应带出，App 据此标注"正在用哪个" */
    face_report_set_current(2);
    len = face_report_build(buf, sizeof(buf));
    CHECK(len > 0 && strstr(buf, "\"cur\":2") != NULL, "设置后应上报 cur=2");
    face_report_set_current(-1);

    printf("=== 分组 ===\n");
    CHECK(strstr(buf, "\"group\":\"clock\"") != NULL, "时钟组应为 clock");
    CHECK(strstr(buf, "\"group\":\"speed\"") != NULL, "速度表组应为 speed");
    CHECK(strstr(buf, "\"group\":\"nav\"") != NULL, "导航组应为 nav");
    CHECK(count_substr(buf, "\"group\":\"clock\"") == 3, "应有 3 个时钟类表盘，实际 %d",
          count_substr(buf, "\"group\":\"clock\""));

    printf("=== 启用状态反映 ===\n");
    /* 全部默认启用 → 掩码 31 */
    CHECK(strstr(buf, "\"mask\":31") != NULL, "5 个全启用掩码应为 31");
    CHECK(count_substr(buf, "\"on\":1") == 5, "应全部 on=1");
    CHECK(count_substr(buf, "\"on\":0") == 0, "不应有 on=0");

    /* 隐藏两个（color 与 nav）*/
    CHECK(ui_face_set_enabled(3, false), "应能隐藏 color");
    CHECK(ui_face_set_enabled(4, false), "应能隐藏 nav");
    len = face_report_build(buf, sizeof(buf));
    CHECK(len > 0, "重生成应成功");
    CHECK(count_substr(buf, "\"on\":1") == 3, "应剩 3 个 on=1，实际 %d",
          count_substr(buf, "\"on\":1"));
    CHECK(count_substr(buf, "\"on\":0") == 2, "应有 2 个 on=0");
    CHECK(strstr(buf, "\"mask\":7") != NULL, "掩码应为 0b00111=7（id 0,1,2 启用）");

    printf("=== 业务规则：不能隐藏最后一个 ===\n");
    ui_face_reset();
    ui_face_register(&(ui_face_desc_t){ "only", "Only", D, UI_FACE_GROUP_CLOCK });
    CHECK(!ui_face_set_enabled(0, false), "唯一表盘不应允许隐藏（由 ui_face 拒绝）");
    len = face_report_build(buf, sizeof(buf));
    CHECK(len > 0 && strstr(buf, "\"on\":1") != NULL, "唯一表盘应仍为启用");

    printf("=== 边界 ===\n");
    ui_face_reset();
    len = face_report_build(buf, sizeof(buf));
    CHECK(len > 0, "无表盘也应生成合法报告（len=%d）", len);
    CHECK(strstr(buf, "\"count\":0") != NULL, "count 应为 0");
    CHECK(strstr(buf, "\"faces\":[]") != NULL, "faces 应为空数组");

    CHECK(face_report_build(NULL, sizeof(buf)) == -1, "buf 为 NULL 应返回 -1");
    CHECK(face_report_build(buf, 0) == -1, "cap 为 0 应返回 -1");
    register_five();
    CHECK(face_report_build(buf, 4) == -1, "缓冲过小应返回 -1（而不是写坏内存）");

    printf("=== 转义 ===\n");
    ui_face_reset();
    ui_face_register(&(ui_face_desc_t){ "we\"ird", "La\\bel", D, UI_FACE_GROUP_CLOCK });
    len = face_report_build(buf, sizeof(buf));
    CHECK(len > 0, "含引号/反斜杠的名字应仍能生成");
    CHECK(strstr(buf, "we\\\"ird") != NULL, "名字里的引号应被转义");
    CHECK(strstr(buf, "La\\\\bel") != NULL, "label 里的反斜杠应被转义");
    /* 转义后仍是合法 JSON 结构（**未被转义的**引号应成对） */
    int quotes = 0;
    for (const char *p = buf; *p; p++) {
        if (*p == '"' && (p == buf || *(p - 1) != '\\')) quotes++;
    }
    CHECK(quotes % 2 == 0, "未转义引号应成对（实际 %d 个）", quotes);

    printf("=== 截断（表盘很多时）===\n");
    ui_face_reset();
    char names[20][16];
    for (int i = 0; i < 20; i++) {
        snprintf(names[i], sizeof(names[i]), "face_%02d_long_name", i);
        ui_face_register(&(ui_face_desc_t){ names[i], "A Fairly Long Label Here", D,
                                            (uint8_t)(i % UI_FACE_GROUP_COUNT) });
    }
    CHECK(ui_face_count() == 20, "应注册 20 个（实际 %d）", ui_face_count());
    len = face_report_build(buf, sizeof(buf));
    CHECK(len > 0, "表盘过多时也应返回成功（截断而非失败），len=%d", len);
    CHECK(len <= FACE_REPORT_MAX_PAYLOAD, "截断后仍须在上限内（%d）", len);
    CHECK(strstr(buf, "\"truncated\":1") != NULL, "★ 截断必须显式标记，App 才能提示不完整");
    CHECK(strstr(buf, "\"count\":20") != NULL, "count 应报真实总数（不是已报数量）");
    /* 截断时至少报出了一些表盘且结构完整 */
    int reported = count_substr(buf, "\"id\":");
    CHECK(reported > 0 && reported < 20, "应报出部分表盘（实际 %d）", reported);
    CHECK(buf[len - 1] == '}', "JSON 应正常收尾");
    /* 未报出的表盘 id 应确实不存在 */
    CHECK(strstr(buf, "\"id\":19") == NULL || reported == 20, "超出上限的 id 不应出现");

    printf("=== 设备显示状态（亮度/方向/息屏）===\n");
    /*
     * 背景：这三项是**设备属性**，由设备上报给 App 显示；App 不得在连接时
     * 下发自己的记忆值去覆盖（那会把用户在设备上设好的值冲掉）。
     * 这里验证字段的存在性与"未设置则省略"的规则。
     */
    ui_face_reset();
    register_five();

    /* 1) 全部未设置（初始态）→ 三个字段都不应出现（不能发假值） */
    face_report_reset_for_test();
    len = face_report_build(buf, sizeof(buf));
    CHECK(len > 0, "未设置显示状态时仍应生成报告");
    CHECK(strstr(buf, "\"bright\"") == NULL, "未设置亮度 → 不应上报 bright（避免 App 显示假值）");
    CHECK(strstr(buf, "\"rot\"") == NULL, "未设置方向 → 不应上报 rot");
    CHECK(strstr(buf, "\"screen\"") == NULL, "未设置息屏 → 不应上报 screen");

    /* 2) 设置后应如实带出 */
    face_report_set_display(&(face_report_display_t){ 70, 90, 1 });
    len = face_report_build(buf, sizeof(buf));
    CHECK(strstr(buf, "\"bright\":70") != NULL, "应上报亮度 70");
    CHECK(strstr(buf, "\"rot\":90") != NULL, "应上报方向 90");
    CHECK(strstr(buf, "\"screen\":1") != NULL, "应上报 screen=1（点亮）");

    /* 3) 0 是**有效值**，必须照实上报，不能当成"未设置"省略。
     *    这是最容易写错的地方：把 0 与"未知"混为一谈，App 就永远显示不出息屏。 */
    face_report_set_display(&(face_report_display_t){ 0, 0, 0 });
    len = face_report_build(buf, sizeof(buf));
    CHECK(strstr(buf, "\"bright\":0") != NULL, "亮度 0 是有效值，必须上报");
    CHECK(strstr(buf, "\"rot\":0") != NULL, "方向 0° 是有效值，必须上报");
    CHECK(strstr(buf, "\"screen\":0") != NULL, "screen=0（息屏）必须上报");

    /* 4) 部分设置：只带已设置的那一项 */
    face_report_set_display(&(face_report_display_t){ 128, -1, -1 });
    len = face_report_build(buf, sizeof(buf));
    CHECK(strstr(buf, "\"bright\":128") != NULL, "应上报亮度 128");
    CHECK(strstr(buf, "\"rot\"") == NULL, "方向未设置 → 不应出现");
    CHECK(strstr(buf, "\"screen\"") == NULL, "息屏未设置 → 不应出现");

    /* 5) 传 NULL 复位 */
    face_report_set_display(NULL);
    len = face_report_build(buf, sizeof(buf));
    CHECK(strstr(buf, "\"bright\"") == NULL, "传 NULL 应复位为未设置");

    /* 6) ★ 截断时仍必须带出显示状态 —— 否则表盘一多 App 就退回默认值，
     *      又回到"亮度被凭空改掉"的老 bug。 */
    face_report_reset_for_test();
    face_report_set_display(&(face_report_display_t){ 70, 90, 1 });
    ui_face_reset();
    char many[20][16];
    for (int i = 0; i < 20; i++) {
        snprintf(many[i], sizeof(many[i]), "face_%02d_long_name", i);
        ui_face_register(&(ui_face_desc_t){ many[i], "A Fairly Long Label Here", D,
                                            (uint8_t)(i % UI_FACE_GROUP_COUNT) });
    }
    len = face_report_build(buf, sizeof(buf));
    CHECK(strstr(buf, "\"truncated\":1") != NULL, "该场景应触发截断");
    CHECK(strstr(buf, "\"bright\":70") != NULL, "★ 截断时也必须带出亮度");
    CHECK(strstr(buf, "\"rot\":90") != NULL, "★ 截断时也必须带出方向");
    CHECK(strstr(buf, "\"screen\":1") != NULL, "★ 截断时也必须带出息屏状态");
    CHECK(len <= FACE_REPORT_MAX_PAYLOAD, "加了显示状态后截断仍须在上限内（%d）", len);

    face_report_reset_for_test(); /* 复位，避免影响后续新增用例 */

    printf("结果: %d 通过, %d 失败\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
