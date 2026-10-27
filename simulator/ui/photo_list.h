/**
 * photo_list.h —— 设备端照片清单（纯逻辑）
 *
 * 职责：清单的增删/排序/查找、文件名白名单、清单分页 JSON 生成。
 * **不碰** SD、不碰 BLE、不碰 LVGL —— 固件与 PC 单测共用同一份实现。
 *
 * 清单元素来自两种情况：
 *   1. 设备启动/上传后扫描 `/photos/`；
 *   2. 上传提交成功后插入（不必重新扫描整目录）。
 *
 * 排序规则：**按文件名升序**（`strcmp`）。新文件由设备分配 `p%05u.<ext>`，
 * 所以序号递增 == 播放顺序；历史 `.pgi` 按名字排，顺序稳定可预期。
 */
#ifndef PHOTO_LIST_H
#define PHOTO_LIST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 清单容量（协议上限；见 docs/photo-protocol.md §9） */
#define PHOTO_MAX_FILES 128
/** 文件名缓冲（含结尾 0） */
#define PHOTO_NAME_MAX 64

/** 文件类型（与协议 `k` 字段一致） */
typedef enum {
    PHOTO_KIND_PGJ = 0, /* 静态照片（32 B 头 + JPEG） */
    PHOTO_KIND_PGI = 1  /* 动图 / 旧格式（pgi.h） */
} photo_kind_t;

/** 清单中的一项（与协议 `pf` 字段一致） */
typedef struct {
    char     name[PHOTO_NAME_MAX];
    uint8_t  kind;      /* photo_kind_t */
    uint32_t bytes;     /* 文件总大小 */
    uint16_t frames;    /* 1 = 静态；>1 = 动图 */
    uint8_t  fps;       /* 0 = 静态 */
    uint16_t width;
    uint16_t height;
} photo_item_t;

typedef struct {
    photo_item_t items[PHOTO_MAX_FILES];
    int          count;
} photo_list_t;

/** 清空 */
void photo_list_init(photo_list_t *l);

/**
 * 追加一项（不排序）。
 * @return false：清单满 / name 非法 / 已存在同名 / 字段非法
 */
bool photo_list_add(photo_list_t *l, const photo_item_t *item);

/** 按文件名升序排序（幂等） */
void photo_list_sort(photo_list_t *l);

/** @return 下标；不存在返回 -1 */
int photo_list_find(const photo_list_t *l, const char *name);

/** 删除一项（保持其余顺序）；@return 是否找到并删除 */
bool photo_list_remove(photo_list_t *l, const char *name);

/**
 * 文件名白名单：长度 1..63；只允许 `A-Za-z0-9_.-`；
 * 不允许以 `.` 开头、不允许 `..`；扩展名（大小写不敏感）必须是 `.pgj` 或 `.pgi`。
 */
bool photo_name_valid(const char *name);

/** 由序号生成设备文件名，如 `p00012.pgj`。cap 必须 >= PHOTO_NAME_MAX。 */
bool photo_make_name(char *out, size_t cap, uint32_t seq, photo_kind_t kind);

/**
 * 清单分页 JSON（Notify 用，每条必须 ≤180 B）：
 *   开始： {"t":"pq","n":12}
 *   每项： {"t":"pf","i":0,"id":"p00012.pgj","b":45210,"f":1,"k":0,"w":466,"h":466,"fps":0}
 *   结束： {"t":"pz"}
 *
 * @return 写入字节数；容量不够返回 -1
 */
int photo_list_begin_json(char *out, size_t cap, int count);
int photo_list_item_json(const photo_list_t *l, int index, char *out, size_t cap);
int photo_list_end_json(char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* PHOTO_LIST_H */
