/**
 * test_face_report.c —— 表盘列表上报测试（PC 上运行，不需要硬件）
 *
 * 运行：cd tests && make test_face_report && ./test_face_report
 *
 * ## 协议（b6f2b5b 起的紧凑格式）
 *
 * ```json
 * {"t":"f","c":6,"m":63,"u":5,"bright":200,"rot":0,"screen":1,
 *  "f":["analog","digital","speed","color","nav","photo"]}
 * ```
 *
 *   - `t` = "f"（**不是** "faces"）
 *   - `c` = 表盘总数（**不是** count）
 *   - `m` = 启用位掩码（**不是** mask）
 *   - `u` = 当前表盘 id（**不是** cur，-1 = 未知）
 *   - `bright` / `rot` / `screen` = 显示状态，位置在表头之后、`f` 之前
 *   - `f` = 名字字符串数组，下标即 id（**不是** faces 对象数组）
 *   - 截断时额外带 `"trunc":1`（**不是** truncated）
 *
 * `label` / `group` / `id` / `name` / `on` 都已删除：id = 下标、
 * group / 显示名由 App 按 name 映射、on = `(m >> id) & 1`。
 *
 * 覆盖：JSON 结构、名字数组顺序、启用掩码（位语义）、当前表盘、转义、
 *       截断（trunc / 真实总数 / 部分条目 / 显示状态）、6 表盘 < 184 字节、
 *       边界（无表盘 / 缓冲过小 / 空指针）、显示状态、行程。
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

static void D(void *p) { (void)p; }

/* 按固件真实顺序注册 5 个表盘（与 ui_pages.c 一致） */
static void register_five(void)
{
    ui_face_reset();
    ui_face_register(&(ui_face_desc_t){ "analog", "Analog", D, UI_FACE_GROUP_CLOCK });
    ui_face_register(&(ui_face_desc_t){ "digital", "Digital", D, UI_FACE_GROUP_CLOCK });
    ui_face_register(&(ui_face_desc_t){ "speed", "Speed", D, UI_FACE_GROUP_SPEED });
    ui_face_register(&(ui_face_desc_t){ "color", "Color", D, UI_FACE_GROUP_CLOCK });
    ui_face_register(&(ui_face_desc_t){ "nav", "Nav", D, UI_FACE_GROUP_NAV });
}

/* 固件当前的 6 个表盘（末尾多一个 photo）——用于 184 字节上限断言 */
static void register_six(void)
{
    register_five();
    ui_face_register(&(ui_face_desc_t){ "photo", "Photo", D, UI_FACE_GROUP_PHOTO });
}

/** 粗略统计子串出现次数 */
static int count_substr(const char *hay, const char *needle)
{
    int n = 0;
    for (const char *p = hay; (p = strstr(p, needle)) != NULL; p += strlen(needle)) n++;
    return n;
}

/**
 * 在 buf 中按 names[0..total) 的顺序**连续**查找，返回连续命中的条数。
 *
 * 用来验证「报出的表盘是 id 的连续前缀且顺序正确」，不依赖报文用的是
 * `f` 名字数组还是（截断分支目前仍在用的）`faces` 对象数组。
 */
static int count_prefix_in_order(const char *buf, char names[][32], int total)
{
    const char *cursor = buf;
    int n = 0;
    for (int i = 0; i < total; i++) {
        const char *p = strstr(cursor, names[i]);
        if (p == NULL) break;
        n++;
        cursor = p + strlen(names[i]);
    }
    return n;
}

int main(void)
{
    char buf[FACE_REPORT_BUF_SIZE];

    printf("=== 上报结构（紧凑格式）===\n");
    register_five();
    face_report_reset_for_test(); /* u=-1，显示状态未设置 */
    int len = face_report_build(buf, sizeof(buf));
    CHECK(len > 0, "应生成非空报告（len=%d）", len);
    CHECK((size_t)len == strlen(buf), "返回长度应等于字符串长度");
    CHECK(len <= FACE_REPORT_MAX_PAYLOAD, "载荷应在单次上限内（%d ≤ %d）", len,
          FACE_REPORT_MAX_PAYLOAD);

    /* 顶层键全部换成短名 */
    CHECK(strstr(buf, "\"t\":\"f\"") != NULL, "判别字段应为 t=f（旧为 t=faces）");
    CHECK(strstr(buf, "\"c\":5") != NULL, "总数应为 c=5（旧为 count）");
    CHECK(strstr(buf, "\"m\":31") != NULL, "掩码应为 m=31（旧为 mask）");
    CHECK(strstr(buf, "\"u\":-1") != NULL, "当前表盘应为 u=-1（旧为 cur）");

    /* 旧键一个都不能再出现 */
    CHECK(strstr(buf, "\"count\":") == NULL, "旧键 count 不应再出现");
    CHECK(strstr(buf, "\"mask\":") == NULL, "旧键 mask 不应再出现");
    CHECK(strstr(buf, "\"cur\":") == NULL, "旧键 cur 不应再出现");
    CHECK(strstr(buf, "\"faces\":") == NULL, "旧键 faces 不应再出现（已改为 f 名字数组）");
    CHECK(strstr(buf, "truncated") == NULL, "旧截断键 truncated 不应再出现");
    CHECK(strstr(buf, "\"v\":") == NULL, "协议版本字段 v 已删除");

    /* 对象里的字段全部删除 —— id 是下标、group/on 由 App 推导 */
    CHECK(count_substr(buf, "\"id\":") == 0, "id 已删除（下标即 id），实际 %d",
          count_substr(buf, "\"id\":"));
    CHECK(count_substr(buf, "\"name\":") == 0, "name 已删除（数组元素就是名字），实际 %d",
          count_substr(buf, "\"name\":"));
    CHECK(count_substr(buf, "\"label\":") == 0, "label 已删除，实际 %d",
          count_substr(buf, "\"label\":"));
    CHECK(count_substr(buf, "\"group\":") == 0, "group 已删除（App 按 name 映射），实际 %d",
          count_substr(buf, "\"group\":"));
    CHECK(count_substr(buf, "\"on\":") == 0, "on 已删除（由 m 推导），实际 %d",
          count_substr(buf, "\"on\":"));

    /* ★ 名字数组的顺序必须等于 id 顺序（App 靠下标还原 id） */
    CHECK(strstr(buf, "\"f\":[\"analog\",\"digital\",\"speed\",\"color\",\"nav\"]") != NULL,
          "★ f 数组顺序必须等于 id 顺序（实际 %s）", buf);
    /* 紧凑格式整份只有一个对象（数组里是裸字符串，不再是对象） */
    CHECK(count_substr(buf, "{") == 1 && count_substr(buf, "}") == 1,
          "紧凑格式应只有一个 JSON 对象（{ %d 个，} %d 个）",
          count_substr(buf, "{"), count_substr(buf, "}"));

    printf("=== 当前表盘（u）===\n");
    /* 未设置时为 -1（上面已断言）；设置后应带出，App 据此标注"正在用哪个" */
    face_report_set_current(2);
    len = face_report_build(buf, sizeof(buf));
    CHECK(len > 0 && strstr(buf, "\"u\":2") != NULL, "设置后应上报 u=2");
    face_report_set_current(-1);

    printf("=== 启用掩码（m）===\n");
    register_five();
    face_report_reset_for_test();
    len = face_report_build(buf, sizeof(buf));
    /* 全部默认启用 → 掩码 31 */
    CHECK(len > 0, "重生成应成功");
    CHECK(strstr(buf, "\"m\":31") != NULL, "5 个全启用掩码应为 31");
    CHECK(count_substr(buf, "\"on\":") == 0, "启用状态改由 m 推导，不再逐项上报 on");

    /* 隐藏两个（color 与 nav）→ 掩码 0b00111 = 7 */
    CHECK(ui_face_set_enabled(3, false), "应能隐藏 color");
    CHECK(ui_face_set_enabled(4, false), "应能隐藏 nav");
    len = face_report_build(buf, sizeof(buf));
    CHECK(len > 0, "重生成应成功");
    CHECK(strstr(buf, "\"m\":7") != NULL, "掩码应为 0b00111=7（id 0,1,2 启用）");
    CHECK(strstr(buf, "\"c\":5") != NULL, "c 应仍为总数 5（不是启用数 3）");
    CHECK((ui_face_mask() & (1u << 3)) == 0, "bit3（color）应为 0");
    CHECK((ui_face_mask() & (1u << 4)) == 0, "bit4（nav）应为 0");
    CHECK((ui_face_mask() & (1u << 2)) != 0, "bit2（speed）应为 1");

    printf("=== 业务规则：不能隐藏最后一个 ===\n");
    ui_face_reset();
    ui_face_register(&(ui_face_desc_t){ "only", "Only", D, UI_FACE_GROUP_CLOCK });
    face_report_reset_for_test();
    CHECK(!ui_face_set_enabled(0, false), "唯一表盘不应允许隐藏（由 ui_face 拒绝）");
    len = face_report_build(buf, sizeof(buf));
    CHECK(len > 0 && strstr(buf, "\"m\":1") != NULL, "唯一表盘应仍为启用（m=1）");
    CHECK(strstr(buf, "\"f\":[\"only\"]") != NULL, "唯一表盘的名字应出现在 f[0]");

    printf("=== 边界 ===\n");
    ui_face_reset();
    face_report_reset_for_test();
    len = face_report_build(buf, sizeof(buf));
    CHECK(len > 0, "无表盘也应生成合法报告（len=%d）", len);
    CHECK(strcmp(buf, "{\"t\":\"f\",\"c\":0,\"m\":0,\"u\":-1,\"f\":[]}") == 0,
          "空表盘报文应完全符合紧凑格式（实际 %s）", buf);

    CHECK(face_report_build(NULL, sizeof(buf)) == -1, "buf 为 NULL 应返回 -1");
    CHECK(face_report_build(buf, 0) == -1, "cap 为 0 应返回 -1");
    register_five();
    CHECK(face_report_build(buf, 4) == -1, "缓冲过小应返回 -1（而不是写坏内存）");

    printf("=== 转义（名字里的 \" 与 \\）===\n");
    ui_face_reset();
    ui_face_register(&(ui_face_desc_t){ "we\"ird", "X", D, UI_FACE_GROUP_CLOCK });
    ui_face_register(&(ui_face_desc_t){ "back\\slash", "Y", D, UI_FACE_GROUP_CLOCK });
    face_report_reset_for_test();
    len = face_report_build(buf, sizeof(buf));
    CHECK(len > 0, "含引号/反斜杠的名字应仍能生成");
    CHECK(strstr(buf, "we\\\"ird") != NULL, "名字里的引号应被转义");
    CHECK(strstr(buf, "back\\\\slash") != NULL, "名字里的反斜杠应被转义");
    /* 转义后仍是合法 JSON 结构（**未被转义的**引号应成对） */
    int quotes = 0;
    for (const char *p = buf; *p; p++) {
        if (*p == '"' && (p == buf || *(p - 1) != '\\')) quotes++;
    }
    CHECK(quotes % 2 == 0, "未转义引号应成对（实际 %d 个）", quotes);
    CHECK(strstr(buf, "\"f\":[\"we\\\"ird\",\"back\\\\slash\"]") != NULL,
          "★ 转义后的名字仍应组成紧凑数组，且顺序不变");

    printf("=== 6 表盘报文长度（iOS 单次 BLE 读取上限 ≈ 184）===\n");
    /*
     * ★ 这是 b6f2b5b 压缩的**目的**：6 个表盘的完整报文必须能一次读完，
     *   否则排在最后面的照片表盘会被 BLE 截掉（用户"看不到照片表盘"的真因）。
     *   固化成断言，防止以后加字段时又涨回去。
     */
    register_six();
    face_report_reset_for_test();
    face_report_set_current(5);
    face_report_set_display(&(face_report_display_t){
        .brightness = 200, .rotation = 0, .screen_on = 1,
        .trip_distance_km = -1, .trip_max_kmh = -1, .trip_avg_kmh = -1, .trip_time_s = -1,
    });
    len = face_report_build(buf, sizeof(buf));
    CHECK(len > 0, "6 表盘报告应能生成");
    CHECK(strstr(buf, "\"c\":6") != NULL, "6 表盘总数应为 c=6");
    CHECK(strstr(buf, "\"m\":63") != NULL, "6 个全启用掩码应为 63");
    CHECK(strstr(buf, "\"u\":5") != NULL, "当前表盘应为 u=5（photo）");
    CHECK(strstr(buf, "\"f\":[\"analog\",\"digital\",\"speed\",\"color\",\"nav\",\"photo\"]") != NULL,
          "★ 6 项 f 数组顺序必须等于 id 顺序");
    CHECK(strstr(buf, "\"bright\":200") != NULL && strstr(buf, "\"rot\":0") != NULL &&
              strstr(buf, "\"screen\":1") != NULL,
          "6 表盘报告应带显示状态");
    CHECK(len < 184, "★ 6 表盘报文必须 < 184 字节（iOS BLE 单次读取上限），实际 %d", len);
    printf("  实测 6 表盘报文 = %d 字节\n", len);

    printf("=== 截断（表盘占满注册表）===\n");
    ui_face_reset();
    face_report_reset_for_test();
    char many[UI_FACE_MAX][32];
    for (int i = 0; i < UI_FACE_MAX; i++) {
        snprintf(many[i], sizeof(many[i]), "face_%02d_very_long_name", i);
        ui_face_register(&(ui_face_desc_t){ many[i], "A Fairly Long Label Here", D,
                                            (uint8_t)(i % UI_FACE_GROUP_COUNT) });
    }
    const int total = ui_face_count();
    CHECK(total == UI_FACE_MAX, "应注册到上限 %d 个（实际 %d）", UI_FACE_MAX, total);

    len = face_report_build(buf, sizeof(buf));
    CHECK(len > 0, "表盘过多时也应返回成功（截断而非失败），len=%d", len);
    CHECK(len <= FACE_REPORT_MAX_PAYLOAD, "截断后仍须在上限内（%d）", len);
    CHECK(strstr(buf, "\"trunc\":1") != NULL, "★ 截断必须用 trunc 标记（不是 truncated）");
    CHECK(strstr(buf, "truncated") == NULL, "★ 截断标志键名不能是 truncated");
    char total_needle[32];
    snprintf(total_needle, sizeof(total_needle), "\"c\":%d", total);
    CHECK(strstr(buf, total_needle) != NULL, "c 应报真实总数 %d（不是已报数量）", total);
    CHECK(strstr(buf, "\"t\":\"f\"") != NULL, "截断报文仍应带紧凑表头 t=f");
    CHECK(strstr(buf, "\"m\":") != NULL, "截断报文仍应带掩码 m");
    CHECK(buf[len - 1] == '}', "JSON 应正常收尾");

    /*
     * 未报出的表盘 id 不应出现。
     *
     * 断言写成**格式无关**：按 id 顺序连续查找名字。这样无论条目是
     * `f` 名字数组还是 `faces` 对象数组都能验证"连续前缀 + 顺序正确"。
     *
     * ⚠️ 已知实现缺口（不在本次改动范围内）：`simulator/ui/face_report.c`
     *    的截断分支仍输出旧的对象数组（`"faces":[{"id":...}]`），所以
     *    这里无法直接断言 `"f":[`。待该分支也改成紧凑格式后，应把下面
     *    这组断言升级为 `strstr(buf, "\"f\":[\"face_00_very_long_name\"")`。
     */
    const int reported = count_prefix_in_order(buf, many, total);
    CHECK(reported > 0 && reported < total, "★ 截断时应报出部分表盘（实际 %d/%d）",
          reported, total);
    CHECK(strstr(buf, many[0]) != NULL, "至少应报出 id 0");
    int occurrences = 0;
    for (int i = 0; i < total; i++) occurrences += count_substr(buf, many[i]);
    CHECK(occurrences == reported, "报出的表盘应是 id 连续前缀且顺序正确（%d vs %d）",
          occurrences, reported);

    /* 截断时显示状态也必须带出（否则 App 会拿不到亮度/方向，退回默认值） */
    face_report_set_display(&(face_report_display_t){
        .brightness = 70, .rotation = 90, .screen_on = 1,
        .trip_distance_km = -1, .trip_max_kmh = -1, .trip_avg_kmh = -1, .trip_time_s = -1,
    });
    len = face_report_build(buf, sizeof(buf));
    CHECK(strstr(buf, "\"trunc\":1") != NULL, "该场景应触发截断");
    CHECK(strstr(buf, "\"bright\":70") != NULL, "★ 截断时也必须带出亮度");
    CHECK(strstr(buf, "\"rot\":90") != NULL, "★ 截断时也必须带出方向");
    CHECK(strstr(buf, "\"screen\":1") != NULL, "★ 截断时也必须带出息屏状态");
    CHECK(strstr(buf, total_needle) != NULL, "带显示状态时 c 仍应报真实总数 %d", total);
    CHECK(len <= FACE_REPORT_MAX_PAYLOAD, "加了显示状态后截断仍须在上限内（%d）", len);

    printf("=== 设备显示状态（亮度/方向/息屏）===\n");
    /*
     * 背景：这三项是**设备属性**，由设备上报给 App 显示；App 不得在连接时
     * 下发自己的记忆值去覆盖（那会把用户在设备上设好的值冲掉）。
     * 这里验证字段的存在性与"未设置则省略"的规则。
     */
    ui_face_reset();
    register_five();
    face_report_reset_for_test();

    /* 1) 全部未设置（初始态）→ 三个字段都不应出现（不能发假值） */
    len = face_report_build(buf, sizeof(buf));
    CHECK(len > 0, "未设置显示状态时仍应生成报告");
    CHECK(strstr(buf, "\"bright\"") == NULL, "未设置亮度 → 不应上报 bright（避免 App 显示假值）");
    CHECK(strstr(buf, "\"rot\"") == NULL, "未设置方向 → 不应上报 rot");
    CHECK(strstr(buf, "\"screen\"") == NULL, "未设置息屏 → 不应上报 screen");

    /* 2) 设置后应如实带出 */
    face_report_set_display(&(face_report_display_t){ .brightness = 70, .rotation = 90, .screen_on = 1 });
    len = face_report_build(buf, sizeof(buf));
    CHECK(strstr(buf, "\"bright\":70") != NULL, "应上报亮度 70");
    CHECK(strstr(buf, "\"rot\":90") != NULL, "应上报方向 90");
    CHECK(strstr(buf, "\"screen\":1") != NULL, "应上报 screen=1（点亮）");

    /* 3) 0 是**有效值**，必须照实上报，不能当成"未设置"省略。
     *    这是最容易写错的地方：把 0 与"未知"混为一谈，App 就永远显示不出息屏。 */
    face_report_set_display(&(face_report_display_t){ .brightness = 0, .rotation = 0, .screen_on = 0 });
    len = face_report_build(buf, sizeof(buf));
    CHECK(strstr(buf, "\"bright\":0") != NULL, "亮度 0 是有效值，必须上报");
    CHECK(strstr(buf, "\"rot\":0") != NULL, "方向 0° 是有效值，必须上报");
    CHECK(strstr(buf, "\"screen\":0") != NULL, "screen=0（息屏）必须上报");

    /*
     * 4) 部分设置：只带已设置的那一项。
     *
     * ⚠️ 必须**显式**把不要的字段写成 -1：C 的指定初始化会把未列出的字段
     *    清零，而 0 对 screen_on/rotation 是**有效值**（息屏 / 0°），
     *    会被当成"设备确实上报了 0"——这正是"0 与未设置必须区分"的坑。
     */
    face_report_set_display(&(face_report_display_t){
        .brightness = 128, .rotation = -1, .screen_on = -1,
        .trip_distance_km = -1, .trip_max_kmh = -1, .trip_avg_kmh = -1, .trip_time_s = -1,
    });
    len = face_report_build(buf, sizeof(buf));
    CHECK(strstr(buf, "\"bright\":128") != NULL, "应上报亮度 128");
    CHECK(strstr(buf, "\"rot\"") == NULL, "方向未设置 → 不应出现");
    CHECK(strstr(buf, "\"screen\"") == NULL, "息屏未设置 → 不应出现");

    /* 5) 传 NULL 复位 */
    face_report_set_display(NULL);
    len = face_report_build(buf, sizeof(buf));
    CHECK(strstr(buf, "\"bright\"") == NULL, "传 NULL 应复位为未设置");

    printf("=== 行程统计（P1-1）===\n");
    /*
     * 行程由**设备**累积并上报（App 本地算会在重启后归零，与仪表数字对不上）。
     * 这里验证字段存在性、未设置时省略、以及**加了行程后仍不超载荷上限**。
     */
    ui_face_reset();
    register_five();
    face_report_reset_for_test();

    /* 未设置 → 不应出现 */
    len = face_report_build(buf, sizeof(buf));
    CHECK(strstr(buf, "\"trip_km\"") == NULL, "未设置时不应上报 trip_km");
    CHECK(strstr(buf, "\"trip_max\"") == NULL, "未设置时不应上报 trip_max");

    /* 设置后如实带出（含 0 —— 刚出发时里程就是 0，必须能上报） */
    face_report_set_display(&(face_report_display_t){
        .brightness = 200, .rotation = 0, .screen_on = 1,
        .trip_distance_km = 12.34, .trip_max_kmh = 87.6,
        .trip_avg_kmh = 45.25, .trip_time_s = 977,
    });
    len = face_report_build(buf, sizeof(buf));
    CHECK(len > 0, "带行程的报告应能生成");
    CHECK(strstr(buf, "\"trip_km\":12.34") != NULL, "应上报里程 12.34km");
    CHECK(strstr(buf, "\"trip_max\":87.60") != NULL, "应上报最高速 87.60km/h");
    CHECK(strstr(buf, "\"trip_avg\":45.25") != NULL, "应上报均速 45.25km/h");
    CHECK(strstr(buf, "\"trip_s\":977") != NULL, "应上报行驶时长 977s");
    CHECK(len <= FACE_REPORT_MAX_PAYLOAD,
          "★ 加上行程字段后仍须在上限内（%d ≤ %d）", len, FACE_REPORT_MAX_PAYLOAD);

    /* ★ 里程 0 是**有效值**（刚出发），必须与"未设置"区分 */
    face_report_set_display(&(face_report_display_t){
        .brightness = -1, .rotation = -1, .screen_on = -1,
        .trip_distance_km = 0.0, .trip_max_kmh = 0.0,
        .trip_avg_kmh = 0.0, .trip_time_s = 0,
    });
    len = face_report_build(buf, sizeof(buf));
    CHECK(strstr(buf, "\"trip_km\":0.00") != NULL, "★ 里程 0 是有效值，必须上报");

    face_report_reset_for_test(); /* 复位，避免影响后续新增用例 */

    printf("结果: %d 通过, %d 失败\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
