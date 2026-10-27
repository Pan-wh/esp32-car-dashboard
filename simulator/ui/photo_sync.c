/**
 * photo_sync.c —— 「接收一张照片」的纯逻辑状态机
 *
 * 设计要点见 photo_sync.h。本模块**不碰 BLE / SD / JSON / LVGL**：
 *   - BLE 回调只把 (offset,len) 交给 `photo_sync_accept_chunk()` 做偏移校验；
 *   - 主循环把写盘进度交给 `photo_sync_on_committed()`；
 *   - 提交结果由调用方用 `commit_ok()/commit_fail()` 回填。
 *
 * 这样"什么时候该拒绝、该回什么偏移、什么时候算超时"这些容易写错的语义
 * 全部在 PC 上穷举测试，固件侧只剩 IO。
 *
 * 错误码（写进 `s->err`）统一用协议 §8.5 的 code，便于运行时原样回给 App：
 *   busy / no_space / too_large / unsupported / offset / io / bad_arg。
 */
#include "photo_sync.h"

#include "pgj.h" /* PGJ_MAX_DIM / PGJ_MAX_JPEG_BYTES：上传上限的唯一来源 */

#include <string.h>

/** `.pgj` 单文件上限 = 32 B 头 + 512 KB 载荷（协议 §7） */
#define SYNC_MAX_BYTES_PGJ ((uint32_t)PGJ_HEADER_BYTES + (uint32_t)PGJ_MAX_JPEG_BYTES)
/** `.pgi` 沿用旧格式上限 8 MB（设计 §6.3）；再大屏幕也放不下、传完也没意义 */
#define SYNC_MAX_BYTES_PGI (8u * 1024u * 1024u)

/** 写协议错误码（err 只有 16 B，必须截断保护） */
static void set_err(photo_sync_t *s, const char *code)
{
    if (s == NULL) return;
    if (code == NULL) {
        s->err[0] = '\0';
        return;
    }
    strncpy(s->err, code, sizeof(s->err) - 1);
    s->err[sizeof(s->err) - 1] = '\0';
}

void photo_sync_init(photo_sync_t *s)
{
    if (s == NULL) return;
    memset(s, 0, sizeof(*s));
    s->state = PHOTO_SYNC_IDLE;
}

photo_sync_result_t photo_sync_begin(photo_sync_t *s, uint8_t kind, uint32_t bytes, uint32_t crc,
                                     uint16_t w, uint16_t h, uint16_t frames, uint8_t fps,
                                     uint32_t free_bytes, uint32_t resume_offset,
                                     const char *name, uint32_t now_ms)
{
    if (s == NULL) return PHOTO_SYNC_BAD_ARG;

    /*
     * 先判 busy：已有会话时，App 收到的 code 应是 busy（提示排队/稍后），
     * 而不是被后面的参数校验掩盖成 bad_arg —— 后者会让 App 误以为命令写错了。
     */
    if (photo_sync_busy(s)) {
        set_err(s, "busy");
        return PHOTO_SYNC_BUSY;
    }
    set_err(s, NULL);

    /* kind：未知格式直接 unsupported（与协议 §8.5 的语义一致） */
    if (kind > (uint8_t)PHOTO_KIND_PGI) {
        set_err(s, "unsupported");
        return PHOTO_SYNC_UNSUPPORTED;
    }
    if (frames == 0) {
        set_err(s, "bad_arg");
        return PHOTO_SYNC_BAD_ARG;
    }
    /* .pgj 定义上就是单张静态照片；声明多帧说明调用方把 kind 搞错了 */
    if (kind == (uint8_t)PHOTO_KIND_PGJ && frames != 1) {
        set_err(s, "bad_arg");
        return PHOTO_SYNC_BAD_ARG;
    }
    /* 多帧动图没有 fps 无法播放：在 begin 就拒，避免收完 1 MB 才失败 */
    if (kind == (uint8_t)PHOTO_KIND_PGI && frames > 1 && fps == 0) {
        set_err(s, "unsupported");
        return PHOTO_SYNC_UNSUPPORTED;
    }
    /* 宽高必须落在屏幕能显示的范围内（两种格式都按 466 封顶） */
    if (w == 0 || w > PGJ_MAX_DIM || h == 0 || h > PGJ_MAX_DIM) {
        set_err(s, "bad_arg");
        return PHOTO_SYNC_BAD_ARG;
    }
    if (bytes == 0) {
        set_err(s, "bad_arg");
        return PHOTO_SYNC_BAD_ARG;
    }

    uint32_t cap = (kind == (uint8_t)PHOTO_KIND_PGJ) ? SYNC_MAX_BYTES_PGJ : SYNC_MAX_BYTES_PGI;
    if (bytes > cap) {
        /* 协议错误码是 too_large；结果枚举没有单独项，用 BAD_ARG 表达"参数非法" */
        set_err(s, "too_large");
        return PHOTO_SYNC_BAD_ARG;
    }
    if (resume_offset > bytes) {
        set_err(s, "bad_arg");
        return PHOTO_SYNC_BAD_ARG;
    }
    if (name == NULL || !photo_name_valid(name)) {
        set_err(s, "bad_arg");
        return PHOTO_SYNC_BAD_ARG;
    }

    /*
     * 空间判断用 64 位：bytes 最大 8 MB，但先把比较写对，避免以后上限调大后
     * `bytes + RESERVE` 在 u32 上回绕成"空间够"。
     * 严格用 `>`（头文件语义：> bytes + reserve 才允许），等号也算不够。
     */
    if ((uint64_t)free_bytes <= (uint64_t)bytes + (uint64_t)PHOTO_SYNC_SPACE_RESERVE) {
        set_err(s, "no_space");
        return PHOTO_SYNC_NO_SPACE;
    }

    s->kind = kind;
    s->width = w;
    s->height = h;
    s->frames = frames;
    s->fps = fps;
    s->total_bytes = bytes;
    s->crc_expect = crc;
    s->received_bytes = resume_offset;
    s->committed_bytes = resume_offset;
    s->started_ms = now_ms;
    s->last_data_ms = now_ms;
    memset(s->name, 0, sizeof(s->name));
    strncpy(s->name, name, sizeof(s->name) - 1);
    set_err(s, NULL);

    /* 续传时 .part 已有数据，等价于"已收到过数据" → 直接进 RECEIVING */
    s->state = (resume_offset > 0) ? PHOTO_SYNC_RECEIVING : PHOTO_SYNC_WAIT_DATA;
    return PHOTO_SYNC_OK;
}

bool photo_sync_accept_chunk(photo_sync_t *s, uint32_t offset, uint32_t len,
                             uint32_t *want_off, const char **err, uint32_t now_ms)
{
    if (s == NULL) {
        if (err != NULL) *err = "bad_arg";
        return false;
    }

    if (s->state == PHOTO_SYNC_COMMITTING) {
        /* end 之后、提交完成之前到达的分片一律拒：绝不能污染正在校验的 .part */
        set_err(s, "busy");
        if (err != NULL) *err = "busy";
        return false;
    }
    if (s->state != PHOTO_SYNC_WAIT_DATA && s->state != PHOTO_SYNC_RECEIVING) {
        /* IDLE / PAUSED / ERROR：必须重新 begin（续传偏移从 begin 的 go 拿） */
        set_err(s, "bad_arg");
        if (err != NULL) *err = "bad_arg";
        return false;
    }
    if (len == 0) {
        set_err(s, "bad_arg");
        if (err != NULL) *err = "bad_arg";
        return false;
    }

    /*
     * 偏移必须**严格等于**已接收字节数。BLE 无逐包 ACK，重复/乱序只可能来自
     * App 侧状态错；这里给出 want_off 让 App 从正确位置续发，而不是猜。
     */
    if (offset != s->received_bytes) {
        if (want_off != NULL) *want_off = s->received_bytes;
        set_err(s, "offset");
        if (err != NULL) *err = "offset";
        return false;
    }

    /* offset + len 可能超过声明的 total：App 多发数据必须拒（u64 防回绕） */
    if ((uint64_t)offset + (uint64_t)len > (uint64_t)s->total_bytes) {
        set_err(s, "bad_arg");
        if (err != NULL) *err = "bad_arg";
        return false;
    }

    s->received_bytes = offset + len;
    s->state = PHOTO_SYNC_RECEIVING;
    s->last_data_ms = now_ms;
    set_err(s, NULL);
    return true;
}

void photo_sync_on_committed(photo_sync_t *s, uint32_t committed_bytes)
{
    if (s == NULL) return;
    /* 落盘进度只能前进：主循环偶尔重复上报同一块不应让进度倒退 */
    if (committed_bytes <= s->committed_bytes) return;
    /* 防御调用方算错：committed 永远不能超过已接收（否则 credit 会虚报） */
    if (committed_bytes > s->received_bytes) committed_bytes = s->received_bytes;
    s->committed_bytes = committed_bytes;
}

bool photo_sync_request_end(photo_sync_t *s, const char **err)
{
    if (s == NULL) {
        if (err != NULL) *err = "bad_arg";
        return false;
    }
    if (s->state != PHOTO_SYNC_WAIT_DATA && s->state != PHOTO_SYNC_RECEIVING) {
        set_err(s, "bad_arg");
        if (err != NULL) *err = "bad_arg";
        return false;
    }
    if (s->received_bytes != s->total_bytes) {
        /*
         * 数据没发完就 end：这是 App 的逻辑错，不是"分片偏移错"，
         * 也不该伪装成 crc 失败（那会触发整份重传）。按 bad_arg 回，
         * App 从最后一次 credit 的偏移续传即可。
         */
        set_err(s, "bad_arg");
        if (err != NULL) *err = "bad_arg";
        return false;
    }
    s->state = PHOTO_SYNC_COMMITTING;
    set_err(s, NULL);
    if (err != NULL) *err = NULL;
    return true;
}

void photo_sync_commit_ok(photo_sync_t *s)
{
    if (s == NULL) return;
    s->state = PHOTO_SYNC_IDLE;
    set_err(s, NULL);
}

void photo_sync_commit_fail(photo_sync_t *s, const char *err)
{
    if (s == NULL) return;
    s->state = PHOTO_SYNC_ERROR;
    set_err(s, (err != NULL && err[0] != '\0') ? err : "io");
}

void photo_sync_pause(photo_sync_t *s)
{
    if (s == NULL) return;
    /*
     * 只有"正在等数据/收数据"才需要暂停。COMMITTING 期间断连不能改状态：
     * rename/CRC 是主循环里的一次原子流程，改状态会让调用方误判会话已结束。
     */
    if (s->state == PHOTO_SYNC_WAIT_DATA || s->state == PHOTO_SYNC_RECEIVING) {
        s->state = PHOTO_SYNC_PAUSED;
    }
}

void photo_sync_tick(photo_sync_t *s, uint32_t now_ms)
{
    if (s == NULL) return;
    if (s->state != PHOTO_SYNC_WAIT_DATA && s->state != PHOTO_SYNC_RECEIVING) return;
    /* 无符号相减：now_ms 回绕（49 天）时仍然算得出正确间隔 */
    if ((uint32_t)(now_ms - s->last_data_ms) >= PHOTO_SYNC_TIMEOUT_MS) {
        s->state = PHOTO_SYNC_PAUSED;
    }
}

bool photo_sync_busy(const photo_sync_t *s)
{
    if (s == NULL) return false;
    return s->state == PHOTO_SYNC_WAIT_DATA || s->state == PHOTO_SYNC_RECEIVING ||
           s->state == PHOTO_SYNC_COMMITTING;
}

bool photo_sync_active(const photo_sync_t *s)
{
    if (s == NULL) return false;
    return s->state == PHOTO_SYNC_WAIT_DATA || s->state == PHOTO_SYNC_RECEIVING;
}

uint32_t photo_sync_outstanding(const photo_sync_t *s)
{
    if (s == NULL) return 0;
    if (s->received_bytes <= s->committed_bytes) return 0;
    return s->received_bytes - s->committed_bytes;
}

bool photo_sync_need_credit(const photo_sync_t *s, uint32_t last_credit_off,
                            uint32_t last_credit_ms, uint32_t now_ms)
{
    if (s == NULL) return false;
    /* 没有新落盘就不发 credit：空 notify 只会抢空口 */
    if (s->committed_bytes <= last_credit_off) return false;

    uint32_t delta = s->committed_bytes - last_credit_off;
    if (delta >= PHOTO_SYNC_CREDIT_STEP_BYTES) return true;
    /* 小文件/慢链路：攒不满 1 KB 也要定期告诉 App 进度，避免它以为设备卡死 */
    return (uint32_t)(now_ms - last_credit_ms) >= PHOTO_SYNC_CREDIT_INTERVAL_MS;
}
