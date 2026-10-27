/**
 * ble_photo.h —— BLE 照片服务（GATT Server 侧）
 *
 * 协议见 `docs/photo-protocol.md`；设计见 `docs/photo-wireless-design.md` §8。
 *
 * 本模块只做 **GATT + JSON 编解码 + 命令排队**，业务全部转给 `photo_rt.h`：
 *   - 5 个特征：Ctl(WRITE) / Data(WRITE_NR) / Evt(NOTIFY) / Info(READ) / State(READ+NOTIFY)
 *   - 命令在 BLE 回调里只入队；**所有会碰 SD/NVS 的动作都在主循环执行**（沿用仓库铁律）
 *   - 数据分片在回调里只做偏移校验 + 拷进 PSRAM 环形缓冲（由 photo_rt 实现）
 */
#ifndef BLE_PHOTO_H
#define BLE_PHOTO_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 注册照片 GATT 服务。必须在 `ble_telemetry_begin()` 成功之后、
 * 且 `photo_rt_init()` 之后调用（用到 BLEDevice 的 server 与照片运行时）。
 */
bool ble_photo_begin(void);

/** 主循环每拍调用：处理命令队列、发 credit、清单分页、刷新 Read 值。 */
void ble_photo_poll(void);

#ifdef __cplusplus
}
#endif

#endif /* BLE_PHOTO_H */
