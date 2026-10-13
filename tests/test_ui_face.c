/**
 * test_ui_face.c —— 表盘注册表单元测试（PC 上运行，不需要硬件/LVGL 渲染）
 *
 * 运行：cd tests && make test_ui_face && ./test_ui_face
 * 覆盖：注册/去重/容量上限、查找、next/prev 的循环与非循环语义、边界、复位
 */
#include <stdio.h>
#include <string.h>

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

/* 假 build 回调（本模块不调用它，只要非空即可） */
static void dummy_build(void *p) { (void)p; }

static const ui_face_desc_t F_ANALOG = { "analog", "Analog", dummy_build };
static const ui_face_desc_t F_DIGITAL = { "digital", "Digital", dummy_build };
static const ui_face_desc_t F_SPEED = { "speed", "Speed", dummy_build };

int main(void)
{
    printf("=== 表盘注册表 ===\n");

    /* ---- 空表 ---- */
    ui_face_reset();
    CHECK(ui_face_count() == 0, "初始为空");
    CHECK(ui_face_get(0) == NULL, "空表取描述返回 NULL");
    CHECK(ui_face_find("analog") == -1, "空表查找返回 -1");
    CHECK(ui_face_next(0, 0) == -1, "空表 next 返回 -1");
    CHECK(ui_face_prev(0, 0) == -1, "空表 prev 返回 -1");

    /* ---- 注册 ---- */
    CHECK(ui_face_register(&F_ANALOG) == 0, "第一个表盘 id=0");
    CHECK(ui_face_register(&F_DIGITAL) == 1, "第二个表盘 id=1");
    CHECK(ui_face_register(&F_SPEED) == 2, "第三个表盘 id=2");
    CHECK(ui_face_count() == 3, "计数为 3");

    /* ---- 去重与非法参数 ---- */
    CHECK(ui_face_register(&F_ANALOG) == -1, "同名重复注册被拒");
    CHECK(ui_face_register(NULL) == -1, "NULL 描述被拒");
    {
        ui_face_desc_t bad = { NULL, "x", dummy_build };
        CHECK(ui_face_register(&bad) == -1, "缺 name 被拒");
        ui_face_desc_t bad2 = { "noname", "x", NULL };
        CHECK(ui_face_register(&bad2) == -1, "缺 build 回调被拒");
    }
    CHECK(ui_face_count() == 3, "被拒的注册不影响计数");

    /* ---- 查找与取描述 ---- */
    CHECK(ui_face_find("analog") == 0, "查找 analog");
    CHECK(ui_face_find("speed") == 2, "查找 speed");
    CHECK(ui_face_find("nope") == -1, "查找不存在返回 -1");
    CHECK(ui_face_find(NULL) == -1, "查找 NULL 返回 -1");
    CHECK(ui_face_get(1) != NULL && strcmp(ui_face_get(1)->name, "digital") == 0, "取 id=1 描述");
    CHECK(ui_face_get(-1) == NULL && ui_face_get(3) == NULL, "越界取描述返回 NULL");

    /* ---- next / prev：不循环 ---- */
    CHECK(ui_face_next(0, 0) == 1, "next(0) → 1");
    CHECK(ui_face_next(1, 0) == 2, "next(1) → 2");
    CHECK(ui_face_next(2, 0) == 2, "末个 next 不循环：停在 2");
    CHECK(ui_face_prev(2, 0) == 1, "prev(2) → 1");
    CHECK(ui_face_prev(0, 0) == 0, "首个 prev 不循环：停在 0");

    /* ---- next / prev：循环 ---- */
    CHECK(ui_face_next(2, 1) == 0, "末个 next 循环 → 0");
    CHECK(ui_face_prev(0, 1) == 2, "首个 prev 循环 → 2");

    /* ---- 非法 id ---- */
    CHECK(ui_face_next(-1, 1) == 0, "非法 id 的 next → 0");
    CHECK(ui_face_prev(99, 1) == 0, "越界 id 的 prev → 0");

    /* ---- 容量上限 ---- */
    ui_face_reset();
    {
        /*
         * 程序化生成 UI_FACE_MAX + 2 个描述符。
         * ⚠️ 原先写死 10 个元素的数组并循环到 UI_FACE_MAX+2 —— UI_FACE_MAX 从 8
         * 提到 32 后就会**越界读**（读到 0 初始化项，表现为"只注册成功 10 个"）。
         */
        static char names[UI_FACE_MAX + 2][8];
        static ui_face_desc_t many[UI_FACE_MAX + 2];
        for (int i = 0; i < UI_FACE_MAX + 2; i++) {
            snprintf(names[i], sizeof(names[i]), "f%d", i);
            many[i].name = names[i];
            many[i].label = "x";
            many[i].build = dummy_build;
            many[i].group = UI_FACE_GROUP_CLOCK;
        }
        int ok = 0, fail = 0;
        for (int i = 0; i < UI_FACE_MAX + 2; i++) {
            if (ui_face_register(&many[i]) >= 0) ok++;
            else fail++;
        }
        CHECK(ok == UI_FACE_MAX, "最多注册 %d 个（实际 %d）", UI_FACE_MAX, ok);
        CHECK(fail == 2, "超出容量被拒（实际拒 %d）", fail);
    }

    /* ---- 复位 ---- */
    ui_face_reset();
    CHECK(ui_face_count() == 0, "复位后为空");

    /* ---- 分组 ---- */
    printf("=== 分组与启用状态 ===\n");
    ui_face_reset();
    {
        static const ui_face_desc_t F_CLK1 = { "c1", "C1", dummy_build, UI_FACE_GROUP_CLOCK };
        static const ui_face_desc_t F_CLK2 = { "c2", "C2", dummy_build, UI_FACE_GROUP_CLOCK };
        static const ui_face_desc_t F_SPD  = { "sp", "SP", dummy_build, UI_FACE_GROUP_SPEED };
        static const ui_face_desc_t F_NAV  = { "nv", "NV", dummy_build, UI_FACE_GROUP_NAV };

        ui_face_register(&F_CLK1); /* 0 */
        ui_face_register(&F_CLK2); /* 1 */
        ui_face_register(&F_SPD);  /* 2 */
        ui_face_register(&F_NAV);  /* 3 */

        CHECK(ui_face_get(0)->group == UI_FACE_GROUP_CLOCK, "id0 属时钟组");
        CHECK(ui_face_get(3)->group == UI_FACE_GROUP_NAV, "id3 属导航组（预留）");
        CHECK(strcmp(ui_face_group_name(UI_FACE_GROUP_NAV), "nav") == 0, "分组名 nav");
        CHECK(strcmp(ui_face_group_name(99), "unknown") == 0, "非法分组名 → unknown");

        /* 默认全部启用 */
        CHECK(ui_face_enabled_count() == 4, "注册后默认全部启用");
        CHECK(ui_face_is_enabled(0) && ui_face_is_enabled(3), "默认各表盘均启用");
        CHECK(ui_face_enabled_count_in_group(UI_FACE_GROUP_CLOCK) == 2, "时钟组启用 2 个");
        CHECK(ui_face_enabled_count_in_group(UI_FACE_GROUP_PHOTO) == 0,
              "照片组（预留）启用 0 个");

        /* 关闭一个 → next 跳过它 */
        CHECK(ui_face_set_enabled(1, false), "可关闭 id1");
        CHECK(!ui_face_is_enabled(1), "id1 已关闭");
        CHECK(ui_face_next(0, 1) == 2, "从 0 前进跳过已关闭的 1 → 2");
        CHECK(ui_face_enabled_count() == 3, "启用数减为 3");
        CHECK(ui_face_next(3, 1) == 0, "从 3 循环前进跳过 1 → 0");
        CHECK(ui_face_prev(2, 1) == 0, "从 2 后退跳过已关闭的 1 → 0");
        CHECK(ui_face_prev(0, 1) == 3, "从 0 后退循环 → 3");

        /* 再启用回来 */
        CHECK(ui_face_set_enabled(1, true), "可重新启用 id1");
        CHECK(ui_face_next(0, 1) == 1, "恢复后 next 回到 1");
        CHECK(ui_face_set_enabled(1, true), "重复启用为幂等成功");

        /* 至少保留一个：不能把最后一个关掉 */
        ui_face_reset();
        ui_face_register(&F_SPD);
        CHECK(ui_face_enabled_count() == 1, "仅 1 个表盘");
        CHECK(!ui_face_set_enabled(0, false), "关掉最后一个 → 拒绝");
        CHECK(ui_face_is_enabled(0), "被拒后仍为启用");

        /* 掩码整体设置 */
        ui_face_reset();
        ui_face_register(&F_CLK1);
        ui_face_register(&F_CLK2);
        ui_face_register(&F_SPD);
        CHECK(ui_face_mask() == 0x7u, "默认掩码 = 0b111");
        CHECK(ui_face_set_mask(0x5u), "设置掩码 0b101 成功");
        CHECK(!ui_face_is_enabled(1) && ui_face_is_enabled(2), "掩码生效（关闭 id1）");
        CHECK(ui_face_mask() == 0x5u, "掩码读回一致");
        CHECK(!ui_face_set_mask(0), "掩码 0 → 拒绝（至少保留一个）");
        CHECK(ui_face_mask() == 0x5u, "被拒后掩码不变");
        CHECK(!ui_face_set_mask(0xF0u), "掩码只含无效位 → 拒绝");
        CHECK(ui_face_mask() == 0x5u, "无效掩码不生效");
        CHECK(ui_face_set_mask(0xFFu), "掩码含无效高位 → 只取有效位");
        CHECK(ui_face_mask() == 0x7u, "高位被裁掉");

        /* 非法 id */
        CHECK(!ui_face_set_enabled(-1, true), "负 id → 拒绝");
        CHECK(!ui_face_set_enabled(99, true), "越界 id → 拒绝");
        CHECK(!ui_face_is_enabled(99), "越界查询返回 false");

        /* 全部关闭后 next/prev 不应死循环（set_mask 保证不会发生，但防御性检查）*/
        CHECK(ui_face_next(0, 1) >= 0, "正常状态 next 有结果");
    }

    printf("结果: %d 通过, %d 失败\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
