/**
 * nav_parse.h —— 导航消息解析（BLE 下行 JSON，纯 C，无 LVGL/硬件依赖）
 *
 * 设计取向（阶段 2）：
 *   - **沿用本项目既有的 JSON 风格**，不引入 moto-gps 的二进制帧。
 *     理由：我们已有可单测的稳定 JSON 解析器；二进制帧（magic/CRC/分片/会话）
 *     在 512 字节重组上限内收益有限，却要两端同时改造且丢掉现有测试覆盖。
 *   - 参考 moto-gps 的**字段取舍**（它验证了哪些字段真正必要）。
 *   - **不显示路名**（用户决策 J2）→ 本阶段不引入任何字符串字段，
 *     既省 Flash（无需中文字体）也避免解析器改造。
 *
 * 协议（与遥测同一特征下发，用 "t" 区分消息类型）：
 *   {"t":"nav","state":1,"maneuver":2,"dist_m":500}
 *
 * 与既有遥测消息的区分：
 *   遥测包无 "t" 字段（或 t="tel"），导航包必带 "t":"nav"。
 *   解析失败一律返回 false，调用方**保持原状态**（不误清、不误设）。
 */
#ifndef NAV_PARSE_H
#define NAV_PARSE_H

#include <stdbool.h>
#include <stddef.h>

#include "nav.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 导航消息结构（解析结果）*/
typedef struct {
    bool has_state;
    bool has_maneuver;
    bool has_dist;
    /**
     * 是否带回了**自车在窗口局部坐标系中的偏移**（`ox`/`oy`，单位米）。
     *
     * 为什么需要：地图窗口的坐标原点在"手机发起请求时的位置"，而车在动。
     * 让设备自己做经纬度换算会把投影数学带进固件（不必要），
     * 因此由手机把偏移算好一起下发 —— 设备只做旋转/缩放/平移。
     * 两个字段必须**同时**出现才算有效。
     */
    bool has_offset;

    nav_state_t state;
    nav_maneuver_t maneuver;
    int distance_m; /* NAV_DISTANCE_INVALID 表示"明确无效" */
    int offset_x_m; /* 自车相对窗口中心：x 向东为正 */
    int offset_y_m; /* 自车相对窗口中心：y 向北为正 */
} nav_msg_t;

/**
 * 判断一包 JSON 是否为导航消息（用于与遥测分流）。
 * 只看类型标记，不做完整校验；找不到 "t":"nav" 即返回 false。
 *
 * 说明：遥测包很密（2Hz），分流必须便宜——本函数只做一次浅扫描。
 */
bool nav_is_nav_message(const char *json, size_t len);

/**
 * 解析导航消息。
 * @return true 表示是导航消息且至少解析出 1 个有效字段；
 *         false 表示不是导航消息、或格式错误（调用方应保持原状态）。
 *
 * 容错策略（对齐项目既有原则）：
 *   - 字段缺失：**不置 has_xxx**，调用方保留旧值（不因缺字段就清空显示）
 *   - 字段越界：钳制（动作/状态）或标记无效（距离），绝不越界访问
 *   - 出现 "dist_m": null → has_dist=true, distance_m=NAV_DISTANCE_INVALID
 *     （"明确无效" 与 "字段缺失" 语义不同）
 */
bool nav_parse(const char *json, size_t len, nav_msg_t *out);

#ifdef __cplusplus
}
#endif

#endif /* NAV_PARSE_H */
