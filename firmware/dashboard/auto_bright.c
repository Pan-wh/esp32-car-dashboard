/**
 * auto_bright.c —— 按白天/夜间时段自动调亮度（纯 C，模拟器/固件共用）
 *
 * 设计说明见 auto_bright.h。本文件**不做任何硬件调用**，故可在 PC 上单测。
 *
 * ## 为什么没有天文计算
 *
 * 2026-10-17 与用户确认：**手动给两个时间点即可**（如 07:00 亮 / 18:30 暗），
 * 用户可左右滑动调整。比"算日出日落"更简单、更可预测，且不需要位置信息。
 * 详见 auto_bright.h 的设计说明。
 */
#include "auto_bright.h"

#include <stddef.h> /* NULL —— 不依赖其他头文件顺带引入，保证可独立编译 */

auto_bright_config_t auto_bright_default_config(void)
{
    auto_bright_config_t c;
    c.enabled = 1;
    c.day_start_min = AUTO_BRIGHT_DEFAULT_DAY_START_MIN;
    c.day_end_min = AUTO_BRIGHT_DEFAULT_DAY_END_MIN;
    c.day_level = AUTO_BRIGHT_DEFAULT_DAY_LEVEL;
    c.night_level = AUTO_BRIGHT_DEFAULT_NIGHT_LEVEL;
    c.ramp_min = AUTO_BRIGHT_DEFAULT_RAMP_MIN;
    return c;
}

int auto_bright_minutes_of_day(int hour, int minute)
{
    return hour * 60 + minute;
}

/** 夹取到 1-255（**不允许 0**：屏全黑会被当成死机，见 control_clamp_brightness）*/
static int clamp_level(int v)
{
    if (v < 1) return 1;
    if (v > 255) return 255;
    return v;
}

/** 把分钟数归一到 [0,1440) —— 用户可能拖到边界外 */
static int wrap_min(int m)
{
    m %= 1440;
    if (m < 0) m += 1440;
    return m;
}

/** 线性插值并四舍五入 */
static int lerp_int(int a, int b, double t)
{
    if (t <= 0.0) return a;
    if (t >= 1.0) return b;
    return (int)(a + (b - a) * t + 0.5);
}

/**
 * 判断"当前是否属于白天"（含过渡段的归属规则，见头文件说明）。
 *
 * 把时段判定单独抽出来，是因为"跨午夜"（day_start > day_end）这个情形
 * 很容易写错 —— 单测里专门覆盖了它。
 */
static bool is_day_period(int now_min, const auto_bright_config_t *cfg)
{
    const int start = wrap_min(cfg->day_start_min);
    const int end = wrap_min(cfg->day_end_min);

    /* 零长度白天没有意义 → 视为全天夜间（更保守：不会突然变亮）*/
    if (start == end) return false;

    if (start < end) {
        /* 常规：白天在 [start, end) 内 */
        return now_min >= start && now_min < end;
    }
    /* 跨午夜：白天 = [start, 24:00) ∪ [0:00, end) */
    return now_min >= start || now_min < end;
}

auto_bright_period_t auto_bright_period_at(int now_min, const auto_bright_config_t *cfg)
{
    if (cfg == NULL) return AUTO_BRIGHT_PERIOD_NIGHT;
    now_min = wrap_min(now_min);
    return is_day_period(now_min, cfg) ? AUTO_BRIGHT_PERIOD_DAY : AUTO_BRIGHT_PERIOD_NIGHT;
}

int auto_bright_level_at(int now_min, const auto_bright_config_t *cfg)
{
    if (cfg == NULL) return 1;

    /* 自动关闭 → 返回白天亮度（调用方本就不该用，兜底给个可见值）*/
    if (!cfg->enabled) return clamp_level(cfg->day_level);

    const int day = clamp_level(cfg->day_level);
    const int night = clamp_level(cfg->night_level);

    /* 白天/夜间相同 → 没什么可渐变的，直接返回（也避免下面的边界算术）*/
    if (day == night) return day;

    now_min = wrap_min(now_min);

    int ramp = cfg->ramp_min;
    if (ramp < 0) ramp = 0;
    const bool inDay = is_day_period(now_min, cfg);

    if (ramp == 0) return inDay ? day : night;

    const int start = wrap_min(cfg->day_start_min);
    const int end = wrap_min(cfg->day_end_min);

    /*
     * ⚠️ 零长度白天（start == end）必须在过渡判定**之前**返回。
     *    否则 fromEnd = (now - end + 1440) % 1440 恒为 0，会被误判为
     *    "刚过天黑终点"，于是返回 day 亮度 —— 而零长度白天的语义是
     *    **全天夜间**。本文件的自测用例抓到了这一条。
     */
    if (start == end) return night;

    /*
     * ★ 限制 ramp 不得超过"**夜间段**长度的一半"，防止两个过渡窗口重叠。
     *
     * 天亮过渡在 [start-ramp, start)，天黑过渡在 [end, end+ramp) ——
     * 这两段都落在**夜间段**里（白天段的两侧边缘之外）。若 2*ramp 超过
     * 夜间段长度，两个窗口就会重叠：同一时刻既算"正在天亮"又算"正在天黑"，
     * 而下面的代码用 if/else 只会取其一，结果是**亮度突跳**。
     *
     * 实测（属性测试抓到）：ds=10:51 de=06:33 ramp=222，
     * 夜间段仅 258 分钟，两个 222 分钟的过渡必然重叠 →
     * 07:05 是 114，07:10 突然变成 200。
     *
     * ⚠️ 早期版本误用"白天段/2"做上限，结果这个配置根本触发不到约束
     *    （白天段 1182 分钟，cap=591 > 222），bug 依然复现。
     *    约束必须针对**夜间段**。
     *
     * 本机默认 ramp=30、夜间段约 780 分钟，远不会触发；但协议允许 ramp
     * 到 240，用户把白天段拉得很长（夜间段很短）再配长过渡就会撞上。
     */
    {
        /* 夜间段长度（环形）：白天段的两侧边缘之外的部分 */
        const int dayLen = (start <= end) ? (end - start) : (1440 - start + end);
        const int nightLen = 1440 - dayLen;
        /* 每个过渡最多占夜间段的一半，两个加起来刚好不重叠 */
        const int cap = nightLen / 2;
        /*
         * 若夜间段极短（< 2 分钟），cap 可能为 0 → ramp 退化为硬切换。
         * 这比"突跳"温和：用户几乎全天都设成白天时，
         * 本来也没有真正的夜间可过渡。
         */
        if (ramp > cap) ramp = cap;
        if (ramp == 0) return inDay ? day : night;
    }

    /*
     * 过渡段处理。用"到边界的有符号距离"判断，而不是简单比较大小 ——
     * 这样**跨午夜的白天区间**（start > end）也能正确工作，
     * 不必为它再写一套分支（写两套迟早改漏一处）。
     *
     * 约定用 [0,1440) 环形距离：
     *   起点前 ramp 分钟内 → 从 night 升到 day（天亮）
     *   终点后 ramp 分钟内 → 从 day 降到 night（天黑）
     */
    /* 当前时刻距"白天起点"的环形距离（向前为正）*/
    const int toStart = (start - now_min + 1440) % 1440;
    const int fromEnd = (now_min - end + 1440) % 1440;

    if (toStart > 0 && toStart <= ramp) {
        /* 天亮过渡：还有 toStart 分钟到起点 → 进度 = 1 - toStart/ramp */
        const double t = 1.0 - (double)toStart / (double)ramp;
        return clamp_level(lerp_int(night, day, t));
    }
    if (fromEnd >= 0 && fromEnd < ramp) {
        /* 天黑过渡：已过终点 fromEnd 分钟 → 进度 = fromEnd/ramp */
        const double t = (double)fromEnd / (double)ramp;
        return clamp_level(lerp_int(day, night, t));
    }

    return inDay ? day : night;
}
