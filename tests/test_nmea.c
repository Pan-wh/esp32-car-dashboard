/**
 * test_nmea.c —— NMEA 解析单元测试（PC 上运行，不需要硬件）
 *
 * 运行：cd tests && make run
 * 覆盖：真实语句样本、校验和、逐字节组行、无定位降级、数据过期、字段缺失/垃圾数据
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "gnss_nmea.h"

static int g_pass, g_fail;

#define CHECK(cond, fmt, ...)                                              \
    do {                                                                   \
        if (cond) { g_pass++; }                                            \
        else {                                                             \
            g_fail++;                                                      \
            printf("  ✗ %s:%d  " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__); \
        }                                                                  \
    } while (0)

#define CHECK_NEAR(a, b, eps, msg) \
    CHECK(fabs((double)(a) - (double)(b)) <= (eps), "%s: %.3f vs %.3f", msg, (double)(a), (double)(b))

/* 计算 NMEA 校验和（用于构造测试语句） */
static void build_sentence(char *out, size_t out_sz, const char *body)
{
    unsigned cs = 0;
    for (const char *p = body; *p; p++) cs ^= (unsigned char)*p;
    snprintf(out, out_sz, "$%s*%02X", body, cs & 0xFF);
}

static void test_real_sentences(void)
{
    printf("[1] 真实语句样本（校验和已被外部校验）\n");
    gnss_ctx_t c;
    gnss_init(&c);
    gnss_set_time(&c, 1000);

    CHECK(gnss_parse_line(&c, "$GPRMC,225446,A,4916.45,N,12311.12,W,000.5,054.7,191194,020.3,E*68"),
          "RMC 应被解析 (%u)", c.unknown);
    CHECK(c.data.valid, "RMC 状态 A → 定位有效");
    CHECK_NEAR(c.data.speed_kmh, 0.5 * 1.852, 0.01, "RMC 节→km/h");
    CHECK_NEAR(c.data.course_deg, 54.7, 0.01, "RMC 航向");
    CHECK(c.data.day == 19 && c.data.month == 11 && c.data.year == 1994, "RMC 日期 %d-%d-%d",
          c.data.day, c.data.month, c.data.year);
    CHECK(c.data.utc_h == 22 && c.data.utc_m == 54 && c.data.utc_s == 46, "RMC UTC 时间 %02d:%02d:%02d",
          c.data.utc_h, c.data.utc_m, c.data.utc_s);

    CHECK(gnss_parse_line(&c, "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47"),
          "GGA 应被解析");
    CHECK(c.data.gga_quality == 1, "GGA 质量=%d", c.data.gga_quality);
    CHECK(c.data.sats_used == 8, "GGA 卫星数=%d", c.data.sats_used);
    CHECK_NEAR(c.data.hdop, 0.9, 0.01, "GGA HDOP");
    CHECK_NEAR(c.data.altitude_m, 545.4, 0.05, "GGA 海拔");

    CHECK(gnss_parse_line(&c, "$GPGSA,A,3,04,05,,09,12,,,24,,,,,2.5,1.3,2.1*39"), "GSA 应被解析");
    CHECK(c.data.fix == GNSS_FIX_3D, "GSA 定位类型=%d (期望 3D)", (int)c.data.fix);
    CHECK_NEAR(c.data.hdop, 1.3, 0.01, "GSA HDOP 覆盖");

    CHECK(gnss_parse_line(&c, "$GPGSV,2,1,08,01,40,083,46,02,17,308,41,12,07,344,39,14,22,228,45*75"),
          "GSV 应被解析");
    CHECK(c.data.sats_view == 8, "GSV 可见卫星=%d", c.data.sats_view);

    CHECK(c.checksum_err == 0, "不应有校验错误 (%u)", c.checksum_err);
}

static void test_checksum(void)
{
    printf("[2] 校验和与容错\n");
    gnss_ctx_t c;
    gnss_init(&c);
    gnss_set_time(&c, 0);

    /* 改坏校验和 */
    CHECK(!gnss_parse_line(&c, "$GPRMC,225446,A,4916.45,N,12311.12,W,000.5,054.7,191194,020.3,E*00"),
          "错误校验和应被拒绝");
    CHECK(c.checksum_err == 1, "校验错误计数=%u", c.checksum_err);
    CHECK(c.sentences == 0, "被拒绝的语句不计入成功数");

    /* 无校验和字段：容错接受 */
    CHECK(gnss_parse_line(&c, "$GNRMC,120000,A,3000.00,N,12000.00,E,10.0,90.0,010120,000.0,E"),
          "无校验和字段应容错接受");
    CHECK_NEAR(c.data.speed_kmh, 18.52, 0.02, "10 节 → 18.52 km/h");

    /* 垃圾行 / 空字段：不得崩溃 */
    CHECK(!gnss_parse_line(&c, "hello world"), "垃圾行应被忽略");
    CHECK(!gnss_parse_line(&c, ""), "空行应被忽略");
    /* 空字段语句：可被识别，但必须不崩溃且判定为无效（不得沿用上一次的有效值） */
    gnss_parse_line(&c, "$GNRMC,,,,,,,,,");
    CHECK(!c.data.valid, "全空字段 RMC → 定位无效且不崩溃");
}

static void test_no_fix_and_stale(void)
{
    printf("[3] 无定位与数据过期\n");
    gnss_ctx_t c;
    gnss_init(&c);
    char line[160];

    /* RMC 状态 V = 未定位 */
    build_sentence(line, sizeof(line), "GNRMC,120000,V,3000.00,N,12000.00,E,,,010120,,,N");
    gnss_set_time(&c, 5000);
    gnss_parse_line(&c, line);
    CHECK(!c.data.valid, "RMC 状态 V → 定位无效");
    CHECK(!gnss_is_usable(&c), "无效定位 → 不可用（UI 应显示 --）");

    /* 有效定位 + 未过期 */
    build_sentence(line, sizeof(line), "GNRMC,120001,A,3000.00,N,12000.00,E,20.0,90.0,010120,,,A");
    gnss_parse_line(&c, line);
    CHECK(c.data.valid, "RMC 状态 A → 定位有效");
    CHECK(gnss_is_usable(&c), "刚收到数据 → 可用");
    CHECK(gnss_age_ms(&c) == 0, "age=%u", gnss_age_ms(&c));

    /* 推进 2s：仍可用；再推进到 4s：过期 */
    gnss_set_time(&c, 7000);
    CHECK(gnss_is_usable(&c), "2s 未更新仍可用");
    gnss_set_time(&c, 9000);
    CHECK(!gnss_is_usable(&c), "4s 未更新 → 过期不可用");
    CHECK(gnss_age_ms(&c) == 4000, "age=%u (期望 4000)", gnss_age_ms(&c));
}

static void test_stream_feeding(void)
{
    printf("[4] 字节流组行（模拟 I2C/串口分片读取）\n");
    gnss_ctx_t c;
    gnss_init(&c);
    gnss_set_time(&c, 100);

    /* 两句拼在一起，一次喂入（校验和由 build_sentence 计算） */
    char l1[160], l2[160], buf[400];
    build_sentence(l1, sizeof(l1), "GNRMC,120000,A,3000.00,N,12000.00,E,30.0,45.0,010120,,,A");
    build_sentence(l2, sizeof(l2), "GNGGA,120000,3000.00,N,12000.00,E,1,09,1.1,12.3,M,0.0,M,,");
    snprintf(buf, sizeof(buf), "%s\r\n%s\r\n", l1, l2);

    gnss_feed(&c, buf, strlen(buf));
    CHECK(c.sentences == 2, "两句都应解析 (%u)", c.sentences);
    CHECK_NEAR(c.data.speed_kmh, 30.0 * 1.852, 0.02, "速度 30 节");
    CHECK(c.data.sats_used == 9, "卫星数 %d", c.data.sats_used);

    /* 逐字节喂入同一句：分片不影响结果 */
    gnss_ctx_t c2;
    gnss_init(&c2);
    gnss_set_time(&c2, 200);
    for (size_t i = 0; i < strlen(l1); i++) gnss_feed(&c2, &l1[i], 1);
    gnss_feed(&c2, "\r\n", 2);
    CHECK(c2.sentences == 1, "逐字节喂入应解析出 1 句 (%u)", c2.sentences);
    CHECK_NEAR(c2.data.course_deg, 45.0, 0.01, "航向 45°");

    /* 半句 + 新句（$ 重置）：不应解析出错误数据 */
    gnss_ctx_t c3;
    gnss_init(&c3);
    gnss_set_time(&c3, 300);
    gnss_feed(&c3, "$GNRMC,120000,A,3000", 19);
    gnss_feed(&c3, l1, strlen(l1));
    gnss_feed(&c3, "\r\n", 2);
    CHECK(c3.sentences == 1, "残行应被丢弃，仅解析完整句 (%u)", c3.sentences);
    CHECK(c3.overflow >= 1, "残行应计入 overflow (%u)", c3.overflow);
}

static void test_vtg_fallback(void)
{
    printf("[5] VTG 作为速度/航向来源\n");
    gnss_ctx_t c;
    gnss_init(&c);
    gnss_set_time(&c, 0);

    CHECK(gnss_parse_line(&c, "$GPVTG,054.7,T,034.4,M,005.5,N,010.2,K*48"), "VTG 应被解析");
    CHECK_NEAR(c.data.course_deg, 54.7, 0.01, "VTG 真航向");
    CHECK_NEAR(c.data.speed_kmh, 10.2, 0.01, "VTG km/h 字段");
    CHECK(!c.data.valid, "只有 VTG 时不应自称已定位（valid 需 RMC/GGA）");
}

int main(void)
{
    printf("=== NMEA 解析单元测试 ===\n");
    test_real_sentences();
    test_checksum();
    test_no_fix_and_stale();
    test_stream_feeding();
    test_vtg_fallback();

    printf("\n结果: %d 通过, %d 失败\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
