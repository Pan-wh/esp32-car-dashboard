/**
 * test_auto_bright.c —— 按白天/夜间时段自动调亮度测试（PC 上运行，无硬件）
 *
 * 运行：cd tests && make test_auto_bright && ./test_auto_bright
 *
 * 覆盖：常规时段、过渡渐变（单调性）、跨午夜区间、零长度区间、
 *       边界（同值/开关/0 与 255）、以及"亮度永不为 0"的安全约束。
 */
#include <stdio.h>
#include <string.h>

#include "auto_bright.h"

static int g_pass, g_fail;

#define CHECK(cond, fmt, ...)                                                  \
    do {                                                                       \
        if (cond) { g_pass++; }                                                \
        else {                                                                 \
            g_fail++;                                                          \
            printf("  ✗ %s:%d  " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__); \
        }                                                                      \
    } while (0)

static int hhmm(int h, int m) { return h * 60 + m; }

/** 构造一个测试配置：07:00-18:30 白天，夜间 40 / 白天 220，过渡 30 分钟 */
static auto_bright_config_t cfg_basic(void)
{
    auto_bright_config_t c = auto_bright_default_config();
    c.enabled = 1;
    c.day_start_min = hhmm(7, 0);
    c.day_end_min = hhmm(18, 30);
    c.day_level = 220;
    c.night_level = 40;
    c.ramp_min = 30;
    return c;
}

int main(void)
{
    printf("=== 常规时段 ===\n");
    {
        auto_bright_config_t c = cfg_basic();

        /* 正午 → 白天亮度 */
        CHECK(auto_bright_level_at(hhmm(12, 0), &c) == 220, "正午应为白天亮度");
        /* 深夜 → 夜间亮度 */
        CHECK(auto_bright_level_at(hhmm(23, 0), &c) == 40, "深夜应为夜间亮度");
        CHECK(auto_bright_level_at(hhmm(3, 0), &c) == 40, "凌晨应为夜间亮度");

        /* 白天区间内任意时刻都是白天亮度（远离过渡）*/
        CHECK(auto_bright_level_at(hhmm(8, 0), &c) == 220, "08:00 应是白天亮度");
        CHECK(auto_bright_level_at(hhmm(18, 0), &c) == 220, "18:00 应是白天亮度");

        /* 时段判定 */
        CHECK(auto_bright_period_at(hhmm(12, 0), &c) == AUTO_BRIGHT_PERIOD_DAY, "正午=白天");
        CHECK(auto_bright_period_at(hhmm(23, 0), &c) == AUTO_BRIGHT_PERIOD_NIGHT, "深夜=夜间");
    }

    printf("=== 过渡渐变 ===\n");
    {
        auto_bright_config_t c = cfg_basic();
        const int start = hhmm(7, 0), end = hhmm(18, 30), ramp = 30;

        /* 过渡段起点（开始前正好 ramp 分钟）→ 夜间亮度 */
        CHECK(auto_bright_level_at(start - ramp, &c) == 40,
              "★ 天亮过渡起点应为夜间亮度（实际 %d）",
              auto_bright_level_at(start - ramp, &c));

        /* 白天起点 → 正好到白天亮度 */
        CHECK(auto_bright_level_at(start, &c) == 220,
              "★ 到达白天起点时应恰好是白天亮度（实际 %d）",
              auto_bright_level_at(start, &c));

        /* 过渡中点 → 严格介于两者之间（这是"渐变"的核心）*/
        const int mid_rise = auto_bright_level_at(start - ramp / 2, &c);
        CHECK(mid_rise > 40 && mid_rise < 220,
              "★ 天亮过渡中点应介于夜间与白天之间（实际 %d）", mid_rise);

        /* 天黑：终点后 ramp/2 → 介于两者之间 */
        const int mid_set = auto_bright_level_at(end + ramp / 2, &c);
        CHECK(mid_set > 40 && mid_set < 220,
              "★ 天黑过渡中点应介于夜间与白天之间（实际 %d）", mid_set);

        /* 天黑过渡结束 → 夜间亮度 */
        CHECK(auto_bright_level_at(end + ramp, &c) == 40,
              "★ 天黑过渡结束应为夜间亮度（实际 %d）",
              auto_bright_level_at(end + ramp, &c));

        /* 单调性：天亮段必须单调不降（否则亮度会来回抖）*/
        int prev = -1;
        bool mono = true;
        for (int m = start - ramp; m <= start; m++) {
            int lv = auto_bright_level_at(m, &c);
            if (prev >= 0 && lv < prev) mono = false;
            prev = lv;
        }
        CHECK(mono, "★ 天亮过渡必须单调不降");

        prev = -1;
        mono = true;
        for (int m = end; m <= end + ramp; m++) {
            int lv = auto_bright_level_at(m, &c);
            if (prev >= 0 && lv > prev) mono = false;
            prev = lv;
        }
        CHECK(mono, "★ 天黑过渡必须单调不升");
    }

    printf("=== 用户左右滑动调整区间 ===\n");
    {
        auto_bright_config_t c = cfg_basic();

        /* 用户把白天起点往左拖到 05:30：06:00 应变亮 */
        c.day_start_min = hhmm(5, 30);
        CHECK(auto_bright_level_at(hhmm(6, 0), &c) == 220,
              "★ 起点拖到 05:30 后，06:00 应是白天亮度（实际 %d）",
              auto_bright_level_at(hhmm(6, 0), &c));

        /* 起点拖到 05:30 后，04:20（早于 start-ramp=05:00）仍是夜间 */
        CHECK(auto_bright_level_at(hhmm(4, 20), &c) == 40,
              "起点提前后，更早的时刻仍应是夜间");

        /* 用户把白天结束往右拖到 21:00：20:30 应仍亮 */
        auto_bright_config_t c2 = cfg_basic();
        c2.day_end_min = hhmm(21, 0);
        CHECK(auto_bright_level_at(hhmm(20, 30), &c2) == 220,
              "★ 终点拖到 21:00 后，20:30 应是白天亮度（实际 %d）",
              auto_bright_level_at(hhmm(20, 30), &c2));
        /* 而原来（18:30 结束）20:30 已是夜间 */
        auto_bright_config_t base = cfg_basic();
        CHECK(auto_bright_level_at(hhmm(20, 30), &base) == 40,
              "默认 18:30 结束时，20:30 应是夜间亮度");
    }

    printf("=== 跨午夜区间（值夜班场景：20:00→06:00 算白天）===\n");
    {
        auto_bright_config_t c = cfg_basic();
        c.day_start_min = hhmm(20, 0);  /* 晚上 8 点亮 */
        c.day_end_min = hhmm(6, 0);     /* 早上 6 点暗 */
        c.ramp_min = 30;

        /* 23:00 在白天区间内（跨午夜）*/
        CHECK(auto_bright_level_at(hhmm(23, 0), &c) == 220,
              "★ 跨午夜区间内 23:00 应是白天亮度（实际 %d）",
              auto_bright_level_at(hhmm(23, 0), &c));
        CHECK(auto_bright_level_at(hhmm(2, 0), &c) == 220,
              "★ 跨午夜区间内 02:00 应是白天亮度（实际 %d）",
              auto_bright_level_at(hhmm(2, 0), &c));

        /* 白天区间外（12:00）应是夜间 */
        CHECK(auto_bright_level_at(hhmm(12, 0), &c) == 40,
              "★ 跨午夜区间外的 12:00 应是夜间亮度（实际 %d）",
              auto_bright_level_at(hhmm(12, 0), &c));

        /* 时段判定也要跨午夜正确 */
        CHECK(auto_bright_period_at(hhmm(23, 0), &c) == AUTO_BRIGHT_PERIOD_DAY,
              "23:00 应判为白天");
        CHECK(auto_bright_period_at(hhmm(12, 0), &c) == AUTO_BRIGHT_PERIOD_NIGHT,
              "12:00 应判为夜间");

        /* 全天采样不得越界（跨午夜最容易写出越界）*/
        bool ok = true;
        for (int m = 0; m < 1440; m++) {
            int lv = auto_bright_level_at(m, &c);
            if (lv < 1 || lv > 255) { ok = false; printf("      越界于 %d 分: %d\n", m, lv); break; }
        }
        CHECK(ok, "★ 跨午夜配置全天采样亮度应在 1-255");
    }

    printf("=== 安全边界 ===\n");
    {
        auto_bright_config_t c = cfg_basic();

        /* ★ 亮度**永不为 0**：0 会让屏全黑，用户会以为死机 */
        bool ok = true;
        for (int m = 0; m < 1440; m++) {
            int lv = auto_bright_level_at(m, &c);
            if (lv < 1 || lv > 255) { ok = false; break; }
        }
        CHECK(ok, "★ 全天采样亮度均在 1-255");

        /* 配置被写成 0 也要夹到 1 */
        auto_bright_config_t z = cfg_basic();
        z.day_level = 0;
        z.night_level = 0;
        CHECK(auto_bright_level_at(hhmm(12, 0), &z) == 1, "★ 白天亮度 0 应夹到 1");
        CHECK(auto_bright_level_at(hhmm(23, 0), &z) == 1, "★ 夜间亮度 0 应夹到 1");

        /* 超上限夹到 255 */
        auto_bright_config_t hi = cfg_basic();
        hi.day_level = 999;
        CHECK(auto_bright_level_at(hhmm(12, 0), &hi) == 255, "超上限应夹到 255");

        /* ramp = 0：硬切换，不得除零 */
        auto_bright_config_t r0 = cfg_basic();
        r0.ramp_min = 0;
        CHECK(auto_bright_level_at(hhmm(7, 0) - 1, &r0) == 40, "ramp=0 时起点前一分钟是夜间");
        CHECK(auto_bright_level_at(hhmm(7, 0), &r0) == 220, "ramp=0 时起点即白天");

        /* day_start == day_end（零长度白天）→ 全天夜间（保守，不会突然变亮）*/
        auto_bright_config_t eq = cfg_basic();
        eq.day_start_min = eq.day_end_min = hhmm(12, 0);
        CHECK(auto_bright_level_at(hhmm(12, 0), &eq) == 40,
              "★ 零长度白天应视为全天夜间（实际 %d）",
              auto_bright_level_at(hhmm(12, 0), &eq));

        /* 白天/夜间亮度相同 → 恒为那个值（不该出现 NaN/越界）*/
        auto_bright_config_t same = cfg_basic();
        same.day_level = same.night_level = 128;
        CHECK(auto_bright_level_at(hhmm(3, 0), &same) == 128, "同值时深夜也是该值");
        CHECK(auto_bright_level_at(hhmm(12, 0), &same) == 128, "同值时正午也是该值");

        /* 关闭自动 → 返回白天亮度（兜底可见）*/
        auto_bright_config_t off = cfg_basic();
        off.enabled = 0;
        CHECK(auto_bright_level_at(hhmm(23, 0), &off) == 220, "关闭自动时兜底给白天亮度");

        /* NULL 配置不得崩溃 */
        CHECK(auto_bright_level_at(hhmm(12, 0), NULL) >= 1, "NULL 配置不得崩溃");
        CHECK(auto_bright_period_at(hhmm(12, 0), NULL) == AUTO_BRIGHT_PERIOD_NIGHT,
              "NULL 配置的时段判定不得崩溃");

        /* 越界的 now_min 也要能处理（归一到当天）*/
        CHECK(auto_bright_level_at(-60, &c) == auto_bright_level_at(1380, &c),
              "负分钟数应按环形归一（-60 == 1380）");
        CHECK(auto_bright_level_at(1440 + 60, &c) == auto_bright_level_at(60, &c),
              "超过 1440 的分钟数也应归一");

        /* 时:分换算 */
        CHECK(auto_bright_minutes_of_day(13, 45) == 825, "13:45 应为 825 分钟");
    }

    printf("=== 过渡窗口重叠（属性测试抓到的 bug）===\n");
    {
        /*
         * 回归用例：跨午夜配置下，若 ramp 太大，天亮/天黑两个过渡窗口
         * 会在**夜间段**里重叠，导致亮度突跳。
         *
         * 实测现象（修复前）：ds=10:51 de=06:33 ramp=222，夜间段仅 258 分钟，
         *   07:05 -> 114，07:10 -> 200（一步跳 86）
         * 修复：ramp 被夹到"夜间段长度/2"，两个窗口不再重叠。
         */
        auto_bright_config_t c = cfg_basic();
        c.day_start_min = 651;  /* 10:51 */
        c.day_end_min = 393;    /* 06:33 */
        c.day_level = 100;
        c.night_level = 200;
        c.ramp_min = 222;       /* 远大于夜间段的一半(129) → 应被夹取 */

        /* 逐个相邻分钟检查，不允许出现大跳变 */
        int worst = 0;
        for (int m = 0; m < 1440; m++) {
            int a = auto_bright_level_at(m, &c);
            int b = auto_bright_level_at((m + 1) % 1440, &c);
            int d = b - a;
            if (d < 0) d = -d;
            if (d > worst) worst = d;
        }
        /*
         * 夹取后 ramp=129，day/night 差 100 → 每步最多约 1。
         * 给到 3 的容差（跨越"白天/夜间"边界的相邻分钟可能有取整差异）。
         */
        CHECK(worst <= 3, "★ 跨午夜 + 大 ramp 不得突跳（最大相邻差 %d，期望 ≤3）", worst);

        /* 且全天亮度必须合法 */
        bool ok = true;
        for (int m = 0; m < 1440; m++) {
            int v = auto_bright_level_at(m, &c);
            if (v < 1 || v > 255) { ok = false; break; }
        }
        CHECK(ok, "跨午夜大 ramp 全天亮度应在 1-255");
    }

    printf("=== ramp 超过夜间段一半时会被夹取 ===\n");
    {
        /* 白天占 22 小时，夜间段仅 2 小时(120 分钟) */
        auto_bright_config_t c = cfg_basic();
        c.day_start_min = 60;    /* 01:00 */
        c.day_end_min = 1380;    /* 23:00 */
        c.day_level = 200;
        c.night_level = 20;
        c.ramp_min = 240;        /* 请求 240 分钟，但夜间段只有 120 → 夹到 60 */

        /* 夜间段中点（00:00 附近）应仍可达夜间亮度 */
        CHECK(auto_bright_level_at(0, &c) == 20,
              "★ 夜间段应能取到夜间亮度（实际 %d）", auto_bright_level_at(0, &c));

        int worst = 0;
        for (int m = 0; m < 1440; m++) {
            int a = auto_bright_level_at(m, &c);
            int b = auto_bright_level_at((m + 1) % 1440, &c);
            int d = b - a; if (d < 0) d = -d;
            if (d > worst) worst = d;
        }
        CHECK(worst <= 4, "★ 夜间段很短时也不得突跳（最大相邻差 %d）", worst);
    }

    printf("=== 默认配置合理性 ===\n");
    {
        auto_bright_config_t c = auto_bright_default_config();
        CHECK(c.enabled == 1, "默认应启用自动");
        CHECK(c.day_start_min < c.day_end_min,
              "默认白天区间应有序（%d < %d）", c.day_start_min, c.day_end_min);
        CHECK(c.day_level > c.night_level,
              "★ 白天亮度应高于夜间（否则逻辑反了，实际 %d vs %d）",
              c.day_level, c.night_level);
        CHECK(c.night_level >= 1 && c.day_level <= 255, "默认亮度应在合法范围");
        CHECK(c.ramp_min > 0, "默认应有过渡时间（避免亮度骤变）");
        CHECK(auto_bright_level_at(hhmm(12, 0), &c) == c.day_level, "默认：正午=白天亮度");
        CHECK(auto_bright_level_at(hhmm(2, 0), &c) == c.night_level, "默认：凌晨=夜间亮度");
    }

    printf("结果: %d 通过, %d 失败\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
