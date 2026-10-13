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

/* 启用位掩码：bit i = face id i 已启用。注册时默认启用。 */
static uint32_t s_enabled_mask = 0;

static const char *k_group_names[UI_FACE_GROUP_COUNT] = {
    "clock", "speed", "nav", "photo",
};

const char *ui_face_group_name(int group)
{
    if (group < 0 || group >= UI_FACE_GROUP_COUNT) return "unknown";
    return k_group_names[group];
}

/* 只用数组下标，不做动态分配：表盘数量固定且很小 */
int ui_face_register(const ui_face_desc_t *desc)
{
    if (desc == NULL || desc->name == NULL || desc->build == NULL) return -1;
    if (s_count >= UI_FACE_MAX) return -1;
    if (ui_face_find(desc->name) >= 0) return -1; /* 名字重复 */

    s_faces[s_count] = *desc;
    s_enabled_mask |= (1u << s_count); /* 新注册的表盘默认启用 */
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

/* 从 id 出发沿 dir 方向找下一个**已启用**的表盘；找不到返回 -1 */
static int find_enabled(int id, int dir, int wrap)
{
    if (s_count == 0) return -1;
    if (id < 0 || id >= s_count) id = (dir > 0) ? -1 : s_count; /* 非法输入 → 从头/尾扫描 */

    int cur = id;
    for (int step = 0; step < s_count; step++) {
        cur += dir;
        if (cur >= s_count) {
            if (!wrap) return -1;
            cur = 0;
        }
        else if (cur < 0) {
            if (!wrap) return -1;
            cur = s_count - 1;
        }
        if (ui_face_is_enabled(cur)) return cur;
    }
    return -1; /* 没有任何启用的表盘（正常情况下不会发生，见 set_mask 的约束）*/
}

/* 第一个已启用的表盘（注册表非空且至少一个启用时必然存在）*/
static int first_enabled(void)
{
    for (int i = 0; i < s_count; i++) {
        if (ui_face_is_enabled(i)) return i;
    }
    return 0;
}

int ui_face_next(int id, int wrap)
{
    if (s_count == 0) return -1;
    if (id < 0 || id >= s_count) return first_enabled(); /* 非法 id → 回到第一个 */

    int n = find_enabled(id, +1, wrap);
    return (n >= 0) ? n : id; /* 到边界且不循环 → 保持原 id */
}

int ui_face_prev(int id, int wrap)
{
    if (s_count == 0) return -1;
    if (id < 0 || id >= s_count) return first_enabled(); /* 非法 id → 回到第一个 */

    int p = find_enabled(id, -1, wrap);
    return (p >= 0) ? p : id;
}

/* ---------------- 启用状态 ---------------- */

bool ui_face_is_enabled(int id)
{
    if (id < 0 || id >= s_count) return false;
    return (s_enabled_mask & (1u << id)) != 0;
}

int ui_face_enabled_count(void)
{
    int n = 0;
    for (int i = 0; i < s_count; i++) {
        if (ui_face_is_enabled(i)) n++;
    }
    return n;
}

int ui_face_enabled_count_in_group(int group)
{
    int n = 0;
    for (int i = 0; i < s_count; i++) {
        if (s_faces[i].group == (uint8_t)group && ui_face_is_enabled(i)) n++;
    }
    return n;
}

bool ui_face_set_enabled(int id, bool on)
{
    if (id < 0 || id >= s_count) return false;

    if (on) {
        s_enabled_mask |= (1u << id);
        return true;
    }

    /* 关闭：不允许关掉最后一个启用项（否则仪表无内容）*/
    if (!ui_face_is_enabled(id)) return true; /* 本来就是关的，视为成功 */
    if (ui_face_enabled_count() <= 1) return false;

    s_enabled_mask &= ~(1u << id);
    return true;
}

uint32_t ui_face_mask(void)
{
    /* 只保留已注册表盘对应的位，避免把无效位暴露出去 */
    uint32_t valid = (s_count >= 32) ? 0xFFFFFFFFu : ((1u << s_count) - 1u);
    return s_enabled_mask & valid;
}

bool ui_face_set_mask(uint32_t mask)
{
    if (s_count == 0) return false;

    uint32_t valid = (s_count >= 32) ? 0xFFFFFFFFu : ((1u << s_count) - 1u);
    uint32_t wanted = mask & valid;

    if (wanted == 0) return false; /* 至少保留一个 */

    s_enabled_mask = wanted;
    return true;
}

void ui_face_reset(void)
{
    memset(s_faces, 0, sizeof(s_faces));
    s_count = 0;
    s_enabled_mask = 0;
}
