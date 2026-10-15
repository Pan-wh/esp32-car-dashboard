/**
 * face_report.h —— 表盘列表上报（设备 → App）
 *
 * ## 为什么需要
 *
 * 此前 App 端把表盘列表**硬编码**在 `ContentView.swift` 的 `faces` 数组里，
 * 而设备侧按注册顺序动态生成。两边必须手工保持一致，**已经漏过一次**
 * （新增 nav 表盘后 App 选择器里没有它）。
 *
 * 本模块让设备把真实注册表**上报**出去，App 据此渲染：
 *   - 新增表盘只需改设备一处，App 自动发现（未知表盘用设备给的 label 兜底）
 *   - App 能拿到每个表盘的分组与启用状态，从而做分组显示与开关
 *
 * ## 载荷格式
 *
 * ```json
 * {"t":"faces","v":1,"count":5,"mask":31,"cur":1,
 *  "bright":70,"rot":90,"screen":1,
 *  "faces":[{"id":0,"name":"analog","label":"Analog","group":"clock","on":1}, ...]}
 * ```
 *
 * 字段说明：
 *   - `name`  ：稳定 ASCII 标识（App 用它做本地化与持久化，**不要改**）
 *   - `label` ：ASCII 显示名（App 未收录该 name 时的兜底显示）
 *   - `group` ：模块分组 ASCII 名（App 据此分组并本地化分组标题）
 *   - `on`    ：1 启用 / 0 隐藏（滑动时跳过的表盘）
 *   - `mask`  ：启用位掩码（bit i = face id i），便于快速比对
 *   - `cur`   ：**当前正在显示**的表盘 id。App 用它标注"正在用哪个"，
 *               避免界面上的选中态与设备实际状态不一致
 *   - `bright`：当前亮度 0-255（设备**拥有**该状态，见下）
 *   - `rot`   ：当前屏幕方向（度，0/90/180/270）
 *   - `screen`：1 点亮 / 0 息屏
 *
 * ## 为什么亮度/方向/息屏也由**设备**上报（2026-10-15）
 *
 * 这三项和表盘一样，都是**设备的属性**，不是 App 的设置：
 *
 *   - 用 App 调好亮度 → 断掉 App、带着仪表上车，亮度仍是那个值（存在设备上）；
 *   - 下次给仪表单独上电（根本没开 App），设备也要保持自己的状态。
 *
 * 因此**绝不能**在连接时由 App 下发一份"我记得的亮度" —— 那会把用户在设备上
 * （或串口上调）设好的值悄悄冲掉。正确方向是反过来：**设备上报、App 显示**。
 *
 * 之前的 bug 正是这个方向错了：App 用 `@State brightness = 200` 当默认值，
 * 重开 App 后显示 200（并不是设备的真实值），用户一碰滑块就把设备改成 200。
 *
 * > 唯一该在连接时由 App 下发的是**时间**（`ts_utc_ms`）—— 时间由 App 拥有
 * > （设备无 GNSS、RTC 精度不足），性质与上面三项相反。
 *
 * 兼容性：新增字段是**向后兼容**的（旧 App 忽略未知字段即可），
 * 故 `v` 保持 1 不升版本。
 *
 * ## 截断
 *
 * 表盘很多时可能超出 BLE 单次载荷。此时**尽量多报**并置 `"truncated":1`，
 * App 应提示"列表不完整"而不是静默显示错误的开关状态。
 *
 * ⚠️ 设备状态字段（bright/rot/screen）在**表头**，截断时也一定带出 ——
 *    否则表盘多到截断时 App 会拿不到亮度/方向，退回到默认值的坑里。
 */
#ifndef FACE_REPORT_H
#define FACE_REPORT_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 建议的缓冲区大小。
 *
 * 5 个表盘时实际约 380 字节；给到 1024 可容纳约 14 个表盘。
 * BLE 单次通知的实际上限见 FACE_REPORT_MAX_PAYLOAD。
 */
#define FACE_REPORT_BUF_SIZE 1024

/**
 * 单次上报的载荷上限（字节，含结尾 NUL 前的有效字符）。
 *
 * 取 480：ESP32 BLE 默认 MTU 下单次通知的安全值（有效载荷 512 - 协议开销）。
 * 超过此长度的表盘会被省略，并置 `"truncated":1`。
 */
#define FACE_REPORT_MAX_PAYLOAD 480

/**
 * 设置"当前表盘 id"（会随下次上报带出，字段名 cur）。
 * 传入负数表示未知。不设置时默认 -1。
 */
void face_report_set_current(int face_id);

/**
 * 设备显示状态（亮度 / 方向 / 息屏）—— 随上报带给 App，让 App 显示**设备的真值**
 * 而不是本地默认值。
 *
 * 调用方（dashboard.ino）在值**变化时**调用；未设置过的字段不上报
 * （见 face_report_set_display 的 "有值才带" 规则）。
 */
typedef struct {
    int brightness; /**< 0-255；负数表示"尚未设置"，不上报该字段 */
    int rotation;   /**< 0/90/180/270；负数表示"尚未设置" */
    int screen_on;  /**< 1 亮 / 0 息屏；负数表示"尚未设置" */
} face_report_display_t;

/**
 * 更新设备显示状态（下次上报带出）。
 *
 * 传负数表示该字段"未知/不适用"，此时**省略该字段**而不是发一个假值 ——
 * App 据此保持上一次的值，不会被一个凭空的 0 覆盖（那是之前亮度 bug 的形态）。
 */
void face_report_set_display(const face_report_display_t *d);

/**
 * 读取当前已设置的显示状态（主要供单测断言；测试需自行复位）。
 */
face_report_display_t face_report_get_display(void);

/** 复位所有状态（当前表盘 + 显示状态）——**仅供测试**使用，固件不调用。 */
void face_report_reset_for_test(void);

/**
 * 生成表盘列表 JSON。
 *
 * @param buf 输出缓冲
 * @param cap 缓冲容量（含结尾 NUL）
 * @return 写入的字符数（不含结尾 NUL）；参数非法或容量不足返回 -1。
 *
 * 注：容量不足（连表头都放不下）才返回 -1；表盘条目放不下属于"截断"，
 * 仍返回成功并置 truncated 标志。
 */
int face_report_build(char *buf, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* FACE_REPORT_H */
