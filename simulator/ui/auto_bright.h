/**
 * auto_bright.h —— 按**白天/夜间时段**自动调亮度（纯逻辑，可在 PC 上单测）
 *
 * ## 要解决的真机痛点
 *
 * **晚上太亮刺眼、白天太暗看不清** —— 用户每次都得手动拖滑块。
 *
 * ## 设计（2026-10-17 与用户确认）
 *
 * **不做天文计算**（不算日出日落）。用户直接给两个时间点：
 *
 * ```
 *        夜间亮度                     白天亮度              夜间亮度
 *   ──●────────────●═══════════════════●──────────────●──
 *   0:00       day_start            day_end         24:00
 *              （如 07:00）        （如 18:30）
 *                 ↑ ramp ↑            ↑ ramp ↑
 * ```
 *
 * - `day_start` / `day_end`：白天区间的起止（**用户可左右滑动调整**）
 * - 两个时间点附近各有 `ramp` 分钟**线性过渡**，避免亮度骤变闪眼
 * - 白天区间内用 `day` 亮度，区间外用 `night` 亮度
 *
 * 为什么不用天文计算：用户实测"早上 7 点该亮、晚上 6 点半该暗"就够用，
 * 而且**手动区间可预测、可解释**（用户知道为什么此刻是这个亮度）。
 * 日出日落算法还要位置信息、还要处理极昼极夜，复杂度换不来实际收益。
 *
 * ## 本模块管不了什么（不要假装它能）
 *
 * 时间方案**管不了**环境光突变，它们与钟点无关：
 *   - 白天进隧道 / 地库 / 立交桥下
 *   - 夜晚被对向车灯直射
 * 这些只能靠用户手动覆盖（见"手动优先"）。
 *
 * ## 手动优先（关键设计，已与用户确认）
 *
 * 用户一旦手动调过亮度，自动曲线就**让位**，直到：
 *   - 用户在 App 里重新打开自动开关，或
 *   - 设备重启（重新上线）
 * 否则会出现"我调好了它又给我改回去"的经典烦躁体验。
 *
 * 本文件不依赖任何硬件/LVGL，可在 PC 上直接单测。
 */
#ifndef AUTO_BRIGHT_H
#define AUTO_BRIGHT_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 默认过渡时长（分钟）：白天区间起止各 30 分钟线性渐变 */
#define AUTO_BRIGHT_DEFAULT_RAMP_MIN 30

/** 默认时段与亮度（针对本机 AMOLED 实测观感）*/
#define AUTO_BRIGHT_DEFAULT_DAY_START_MIN (7 * 60)   /* 07:00 */
#define AUTO_BRIGHT_DEFAULT_DAY_END_MIN   (18 * 60 + 30) /* 18:30 */
#define AUTO_BRIGHT_DEFAULT_DAY_LEVEL     220
#define AUTO_BRIGHT_DEFAULT_NIGHT_LEVEL   40

/**
 * 亮度曲线配置。
 *
 * ⚠️ 时间用"当天 0 点起的分钟数"（0-1439），**不是**时:分结构 ——
 *    这样比较与插值都是一次整数运算，也不会有"跨天"的歧义。
 */
typedef struct {
    int enabled;       /**< 1 启用自动；0 关闭（用户手动模式）*/
    int day_start_min; /**< 白天开始（分钟，0-1439）*/
    int day_end_min;   /**< 白天结束（分钟，0-1439）*/
    int day_level;     /**< 白天亮度（1-255）*/
    int night_level;   /**< 夜间亮度（1-255）*/
    int ramp_min;      /**< 过渡时长（分钟，0 = 硬切换）*/
} auto_bright_config_t;

/** 默认配置 */
auto_bright_config_t auto_bright_default_config(void);

/** 时:分 → 当天分钟数 */
int auto_bright_minutes_of_day(int hour, int minute);

/**
 * 计算某时刻的亮度。
 *
 * @param now_min  当前本地时间（当天分钟数，0-1439）
 * @param cfg      配置
 * @return 亮度 1-255（已夹取；**永不为 0** —— 0 会让屏全黑，用户会以为死机）
 *
 * ## 边界处理（都有单测）
 *
 * - `enabled = 0`   → 返回白天亮度（兜底可见；调用方本就不该用）
 * - `ramp = 0`      → 硬切换（不除零）
 * - `day_start == day_end` → 视为**全天夜间**（零长度白天没有意义）
 * - `day_start > day_end` → 视为**跨午夜**的白天（如 20:00→06:00 值夜班场景）
 * - `cfg == NULL`   → 返回 1（不崩溃）
 */
int auto_bright_level_at(int now_min, const auto_bright_config_t *cfg);

/**
 * 当前处于哪个时段（供界面提示与测试）。
 *
 * 注意：过渡段（ramp）归属"目标时段"：
 *   - 天亮过渡（day_start 前 ramp 分钟内）→ 算 DAY
 *   - 天黑过渡（day_end 后 ramp 分钟内）→ 算 NIGHT
 * 这样"时段"与"亮度趋势"一致，不会出现"显示白天但亮度在降"的矛盾。
 */
typedef enum {
    AUTO_BRIGHT_PERIOD_NIGHT = 0,
    AUTO_BRIGHT_PERIOD_DAY = 1,
} auto_bright_period_t;

auto_bright_period_t auto_bright_period_at(int now_min, const auto_bright_config_t *cfg);

#ifdef __cplusplus
}
#endif

#endif /* AUTO_BRIGHT_H */
