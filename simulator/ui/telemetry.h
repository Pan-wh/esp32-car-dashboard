/*
 * telemetry.h —— iPhone BLE 遥测协议解析（平台无关纯 C）
 *
 * 协议见 AI_TASK_IPHONE_BLE_DASHBOARD.md §4.1（版本化 JSON）：
 *   {
 *     "v": 1,
 *     "ts_utc_ms": 1791765900000,
 *     "speed_mps": 19.03,
 *     "speed_accuracy_mps": 0.8,
 *     "latitude": 1.3521,
 *     "longitude": 103.8198,
 *     "horizontal_accuracy_m": 5.2,
 *     "course_deg": 120.0,
 *     "location_valid": true,
 *     "speed_valid": true
 *   }
 *
 * 设计要点：
 *   - 本文件不依赖 LVGL / Arduino / 硬件，可在 PC 上做单元测试（见 tests/）。
 *   - 字段可缺省；缺省/为 null 的字段以 has_* 标志区分，**不伪装成 0**。
 *   - 速度换算在消费侧做：km/h = speed_mps × 3.6。
 */
#ifndef TELEMETRY_H
#define TELEMETRY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* ---- 协议版本 ---- */
    int v; /* 0 表示未提供 */

    /* ---- 时间 ---- */
    bool has_ts;
    int64_t ts_utc_ms; /* UTC Unix epoch 毫秒 */

    /* ---- 速度 ---- */
    float speed_mps;        /* 米/秒 */
    bool speed_valid;       /* 缺省 false（保守）：CLLocation.speed<0 时为 false */
    bool has_speed_acc;
    float speed_accuracy_mps;

    /* ---- 位置 ---- */
    double latitude;
    bool has_lat;
    double longitude;
    bool has_lon;
    bool has_hacc;
    float horizontal_accuracy_m;

    /* ---- 航向（地面行进方向，0..360，不是磁罗盘）---- */
    bool has_course;
    float course_deg;

    /* ---- 状态 ---- */
    bool location_valid;
} telemetry_msg_t;

/*
 * 解析 JSON 为遥测结构。
 * 返回 true 表示输入是语法合法的 JSON 对象（字段可缺省）；
 * 返回 false 表示格式错误，out 内容无保证（但会先清零）。
 */
bool telemetry_parse(const char *json, size_t len, telemetry_msg_t *out);

/* 速度换算：km/h = m/s × 3.6 */
float telemetry_speed_kmh(const telemetry_msg_t *m);

/*
 * 速度合理性上限（km/h）。超过视为异常数据（§7「对无效、过期、超范围数据做校验」）。
 * 乘用车不可能达到，用于拦截明显错误的包。集中定义，便于调整。
 */
#define TELEMETRY_SPEED_MAX_KMH 400.0f

/*
 * 数据合理性校验：字段语义与物理范围检查。
 * 返回 false 表示该包应被丢弃（超范围/自相矛盾）。用于固件侧兜底校验，
 * 不替代 iOS 侧的质量门槛（§5.4，在 App 内按精度阈值判定 speed_valid/location_valid）。
 */
bool telemetry_plausible(const telemetry_msg_t *m);

#ifdef __cplusplus
}
#endif

#endif /* TELEMETRY_H */