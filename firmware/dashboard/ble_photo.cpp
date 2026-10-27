/*
 * ble_photo.cpp —— BLE 照片服务实现
 *
 * 详见 ble_photo.h 与 docs/photo-protocol.md。
 *
 * 线程模型（重要）：
 *   - BLE 回调运行在 NimBLE host 任务（core 0）。回调里**只**做：
 *       ① Ctl：解析 JSON → 入命令队列（portMUX 保护），不做 SD/NVS/LVGL；
 *       ② Data：偏移校验 + memcpy 进 PSRAM 环形（photo_rt 内部实现），不写盘。
 *   - 所有副作用（begin/end/del/play/set/bench、发 notify、发 credit、清单分页）
 *     都在主循环的 `ble_photo_poll()` 里执行。
 */
#include "ble_photo.h"

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLE2902.h>
#include <stdio.h>
#include <string.h>

#include "ble_telemetry.h" /* ble_telemetry_connected()：判断是否已连接 */
#include "photo_cmd.h"
#include "photo_rt.h"
#include "photo_sync.h" /* PHOTO_SYNC_CHUNK_HINT / WINDOW_BYTES / CREDIT_* */

#define PHOTO_SERVICE_UUID "7D2A0010-8F4A-4B7C-9D21-5E6A3B8C1000"
#define PHOTO_CTL_UUID     "7D2A0011-8F4A-4B7C-9D21-5E6A3B8C1000"
#define PHOTO_DATA_UUID    "7D2A0012-8F4A-4B7C-9D21-5E6A3B8C1000"
#define PHOTO_EVT_UUID     "7D2A0013-8F4A-4B7C-9D21-5E6A3B8C1000"
#define PHOTO_INFO_UUID    "7D2A0014-8F4A-4B7C-9D21-5E6A3B8C1000"
#define PHOTO_STATE_UUID   "7D2A0015-8F4A-4B7C-9D21-5E6A3B8C1000"

/* JSON 硬上限：iOS Notify/Read 有效载荷约 184 B（见 docs/ble.md） */
#define PHOTO_JSON_MAX 180
/* 命令队列深度（App 是串行问答，8 足够；溢出说明有异常，丢最旧） */
#define PHOTO_CMD_QUEUE 8

static bool s_inited = false;

static BLECharacteristic *s_ctl = NULL;
static BLECharacteristic *s_data = NULL;
static BLECharacteristic *s_evt = NULL;
static BLECharacteristic *s_info = NULL;
static BLECharacteristic *s_state = NULL;

/* ---- 命令队列（BLE 回调 → 主循环） ---- */
static portMUX_TYPE s_cmd_mux = portMUX_INITIALIZER_UNLOCKED;
static photo_cmd_t s_cmd_q[PHOTO_CMD_QUEUE];
static int s_cmd_head = 0; /* 生产者（BLE 回调） */
static int s_cmd_tail = 0; /* 消费者（主循环） */
static uint32_t s_cmd_dropped = 0;

/* ---- 分片错误暂存（回调里不 notify，主循环补发） ---- */
static portMUX_TYPE s_err_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_err_pending = false;
static char s_err_code[16] = {0};
static int32_t s_err_want = -1;

/* ---- 上传 credit 状态（主循环） ---- */
static char s_upload_name[PHOTO_NAME_MAX] = {0};
static uint32_t s_credit_off = 0;
static uint32_t s_credit_ms = 0;
/* go 重发（App 可能错过一条 notify；收到第一片数据后即停止）*/
static uint32_t s_go_off = 0;

static uint32_t s_go_ms = 0;
static bool s_go_pending = false;

/* ---- 清单分页状态（主循环） ---- */
static bool s_list_pending = false;
static int s_list_phase = 0; /* 0=begin, 1=items */
static int s_list_next = 0;
static int s_list_total = 0;
static uint32_t s_list_last_ms = 0;

/* ---- 周期刷新 ---- */
static uint32_t s_last_state_ms = 0;
static uint32_t s_last_info_ms = 0;

/* ---------------- 小工具 ---------------- */

/**
 * 发一条事件 JSON（仅主循环调用）。
 *
 * ⚠️ **不要用 `ble_telemetry_connected()` 做门禁**：实测 NimBLE 的连接判断在传输中
 * 会短时归零（即使链路正常），一旦在门禁里 return，credit 就永久丢了 →
 * App 发满窗口僵死（真机表现为随机卡在 13/25/33 KB）。直接 notify 即可；
 * 真断连时 notify 自然失败。
 *
 * 注意：这个 BLE 库的 `notify()` 返回 void，拿不到"是否成功"；
 * 所以 credit 采用"周期性重发当前水位"来容忍丢包（见下方 credit 逻辑）。
 */
static void evt_send(const char *json)
{
    if (s_evt == NULL || json == NULL) return;
    size_t n = strlen(json);
    if (n > PHOTO_JSON_MAX) {
        Serial.printf("[照片] 事件 JSON 超长（%u B），丢弃\n", (unsigned)n);
        return;
    }
    s_evt->setValue((uint8_t *)json, n);
    s_evt->notify();
}

static void evt_err(const char *code, int32_t want)
{
    char buf[96];
    if (want >= 0) {
        snprintf(buf, sizeof(buf), "{\"t\":\"pe\",\"ev\":\"err\",\"code\":\"%s\",\"want\":%d}",
                 code != NULL ? code : "bad_arg", (int)want);
    }
    else {
        snprintf(buf, sizeof(buf), "{\"t\":\"pe\",\"ev\":\"err\",\"code\":\"%s\"}",
                 code != NULL ? code : "bad_arg");
    }
    evt_send(buf);
}

static void state_refresh(bool do_notify)
{
    if (s_state == NULL) return;
    char buf[220];
    int n = photo_rt_state_json(buf, sizeof(buf));
    if (n <= 0) return;
    s_state->setValue((uint8_t *)buf, (size_t)n);
    if (do_notify && ble_telemetry_connected()) s_state->notify();
}

static void info_refresh(void)
{
    if (s_info == NULL) return;
    char buf[220];
    int n = photo_rt_info_json(buf, sizeof(buf));
    if (n <= 0) return;
    s_info->setValue((uint8_t *)buf, (size_t)n);
}

/* ---------------- 运行时事件 → BLE 通知 ---------------- */

static void on_rt_event(const char *json)
{
    if (json == NULL) return;

    /*
     * 运行时只在主循环调用本回调（photo_rt_poll 内部），所以这里发 notify 是安全的。
     * "changed" 同时刷新 State 特征值（App 收到事件后会 Read，不必依赖 Notify 载荷）。
     */
    if (strstr(json, "\"changed\"") != NULL) {
        state_refresh(true);
        info_refresh(); /* 数量/能力可能变化（删除/上传完成） */
    }
    else if (strstr(json, "\"info\"") != NULL) {
        info_refresh();
    }
    evt_send(json);
}

/* ---------------- BLE 回调 ---------------- */

class PhotoCtlCallbacks : public BLECharacteristicCallbacks {
public:
    void onWrite(BLECharacteristic *c) override
    {
        const uint8_t *d = c->getData();
        size_t n = c->getLength();
        if (d == NULL || n == 0) return;

        photo_cmd_t cmd;
        if (!photo_cmd_parse((const char *)d, n, &cmd)) {
            /* 回调里不直接 notify：记错误，由 poll 补发（避免在 host 任务里操作特征值） */
            portENTER_CRITICAL(&s_err_mux);
            s_err_pending = true;
            snprintf(s_err_code, sizeof(s_err_code), "bad_arg");
            s_err_want = -1;
            portEXIT_CRITICAL(&s_err_mux);
            return;
        }

        portENTER_CRITICAL(&s_cmd_mux);
        int next = (s_cmd_head + 1) % PHOTO_CMD_QUEUE;
        if (next == s_cmd_tail) {
            /* 队列满：丢最旧（正常串行问答不会发生） */
            s_cmd_tail = (s_cmd_tail + 1) % PHOTO_CMD_QUEUE;
            s_cmd_dropped++;
        }
        s_cmd_q[s_cmd_head] = cmd;
        s_cmd_head = next;
        portEXIT_CRITICAL(&s_cmd_mux);
    }
};

class PhotoDataCallbacks : public BLECharacteristicCallbacks {
public:
    void onWrite(BLECharacteristic *c) override
    {
        const uint8_t *d = c->getData();
        size_t n = c->getLength();
        if (d == NULL || n < 5) return; /* 4 B offset + 至少 1 B */

        /*
         * 分片格式：[u32 offset LE][payload]
         * 手工拼，避免未对齐指针强转（ESP32-S3 允许非对齐但没必要冒险）。
         */
        uint32_t off = (uint32_t)d[0] | ((uint32_t)d[1] << 8) | ((uint32_t)d[2] << 16) |
                       ((uint32_t)d[3] << 24);
        uint32_t want = 0;
        const char *err = NULL;
        if (!photo_rt_upload_chunk(off, d + 4, (uint32_t)(n - 4), &want, &err)) {
            portENTER_CRITICAL(&s_err_mux);
            s_err_pending = true;
            snprintf(s_err_code, sizeof(s_err_code), "%s", err != NULL ? err : "offset");
            s_err_want = (int32_t)want;
            portEXIT_CRITICAL(&s_err_mux);
        }
    }
};

/* ---------------- 命令执行（主循环） ---------------- */

static void send_go(uint32_t off, const char *name)
{
    char buf[200];
    snprintf(buf, sizeof(buf),
             "{\"t\":\"pe\",\"ev\":\"go\",\"off\":%u,\"chunk\":%u,\"win\":%u,\"id\":\"%s\"}",
             (unsigned)off, (unsigned)PHOTO_SYNC_CHUNK_HINT, (unsigned)PHOTO_SYNC_WINDOW_BYTES,
             name != NULL ? name : "");
    evt_send(buf);
}

static const char *begin_code(photo_rt_begin_t r)
{
    switch (r) {
        case PHOTO_RT_BEGIN_BUSY: return "busy";
        case PHOTO_RT_BEGIN_BAD_ARG: return "bad_arg";
        case PHOTO_RT_BEGIN_NO_SPACE: return "no_space";
        case PHOTO_RT_BEGIN_UNSUPPORTED: return "unsupported";
        case PHOTO_RT_BEGIN_IO: return "io";
        default: return "bad_arg";
    }
}

static void handle_cmd(const photo_cmd_t *cmd)
{
    switch (cmd->op) {
        case PHOTO_OP_BEGIN: {
            uint32_t off = 0;
            char name[PHOTO_NAME_MAX] = {0};
            const char *err = NULL;
            photo_rt_begin_t r = photo_rt_upload_begin(
                cmd->kind, cmd->bytes, cmd->crc, cmd->width, cmd->height, cmd->frames, cmd->fps, &off,
                name, sizeof(name), &err);
            if (r == PHOTO_RT_BEGIN_OK || r == PHOTO_RT_BEGIN_RESUME) {
                snprintf(s_upload_name, sizeof(s_upload_name), "%s", name);
                s_credit_off = off;
                s_credit_ms = millis();
                s_go_off = off;
                s_go_ms = millis();
                s_go_pending = true;
                send_go(off, name);
            }
            else {
                evt_err(err != NULL ? err : begin_code(r), -1);
            }
        } break;

        case PHOTO_OP_END: {
            bool committed = false;
            const char *err = NULL;
            if (photo_rt_upload_end(&committed, &err) && committed) {
                char buf[120];
                snprintf(buf, sizeof(buf), "{\"t\":\"pe\",\"ev\":\"done\",\"id\":\"%s\"}", s_upload_name);
                evt_send(buf);
                state_refresh(true);
            }
            else {
                /*
                 * "offset" 表示"还差几片"：把已收字节数作为 want 回给 App，
                 * App 会从该偏移续发（见协议 §6）。其它错误（crc/io/...）正常报。
                 */
                int32_t want = -1;
                if (err != NULL && strcmp(err, "offset") == 0) {
                    want = (int32_t)photo_rt_upload_received();
                }
                evt_err(err != NULL ? err : "crc", want);
            }
        } break;

        case PHOTO_OP_ABORT:
            photo_rt_upload_abort();
            state_refresh(true);
            break;

        case PHOTO_OP_DEL: {
            const char *err = NULL;
            if (!photo_rt_delete(cmd->id, &err)) evt_err(err != NULL ? err : "io", -1);
        } break;

        case PHOTO_OP_PLAY:
            if (!photo_rt_play(cmd->index)) evt_err("bad_arg", -1);
            break;

        case PHOTO_OP_SET:
            if (!photo_rt_apply_settings(cmd->has_slide, cmd->slide, cmd->has_intv, cmd->intv,
                                         cmd->has_shift, cmd->shift, cmd->has_soff, cmd->soff_s)) {
                evt_err("bad_arg", -1);
            }
            break;

        case PHOTO_OP_LIST:
            s_list_pending = true;
            s_list_phase = 0;
            s_list_next = 0;
            s_list_total = photo_rt_count();
            s_list_last_ms = 0;
            break;

        case PHOTO_OP_BENCH:
            photo_rt_bench(); /* 结束时由运行时发 {"ev":"info"} */
            break;

        case PHOTO_OP_FORMAT: {
            /*
             * 格式化 SD 卡：耗时约 1~3 s（主循环阻塞），完成后回一条显式事件。
             * App 必须二次确认后才发；这里再做一次"正在上传就拒绝"的业务校验。
             */
            const char *e = NULL;
            if (photo_rt_format_card(&e)) {
                evt_send("{\"t\":\"pe\",\"ev\":\"fmt\",\"ok\":1}");
                state_refresh(true);
                info_refresh();
            }
            else {
                evt_err(e != NULL ? e : "io", -1);
            }
        } break;

        default:
            evt_err("bad_arg", -1);
            break;
    }
}

/* ---------------- 对外接口 ---------------- */

bool ble_photo_begin(void)
{
    if (s_inited) return true;

    BLEServer *server = BLEDevice::getServer();
    if (server == NULL) return false;

    BLEService *svc = server->createService(PHOTO_SERVICE_UUID);
    if (svc == NULL) return false;

    s_ctl = svc->createCharacteristic(PHOTO_CTL_UUID, BLECharacteristic::PROPERTY_WRITE);
    /*
     * 照片分片同时支持 WRITE（带响应）与 WRITE_NR。
     * 真机（iOS 17）上 WRITE_NR 的 `canSendWriteWithoutResponse` 会永久卡 false，
     * 上层改为逐片带响应写：慢一点（~6 KB/s），但流控确定、不会半路僵死。
     */
    s_data = svc->createCharacteristic(PHOTO_DATA_UUID,
                                       BLECharacteristic::PROPERTY_WRITE |
                                           BLECharacteristic::PROPERTY_WRITE_NR);
    s_evt = svc->createCharacteristic(PHOTO_EVT_UUID, BLECharacteristic::PROPERTY_NOTIFY |
                                                          BLECharacteristic::PROPERTY_READ);
    s_info = svc->createCharacteristic(PHOTO_INFO_UUID, BLECharacteristic::PROPERTY_READ);
    s_state = svc->createCharacteristic(PHOTO_STATE_UUID, BLECharacteristic::PROPERTY_READ |
                                                            BLECharacteristic::PROPERTY_NOTIFY);
    if (s_ctl == NULL || s_data == NULL || s_evt == NULL || s_info == NULL || s_state == NULL) {
        return false;
    }

    s_ctl->setCallbacks(new PhotoCtlCallbacks());
    s_data->setCallbacks(new PhotoDataCallbacks());
    s_evt->addDescriptor(new BLE2902());
    s_state->addDescriptor(new BLE2902());

    /* 先填初值，App 一连上 Read 就能拿到（不等第一次 poll） */
    info_refresh();
    state_refresh(false);

    svc->start();

    photo_rt_listener_t listener = {on_rt_event};
    photo_rt_set_listener(&listener);

    s_inited = true;
    Serial.println("[照片] BLE 照片服务已就绪（1001~1005）");
    return true;
}

void ble_photo_poll(void)
{
    if (!s_inited) return;

    const uint32_t now = millis();
    /* ① 分片错误补发（回调里只暂存） */
    bool err_pending = false;
    char err_code[16] = {0};
    int32_t err_want = -1;
    portENTER_CRITICAL(&s_err_mux);
    if (s_err_pending) {
        err_pending = true;
        s_err_pending = false;
        strncpy(err_code, s_err_code, sizeof(err_code) - 1);
        err_want = s_err_want;
    }
    portEXIT_CRITICAL(&s_err_mux);
    if (err_pending) evt_err(err_code, err_want);

    /* ② 执行排队命令（会碰 SD/NVS 的都在主循环） */
    while (true) {
        photo_cmd_t cmd;
        bool have = false;
        portENTER_CRITICAL(&s_cmd_mux);
        if (s_cmd_tail != s_cmd_head) {
            cmd = s_cmd_q[s_cmd_tail];
            s_cmd_tail = (s_cmd_tail + 1) % PHOTO_CMD_QUEUE;
            have = true;
        }
        portEXIT_CRITICAL(&s_cmd_mux);
        if (!have) break;
        handle_cmd(&cmd);
    }

    /*
     * ②.5 go 重发：begin 的 `go` 是唯一起步信号，丢一条 App 就永远等不到。
     * 在收到第一片数据前每 400 ms 重发（幂等；App 已开始接收则停止）。
     */
    if (s_go_pending) {
        if (!photo_rt_upload_active() || photo_rt_upload_received() > 0) {
            s_go_pending = false;
        }
        else if ((uint32_t)(millis() - s_go_ms) >= 400u) {
            s_go_ms = millis();
            send_go(s_go_off, s_upload_name);
        }
    }

    /* ③ 上传 credit：按"已落盘字节"推进，不是逐片 ACK */
    if (photo_rt_upload_active()) {
        const uint32_t committed = photo_rt_upload_committed();
        const bool step = (committed > s_credit_off) &&
                          (committed - s_credit_off >= PHOTO_SYNC_CREDIT_STEP_BYTES ||
                           (uint32_t)(now - s_credit_ms) >= PHOTO_SYNC_CREDIT_INTERVAL_MS);
        /*
         * 除了按水位推进，每 400 ms 再"重发一次当前水位"：
         * NimBLE 的 notify 在 mbuf 紧张时可能悄悄丢，而 API 不返回成败；
         * 重复 credit 对 App 是幂等的（只会把 acked 更新到同一/更大值），
         * 这样丢一两条也不会卡死（真机随机卡死的原因之一）。
         */
        const bool keepalive = (committed > 0) && ((uint32_t)(now - s_credit_ms) >= 400u);
        if (step || keepalive) {
            char buf[80];
            snprintf(buf, sizeof(buf), "{\"t\":\"pe\",\"ev\":\"credit\",\"off\":%u}",
                     (unsigned)committed);
            evt_send(buf);
            if (step) s_credit_off = committed;
            s_credit_ms = now;
        }
    }

    /* ④ 清单分页：一条 notify / 8 ms（128 张 ≈1.1 s，不挤占 credit） */
    if (s_list_pending && (uint32_t)(now - s_list_last_ms) >= 8) {
        s_list_last_ms = now;
        char buf[220];
        int n = 0;
        if (s_list_phase == 0) {
            n = photo_list_begin_json(buf, sizeof(buf), s_list_total);
            s_list_phase = 1;
        }
        else if (s_list_next < s_list_total) {
            n = photo_rt_manifest_item_json(s_list_next, buf, sizeof(buf));
            s_list_next++;
        }
        else {
            n = photo_list_end_json(buf, sizeof(buf));
            s_list_pending = false;
        }
        if (n > 0) {
            evt_send(buf);
        }
        else {
            /* 构造失败（理论上不会）→ 结束分页，避免 App 一直等 */
            s_list_pending = false;
            evt_err("io", -1);
        }
    }

    /* ⑤ 周期刷新 Read 特征值（App 只会在连接/事件后 Read，这里兜底） */
    if ((uint32_t)(now - s_last_state_ms) >= 1000u) {
        s_last_state_ms = now;
        state_refresh(false);
    }
    if ((uint32_t)(now - s_last_info_ms) >= 5000u) {
        s_last_info_ms = now;
        info_refresh();
    }
}
