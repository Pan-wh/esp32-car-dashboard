/**
 * gnss_nmea.h —— NMEA 0183 解析（纯 C，无 LVGL / 硬件依赖）
 *
 * 作用：把 LC76G 输出的 NMEA 语句流解析成「车速 / 航向 / 定位状态」，供 UI 使用。
 * 数据通路（真机）：I2C 0x50 写命令 / 0x54 读回一段 NMEA 文本块，
 *   协议见官方示例 09_LC76G_I2C（先读 4 字节长度，再读取该长度的数据）。
 *
 * 设计取舍：
 *   - 只依赖 libc（可单元测试：tests/test_nmea.c），平台无关；
 *   - 不自己读时间：由调用方通过 gnss_set_time() 提供毫秒时基，
 *     以便判断「数据过期」（断定位时 UI 必须显示无效，不能沿用旧速度）。
 *
 * 支持的语句（自动兼容各 talker：GP/GN/GL/GA/GB…）：
 *   RMC → 定位有效性、地面速度、航向、UTC 时间/日期
 *   GGA → 定位质量、参与定位卫星数、HDOP、海拔
 *   GSA → 定位类型（1 未定位 / 2 2D / 3 3D）、PDOP/HDOP/VDOP
 *   VTG → 地面速度与航向（无 RMC 时的补充来源）
 *   GSV → 可见卫星数
 */
#ifndef GNSS_NMEA_H
#define GNSS_NMEA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* 一行最长长度（NMEA 标准 82 字符，留余量） */
#ifndef GNSS_LINE_MAX
#define GNSS_LINE_MAX 128
#endif

/* 超过该时间没有收到定位语句，则视为数据过期（默认 3s） */
#ifndef GNSS_STALE_MS
#define GNSS_STALE_MS 3000u
#endif

typedef enum {
    GNSS_FIX_NONE = 0,
    GNSS_FIX_2D,
    GNSS_FIX_3D
} gnss_fix_t;

typedef struct {
    /* ---- 定位状态 ---- */
    bool valid;        /* 定位是否有效（RMC 的 A/V，或 GGA 质量） */
    gnss_fix_t fix;    /* GSA 给出的定位维度 */
    int sats_used;     /* 参与定位的卫星数（GGA 字段 7） */
    int sats_view;     /* 可见卫星数（GSV 字段 3，多句取最大值） */
    int gga_quality;   /* GGA 字段 6：0=无效 1=GPS 2=DGPS */
    float hdop;
    float altitude_m;

    /* ---- 运动信息 ---- */
    float speed_kmh;   /* 地面速度 km/h（RMC/VTG） */
    float course_deg;  /* 地面航向，0=正北，顺时针（RMC/VTG） */

    /* ---- 时间（UTC）---- */
    int utc_h, utc_m, utc_s;
    int day, month, year; /* year 为 4 位（20xx） */

    /* ---- 元信息 ---- */
    uint32_t last_rx_ms; /* 最后一条被接受的语句时刻（由 gnss_set_time 提供） */
} gnss_data_t;

typedef struct {
    gnss_data_t data;

    char line[GNSS_LINE_MAX]; /* 组行缓冲 */
    size_t line_len;

    uint32_t now_ms;   /* 调用方时基 */
    bool has_rmc;      /* 是否收到过 RMC（决定 valid 的来源） */
    bool has_gga;

    /* 统计（便于排障） */
    uint32_t sentences;    /* 校验通过并被识别的语句数 */
    uint32_t checksum_err; /* 校验和错误数 */
    uint32_t unknown;      /* 校验通过但未处理的语句数 */
    uint32_t overflow;     /* 超长行丢弃次数 */
} gnss_ctx_t;

/* 初始化（清零） */
void gnss_init(gnss_ctx_t *ctx);

/* 由调用方提供毫秒时基（每次主循环调用即可） */
void gnss_set_time(gnss_ctx_t *ctx, uint32_t now_ms);

/* 逐字节喂入数据流（串口/I2C 读到的原始字节）；遇 '\n' 自动解析一行 */
void gnss_feed(gnss_ctx_t *ctx, const char *buf, size_t len);

/* 直接解析一行（可含/不含首尾的 $ * \r \n）；返回 true 表示被识别并解析 */
bool gnss_parse_line(gnss_ctx_t *ctx, const char *line);

/* 距最后一条被接受语句的毫秒数（无数据时返回 UINT32_MAX） */
uint32_t gnss_age_ms(const gnss_ctx_t *ctx);

/* 数据是否可用于显示：定位有效且未过期 */
bool gnss_is_usable(const gnss_ctx_t *ctx);

#endif /* GNSS_NMEA_H */
