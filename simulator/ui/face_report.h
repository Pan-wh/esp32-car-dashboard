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
 *
 * ## 截断
 *
 * 表盘很多时可能超出 BLE 单次载荷。此时**尽量多报**并置 `"truncated":1`，
 * App 应提示"列表不完整"而不是静默显示错误的开关状态。
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
