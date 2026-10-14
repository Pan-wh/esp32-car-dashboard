/**
 * test_telemetry.c —— BLE 遥测 JSON 解析单元测试（PC 上运行，不需要硬件）
 *
 * 运行：cd tests && make test_telemetry && ./test_telemetry
 * 覆盖：完整字段、缺省字段、null、布尔、负数、嵌套容错、格式错误、未知键、换算
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "telemetry.h"

static int g_pass, g_fail;

#define CHECK(cond, fmt, ...)                                                  \
    do {                                                                       \
        if (cond) { g_pass++; }                                                \
        else {                                                                 \
            g_fail++;                                                          \
            printf("  ✗ %s:%d  " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__); \
        }                                                                      \
    } while (0)

#define CHECK_NEAR(a, b, eps, msg) \
    CHECK(fabs((double)(a) - (double)(b)) <= (eps), "%s: %.4f vs %.4f", msg, (double)(a), (double)(b))

static telemetry_msg_t parse(const char *s)
{
    telemetry_msg_t m;
    bool ok = telemetry_parse(s, strlen(s), &m);
    CHECK(ok, "应能解析: %s", s);
    return m;
}

static void test_full_packet(void)
{
    const char *j = "{\"v\":1,\"ts_utc_ms\":1791765900000,"
                    "\"speed_mps\":19.03,\"speed_accuracy_mps\":0.8,"
                    "\"latitude\":1.3521,\"longitude\":103.8198,"
                    "\"horizontal_accuracy_m\":5.2,\"course_deg\":120.0,"
                    "\"location_valid\":true,\"speed_valid\":true}";
    telemetry_msg_t m = parse(j);

    CHECK(m.v == 1, "v == 1（实际 %d）", m.v);
    CHECK(m.has_ts, "has_ts");
    CHECK(m.ts_utc_ms == 1791765900000LL, "ts_utc_ms（实际 %lld）", (long long)m.ts_utc_ms);
    CHECK_NEAR(m.speed_mps, 19.03, 0.001, "speed_mps");
    CHECK(m.speed_valid, "speed_valid");
    CHECK(m.has_speed_acc, "has_speed_acc");
    CHECK_NEAR(m.speed_accuracy_mps, 0.8, 0.001, "speed_accuracy");
    CHECK(m.has_lat && m.has_lon, "has_latlon");
    CHECK_NEAR(m.latitude, 1.3521, 0.0001, "latitude");
    CHECK_NEAR(m.longitude, 103.8198, 0.0001, "longitude");
    CHECK(m.has_hacc, "has_hacc");
    CHECK_NEAR(m.horizontal_accuracy_m, 5.2, 0.001, "hacc");
    CHECK(m.has_course, "has_course");
    CHECK_NEAR(m.course_deg, 120.0, 0.001, "course");
    CHECK(m.location_valid, "location_valid");
}

static void test_missing_fields_default_safe(void)
{
    /* 只有 v，其余字段缺省 → 速度/定位应保持无效（不伪装） */
    telemetry_msg_t m = parse("{\"v\":1}");
    CHECK(m.v == 1, "v == 1");
    CHECK(!m.has_ts, "无 ts 时 has_ts=false");
    CHECK(!m.speed_valid, "无 speed_valid 时缺省 false");
    CHECK(!m.location_valid, "无 location_valid 时缺省 false");
    CHECK(!m.has_lat && !m.has_lon, "无经纬度");
    CHECK(!m.has_course, "无航向");
}

static void test_null_fields(void)
{
    /* 经纬度/course 为 null → has_* 为 false，不得伪装成 0 */
    telemetry_msg_t m = parse("{\"v\":1,\"latitude\":null,\"longitude\":null,\"course_deg\":null}");
    CHECK(!m.has_lat && !m.has_lon, "null 经纬度 → has false");
    CHECK(!m.has_course, "null course → has false");
}

static void test_negative_speed(void)
{
    /* speed_mps 为负数（iOS 表示无效）→ 数值保留，消费侧按 speed_valid 处理 */
    telemetry_msg_t m = parse("{\"v\":1,\"speed_mps\":-1.0,\"speed_valid\":false}");
    CHECK_NEAR(m.speed_mps, -1.0, 0.001, "speed_mps 保留负数");
    CHECK(!m.speed_valid, "speed_valid=false");
}

static void test_whitespace_tolerance(void)
{
    telemetry_msg_t m = parse("  {  \"v\" : 1 , \"speed_mps\" : 7.5 , \"speed_valid\" : true } \r\n");
    CHECK(m.v == 1, "带空白 v");
    CHECK_NEAR(m.speed_mps, 7.5, 0.001, "带空白 speed_mps");
    CHECK(m.speed_valid, "带空白 speed_valid");
}

static void test_unknown_keys_ignored(void)
{
    telemetry_msg_t m = parse("{\"v\":1,\"foo\":42,\"bar\":\"hello\",\"speed_mps\":9.0}");
    CHECK(m.v == 1, "v");
    CHECK_NEAR(m.speed_mps, 9.0, 0.001, "speed_mps 不受未知键影响");
}

static void test_scientific_notation(void)
{
    telemetry_msg_t m = parse("{\"v\":1,\"speed_mps\":1.5e1}");
    CHECK_NEAR(m.speed_mps, 15.0, 0.001, "科学计数法");
}

static void test_malformed(void)
{
    CHECK(!telemetry_parse("not json", 8, NULL), "非 JSON");
    CHECK(!telemetry_parse("{", 1, NULL), "未闭合对象");
    {
        telemetry_msg_t m;
        CHECK(!telemetry_parse("{\"v\":}", 6, &m), "缺值");
    }
    {
        telemetry_msg_t m;
        CHECK(!telemetry_parse("{\"v\":1,}", 8, &m), "尾逗号");
    }
    {
        telemetry_msg_t m;
        CHECK(!telemetry_parse("{\"v\":1x}", 8, &m), "非法数字后缀");
    }
    {
        telemetry_msg_t m;
        CHECK(!telemetry_parse("{\"v\":1,\"speed_mps\":}", 23, &m), "空数字值");
    }
}

static void test_speed_kmh_conversion(void)
{
    telemetry_msg_t m = parse("{\"v\":1,\"speed_mps\":19.03}");
    CHECK_NEAR(telemetry_speed_kmh(&m), 19.03 * 3.6, 0.001, "km/h = m/s × 3.6");
}

/* 合理性校验（§7 超范围/自相矛盾） */
static void test_plausible(void)
{
    /* 正常 */
    {
        telemetry_msg_t m = parse("{\"v\":1,\"speed_mps\":19.03,\"speed_valid\":true,"
                                  "\"latitude\":1.35,\"longitude\":103.8,\"course_deg\":120}");
        CHECK(telemetry_plausible(&m), "正常包应合理");
    }
    /* 速度超范围（>400km/h ≈ 111.1m/s） */
    {
        telemetry_msg_t m = parse("{\"v\":1,\"speed_mps\":200.0,\"speed_valid\":true}");
        CHECK(!telemetry_plausible(&m), "555km/h 应判为不合理");
    }
    /* 负速度（iOS 无效语义）允许 */
    {
        telemetry_msg_t m = parse("{\"v\":1,\"speed_mps\":-1.0,\"speed_valid\":false}");
        CHECK(telemetry_plausible(&m), "负速度(无效语义)应允许");
    }
    /* 航向超范围 */
    {
        telemetry_msg_t m = parse("{\"v\":1,\"course_deg\":400.0}");
        CHECK(!telemetry_plausible(&m), "航向 400 应不合理");
    }
    /* 纬度超范围 */
    {
        telemetry_msg_t m = parse("{\"v\":1,\"latitude\":95.0}");
        CHECK(!telemetry_plausible(&m), "纬度 95 应不合理");
    }
    /* 经度超范围 */
    {
        telemetry_msg_t m = parse("{\"v\":1,\"longitude\":-200.0}");
        CHECK(!telemetry_plausible(&m), "经度 -200 应不合理");
    }
    /* 定位有效但无经纬度 → 自相矛盾 */
    {
        telemetry_msg_t m = parse("{\"v\":1,\"location_valid\":true}");
        CHECK(!telemetry_plausible(&m), "location_valid 但无经纬度应不合理");
    }
    /* NULL 安全 */
    CHECK(!telemetry_plausible(NULL), "NULL 应返回 false");
}

/* 链路状态机（§6.4 五态 + 并发保持） */
static void test_link_state(void)
{
    const uint32_t STALE = 2500;

    /* 未连接 → BT OFF（无论其它输入） */
    CHECK(telemetry_link_state(false, false, TELEMETRY_AGE_NEVER, false, STALE, LINK_OK)
              == LINK_CONNECTING, "未连接 → CONNECTING");
    CHECK(telemetry_link_state(false, true, 100, true, STALE, LINK_OK)
              == LINK_CONNECTING, "未连接优先于有数据");

    /* 已连接但从未收到 → WAIT GPS */
    CHECK(telemetry_link_state(true, false, TELEMETRY_AGE_NEVER, false, STALE, LINK_OK)
              == LINK_WAIT_LOC, "已连接未收到 → WAIT_LOC");

    /* 数据超时 → STALE */
    CHECK(telemetry_link_state(true, true, STALE + 1, true, STALE, LINK_OK) == LINK_STALE,
          "超过阈值 → STALE");
    CHECK(telemetry_link_state(true, true, STALE, true, STALE, LINK_OK) == LINK_OK,
          "恰好等于阈值不算超时");
    CHECK(telemetry_link_state(true, true, TELEMETRY_AGE_NEVER, true, STALE, LINK_OK)
              == LINK_STALE, "从未收到(有 had_data 标记) → STALE");

    /* 并发取不到状态 → HOLD（由调用方沿用 previous） */
    CHECK(telemetry_link_state(true, true, TELEMETRY_AGE_UNKNOWN, true, STALE, LINK_OK)
              == LINK_HOLD, "并发取不到 → HOLD");
    CHECK(telemetry_link_state(true, true, TELEMETRY_AGE_UNKNOWN, true, STALE, LINK_STALE)
              == LINK_HOLD, "并发取不到 → HOLD（与 previous 无关）");

    /* 收到但无效 → NO FIX */
    CHECK(telemetry_link_state(true, true, 100, false, STALE, LINK_OK) == LINK_INVALID,
          "有效期内但数据无效 → INVALID");

    /* 正常 → LIVE */
    CHECK(telemetry_link_state(true, true, 0, true, STALE, LINK_OK) == LINK_OK,
          "新鲜有效 → OK");
    CHECK(telemetry_link_state(true, true, STALE - 1, true, STALE, LINK_OK) == LINK_OK,
          "阈值内 → OK");
}

int main(void)
{
    g_pass = g_fail = 0;
    test_full_packet();
    test_missing_fields_default_safe();
    test_null_fields();
    test_negative_speed();
    test_whitespace_tolerance();
    test_unknown_keys_ignored();
    test_scientific_notation();
    test_malformed();
    test_speed_kmh_conversion();
    test_plausible();
    test_link_state();

    /* ---------------- 行程统计（功能④）---------------- */
    printf("=== 行程统计 ===\n");
    {
        telemetry_trip_t t;
        telemetry_trip_reset(&t);
        CHECK(t.distance_km == 0.0 && t.max_kmh == 0.0f && telemetry_trip_avg_kmh(&t) == 0.0f,
              "复位后全零（均速无有效时长时为 0）");

        /* 60 km/h 跑 1 分钟 → 1 km */
        telemetry_trip_step(&t, 60.0f, true, 60000);
        CHECK_NEAR(t.distance_km, 1.0, 1e-6, "60km/h × 60s = 1km");
        CHECK_NEAR(telemetry_trip_avg_kmh(&t), 60.0, 1e-3, "均速 60");
        CHECK(t.max_kmh == 60.0f, "最高速 60");

        /* 再以 120 km/h 跑 30 秒 → 累计 2 km，最高 120 */
        telemetry_trip_step(&t, 120.0f, true, 30000);
        CHECK_NEAR(t.distance_km, 2.0, 1e-6, "累计 2km");
        CHECK_NEAR(t.max_kmh, 120.0, 1e-3, "最高速更新为 120");
        CHECK_NEAR(telemetry_trip_avg_kmh(&t), 80.0, 1e-3, "加权均速 (60*60+120*30)/90 = 80");

        /* 无效数据不计入（关键原则：失效时不能把旧速度当里程） */
        double before = t.distance_km;
        telemetry_trip_step(&t, 999.0f, false, 60000);
        CHECK_NEAR(t.distance_km, before, 1e-9, "无效步不计入里程");
        CHECK(t.max_kmh == 120.0f, "无效步不影响最高速");

        /* 负速度忽略 */
        telemetry_trip_step(&t, -5.0f, true, 1000);
        CHECK_NEAR(t.distance_km, before, 1e-9, "负速度忽略");

        /* dt=0 忽略 */
        telemetry_trip_step(&t, 50.0f, true, 0);
        CHECK_NEAR(t.distance_km, before, 1e-9, "dt=0 忽略");

        /* 复位 */
        telemetry_trip_reset(&t);
        CHECK(t.distance_km == 0.0 && t.max_kmh == 0.0f && t.time_ms == 0, "复位清空");
    }

    /* ---------------- 速度线性斜坡（#2）---------------- */
    printf("=== 速度线性斜坡 ===\n");
    {
        telemetry_ramp_t r;
        telemetry_ramp_reset(&r);
        CHECK(telemetry_ramp_value(&r) == 0.0f, "复位后为 0");

        /* 首次设定目标：直接对齐（起步即准，不从 0 爬） */
        telemetry_ramp_set_target(&r, 60.0f, true);
        CHECK_NEAR(telemetry_ramp_value(&r), 60.0, 1e-3, "首次对齐到 60");

        /*
         * 加速：**固定时长**插值（450ms 走完 60→100）。
         * 改为固定时长的原因见 telemetry.h：
         * 手机 2Hz 下发，固定速率会让指针"动一段、停一段"。
         */
        telemetry_ramp_set_target(&r, 100.0f, true);
        float v = telemetry_ramp_step(&r, 100);
        CHECK_NEAR(v, 60.0 + 40.0 * (100.0 / 450.0), 1e-3,
                   "100ms 后 = 60 + (100-60)*100/450");

        /* 再 100ms → 按时间比例继续推进（与帧率无关） */
        v = telemetry_ramp_step(&r, 100);
        CHECK_NEAR(v, 60.0 + 40.0 * (200.0 / 450.0), 1e-3,
                   "再 100ms → 60 + (100-60)*200/450");

        /* 走满 450ms 后必须精确落在目标上（不会无限逼近） */
        v = telemetry_ramp_step(&r, 250);
        CHECK_NEAR(v, 100.0, 1e-3, "累计 450ms → 精确到 100");

        /* 帧率无关性：同样的总时长，拆成多帧结果一致 */
        telemetry_ramp_t a, b;
        telemetry_ramp_reset(&a); telemetry_ramp_reset(&b);
        telemetry_ramp_set_target(&a, 0.0f, true);
        telemetry_ramp_set_target(&b, 0.0f, true);
        telemetry_ramp_set_target(&a, 100.0f, true);
        telemetry_ramp_set_target(&b, 100.0f, true);
        for (int i = 0; i < 50; i++) telemetry_ramp_step(&a, 20);   /* 50 帧 × 20ms = 1s */
        for (int i = 0; i < 5; i++) telemetry_ramp_step(&b, 200);   /* 5 帧 × 200ms = 1s */
        CHECK_NEAR(telemetry_ramp_value(&a), telemetry_ramp_value(&b), 1e-3,
                   "1 秒总时长：50×20ms 与 5×200ms 结果一致（帧率无关）");

        /* 减速：用更短时长（250ms 走完 100→0），保证刹车读数及时 */
        telemetry_ramp_reset(&r);
        telemetry_ramp_set_target(&r, 100.0f, true);   /* 对齐 100 */
        telemetry_ramp_set_target(&r, 0.0f, true);
        v = telemetry_ramp_step(&r, 100);
        CHECK_NEAR(v, 100.0 - 100.0 * (100.0 / 250.0), 1e-3,
                   "减速 100ms = 100 + (0-100)*100/250");

        /* 收敛到目标值，不会无限逼近 */
        telemetry_ramp_reset(&r);
        telemetry_ramp_set_target(&r, 50.0f, true);
        telemetry_ramp_set_target(&r, 50.5f, true);
        for (int i = 0; i < 20; i++) telemetry_ramp_step(&r, 50);
        CHECK_NEAR(telemetry_ramp_value(&r), 50.5, 1e-3, "收敛到目标值");

        /* 无效数据：目标归零，但通过斜坡收回（不闪现） */
        telemetry_ramp_reset(&r);
        telemetry_ramp_set_target(&r, 80.0f, true);
        telemetry_ramp_set_target(&r, 0.0f, false);
        v = telemetry_ramp_step(&r, 100);
        CHECK(v < 80.0f && v > 0.0f, "无效后开始下降但未瞬时归零（实测 %.1f）", (double)v);

        /* dt=0 不推进 */
        telemetry_ramp_reset(&r);
        telemetry_ramp_set_target(&r, 50.0f, true);
        telemetry_ramp_set_target(&r, 100.0f, true);
        CHECK_NEAR(telemetry_ramp_step(&r, 0), 50.0, 1e-3, "dt=0 不推进");

        /* 负速度视为 0 */
        telemetry_ramp_reset(&r);
        telemetry_ramp_set_target(&r, -5.0f, true);
        CHECK_NEAR(telemetry_ramp_value(&r), 0.0, 1e-3, "负速度归一为 0");
    }

    /* ---------------- 链路状态防抖 ---------------- */
    printf("=== 链路状态防抖 ===\n");
    {
        telemetry_debounce_t d;
        telemetry_debounce_reset(&d);

        /* 连续 STALE：前 N-1 次不上报 STALE，第 N 次才切换 */
        CHECK(telemetry_debounce_apply(&d, LINK_STALE) == LINK_OK, "第 1 次 STALE 被抑制");
        CHECK(telemetry_debounce_apply(&d, LINK_STALE) == LINK_OK, "第 2 次 STALE 被抑制");
        CHECK(telemetry_debounce_apply(&d, LINK_STALE) == LINK_STALE,
              "第 3 次 STALE 生效（连续超时确认）");

        /* 真实断流场景：持续 STALE 应稳定上报（不会永远压着） */
        CHECK(telemetry_debounce_apply(&d, LINK_STALE) == LINK_STALE, "持续 STALE 保持上报");

        /* 恢复 LIVE：立即生效并清零计数 */
        telemetry_debounce_reset(&d);
        (void)telemetry_debounce_apply(&d, LINK_STALE); /* 1 次抖动 */
        CHECK(telemetry_debounce_apply(&d, LINK_OK) == LINK_OK, "恢复 LIVE 立即生效");
        /* 计数已清零：再抖动仍需连续 N 次 */
        CHECK(telemetry_debounce_apply(&d, LINK_STALE) == LINK_OK, "恢复后计数已清零（第 1 次抑制）");

        /* 抖动序列 LIVE/STALE 交替：不应切到 STALE（正是用户看到的闪烁） */
        telemetry_debounce_reset(&d);
        int flickered = 0;
        for (int i = 0; i < 10; i++) {
            telemetry_link_t r = telemetry_debounce_apply(&d, LINK_STALE);
            if (r == LINK_STALE) flickered++;
            (void)telemetry_debounce_apply(&d, LINK_OK); /* 交替恢复 */
        }
        CHECK(flickered == 0, "交替抖动 10 轮不产生 STALE（实测 %d 次）", flickered);

        /* 非 STALE 状态不参与防抖：立即透传 */
        telemetry_debounce_reset(&d);
        CHECK(telemetry_debounce_apply(&d, LINK_CONNECTING) == LINK_CONNECTING, "CONNECTING 立即透传");
        CHECK(telemetry_debounce_apply(&d, LINK_WAIT_LOC) == LINK_WAIT_LOC, "WAIT_LOC 立即透传");
        CHECK(telemetry_debounce_apply(&d, LINK_INVALID) == LINK_INVALID, "INVALID 立即透传");
        CHECK(telemetry_debounce_apply(&d, LINK_HOLD) == LINK_HOLD, "HOLD 立即透传");

        /* NULL 安全 */
        CHECK(telemetry_debounce_apply(NULL, LINK_STALE) == LINK_STALE, "NULL 防抖器直接透传");
        telemetry_debounce_reset(NULL); /* 不应崩溃 */
    }

    printf("结果: %d 通过, %d 失败\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
