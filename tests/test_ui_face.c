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
        static const ui_face_desc_t many[UI_FACE_MAX + 2] = {
            { "f0", "0", dummy_build }, { "f1", "1", dummy_build },
            { "f2", "2", dummy_build }, { "f3", "3", dummy_build },
            { "f4", "4", dummy_build }, { "f5", "5", dummy_build },
            { "f6", "6", dummy_build }, { "f7", "7", dummy_build },
            { "f8", "8", dummy_build }, { "f9", "9", dummy_build },
        };
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

    printf("结果: %d 通过, %d 失败\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
