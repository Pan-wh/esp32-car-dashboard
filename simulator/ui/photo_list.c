/**
 * photo_list.c —— 设备端照片清单（纯逻辑）
 *
 * 设计要点见 photo_list.h。本模块只维护"内存里的清单"：排序、查找、删除、
 * 文件名白名单、清单分页 JSON。**不碰 SD / BLE / LVGL**，这样固件和 PC 单测
 * 用的是同一份行为。
 *
 * 两个刻意的取舍：
 *   - 排序用插入排序而不是 qsort：n ≤ 128，O(n²) 最多 8 K 次比较，代价可忽略；
 *     换来的是**顺序完全确定**（qsort 不稳定，且要传函数指针）。
 *   - 生成 JSON 只用 snprintf 整数格式化：不引 JSON 库，栈占用固定且很小；
 *     清单项最长（63 B 文件名 + 各字段最大值）实测 145 B，稳定 < 180 B。
 */
#include "photo_list.h"

#include "pgj.h" /* 只为复用 PGJ_MAX_DIM：设备能显示的边长上限就这一个来源 */

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

/** 扩展名比较（大小写不敏感；ext 不含 '.'） */
static bool ext_is(const char *ext, const char *want)
{
    for (size_t i = 0; i < 3; i++) {
        char a = ext[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (a != want[i]) return false;
    }
    return true;
}

bool photo_name_valid(const char *name)
{
    if (name == NULL) return false;
    /* 有界求长：外部传来的缓冲万一没有结尾 0，也不能越界读 64 B 之外 */
    size_t len = 0;
    while (len < PHOTO_NAME_MAX && name[len] != '\0') len++;
    if (len == 0 || len >= PHOTO_NAME_MAX) return false; /* 1..63 */
    if (name[0] == '.') return false;                    /* 隐藏文件 / "." / ".." */

    for (size_t i = 0; i < len; i++) {
        char c = name[i];
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-';
        if (!ok) return false;
    }
    /* ".." 即使出现在中间也拒绝：设备会用它拼路径，绝不允许跳出 /photos/ */
    for (size_t i = 0; i + 1 < len; i++) {
        if (name[i] == '.' && name[i + 1] == '.') return false;
    }

    /* 扩展名必须是最后一个 '.' 之后恰好 3 个字符，且为 pgj/pgi */
    const char *dot = strrchr(name, '.');
    if (dot == NULL) return false;
    size_t extlen = len - (size_t)(dot - name) - 1;
    if (extlen != 3) return false;
    const char *ext = dot + 1;
    return ext_is(ext, "pgj") || ext_is(ext, "pgi");
}

void photo_list_init(photo_list_t *l)
{
    if (l == NULL) return;
    l->count = 0; /* items 内容不用清：count 之外的数据不会被读取 */
}

bool photo_list_add(photo_list_t *l, const photo_item_t *item)
{
    if (l == NULL || item == NULL) return false;
    if (l->count < 0 || l->count >= PHOTO_MAX_FILES) return false;
    if (!photo_name_valid(item->name)) return false;
    if (item->kind > (uint8_t)PHOTO_KIND_PGI) return false;
    /* 帧数 0 在协议里没有意义（静态是 1，动图 ≥2） */
    if (item->frames == 0) return false;
    /* 超出设备显示边长上限的项进清单也只会在播放时失败 → 扫描阶段就挡住 */
    if (item->width == 0 || item->width > PGJ_MAX_DIM) return false;
    if (item->height == 0 || item->height > PGJ_MAX_DIM) return false;
    /* .pgj 是单张静态照片；声明多帧说明文件/扫描结果不对 */
    if (item->kind == (uint8_t)PHOTO_KIND_PGJ && item->frames != 1) return false;
    /* 同名覆盖会破坏"按文件名排序 == 播放顺序"的假设，也掩盖扫描重复 */
    if (photo_list_find(l, item->name) >= 0) return false;

    l->items[l->count] = *item;
    l->count++;
    return true;
}

void photo_list_sort(photo_list_t *l)
{
    if (l == NULL || l->count <= 1) return;
    if (l->count > PHOTO_MAX_FILES) l->count = PHOTO_MAX_FILES; /* 防外部破坏 */

    for (int i = 1; i < l->count; i++) {
        photo_item_t tmp = l->items[i];
        int j = i - 1;
        while (j >= 0 && strcmp(l->items[j].name, tmp.name) > 0) {
            l->items[j + 1] = l->items[j];
            j--;
        }
        l->items[j + 1] = tmp;
    }
}

int photo_list_find(const photo_list_t *l, const char *name)
{
    if (l == NULL || name == NULL) return -1;
    int n = l->count;
    if (n < 0) return -1;
    if (n > PHOTO_MAX_FILES) n = PHOTO_MAX_FILES;
    for (int i = 0; i < n; i++) {
        if (strcmp(l->items[i].name, name) == 0) return i;
    }
    return -1;
}

bool photo_list_remove(photo_list_t *l, const char *name)
{
    int idx = photo_list_find(l, name);
    if (idx < 0) return false;
    /* 用 memmove 保持其余元素顺序：删除后播放顺序不应抖动 */
    memmove(&l->items[idx], &l->items[idx + 1],
            (size_t)(l->count - idx - 1) * sizeof(l->items[0]));
    l->count--;
    return true;
}

bool photo_make_name(char *out, size_t cap, uint32_t seq, photo_kind_t kind)
{
    if (out == NULL || cap < PHOTO_NAME_MAX) return false;
    const char *ext;
    switch (kind) {
    case PHOTO_KIND_PGJ: ext = "pgj"; break;
    case PHOTO_KIND_PGI: ext = "pgi"; break;
    default: return false;
    }
    /* %05u 至少 5 位；序号超过 99999 时会自然变长，排序仍然单调 */
    int n = snprintf(out, cap, "p%05" PRIu32 ".%s", seq, ext);
    if (n < 0 || (size_t)n >= cap) return false;
    return true;
}

int photo_list_begin_json(char *out, size_t cap, int count)
{
    if (out == NULL || cap == 0 || count < 0) return -1;
    int n = snprintf(out, cap, "{\"t\":\"pq\",\"n\":%d}", count);
    if (n < 0 || (size_t)n >= cap) return -1;
    return n;
}

int photo_list_item_json(const photo_list_t *l, int index, char *out, size_t cap)
{
    if (l == NULL || out == NULL || cap == 0) return -1;
    if (l->count < 0 || l->count > PHOTO_MAX_FILES) return -1;
    if (index < 0 || index >= l->count) return -1;
    const photo_item_t *it = &l->items[index];
    /* 名字直接进 JSON 字符串：这里再校验一次白名单，防止外部手工构造的清单
     * 把引号/控制字符注进 Notify 载荷（运行时清单都来自 photo_list_add） */
    if (!photo_name_valid(it->name)) return -1;

    /* 字段顺序与协议 §5 的示例一致，App 侧按 key 解析、不依赖顺序，但一致更便于比对 */
    int n = snprintf(out, cap,
                     "{\"t\":\"pf\",\"i\":%d,\"id\":\"%s\",\"b\":%" PRIu32
                     ",\"f\":%u,\"k\":%u,\"w\":%u,\"h\":%u,\"fps\":%u}",
                     index, it->name, it->bytes, (unsigned)it->frames, (unsigned)it->kind,
                     (unsigned)it->width, (unsigned)it->height, (unsigned)it->fps);
    if (n < 0 || (size_t)n >= cap) return -1;
    return n;
}

int photo_list_end_json(char *out, size_t cap)
{
    if (out == NULL || cap == 0) return -1;
    int n = snprintf(out, cap, "{\"t\":\"pz\"}");
    if (n < 0 || (size_t)n >= cap) return -1;
    return n;
}
