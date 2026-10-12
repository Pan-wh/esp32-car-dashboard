/*
 * ble_telemetry.cpp —— ESP32 BLE Peripheral（GATT Server）+ 遥测接收
 *
 * 详见 ble_telemetry.h。依赖 Arduino BLE（Bluedroid），只在固件侧编译。
 */
#include "ble_telemetry.h"

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <string.h>

#include "telemetry.h"

#define DEVICE_NAME      "CarDashboard"
#define SERVICE_UUID        "7D2A0001-8F4A-4B7C-9D21-5E6A3B8C1000"
#define TELEMETRY_CHAR_UUID "7D2A0002-8F4A-4B7C-9D21-5E6A3B8C1000"
#define STATUS_CHAR_UUID    "7D2A0003-8F4A-4B7C-9D21-5E6A3B8C1000"

#define PROTOCOL_VERSION 1
#define ASM_BUF_MAX 512 /* 重组缓冲上限：JSON 包不得超过，超限丢弃 */

/* ---------------- 共享状态（互斥量保护）---------------- */

static SemaphoreHandle_t s_mtx = NULL;

static telemetry_msg_t s_last;
static bool s_new = false;
static uint32_t s_rx_ms = 0;
static bool s_have_rx = false; /* 是否收到过至少一条有效遥测（与 s_rx_ms==0 区分开） */

static volatile bool s_connected = false;

static uint32_t s_rx_bytes = 0;
static uint32_t s_ok = 0;
static uint32_t s_parse_err = 0;

static BLECharacteristic *s_status = NULL;

/* ---- JSON 重组（MTU 可能小于包长，一次消息会被拆成多个 write） ---- */
static char s_asm[ASM_BUF_MAX];
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

/* 收到一段字节：重组直到完整 JSON（以 '{' 开头、'}' 结尾），再解析入库 */
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

    /* 判完整：以 '{' 开头且以 '}' 结尾即可（本协议为扁平对象） */
    if (s_asm_len > 0 && s_asm[0] == '{' && s_asm[s_asm_len - 1] == '}') {
        telemetry_msg_t m;
        if (telemetry_parse(s_asm, s_asm_len, &m) && m.v == PROTOCOL_VERSION) {
            store_msg(&m);
            s_ok++;
        }
        else {
            s_parse_err++;
        }
        s_asm_len = 0;
    }
    /* 不以 '{' 开头：丢弃开头垃圾，尝试重新对齐（容错） */
    else if (s_asm_len == n && s_asm[0] != '{') {
        s_asm_len = 0;
    }
}

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
        BLEDevice::startAdvertising(); /* 自动恢复广播，支持重连 */
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

void ble_telemetry_set_status(const char *status)
{
    if (s_status != NULL && status != NULL) {
        s_status->setValue(status);
        s_status->notify();
    }
}