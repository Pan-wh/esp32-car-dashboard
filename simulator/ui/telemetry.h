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
 * ---- 速度指针线性斜坡（#2 修复"跳针"）----
 *
 * 原 EMA 方案的问题：它按**调用次数**收敛，
 * 而固件以 50Hz 调用、数据源只有 2Hz → 指针呈"每 500ms 走一段就停"的
 * 阶梯式运动，观感是跳针且更新慢。
 *
 * 本方案改为**按时间**的固定速率斜坡（用户选择的"假线性"）：
 *   - 每帧代入实际经过的 dt_ms，在**固定时长**内插值到目标
 *   - 无论调用频率多少，视觉过程一致（时间基，不受帧率影响）
 *   - 减速时长更短，保证刹车时读数及时下降
 *
 * ⚠️ 为什么是"固定时长"而不是"固定速率"：
 *   手机以 2Hz（500ms 间隔）下发车速。若用固定速率，小幅变化 250ms 就走完，
 *   指针会"动 250ms → 静止 250ms → 再动"，肉眼即一顿一顿。
 *   改成固定时长后，**每次变化都恰好占满一个数据间隔**，指针连续运动。
 */
typedef struct {
    float value_kmh;      /* 当前显示值 */
    float target_kmh;     /* 目标值（最新遥测） */
    float start_kmh;      /* 本次斜坡的起点 */
    uint32_t elapsed_ms;  /* 本次斜坡已用时间 */
    uint32_t duration_ms; /* 本次斜坡总时长；0 = 已到位 */
    bool inited;
} telemetry_ramp_t;

/*
 * 斜坡时长（毫秒）。取值依据：手机遥测为 2Hz（500ms 间隔），
 * 故取略小于 500ms，使指针在下一帧数据到达前刚好走完 —— 全程连续运动。
 */
#define TELEMETRY_RAMP_UP_MS 450u   /* 加速：覆盖整个数据间隔 */
#define TELEMETRY_RAMP_DOWN_MS 250u /* 减速：更快，刹车读数及时 */

void telemetry_ramp_reset(telemetry_ramp_t *r);

/* 设定目标速度（收到新遥测时调用） */
void telemetry_ramp_set_target(telemetry_ramp_t *r, float kmh, bool valid);

/* 按经过时间推进斜坡，返回当前应显示的速度 */
float telemetry_ramp_step(telemetry_ramp_t *r, uint32_t dt_ms);

/* 当前显示值 */
float telemetry_ramp_value(const telemetry_ramp_t *r);

/*
 * ---- 行程统计（功能④：里程 / 均速 / 最高速）----
 *
 * 由固件按固定周期调用 step 累积；纯逻辑放在共享层以便单测。
 * 仅在 valid（速度有效且未过期）时累积里程，避免「失效时把旧速度当里程」。
 */
typedef struct {
    double distance_km; /* 累计里程 */
    float max_kmh;      /* 最高速度 */
    double sum_kmh_ms;  /* 用于均速：速度×时间的积分 */
    uint32_t time_ms;   /* 有效行驶时长 */
} telemetry_trip_t;

/* 清零 */
void telemetry_trip_reset(telemetry_trip_t *t);

/*
 * 累积一步。
 * @param speed_kmh 当前速度（km/h）
 * @param valid     数据是否有效（无效则本步不计入）
 * @param dt_ms     距上一步的毫秒数
 */
void telemetry_trip_step(telemetry_trip_t *t, float speed_kmh, bool valid, uint32_t dt_ms);

/* 平均速度（按有效行驶时长加权；无有效时长返回 0） */
float telemetry_trip_avg_kmh(const telemetry_trip_t *t);

/*
 * 数据龄哨兵（与固件 ble_telemetry.h 的 BLE_AGE_UNKNOWN 对齐，集中定义避免两端漂移）
 */
#define TELEMETRY_AGE_NEVER   (UINT32_MAX)     /* 从未收到过 */
#define TELEMETRY_AGE_UNKNOWN (UINT32_MAX - 1) /* 并发中取不到状态 */

/*
 * 链路状态机（AI_TASK §6.4 五态）。纯逻辑，便于单元测试与模拟器复用。
 *
 *   未连接                      → LINK_CONNECTING (BT OFF)
 *   已连接但从未收到            → LINK_WAIT_LOC   (WAIT GPS)
 *   取不到状态(并发)            → LINK_HOLD：沿用 previous，避免 LIVE→STALE 闪跳
 *   数据超时                    → LINK_STALE      (STALE)
 *   收到但无效                  → LINK_INVALID    (NO FIX)
 *   正常                        → LINK_OK         (LIVE)
 */
typedef enum {
    LINK_CONNECTING = 0,
    LINK_WAIT_LOC,
    LINK_INVALID,
    LINK_STALE,
    LINK_OK,
    LINK_HOLD /* 不是"状态"，而是"保持上一次"的指令 */
} telemetry_link_t;

/*
 * ---- 链路状态防抖（消除瞬态 STALE 闪烁）----
 *
 * 背景（真机长稳实测）：室内静止时 iOS 会把定位更新降到约 1.25Hz
 * （设备不动时的省电策略，非缺陷）。此时若某次回调略延迟，累计 2.5s
 * 无包就会触及超时阈值 → 状态标签瞬间闪一下 STALE 又回到 LIVE，
 * 用户可见"状态偶尔跳变"。
 *
 * 设计原则（安全优先，只对**显示**加防抖）：
 *   - STALE 需**连续 N 次**判定才真正切换（抑制抖动闪烁）；
 *   - 恢复到 LIVE 等"好消息"**立即生效**（不能延迟告知已恢复）；
 *   - **指针行为不受防抖影响**——数据过期仍按规则归零，
 *     绝不用旧值冒充当前车速（§6.3）。
 */
#define TELEMETRY_STALE_DEBOUNCE_N 3 /* 连续 3 次判定（约 3 个采样周期）才切 STALE */

typedef struct {
    int stale_streak; /* 连续判定为 STALE 的次数 */
} telemetry_debounce_t;

void telemetry_debounce_reset(telemetry_debounce_t *d);

/*
 * 对状态机输出做防抖。raw 为 telemetry_link_state() 的原始判定，
 * 返回应当**显示**的状态。指针/数据有效性判断请直接使用 raw，不要用本函数结果。
 */
telemetry_link_t telemetry_debounce_apply(telemetry_debounce_t *d, telemetry_link_t raw);

/*
 * 计算当前链路状态。
 *   connected   : BLE 是否已连接
 *   had_data    : 是否收到过任意遥测
 *   age_ms      : 距最后一条有效遥测的毫秒数（UINT32_MAX=从未收到；UINT32_MAX-1=并发中取不到）
 *   data_valid  : 最新一包是否有可用速度/定位
 *   stale_ms    : 超时阈值
 *   previous    : 上一次的状态（仅 LINK_HOLD 时使用）
 */
telemetry_link_t telemetry_link_state(bool connected, bool had_data, uint32_t age_ms,
                                     bool data_valid, uint32_t stale_ms,
                                     telemetry_link_t previous);

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
