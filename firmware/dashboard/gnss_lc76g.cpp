/*
 * LC76G GNSS 读取层（I2C 传输，Arduino Wire 实现）
 *
 * ⚠️ 本文件必须是 .cpp：Arduino 的 Wire/Print/Stream 是 C++ 类型，
 *    用 .c 编译会报 "unknown type name 'class'"。
 *
 * 协议与接口说明见 gnss_lc76g.h。
 * 本文件只在固件侧参与编译（依赖 Arduino Wire）；
 * NMEA 解析逻辑在平台无关的 gnss_nmea.c 中（模拟器与固件共用）。
 */
#include "gnss_lc76g.h"

#include <Arduino.h>
#include <Wire.h>

#define LC76G_ADDR_W 0x50
#define LC76G_ADDR_R 0x54
#define LC76G_CHUNK_MAX 2048 /* 单轮读取上限，防御异常长度导致越界 */
#define LC76G_SETTLE_MS 20   /* 官方示例 100ms；缩短以降低延迟，失败会自然重试 */

static bool s_ready = false;

static TwoWire *wire_of(void *w)
{
    return (w != NULL) ? (TwoWire *)w : &Wire;
}

bool gnss_lc76g_begin(void *wire)
{
    TwoWire *w = wire_of(wire);

    /* 发一次长度请求即验证握手（能成功写说明器件在线） */
    uint8_t req[8] = { 0x08, 0x00, 0x51, 0xAA, 0x04, 0x00, 0x00, 0x00 };
    w->beginTransmission(LC76G_ADDR_W);
    w->write(req, sizeof(req));
    s_ready = (w->endTransmission() == 0);
    return s_ready;
}

bool gnss_lc76g_ready(void)
{
    return s_ready;
}

size_t gnss_lc76g_read(void *wire, char *buf, size_t buf_size)
{
    TwoWire *w = wire_of(wire);

    if (!s_ready || buf == NULL || buf_size == 0) return 0;

    /* 1) 请求本次数据长度 */
    uint8_t req[8] = { 0x08, 0x00, 0x51, 0xAA, 0x04, 0x00, 0x00, 0x00 };
    w->beginTransmission(LC76G_ADDR_W);
    w->write(req, sizeof(req));
    if (w->endTransmission() != 0) return 0;

    delay(LC76G_SETTLE_MS);

    /* 2) 读 4 字节长度（小端） */
    uint8_t len_raw[4] = { 0 };
    if (w->requestFrom((uint8_t)LC76G_ADDR_R, (uint8_t)4) != 4) return 0;
    for (int i = 0; i < 4; i++) len_raw[i] = (uint8_t)w->read();

    uint32_t len = (uint32_t)len_raw[0] | ((uint32_t)len_raw[1] << 8) |
                   ((uint32_t)len_raw[2] << 16) | ((uint32_t)len_raw[3] << 24);

    if (len == 0) return 0; /* 本轮暂无新数据：正常情况 */

    if (len > LC76G_CHUNK_MAX || len > buf_size) {
        return 0; /* 长度异常或缓冲不足：丢弃本轮，避免越界 */
    }

    /* 3) 带长度再次请求数据块 */
    uint8_t hdr[4] = { 0x00, 0x20, 0x51, 0xAA };
    w->beginTransmission(LC76G_ADDR_W);
    w->write(hdr, sizeof(hdr));
    w->write(len_raw, sizeof(len_raw));
    if (w->endTransmission() != 0) return 0;

    delay(LC76G_SETTLE_MS);

    /* 4) 读取 NMEA 文本（分块，受 Wire 缓冲限制） */
    size_t got = 0;
    while (got < len) {
        size_t want = len - got;
        if (want > 128) want = 128;

        if (w->requestFrom((uint8_t)LC76G_ADDR_R, (uint8_t)want) != (uint8_t)want) {
            break; /* 中途失败：返回已读部分 */
        }
        for (size_t i = 0; i < want; i++) {
            buf[got++] = (char)w->read();
        }
    }

    return got;
}
