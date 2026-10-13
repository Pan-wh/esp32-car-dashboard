/**
 * ui_face.h —— 可更换表盘注册表（功能③）
 *
 * 设计目标：让「表盘」成为可插拔单元，而不是写死的页面。
 *
 * 与既有分页体系的关系：
 *   ui_pages 负责「横向滑动 + 吸附」的页面容器，表盘只是**页面内容**的提供者。
 *   本模块不接管分页，只回答两个问题：
 *     1. 有哪些表盘？（注册表 + id）
 *     2. 某个表盘的内容怎么建？（build 回调）
 *
 * 表盘 id 约定：0..N-1，顺序即注册顺序；BLE/串口 face 命令使用该 id。
 *
 * 为什么现在就把接口留出来：功能②（图片背景 + 时钟）本质是
 * 「背景图 + 信息层」的表盘，届时只需注册一个新表盘，无需改动分页/命令通路。
 */
#ifndef UI_FACE_H
#define UI_FACE_H

#ifdef __cplusplus
extern "C" {
#endif

#define UI_FACE_MAX 8 /* 注册表容量（超出则注册失败） */

/*
 * 表盘构建回调：在给定父对象上创建内容（父对象尺寸 = 屏幕）。
 *
 * 参数刻意用 void* 而非 lv_obj_t*：本注册表是**纯逻辑**，不依赖 LVGL，
 * 因而可以在 PC 上单测（无需引入 lvgl.h / lv_conf.h）。
 * 调用方（ui_pages）负责传入 lv_obj_t*，构建函数内部自行转型。
 */
typedef void (*ui_face_build_cb)(void *parent);

typedef struct {
    const char *name;      /* 稳定标识（ASCII，供日志/调试；不是本地化显示名） */
    const char *label;     /* 用户可见名（ASCII，受限于现有字体字形集） */
    ui_face_build_cb build;/* 创建内容 */
} ui_face_desc_t;

/*
 * 注册一个表盘。
 * @return 分配到的 face id（>=0），失败返回 -1（表满/参数非法/名字重复）。
 */
int ui_face_register(const ui_face_desc_t *desc);

/* 表盘总数 */
int ui_face_count(void);

/* 取表盘描述（越界返回 NULL） */
const ui_face_desc_t *ui_face_get(int id);

/* 按名字查 id（找不到返回 -1） */
int ui_face_find(const char *name);

/*
 * 按 id 取「下一个 / 上一个」表盘 id，用于 BLE 的 next/prev 语义。
 * wrap=1 时循环，wrap=0 时到边界返回原 id。
 */
int ui_face_next(int id, int wrap);
int ui_face_prev(int id, int wrap);

/* 清空注册表（仅测试/初始化用） */
void ui_face_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* UI_FACE_H */
