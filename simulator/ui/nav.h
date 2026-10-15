/**
 * nav.h —— 导航指引卡片：数据模型与纯逻辑（无 LVGL 依赖，可在 PC 单测）
 *
 * 阶段 1（UI 原型）只定义"仪表要显示什么"，不涉及导航数据从哪来：
 *   - 数据来源（自建 MapKit / 第三方）已确认**推迟决策**，故本模块只接受
 *     "已经算好的导航指引"，由上层（模拟器/固件/BLE）喂入。
 *   - 明确**不显示路名**（用户决策 J2）→ 无需字符串解析与中文字模。
 *
 * 设计对齐《导航卡片可行性评估》§3：显示
 *   大号转向箭头 + 距下一转弯距离 + 当前车速 + 导航状态。
 */
#ifndef NAV_H
#define NAV_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 转向动作（对齐评估文档 §3.1 的 8 种）。
 * 枚举值即协议值，新增动作**只能追加**在 ARRIVE 之前并同步两端。
 */
typedef enum {
    NAV_MANEUVER_UNKNOWN = 0, /* 未知动作（数据缺失/无法识别）*/
    NAV_MANEUVER_STRAIGHT,    /* 直行 */
    NAV_MANEUVER_LEFT,        /* 左转 */
    NAV_MANEUVER_RIGHT,       /* 右转 */
    NAV_MANEUVER_U_TURN,      /* 掉头 */
    NAV_MANEUVER_KEEP_LEFT,   /* 靠左 */
    NAV_MANEUVER_KEEP_RIGHT,  /* 靠右 */
    NAV_MANEUVER_ARRIVE,      /* 到达 */
    NAV_MANEUVER_COUNT
} nav_maneuver_t;

/*
 * 导航状态（对齐评估文档 §3.5）。
 *
 * ⚠️ 安全语义：与车速一样，**数据过期绝不得继续显示为有效导航**
 *    （沿用项目既有的 §6.3 原则）。
 */
typedef enum {
    NAV_STATE_IDLE = 0,   /* 未开始（用户没在导航）→ 卡片显示"未开始导航" */
    NAV_STATE_ACTIVE,     /* 导航中 */
    NAV_STATE_REROUTING,  /* 重新规划中 */
    NAV_STATE_ARRIVED,    /* 已到达 */
    NAV_STATE_STALE,      /* 数据过期（超时）*/
    NAV_STATE_NO_DATA,    /* 无数据（BLE 断开 / 从未收到）*/
    NAV_STATE_COUNT
} nav_state_t;

/* 距离无效哨兵（与"距离很远"区分开）*/
#define NAV_DISTANCE_INVALID (-1)
/* 距离上限（米）：超过视为不合理数据（约 600 km），用于拦截明显错误 */
#define NAV_DISTANCE_MAX_M   (600000)

/* 导航数据过期阈值（毫秒）。
 * 比遥测的 2.5s **长得多**：导航数据变化慢（一次转弯可能几分钟），
 * 用遥测阈值会频繁误报过期。 */
#define NAV_STALE_MS (10000)

/*
 * 格式化"距下一转弯距离"。
 *   < 1000 m   → "500 m"（整数米）
 *   1000..9999 → "1.2 km"（一位小数）
 *   >= 10000   → "12 km"（整数公里）
 *   无效/负数/超上限 → "--"
 * @param meters 距离（米），或 NAV_DISTANCE_INVALID
 */
void nav_format_distance(int meters, char *buf, size_t sz);

/*
 * 仅格式化数字部分（供 UI 用大号字体显示，单位另用小字）。
 * 与 nav_format_distance 保持同一套取整规则，保证两者一致。
 *   < 1000 → "500"；1000..9999 → "1.2"；>=10000 → "12"；无效 → "--"
 */
void nav_format_distance_value(int meters, char *buf, size_t sz);

/* 单位部分："m" / "km" / ""（无效时为空，避免显示 "500 km" 这类误导）*/
const char *nav_distance_unit(int meters);

/*
 * **导航是否"刚刚开始"** —— 决定要不要自动跳到导航表盘。
 *
 * 规则（刻意收得很紧）：
 *   - 只有 `idle` / `no_data` → `active` / `rerouting` 才算"开始"；
 *   - `stale` → `active` **不算** —— 那只是导航数据晚到，用户可能早就
 *     手动划走了，这时把他拽回导航页很讨厌；
 *   - `arrived` → `active` **不算** —— 同上。
 *
 * 这样"开始导航就自动跳一次、之后随便划走"才成立：只在**上升沿**跳。
 *
 * 纯函数，可单测（见 tests/test_nav.c）。
 */
bool nav_should_auto_jump(nav_state_t prev, nav_state_t now);

/* 动作/状态的稳定标识（ASCII，供日志与协议调试；不是屏幕文案）*/
const char *nav_maneuver_name(nav_maneuver_t m);
const char *nav_state_name(nav_state_t st);

/*
 * 屏幕显示文案（ASCII，受现有字体字形集限制）。
 * 注意：项目字体 `label_26` 无 CJK 字形，故此处必须为 ASCII。
 */
const char *nav_state_text(nav_state_t st);

/*
 * 该状态下是否应显示转向图标（UI 据此决定显示/隐藏箭头区域）。
 *   ACTIVE / REROUTING / ARRIVED → true（ARRIVED 时图标换成"目的地"符号）
 *   IDLE / STALE / NO_DATA       → false（过期时**绝不**继续显示旧指引）
 */
bool nav_shows_maneuver(nav_state_t st);

/* 动作值合法性（越界一律视为 UNKNOWN，避免协议给错值导致数组越界）*/
nav_maneuver_t nav_clamp_maneuver(int raw);

/* 状态值合法性（越界 → NO_DATA，保守：宁可显示无数据也不假装在导航）*/
nav_state_t nav_clamp_state(int raw);

#ifdef __cplusplus
}
#endif

#endif /* NAV_H */
