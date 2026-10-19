/**
 * bright_parse.h —— 亮度曲线包（App → 设备）的解析（纯 C，可在 PC 上单测）
 *
 * ## 用途
 *
 * App 在**每次连接时**下发一次亮度曲线（与"纯时间包"同一时机），
 * 设备据此按时段自动调亮度。协议与 `nav`/`faces` 一致：靠 `t` 字段分流。
 *
 * ## 载荷格式
 *
 * ```json
 * {"t":"bright","v":1,"on":1,"ds":420,"de":1110,"day":220,"night":40,"ramp":30}
 * ```
 *
 * | 字段 | 含义 |
 * |------|------|
 * | `t`    | 恒为 `"bright"`（分流依据）|
 * | `on`   | 1 启用自动 / 0 关闭（关闭后由用户手动亮度说了算）|
 * | `ds`   | 白天开始（当天分钟数 0-1439，如 420 = 07:00）|
 * | `de`   | 白天结束（当天分钟数 0-1439，如 1110 = 18:30）|
 * | `day`  | 白天亮度 1-255 |
 * | `night`| 夜间亮度 1-255 |
 * | `ramp` | 过渡时长（分钟，0 = 硬切换）|
 *
 * ## 设计取舍
 *
 * - **时间用分钟数而非 "07:00" 字符串**：一次整数比较即可，且协议里
 *   没有时区/格式歧义（设备侧 TZ 固定 +8）。
 * - **字段全部可选、缺失即保持现值**：这样将来加字段（如按周/工作日）
 *   不会让旧固件整包丢弃，也便于分步灰度。
 * - **越界值夹取而不是拒绝整包**：一个字段写坏不该让整个功能失效。
 *   但 `t` 不对则视为"不是亮度包"，交由其他解析器处理。
 *
 * 本文件不做任何硬件调用，可在 PC 上单测。
 */
#ifndef BRIGHT_PARSE_H
#define BRIGHT_PARSE_H

#include <stdbool.h>
#include <stddef.h>

#include "auto_bright.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 解析结果。
 *
 * ## 为什么用 `has_*` 标志而不是 `-1` 当"未提供"哨兵
 *
 * 早先用 `-1` 表示"包中未提供"，但这**同时**也是"提供了一个负值"。
 * 两者撞在一起会出问题：App 发了 `{"ds":-10}`，设备分不清是"没说"还是
 * "说了个负数"，只能按"没说"忽略 —— 于是**用户改了却没反应**，
 * 而且没有任何提示，比夹取更难排查。
 *
 * 改为显式的 `has_*`：字段"有没有出现"与"值是多少"彻底分开，
 * 负值就能正常走夹取路径（→ 0），行为一致且可预测。
 */
typedef struct {
    int has_on;    /**< 1 = 包中提供了 on */
    int has_reset; /**< 1 = 包中提供了 reset */
    int has_ds;
    int has_de;
    int has_day;
    int has_night;
    int has_ramp;

    int on;    /**< 1 启用 / 0 关闭（仅 has_on 时有效）*/
    /**
     1 = **请求恢复自动调节**（解除"手动优先"），即使曲线内容没变。

     为什么需要它：用户点「手动/自动」切回自动时，曲线内容往往**没变**
     （还是原来那些值），固件会把它当成"重连重发"而保持手动亮度。
     有了这个显式标志，App 就能准确表达"我要重新交给自动管"。
     */
    int reset;
    int ds;    /**< 白天开始（分钟；仅 has_ds 时有效）*/
    int de;    /**< 白天结束（分钟；仅 has_de 时有效）*/
    int day;   /**< 白天亮度（仅 has_day 时有效）*/
    int night; /**< 夜间亮度（仅 has_night 时有效）*/
    int ramp;  /**< 过渡分钟（仅 has_ramp 时有效）*/
} bright_packet_t;

/**
 * 判断一条消息是否为亮度包（只看 `t` 字段，尽量便宜）。
 *
 * 遥测包很密（2Hz），分流必须廉价 —— 与 `nav_is_nav_message` 同策略：
 * 浅扫描到 `"t"` 即决定，不做完整解析。
 */
bool bright_is_bright_message(const char *json, size_t len);

/**
 * 解析亮度包。
 *
 * @param json 载荷（**不要求**以 NUL 结尾，用 len 界定）
 * @param len  长度
 * @param out  输出；成功时各字段为解析值或 -1（未提供）
 * @return true 表示这是一条**合法**的亮度包（`t == "bright"` 且 JSON 结构完整）
 *
 * ⚠️ 返回 false 时不要修改 `out` —— 让调用方保持原配置，避免半解析的状态。
 */
bool bright_parse(const char *json, size_t len, bright_packet_t *out);

/**
 * 把包里的字段合并进现有配置（只覆盖包中提供的字段）。
 *
 * 这是调用方**真正该用的入口** —— 它处理"字段可选"与"越界夹取"，
 * 调用方不必重复这些边界判断。
 *
 * @param cfg 现有配置（**原地修改**）
 * @param pkt 解析结果
 */
void bright_apply_packet(auto_bright_config_t *cfg, const bright_packet_t *pkt);

#ifdef __cplusplus
}
#endif

#endif /* BRIGHT_PARSE_H */
