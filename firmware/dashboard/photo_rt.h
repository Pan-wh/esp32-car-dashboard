/**
 * photo_rt.h —— 照片运行时（固件专用）：SD 扫描、播放、解码、上传落盘、清单/状态上报
 *
 * 分工：
 *   - 共享纯逻辑：`pgj.c`（格式）、`photo_list.c`（清单）、`photo_cmd.c`（命令解析）、
 *     `photo_sync.c`（接收状态机）—— 均在 `simulator/ui/`，PC 可测。
 *   - **本模块**（`firmware/dashboard/photo_rt.cpp`）：把上面四块接到 SD / LVGL / BLE 命令上，
 *     负责 .pgj/.pgi 分派、JPEG 解码、上传环形缓冲与写盘、清单缓存、设置 NVS、UI 进度。
 *   - `ble_photo.cpp`：只做 GATT/JSON 编解码，业务全部 via 本头文件。
 *
 * ⚠️ 线程约定：所有函数都在**主循环（core 1）**调用；BLE 回调里只允许调用
 *    `photo_rt_upload_chunk()`（它只做 memcpy 进 PSRAM 环形缓冲 + 偏移校验）。
 */
#ifndef PHOTO_RT_H
#define PHOTO_RT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "photo_list.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 上传 begin 的结果（ble_photo 据此回 go / err） */
typedef enum {
    PHOTO_RT_BEGIN_OK = 0,
    PHOTO_RT_BEGIN_RESUME,   /* 有可续传的 .part：out_offset 为已落盘字节数 */
    PHOTO_RT_BEGIN_BUSY,
    PHOTO_RT_BEGIN_BAD_ARG,
    PHOTO_RT_BEGIN_NO_SPACE,
    PHOTO_RT_BEGIN_UNSUPPORTED,
    PHOTO_RT_BEGIN_IO
} photo_rt_begin_t;

/** 运行时事件回调（由 ble_photo 注册） */
typedef struct {
    void (*on_event)(const char *json); /* 短 JSON：{"t":"pe","ev":"changed"} 等 */
} photo_rt_listener_t;

/* ---------------- 生命周期（dashboard.ino 调用） ---------------- */

/** 在 SD 挂载 + LVGL/UI 初始化之后调用：读 NVS 设置、初始化清单（懒扫描）、注册照片表盘状态。 */
void photo_rt_init(void);

/** 主循环每拍调用：播放/轮播/像素位移/息屏/上传写盘（每拍 ≤4 KB）/超时/进度 UI。 */
void photo_rt_poll(uint32_t now_ms);

/** 注册事件监听（BLE 初始化时调用一次）。传 NULL 取消。 */
void photo_rt_set_listener(const photo_rt_listener_t *listener);

/* ---------------- 上报（ble_photo 读取特征时调用） ---------------- */

/** PhotoInfo JSON（≤180 B）。返回长度，0=失败。 */
int photo_rt_info_json(char *out, size_t cap);

/** PhotoState JSON（≤180 B）。返回长度，0=失败。 */
int photo_rt_state_json(char *out, size_t cap);

/** 清单项数（0..128）。 */
int photo_rt_count(void);

/** 第 index 项的 PF JSON（≤180 B）。返回长度，0=失败。 */
int photo_rt_manifest_item_json(int index, char *out, size_t cap);

/* ---------------- 命令（ble_photo 收到 PhotoCtl 后调用） ---------------- */

/** 播放第 index 张（越界夹取并钉住，语义同旧 photo_play）。 */
bool photo_rt_play(int index);

/** 删除（只允许 /photos/ 内、白名单）。失败时 *err 给协议 code（"not_ready"/"bad_arg"/"io"）。 */
bool photo_rt_delete(const char *name, const char **err);

/** 设置部分字段（has_* 为 true 的才应用）；写 NVS 并触发 changed。 */
bool photo_rt_apply_settings(bool has_slide, int slide, bool has_intv, int intv,
                             bool has_shift, int shift, bool has_soff, int soff_s);

/** 自检：若当前有 .pgj，则解码一次测 dec_ms；把结果写 NVS 并触发 info 事件。 */
void photo_rt_bench(void);

/* ---------------- 上传会话 ---------------- */

/**
 * 尝试开始（或续传）一次上传。
 * @param out_offset 开始发送的偏移（全新=0；续传=已落盘字节数）
 * @param out_name   设备分配的文件名（如 "p00012.pgj"），用于 go/done
 * @param err        失败时协议 code（"busy"/"no_space"/"too_large"/"unsupported"/"bad_arg"/"io"）
 */
photo_rt_begin_t photo_rt_upload_begin(uint8_t kind, uint32_t bytes, uint32_t crc,
                                       uint16_t w, uint16_t h, uint16_t frames, uint8_t fps,
                                       uint32_t *out_offset,
                                       char *out_name, size_t name_cap,
                                       const char **err);

/** BLE 回调里调用：校验偏移 + 拷贝进 PSRAM 环形缓冲。失败时 *want 给期望偏移。 */
bool photo_rt_upload_chunk(uint32_t offset, const uint8_t *data, uint32_t len,
                           uint32_t *want, const char **err);

/** 主循环里调用：把环形缓冲写到 SD（每拍最多 4 KB），返回本次新落盘字节数。 */
uint32_t photo_rt_upload_drain(void);

/**
 * 收到 end：关闭文件 → 读回算 CRC32 + 校验格式头/尺寸 → rename → 更新清单。
 * @param committed 成功时 true
 */
bool photo_rt_upload_end(bool *committed, const char **err);

/** 放弃本次上传（删除 .part + 清缓冲）。 */
void photo_rt_upload_abort(void);

/**
 * 格式化仪表 SD 卡（危险：删除全部照片/未完成上传），成功后重建空目录。
 *
 * 失败时 `*err` 给协议错误码（busy = 正在上传；not_ready = 没卡；io = 格式化/挂载失败）。
 * 必须由 App 二次确认后才能调用。
 */
bool photo_rt_format_card(const char **err);

/**
 * 断连时调用：暂停会话（保留 .part，可续传）。
 * 与 abort 不同：不删文件，下一次 begin 可回 `off>0` 继续。
 */
void photo_rt_upload_pause(void);

/**
 * 线程安全要求（给实现者）：
 *   - `photo_rt_upload_chunk()` 由 **BLE host 任务（core 0）** 调用；
 *     其余 upload_* 都在主循环（core 1）。实现必须用 **SPSC 环形缓冲 + 临界区/互斥**，
 *     且 `end` 开始后要立即拒绝新分片（返回 false, err="busy"）。
 *   - 回调里禁止 malloc / 写 SD / 打印大段日志。
 */

/** 是否有上传会话在进行（WAIT_DATA/RECEIVING/COMMITTING）。 */
bool photo_rt_upload_active(void);

/** 协议进度（字节） */
uint32_t photo_rt_upload_received(void);
uint32_t photo_rt_upload_total(void);
uint32_t photo_rt_upload_committed(void);

/** 上次实测动图帧率（0=未测；写 NVS）。用于 PhotoInfo.anim.fps_meas。 */
uint32_t photo_rt_playback_fps(void);

/** 当前是否正在上传（= PhotoState.rx）。 */
bool photo_rt_is_uploading(void);

/** 存储是否就绪（SD 或内置存储）。 */
bool photo_rt_storage_ready(void);

/** 可用空间（字节）。 */
uint32_t photo_rt_free_bytes(void);

#ifdef __cplusplus
}
#endif

#endif /* PHOTO_RT_H */
