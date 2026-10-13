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

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UI_FACE_MAX 32 /* 注册表容量（位掩码按 uint32 存启用状态，故上限 32）*/

/*
 * 表盘所属**模块分组**。
 *
 * 为什么要分组：表盘会越来越多（时钟类就有 3 个），全部平铺既难找也不便扩展。
 * 分组是 **App 端的组织方式**；设备侧没有"分组切换"手势，滑动仍是线性遍历
 * 「已启用」的表盘。
 *
 * 扩展方式：新增模块时在此加一个枚举值，注册表盘时填对应的 group。
 * ⚠️ **App 端需手动同步**：`ios/.../ContentView.swift` 的 `faces` 数组
 *    必须按注册顺序加一项（见 docs/tasks/new-face.md §5），
 *    否则 App 选择器里选不到该表盘（BLE 侧本身已支持）。
 *    曾计划"由设备上报表盘列表、App 无需改代码"，但**该上报尚未实现**
 *    （docs/ble.md 亦无此协议），故当前必须手工维护。
 * NAV / PHOTO 即为后续「导航模块」「照片模块」预留。
 */
typedef enum {
    UI_FACE_GROUP_CLOCK = 0, /* 时钟 */
    UI_FACE_GROUP_SPEED,     /* 速度表 */
    UI_FACE_GROUP_NAV,       /* 导航（预留，尚未实现） */
    UI_FACE_GROUP_PHOTO,     /* 照片（预留，尚未实现） */
    UI_FACE_GROUP_COUNT
} ui_face_group_t;

/* 分组名（ASCII，供日志/调试；App 侧显示名自行本地化） */
const char *ui_face_group_name(int group);

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
    uint8_t group;         /* ui_face_group_t：所属模块（App 据此分组显示） */
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
 *
 * ⚠️ 本函数**会跳过已禁用的表盘**（默认全部启用，此时行为与"所有表盘"一致）。
 */
int ui_face_next(int id, int wrap);
int ui_face_prev(int id, int wrap);

/*
 * ---- 启用状态（用户可关闭不想要的表盘）----
 *
 * 语义：被禁用的表盘**从滑动序列中移除**（设备侧即生效，App 与设备一致）。
 * 约束：**至少要保留一个启用的表盘**（否则仪表无内容可显示）。
 * 持久化：固件把它存入 NVS；本模块只管内存中的位掩码（纯逻辑，可单测）。
 */
bool ui_face_is_enabled(int id);

/** 设置单个表盘的启用状态。若会关掉最后一个启用项则**拒绝**并返回 false。 */
bool ui_face_set_enabled(int id, bool on);

/** 当前启用位掩码（bit i = face id i）。 */
uint32_t ui_face_mask(void);

/**
 * 整体设置启用位掩码。
 * @return false 表示非法（掩码为 0、或没有任何 bit 对应已注册表盘），此时不生效。
 */
bool ui_face_set_mask(uint32_t mask);

/** 已启用的表盘数量（>=1；若注册表为空则为 0）。 */
int ui_face_enabled_count(void);

/** 指定分组下已启用的表盘数量（App 用于判断某模块是否该显示）。 */
int ui_face_enabled_count_in_group(int group);

/* 清空注册表（仅测试/初始化用） */
void ui_face_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* UI_FACE_H */
