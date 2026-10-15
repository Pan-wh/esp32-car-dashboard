/*
 * ble_telemetry.cpp —— ESP32 BLE Peripheral（GATT Server）+ 遥测接收
 *
 * 详见 ble_telemetry.h。依赖 Arduino BLE（Bluedroid），只在固件侧编译。
 */
#include "ble_telemetry.h"

#include "control.h"

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <string.h>

#include "telemetry.h"
#include "nav_parse.h"
#include "nav_map.h"

#define DEVICE_NAME      "CarDashboard"
#define SERVICE_UUID        "7D2A0001-8F4A-4B7C-9D21-5E6A3B8C1000"
#define TELEMETRY_CHAR_UUID "7D2A0002-8F4A-4B7C-9D21-5E6A3B8C1000"
#define STATUS_CHAR_UUID    "7D2A0003-8F4A-4B7C-9D21-5E6A3B8C1000"
#define COMMAND_CHAR_UUID   "7D2A0004-8F4A-4B7C-9D21-5E6A3B8C1000" /* 下行控制（亮度/息屏/表盘）*/
/*
 * 表盘列表上报（上行，Read + Notify）。
 *
 * ⚠️ **为什么"通知"不能直接承载列表**：BLE 单次通知受 MTU 限制
 *    （iOS 实到约 185 字节），而 5 个表盘的 JSON 约 350 字节，会被截断。
 *    因此约定：**Read 才是取数据的正途**（系统会自动用 Read Blob 取回完整值），
 *    Notify 只当"发生变化"的**提示**用 —— App 收到通知后应重新 Read。
 *    这样与 MTU 大小无关，表盘增多也不会失效。
 */
#define FACES_CHAR_UUID     "7D2A0005-8F4A-4B7C-9D21-5E6A3B8C1000"

#define PROTOCOL_VERSION 1
#define ASM_BUF_MAX 512 /* 重组缓冲上限：JSON 包不得超过，超限丢弃 */

/* ---------------- 共享状态（互斥量保护）---------------- */

static SemaphoreHandle_t s_mtx = NULL;

static telemetry_msg_t s_last;
static bool s_new = false;
static uint32_t s_rx_ms = 0;
static bool s_have_rx = false; /* 是否收到过至少一条有效遥测（与 s_rx_ms==0 区分开） */

static volatile bool s_connected = false;

/*
 * 断连后的"重启广播"请求标志（见 onDisconnect 的说明）。
 * 不在 BLE 回调里直接 startAdvertising()：NimBLE 下断连回调执行时
 * ble_gap host 尚未切回可广播状态，同步立即重启会失败并卡死协商，
 * 之后手机扫描不到设备，只能断电重启。故只置标志，由主循环
 * 经 ble_telemetry_poll() 延迟调用（既避开竞态，也符合"回调不做
 * 硬件副作用"的架构约定）。
 */
static volatile bool s_restart_adv = false;

static uint32_t s_rx_bytes = 0;
static uint32_t s_ok = 0;
static uint32_t s_parse_err = 0;

static BLECharacteristic *s_status = NULL;
static BLECharacteristic *s_faces = NULL;
static portMUX_TYPE s_cmd_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t s_cmd_ok = 0, s_cmd_err = 0;

/* ---- JSON 重组（MTU 可能小于包长，一次消息会被拆成多个 write） ---- */
static char s_asm[ASM_BUF_MAX];

/* 导航消息暂存（回调只写，主循环消费后清除）*/
static nav_msg_t s_pending_nav;
static volatile bool s_have_nav = false;
static uint32_t s_nav_ok = 0;
static size_t s_asm_len = 0;

static void inline store_msg(const telemetry_msg_t *m)
{
    if (s_mtx && xSemaphoreTake(s_mtx, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_last = *m;
        s_rx_ms = millis();
        s_have_rx = true;
        s_new = true;
        xSemaphoreGive(s_mtx);
    }
}

/* 解析并入库一条完整消息（分流：导航 → 地图 → 遥测）*/
static void dispatch_message(const char *json, size_t len)
{
    {
        /*
         * 分流：同一特征承载两类消息，用 "t":"nav" 区分。
         * ⚠️ 必须**先判导航**再按遥测解析 —— 导航包没有 "v" 字段，
         *    走遥测分支必然解析失败并计入 s_parse_err（污染统计）。
         * 回调内只做"解析 + 存变量"，硬件副作用留给主循环（架构约定）。
         */
        if (nav_parse(json, len, &s_pending_nav)) {
            s_have_nav = true;
            s_nav_ok++;
            /*
             * ⚠️ 这里**不要**打印 s_asm 的内容：那是 BLE 回调共用的重组缓冲区，
             *    在回调里对共享缓冲区做 %s 打印会与下一次写入形成竞态
             *    （排查时短暂加过，日志尾部出现过错拼）。需要看内容请在
             *    主循环消费后打印 —— nav_poll 里已有等价日志。
             */
        }
        else if (nav_map_handle(json, len)) {
            /*
             * 地图窗口消息（mw/mr/mb/mp/me）：同样复用本特征，用 "t" 分流。
             * ⚠️ 顺序：**nav → 地图 → 遥测**。地图消息没有 "v" 字段，
             *    落到遥测分支会解析失败并污染 s_parse_err 统计。
             * 该函数内部只做"解析 + 存缓冲"，窗口切换发生在收到 me 时，
             * 都是纯内存操作，适合回调内完成（无硬件副作用）。
             */
        }
        else {
            telemetry_msg_t m;
            if (telemetry_parse(json, len, &m) && m.v == PROTOCOL_VERSION) {
                store_msg(&m);
                s_ok++;
            }
            else {
                s_parse_err++;
            }
        }
    }
}

/*
 * 收到一段字节：重组并**逐个**提取完整的顶层 JSON 对象。
 *
 * ⚠️ 为什么必须"逐个"而不是"整段就是一个对象"：
 *
 * 地图窗口是**一条要素一条消息**、由 App 连续快速下发（一次窗口约 40 条）。
 * BLE 栈会把背靠背的写入**合并**成一次 onWrite 回调，于是缓冲区里会出现
 * `{...}{...}{...}`。旧实现只判断"首字符是 '{'、末字符是 '}'"就整段解析 ——
 * 结果一条都解析不出来（或只解析出最后一段的碎片），
 * 且收不到 `me` 导致地图窗口被 3 秒超时丢弃。
 * 实测现象：`窗口：道路 0 条`、`丢弃` 持续增长。
 *
 * 现在按**括号配平**逐个切分（正确处理字符串内的括号与转义），
 * 切完把剩余未完成的部分前移，等下一包补齐。这样：
 *   - 一条消息被拆成多包 → 仍然正确（累积到配平为止）
 *   - 多條消息挤在一包 → 也正确（逐条分发）
 */
static void handle_chunk(const char *data, size_t n)
{
    if (n == 0) return;

    /* 溢出：丢弃并计数（单包超限属协议错误） */
    if (s_asm_len + n > ASM_BUF_MAX) {
        s_asm_len = 0;
        s_parse_err++;
        return;
    }

    memcpy(s_asm + s_asm_len, data, n);
    s_asm_len += n;

    size_t start = 0;
    while (start < s_asm_len) {
        /* 跳过非 '{' 的前导垃圾（容错：对齐到下一个对象起点）*/
        while (start < s_asm_len && s_asm[start] != '{') start++;
        if (start >= s_asm_len) break;

        /* 从 start 起找与之配平的 '}' */
        int depth = 0;
        bool in_str = false;
        size_t i = start;
        bool closed = false;
        for (; i < s_asm_len; i++) {
            const char c = s_asm[i];
            if (in_str) {
                if (c == '\\') { i++; continue; } /* 跳过被转义字符 */
                if (c == '"') in_str = false;
                continue;
            }
            if (c == '"') { in_str = true; continue; }
            if (c == '{') depth++;
            else if (c == '}') {
                depth--;
                if (depth == 0) { closed = true; break; }
            }
        }
        if (!closed) break; /* 还不完整：等下一包（start 之后的内容保留）*/

        dispatch_message(s_asm + start, i - start + 1);
        start = i + 1;
    }

    /* 丢弃已处理部分，保留未完成尾巴 */
    if (start > 0) {
        const size_t rest = s_asm_len - start;
        if (rest > 0) memmove(s_asm, s_asm + start, rest);
        s_asm_len = rest;
    }

    /* 尾巴里已经没有 '{'：说明是纯垃圾，直接清掉 */
    if (s_asm_len > 0 && s_asm[0] != '{') s_asm_len = 0;
}

/* ---------------- 下行控制命令 ---------------- */

static control_cmd_t s_pending_cmd = { CONTROL_NONE, 0 };
static bool s_have_cmd = false;

class CommandCallbacks : public BLECharacteristicCallbacks {
public:
    void onWrite(BLECharacteristic *c) override
    {
        String v = c->getValue();
        unsigned n = v.length();
        if (n == 0) return;

        control_cmd_t cmd;
        if (control_parse(v.c_str(), n, &cmd) && cmd.kind != CONTROL_NONE) {
            /*
             * 只在此处解析并暂存；硬件副作用（setBrightness / displayOff）
             * 必须回到主循环执行——BLE 回调运行在 Bluedroid 任务里，
             * 不能碰显示驱动（与遥测回调同一约定）。
             */
            portENTER_CRITICAL(&s_cmd_mux);
            s_pending_cmd = cmd;
            s_have_cmd = true;
            portEXIT_CRITICAL(&s_cmd_mux);
            s_cmd_ok++;
        }
        else {
            s_cmd_err++;
        }
    }
};

/* ---------------- BLE 回调 ---------------- */

class TelemetryCallbacks : public BLECharacteristicCallbacks {
public:
    void onWrite(BLECharacteristic *c) override
    {
        String v = c->getValue();
        unsigned n = v.length();
        if (n == 0) return;
        s_rx_bytes += n;
        handle_chunk(v.c_str(), n);
    }
};

class ServerCallbacks : public BLEServerCallbacks {
public:
    void onConnect(BLEServer *s) override
    {
        (void)s;
        s_connected = true;
    }
    void onDisconnect(BLEServer *s) override
    {
        (void)s;
        s_connected = false;
        s_asm_len = 0; /* 断连后清空半包 */
        /*
         * ⚠️ 不要在这里直接 BLEDevice::startAdvertising()。
         *
         * 本固件底层是 NimBLE（sdkconfig CONFIG_BT_NIMBLE_ENABLED）。
         * 断连回调运行时 ble_gap host 还没切回可广播状态，若在此同步
         * 立即重启广播，ble_gap_adv_start 会失败/让 host 卡在忙状态，
         * 之后广播再也恢复不了 —— 症状即"断联太久后手机扫描不到设备，
         * 只能断电重启"。故只置标志，交由主循环的 ble_telemetry_poll()
         * 延迟重启（断连事件 → 主循环之间通常已足够让 host 回到就绪）。
         */
        s_restart_adv = true;
    }
};

/* ---------------- 对外接口 ---------------- */

bool ble_telemetry_begin(void)
{
    if (s_mtx == NULL) {
        s_mtx = xSemaphoreCreateMutex();
        if (s_mtx == NULL) return false;
    }

    if (!BLEDevice::init(DEVICE_NAME)) {
        return false;
    }
    BLEDevice::setMTU(247); /* 请求更大 MTU；iOS 上限约 185，不足时由重组兜底 */

    BLEServer *server = BLEDevice::createServer();
    if (server == NULL) return false;
    server->setCallbacks(new ServerCallbacks());

    BLEService *svc = server->createService(SERVICE_UUID);
    if (svc == NULL) return false;

    BLECharacteristic *tele = svc->createCharacteristic(TELEMETRY_CHAR_UUID,
                                                        BLECharacteristic::PROPERTY_WRITE);
    if (tele == NULL) return false;
    tele->setCallbacks(new TelemetryCallbacks());

    s_status = svc->createCharacteristic(STATUS_CHAR_UUID,
                                         BLECharacteristic::PROPERTY_READ |
                                             BLECharacteristic::PROPERTY_NOTIFY);
    if (s_status != NULL) {
        s_status->addDescriptor(new BLE2902()); /* 通知需 CCCD */
        s_status->setValue("CarDashboard v0");  /* 出厂状态 */
    }

    BLECharacteristic *cmdc = svc->createCharacteristic(COMMAND_CHAR_UUID,
                                                        BLECharacteristic::PROPERTY_WRITE);
    if (cmdc == NULL) return false;
    cmdc->setCallbacks(new CommandCallbacks());

    s_faces = svc->createCharacteristic(FACES_CHAR_UUID,
                                        BLECharacteristic::PROPERTY_READ |
                                            BLECharacteristic::PROPERTY_NOTIFY);
    if (s_faces != NULL) {
        s_faces->addDescriptor(new BLE2902()); /* 通知需 CCCD */
        /* 初值留空；表盘注册完成后由 ui 侧调用 ble_telemetry_set_faces 填真实列表 */
        s_faces->setValue("");
    }

    svc->start();

    BLEAdvertising *adv = BLEDevice::getAdvertising();
    adv->addServiceUUID(SERVICE_UUID);
    adv->setScanResponse(true);
    adv->start();

    return true;
}

bool ble_telemetry_connected(void)
{
    return s_connected;
}

/*
 * 主循环周期调用（配合 ble_poll / ble_update）。
 * 若断连回调已请求重启广播，且 host 已就绪，则在此恢复广播。
 * 失败不致命：保留标志待下次循环重试（millis 自然流逝已拉开与断连的距离）。
 */
void ble_telemetry_poll(void)
{
    if (!s_restart_adv) return;
    if (s_connected) {
        s_restart_adv = false; /* 又连上了，无需重启广播 */
        return;
    }
    BLEDevice::startAdvertising();
    s_restart_adv = false;
}

bool ble_telemetry_fetch(telemetry_msg_t *out, uint32_t *rx_ms)
{
    bool got = false;
    if (s_mtx && xSemaphoreTake(s_mtx, pdMS_TO_TICKS(10)) == pdTRUE) {
        if (s_new) {
            if (out != NULL) *out = s_last;
            if (rx_ms != NULL) *rx_ms = s_rx_ms;
            got = true;
            s_new = false;
        }
        xSemaphoreGive(s_mtx);
    }
    return got;
}

uint32_t ble_telemetry_age_ms(void)
{
    uint32_t r = 0;
    bool have = false;

    if (s_mtx == NULL) return BLE_AGE_UNKNOWN;
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(10)) != pdTRUE) {
        return BLE_AGE_UNKNOWN; /* 取锁失败：不可当作"从未收到"，否则会误报超时 */
    }
    r = s_rx_ms;
    have = s_have_rx;
    xSemaphoreGive(s_mtx);

    if (!have) return UINT32_MAX; /* 确实从未收到过 */

    uint32_t now = millis();
    return (now >= r) ? (now - r) : (UINT32_MAX - r + now); /* 处理 49 天回绕 */
}

void ble_telemetry_stats(uint32_t *rx_bytes, uint32_t *ok, uint32_t *parse_err)
{
    if (rx_bytes != NULL) *rx_bytes = s_rx_bytes;
    if (ok != NULL) *ok = s_ok;
    if (parse_err != NULL) *parse_err = s_parse_err;
}

bool ble_telemetry_take_command(control_cmd_t *out)
{
    if (out == NULL) return false;

    bool got = false;
    portENTER_CRITICAL(&s_cmd_mux);
    if (s_have_cmd) {
        *out = s_pending_cmd;
        s_have_cmd = false;
        got = true;
    }
    portEXIT_CRITICAL(&s_cmd_mux);
    return got;
}

bool ble_telemetry_take_nav(nav_msg_t *out)
{
    if (!s_have_nav) return false;
    if (out != NULL) *out = s_pending_nav;
    s_have_nav = false; /* 取走即清，避免主循环反复应用同一条 */
    return true;
}

void ble_telemetry_nav_stats(uint32_t *ok)
{
    if (ok != NULL) *ok = s_nav_ok;
}

void ble_telemetry_command_stats(uint32_t *ok, uint32_t *err)
{
    if (ok) *ok = s_cmd_ok;
    if (err) *err = s_cmd_err;
}

void ble_telemetry_set_status(const char *status)
{
    if (s_status != NULL && status != NULL) {
        s_status->setValue(status);
        s_status->notify();
    }
}

void ble_telemetry_set_faces(const char *json)
{
    if (s_faces == NULL || json == NULL) return;

    /*
     * 用 (data, len) 形式设置，避免依赖 std::string 的隐式转换；
     * 长度以 strlen 计（JSON 里不会出现裸 NUL）。
     */
    s_faces->setValue((uint8_t *)json, strlen(json));

    /*
     * Notify 只作"变了"的提示（载荷可能因 MTU 被截断，App 不应依赖其内容，
     * 而应重新 Read 完整值 —— 见 FACES_CHAR_UUID 处的说明）。
     * 设备未连接 / 未订阅时 notify 无效，属正常情况。
     */
    s_faces->notify();
}