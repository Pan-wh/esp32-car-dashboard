/**
 * photo_sync.h —— 设备端「接收一张照片」的纯逻辑状态机
 *
 * 只管**协议语义**：偏移校验、续传、窗口/credit 判定、超时、提交前完整性校验。
 * **不碰** BLE、SD、JSON、LVGL —— 便于在 PC 上穷举测试。
 *
 * 数据通路（固件侧）：
 *   BLE 回调 → photo_sync_accept_chunk() → PSRAM 环形缓冲
 *   主循环   → 写 SD → photo_sync_on_committed()
 *   end      → photo_sync_request_end() → 校验 CRC/头 → photo_sync_commit_ok()/fail()
 *
 * 状态机：
 *
 *   IDLE ──begin──▶ WAIT_DATA ──首片──▶ RECEIVING ──end──▶ COMMITTING ──ok──▶ IDLE
 *                        │                  │                     └─fail─▶ ERROR
 *                        └──10 s 无数据──▶ PAUSED（保留 .part，可续传）
 *   PAUSED / RECEIVING ──断连──▶ PAUSED
 */
#ifndef PHOTO_SYNC_H
#define PHOTO_SYNC_H

#include <stdbool.h>
#include <stdint.h>

#include "photo_list.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 在途窗口（协议 `win`）：设备一次允许 App 最多发多少字节 */
#define PHOTO_SYNC_WINDOW_BYTES 4096u
/** 建议单片 data 上限（协议 `go.chunk`） */
#define PHOTO_SYNC_CHUNK_HINT 178u
/** 无数据超时：超过则转 PAUSED */
#define PHOTO_SYNC_TIMEOUT_MS 10000u
/** 提交前必须留出的剩余空间（防止写满卡） */
#define PHOTO_SYNC_SPACE_RESERVE (1024u * 1024u)
/** credit 触发：已落盘增量与最大间隔 */
#define PHOTO_SYNC_CREDIT_STEP_BYTES 1024u
#define PHOTO_SYNC_CREDIT_INTERVAL_MS 100u

typedef enum {
    PHOTO_SYNC_IDLE = 0,
    PHOTO_SYNC_WAIT_DATA,  /* begin 已接受，等第一片 */
    PHOTO_SYNC_RECEIVING,  /* 正在收 */
    PHOTO_SYNC_COMMITTING, /* end 已收，等校验/rename 结果 */
    PHOTO_SYNC_PAUSED,     /* 断连/超时；.part 保留可续传 */
    PHOTO_SYNC_ERROR       /* 本次失败，已丢弃 */
} photo_sync_state_t;

typedef enum {
    PHOTO_SYNC_OK = 0,
    PHOTO_SYNC_BUSY,        /* 已有上传在进行 */
    PHOTO_SYNC_BAD_ARG,     /* 参数非法 */
    PHOTO_SYNC_NO_SPACE,    /* 剩余空间不足 */
    PHOTO_SYNC_UNSUPPORTED, /* kind/fps 等不支持 */
    PHOTO_SYNC_IO           /* 无法创建临时文件（由调用方传入） */
} photo_sync_result_t;

typedef struct {
    photo_sync_state_t state;

    uint8_t  kind;  /* photo_kind_t */
    uint16_t width;
    uint16_t height;
    uint16_t frames;
    uint8_t  fps;

    uint32_t total_bytes;     /* begin 声明的文件大小 */
    uint32_t received_bytes;  /* 已接收（含 PSRAM 缓冲里未落盘的） */
    uint32_t committed_bytes; /* 已落盘（credit 用） */
    uint32_t crc_expect;

    uint32_t started_ms;
    uint32_t last_data_ms;

    char name[PHOTO_NAME_MAX]; /* 设备分配的文件名 */
    char err[16];              /* 最近错误码（协议 code） */
} photo_sync_t;

/** 初始化（清零 + IDLE） */
void photo_sync_init(photo_sync_t *s);

/**
 * 尝试开始一次上传。
 *
 * @param free_bytes    存储可用字节（> bytes + PHOTO_SYNC_SPACE_RESERVE 才允许）
 * @param resume_offset `.part` 已有字节数（0 = 全新；>0 = 续传）
 * @param name          设备已分配的文件名（如 "p00012.pgj"）
 *
 * 成功时：state=WAIT_DATA（resume_offset==0）或 RECEIVING（resume>0），
 *         received_bytes=committed_bytes=resume_offset。
 */
photo_sync_result_t photo_sync_begin(photo_sync_t *s, uint8_t kind, uint32_t bytes, uint32_t crc,
                                     uint16_t w, uint16_t h, uint16_t frames, uint8_t fps,
                                     uint32_t free_bytes, uint32_t resume_offset,
                                     const char *name, uint32_t now_ms);

/**
 * 接收一个分片。
 *
 * 规则：offset 必须 == received_bytes（否则 err="offset"，want_off 给出期望值）；
 *       len>0 且不越过 total_bytes；成功后 received_bytes += len、state=RECEIVING。
 *
 * @param want_off 失败且是 offset 错误时写期望偏移（可 NULL）
 * @param err      失败原因（协议 code，可 NULL）
 */
bool photo_sync_accept_chunk(photo_sync_t *s, uint32_t offset, uint32_t len,
                             uint32_t *want_off, const char **err, uint32_t now_ms);

/** 写盘进度（主循环每写一块调一次）。committed 不得超过 received。 */
void photo_sync_on_committed(photo_sync_t *s, uint32_t committed_bytes);

/** 收到 end：要求 received == total，然后进入 COMMITTING。 */
bool photo_sync_request_end(photo_sync_t *s, const char **err);

/** 提交成功（rename 完成） */
void photo_sync_commit_ok(photo_sync_t *s);

/** 提交失败（CRC/头/IO） */
void photo_sync_commit_fail(photo_sync_t *s, const char *err);

/** 主动暂停（断连） */
void photo_sync_pause(photo_sync_t *s);

/** 超时推进：WAIT_DATA/RECEIVING 超过 PHOTO_SYNC_TIMEOUT_MS 无数据 → PAUSED */
void photo_sync_tick(photo_sync_t *s, uint32_t now_ms);

/** 是否占着上传通道（WAIT_DATA / RECEIVING / COMMITTING） */
bool photo_sync_busy(const photo_sync_t *s);

/** 是否正在收数据（WAIT_DATA / RECEIVING） */
bool photo_sync_active(const photo_sync_t *s);

/** 未落盘字节数 = received - committed */
uint32_t photo_sync_outstanding(const photo_sync_t *s);

/**
 * 是否该发 credit：自上次 credit 以来已落盘增量 ≥ PHOTO_SYNC_CREDIT_STEP_BYTES，
 * 或距上次 ≥ PHOTO_SYNC_CREDIT_INTERVAL_MS 且 committed 有前进。
 */
bool photo_sync_need_credit(const photo_sync_t *s, uint32_t last_credit_off,
                            uint32_t last_credit_ms, uint32_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* PHOTO_SYNC_H */
