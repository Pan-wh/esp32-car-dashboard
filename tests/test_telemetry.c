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

    printf("结果: %d 通过, %d 失败\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}