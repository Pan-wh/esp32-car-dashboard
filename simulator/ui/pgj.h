/**
 * pgj.h —— `.pgj` 容器（静态照片：32 B 头 + 1 张 baseline JPEG）
 *
 * 纯逻辑，不依赖 LVGL / Arduino / SD，可在 PC 上单测。
 * 协议定义见 `docs/photo-protocol.md` §7；设计见 `docs/photo-wireless-design.md` §6。
 *
 * ## 为什么不用裸 JPEG
 * 设备扫描清单时只想读 **32 字节**就能拿到宽高/帧数/大小/CRC，不用把整张 JPEG 读进内存；
 * 同时能在"扫描阶段"就把坏文件挡掉（历史 `.pgi` 那种 `LOAD ERROR` 不允许再出现）。
 *
 * ## 与 .pgi 的关系
 * `.pgj` 只放**一帧静态照片**；动图继续用 `.pgi`（`pgi.h`）。
 *
 * ## CRC32
 * 使用标准 CRC-32（IEEE 802.3，反射，poly 0xEDB88320，初值/异或 0xFFFFFFFF），
 * 与 zlib `crc32()`、Swift 侧实现一致。**覆盖整个文件**（32 B 头 + JPEG 载荷）。
 */
#ifndef PGJ_H
#define PGJ_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 文件头长度 */
#define PGJ_HEADER_BYTES 32u
/** 支持的最大边长（与屏幕一致；设备帧缓冲按 466×466 分配） */
#define PGJ_MAX_DIM 466u
/** 单张 JPEG 载荷上限（正常 40~60 KB，这里留足余量；也是上传单文件上限） */
#define PGJ_MAX_JPEG_BYTES (512u * 1024u)
/** 协议版本 */
#define PGJ_VERSION 1u
/** flags bit0：baseline JPEG（v1 必须为 1） */
#define PGJ_FLAG_BASELINE 0x01u

/** 解析后的文件头 */
typedef struct {
    uint16_t width;
    uint16_t height;
    uint8_t  version;      /* 必须 == PGJ_VERSION */
    uint8_t  flags;        /* bit0 必须为 1 */
    uint16_t quality;      /* 10..100，仅记录 */
    uint32_t jpeg_bytes;   /* == 文件大小 - 32 */
    uint32_t crc32;        /* 整个文件的 CRC32 */
    uint32_t created_unix; /* 0 = 未知 */
} pgj_header_t;

/**
 * 只解析 32 B 头（扫描清单时用），并做字段合法性检查。
 *
 * 校验：magic == "PGJ1"、version == 1、flags bit0 == 1、
 *       1 <= w,h <= PGJ_MAX_DIM、1 <= jpeg_bytes <= PGJ_MAX_JPEG_BYTES、
 *       quality 在 1..100。
 *
 * @param buf 至少 32 字节
 * @param len 可用字节数（<32 → false）
 * @param out 成功时写入
 */
bool pgj_parse_header(const uint8_t *buf, size_t len, pgj_header_t *out);

/**
 * 校验**完整文件**（提交/播放前用）：
 *   头合法 + `len == 32 + jpeg_bytes` + JPEG 是 baseline + JPEG SOF 尺寸 == 头尺寸。
 *
 * @param err 失败原因（静态字符串，可为 NULL）
 */
bool pgj_validate_file(const uint8_t *buf, size_t len, pgj_header_t *out, const char **err);

/**
 * 从 JPEG 字节流里读 baseline SOF 尺寸。
 *
 * - 接受 SOF0 (`0xC0`)；遇到 `0xC2`（progressive）→ 返回 false，err="progressive"；
 * - 遇到其它 SOF（C1/C3/C5..）→ false，err="unsupported_sof"；
 * - 数据被截断/找不到 SOF → false，err="bad_jpeg"。
 *
 * 纯字节扫描，不解码像素，开销 O(头长度)（<1 KB）。
 */
bool pgj_jpeg_info(const uint8_t *jpeg, size_t len, uint16_t *w, uint16_t *h, const char **err);

/** 标准 CRC-32（见文件头注释）。 */
uint32_t pgj_crc32(const void *data, size_t len);

/** 构造 32 B 头（测试/夹具/App 对齐用；设备端不需要）。返回写入字节数（32）。 */
size_t pgj_build_header(uint8_t out[PGJ_HEADER_BYTES], const pgj_header_t *h);

#ifdef __cplusplus
}
#endif

#endif /* PGJ_H */
