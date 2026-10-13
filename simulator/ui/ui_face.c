/**
 * ui_face.c —— 表盘注册表实现（纯逻辑，无 LVGL 调用，可单测）
 *
 * 有意保持「零依赖」：只有数组 + 字符串比较，便于在 PC 上单测注册/查找/翻页语义。
 * 真正的 LVGL 构建回调由调用方（ui_pages）触发，本模块只存指针。
 */
#include "ui_face.h"

#include <string.h>

static ui_face_desc_t s_faces[UI_FACE_MAX];
static int s_count = 0;

/* 只用数组下标，不做动态分配：表盘数量固定且很小 */
int ui_face_register(const ui_face_desc_t *desc)
{
    if (desc == NULL || desc->name == NULL || desc->build == NULL) return -1;
    if (s_count >= UI_FACE_MAX) return -1;
    if (ui_face_find(desc->name) >= 0) return -1; /* 名字重复 */

    s_faces[s_count] = *desc;
    return s_count++;
}

int ui_face_count(void)
{
    return s_count;
}

const ui_face_desc_t *ui_face_get(int id)
{
    if (id < 0 || id >= s_count) return NULL;
    return &s_faces[id];
}

int ui_face_find(const char *name)
{
    if (name == NULL) return -1;
    for (int i = 0; i < s_count; i++) {
        if (s_faces[i].name != NULL && strcmp(s_faces[i].name, name) == 0) return i;
    }
    return -1;
}

int ui_face_next(int id, int wrap)
{
    if (s_count == 0) return -1;
    if (id < 0 || id >= s_count) return 0; /* 非法输入 → 回到第一个 */
    int n = id + 1;
    if (n >= s_count) return wrap ? 0 : id;
    return n;
}

int ui_face_prev(int id, int wrap)
{
    if (s_count == 0) return -1;
    if (id < 0 || id >= s_count) return 0;
    int p = id - 1;
    if (p < 0) return wrap ? s_count - 1 : id;
    return p;
}

void ui_face_reset(void)
{
    memset(s_faces, 0, sizeof(s_faces));
    s_count = 0;
}
