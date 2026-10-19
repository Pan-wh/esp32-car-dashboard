/**
 * test_bright_parse.c —— 亮度曲线包解析测试（PC 上运行，无硬件）
 *
 * 运行：cd tests && make test_bright_parse && ./test_bright_parse
 *
 * 覆盖：类型分流、字段全给/部分给、越界夹取、亮度不得为 0、
 *       畸形输入不崩、向前兼容（未知字段）、以及"半解析不污染配置"。
 */
#include <stdio.h>
#include <string.h>

#include "bright_parse.h"

static int g_pass, g_fail;

#define CHECK(cond, fmt, ...)                                                  \
    do {                                                                       \
        if (cond) { g_pass++; }                                                \
        else {                                                                 \
            g_fail++;                                                          \
            printf("  ✗ %s:%d  " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__); \
        }                                                                      \
    } while (0)

/** 便捷：解析一条 C 字符串 */
static bool parse_str(const char *s, bright_packet_t *out)
{
    return bright_parse(s, strlen(s), out);
}

int main(void)
{
    printf("=== 类型分流 ===\n");
    {
        const char *yes = "{\"t\":\"bright\",\"on\":1}";
        CHECK(bright_is_bright_message(yes, strlen(yes)), "应识别为亮度包");

        const char *nav = "{\"t\":\"nav\",\"maneuver\":2}";
        CHECK(!bright_is_bright_message(nav, strlen(nav)), "nav 包不应被当成亮度包");

        const char *tl = "{\"v\":1,\"ts_utc_ms\":123,\"location_valid\":false}";
        CHECK(!bright_is_bright_message(tl, strlen(tl)), "普通遥测包不应被当成亮度包");

        /* 字段顺序不同也要能识别（不能假设 t 在第一项）*/
        const char *late = "{\"on\":1,\"day\":200,\"t\":\"bright\"}";
        CHECK(bright_is_bright_message(late, strlen(late)), "t 在后面的包也应识别");

        CHECK(!bright_is_bright_message(NULL, 0), "NULL 不得崩溃");
        CHECK(!bright_is_bright_message("", 0), "空串不得崩溃");
        CHECK(!bright_is_bright_message("{", 1), "截断输入不得崩溃");
    }

    printf("=== 完整包 ===\n");
    {
        bright_packet_t p;
        const char *s = "{\"t\":\"bright\",\"v\":1,\"on\":1,\"ds\":420,\"de\":1110,"
                        "\"day\":220,\"night\":40,\"ramp\":30}";
        CHECK(parse_str(s, &p), "完整包应解析成功");
        CHECK(p.has_on && p.on == 1, "on 应为 1（实际 has=%d val=%d）", p.has_on, p.on);
        CHECK(p.has_ds && p.ds == 420, "ds 应为 420（07:00）实际 %d", p.ds);
        CHECK(p.has_de && p.de == 1110, "de 应为 1110（18:30）实际 %d", p.de);
        CHECK(p.has_day && p.day == 220, "day 应为 220（实际 %d）", p.day);
        CHECK(p.has_night && p.night == 40, "night 应为 40（实际 %d）", p.night);
        CHECK(p.has_ramp && p.ramp == 30, "ramp 应为 30（实际 %d）", p.ramp);
    }

    printf("=== 部分字段（其余 has_* 应为 0 = 未提供）===\n");
    {
        bright_packet_t p;
        const char *s = "{\"t\":\"bright\",\"night\":60}";
        CHECK(parse_str(s, &p), "只给 night 也应成功");
        CHECK(p.has_night && p.night == 60, "night 应为 60");
        CHECK(!p.has_on, "★ 未提供的 on 的 has_on 应为 0（不能与 on=0 混淆）");
        CHECK(!p.has_ds, "未提供的 ds 的 has_ds 应为 0");
        CHECK(!p.has_day, "未提供的 day 的 has_day 应为 0");
        CHECK(!p.has_ramp, "未提供的 ramp 的 has_ramp 应为 0");

        /* 只有 t（全默认）也应成功 */
        const char *bare = "{\"t\":\"bright\"}";
        CHECK(parse_str(bare, &p), "只有 t 也应解析成功");
        CHECK(!p.has_on && !p.has_ds && !p.has_day, "所有字段的 has_* 均应为 0");
    }

    printf("=== 应用到配置：只覆盖提供的字段 ===\n");
    {
        auto_bright_config_t cfg = auto_bright_default_config();
        const int orig_start = cfg.day_start_min;
        const int orig_day = cfg.day_level;

        bright_packet_t p;
        parse_str("{\"t\":\"bright\",\"night\":88}", &p);
        bright_apply_packet(&cfg, &p);

        CHECK(cfg.night_level == 88, "night 应被更新为 88（实际 %d）", cfg.night_level);
        CHECK(cfg.day_start_min == orig_start, "★ 未提供的 ds 不应改动（期望 %d 实际 %d）",
              orig_start, cfg.day_start_min);
        CHECK(cfg.day_level == orig_day, "★ 未提供的 day 不应改动（期望 %d 实际 %d）",
              orig_day, cfg.day_level);
    }

    printf("=== 越界夹取 ===\n");
    {
        auto_bright_config_t cfg = auto_bright_default_config();

        /* ★ 亮度 0 必须夹到 1：0 会让屏全黑，用户以为死机 */
        bright_packet_t p;
        parse_str("{\"t\":\"bright\",\"day\":0,\"night\":0}", &p);
        bright_apply_packet(&cfg, &p);
        CHECK(cfg.day_level == 1, "★ day=0 应夹到 1（实际 %d）", cfg.day_level);
        CHECK(cfg.night_level == 1, "★ night=0 应夹到 1（实际 %d）", cfg.night_level);

        /* 超上限夹到 255 */
        parse_str("{\"t\":\"bright\",\"day\":999}", &p);
        bright_apply_packet(&cfg, &p);
        CHECK(cfg.day_level == 255, "day 超上限应夹到 255（实际 %d）", cfg.day_level);

        /* 负亮度夹到 1 */
        parse_str("{\"t\":\"bright\",\"night\":-50}", &p);
        bright_apply_packet(&cfg, &p);
        CHECK(cfg.night_level == 1, "负 night 应夹到 1（实际 %d）", cfg.night_level);

        /* 时间越界夹到 [0,1439] */
        parse_str("{\"t\":\"bright\",\"ds\":-10,\"de\":5000}", &p);
        bright_apply_packet(&cfg, &p);
        CHECK(cfg.day_start_min == 0, "负 ds 应夹到 0（实际 %d）", cfg.day_start_min);
        CHECK(cfg.day_end_min == 1439, "超界 de 应夹到 1439（实际 %d）", cfg.day_end_min);

        /* ramp 夹到 [0,240] */
        parse_str("{\"t\":\"bright\",\"ramp\":9999}", &p);
        bright_apply_packet(&cfg, &p);
        CHECK(cfg.ramp_min == 240, "超大 ramp 应夹到 240（实际 %d）", cfg.ramp_min);
        parse_str("{\"t\":\"bright\",\"ramp\":-5}", &p);
        bright_apply_packet(&cfg, &p);
        CHECK(cfg.ramp_min == 0, "负 ramp 应夹到 0（实际 %d）", cfg.ramp_min);

        /* on = 0 应能关闭（0 是有效值，不能与"未提供"混淆）*/
        parse_str("{\"t\":\"bright\",\"on\":0}", &p);
        bright_apply_packet(&cfg, &p);
        CHECK(cfg.enabled == 0, "★ on=0 应关闭自动（实际 %d）", cfg.enabled);
        parse_str("{\"t\":\"bright\",\"on\":1}", &p);
        bright_apply_packet(&cfg, &p);
        CHECK(cfg.enabled == 1, "on=1 应开启自动");
    }

    printf("=== 畸形输入（不得崩溃、不得污染配置）===\n");
    {
        bright_packet_t p;
        /* 不是亮度包 → 返回 false */
        CHECK(!parse_str("{\"t\":\"nav\",\"x\":1}", &p), "nav 包应返回 false");
        CHECK(!parse_str("{\"v\":1,\"ts_utc_ms\":5}", &p), "遥测包应返回 false");
        CHECK(!parse_str("{}", &p), "空对象应返回 false");
        CHECK(!parse_str("not json", &p), "非 JSON 应返回 false");
        CHECK(!parse_str("{\"t\":\"bright\"", &p), "★ 截断（缺右括号）应返回 false");
        CHECK(!parse_str("{\"t\":\"bright\",\"on\":}", &p), "★ 缺值应返回 false");
        CHECK(!parse_str("{\"t\":\"bright\",\"ds\":", &p), "截断在值上应返回 false");
        CHECK(!bright_parse(NULL, 0, &p), "NULL 载荷应返回 false");
        CHECK(!bright_parse("{\"t\":\"bright\"}", 16, NULL), "NULL 输出应返回 false");

        /* ★ 半解析不得污染：先给一个干净值，再用畸形包解析，out 应保持原样 */
        bright_packet_t good;
        parse_str("{\"t\":\"bright\",\"day\":77}", &good);
        const int before = good.day;
        const int before_has = good.has_day;
        CHECK(!parse_str("{\"t\":\"bright\",\"day\":", &good), "截断包应失败");
        CHECK(good.has_day == before_has && good.day == before,
              "★ 失败时不得修改 out（期望 has=%d val=%d，实际 has=%d val=%d）",
              before_has, before, good.has_day, good.day);
    }

    printf("=== 向前兼容 ===\n");
    {
        bright_packet_t p;
        /* 未来版本加的未知字段应被跳过，不影响已知字段 */
        const char *fwd = "{\"t\":\"bright\",\"v\":2,\"future\":\"x\",\"weekday\":[1,2,3],"
                          "\"day\":200,\"nested\":{\"a\":1}}";
        CHECK(parse_str(fwd, &p), "★ 含未知字段的包应能解析（向前兼容）");
        CHECK(p.has_day && p.day == 200, "已知字段仍应正确解析（实际 %d）", p.day);

        /* 类型不对的已知字段：跳过该值，其余仍生效（不整包失败）*/
        const char *badtype = "{\"t\":\"bright\",\"day\":\"oops\",\"night\":50}";
        CHECK(parse_str(badtype, &p), "字段类型不对不应导致整包失败");
        CHECK(!p.has_day, "类型不对的 day 应视为未提供");
        CHECK(p.has_night && p.night == 50, "同包中正常的 night 仍应解析（实际 %d）", p.night);
    }

    printf("=== reset 字段（恢复自动调节）===\n");
    {
        /*
         * 场景：用户在 App 里点「按时段自动」切回自动。
         * 此时曲线值往往没变，设备若只靠"内容是否变化"判断，
         * 会当成"重连重发"而保持手动亮度 —— 用户点了没反应。
         * 所以协议加 reset:1 明确表达"恢复自动"。
         */
        bright_packet_t p;
        CHECK(parse_str("{\"t\":\"bright\",\"reset\":1}", &p), "带 reset 的包应解析成功");
        CHECK(p.has_reset && p.reset == 1, "★ reset 应为 1（has=%d val=%d）", p.has_reset, p.reset);

        /* 不带 reset 时 has_reset 必须为 0，不能默认成"请求重置" */
        CHECK(parse_str("{\"t\":\"bright\",\"day\":100}", &p), "普通包应解析成功");
        CHECK(!p.has_reset, "★ 未提供 reset 时 has_reset 必须为 0（否则每次下发都会解除手动优先）");

        /* reset:0 也是"提供了，但值为 0" → 语义上不等同于请求重置 */
        CHECK(parse_str("{\"t\":\"bright\",\"reset\":0}", &p), "reset:0 应解析成功");
        CHECK(p.has_reset && p.reset == 0, "reset:0 应被记录为提供的 0");

        /* 与其它字段共存 */
        CHECK(parse_str("{\"t\":\"bright\",\"reset\":1,\"day\":180,\"enabled\":1}", &p),
              "reset 与其它字段共存应解析成功");
        CHECK(p.has_day && p.day == 180, "共存时 day 仍应正确");
    }

    printf("=== 与 auto_bright 联动（端到端）===\n");
    {
        /* 解析一个包 → 应用 → 算亮度，验证链路真的通 */
        auto_bright_config_t cfg = auto_bright_default_config();
        bright_packet_t p;
        parse_str("{\"t\":\"bright\",\"on\":1,\"ds\":420,\"de\":1110,"
                  "\"day\":200,\"night\":30,\"ramp\":30}", &p);
        bright_apply_packet(&cfg, &p);

        CHECK(auto_bright_level_at(12 * 60, &cfg) == 200,
              "正午应为包里的白天亮度 200（实际 %d）", auto_bright_level_at(12 * 60, &cfg));
        CHECK(auto_bright_level_at(2 * 60, &cfg) == 30,
              "凌晨应为包里的夜间亮度 30（实际 %d）", auto_bright_level_at(2 * 60, &cfg));

        /* 关闭后应回到白天亮度（不自动调）*/
        parse_str("{\"t\":\"bright\",\"on\":0}", &p);
        bright_apply_packet(&cfg, &p);
        CHECK(auto_bright_level_at(2 * 60, &cfg) == 200,
              "关闭自动后凌晨应返回白天亮度（实际 %d）", auto_bright_level_at(2 * 60, &cfg));
    }

    printf("结果: %d 通过, %d 失败\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
