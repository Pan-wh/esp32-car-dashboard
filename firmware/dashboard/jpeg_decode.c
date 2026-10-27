/**
 * jpeg_decode.c —— 用 ESP32-S3 ROM 里的 TJpgDec（经 esp_jpeg）把 JPEG 解成 RGB565
 *
 * ## 为什么能"零安装"
 *
 * S3 的 ROM 自带 `jd_prepare/jd_decomp`，而 Arduino-ESP32 3.3.10 已经把
 * `libespressif__esp_jpeg.a` 和它的 include 目录放进默认编译/链接参数，
 * 所以 `#include "jpeg_decoder.h"` 即可用（Flash 增量 ≈0~2 KB）。
 * 设计依据见 `docs/photo-wireless-design.md` §6.1 / §9.4。
 *
 * ## 只用于静态照片
 *
 * 单帧 466×466 约 200~260 ms（真机待标定），远慢于 8bpp 索引解码（21 ms）。
 * 所以**只有** `.pgj` 静态照片走这条路；动图继续用 `.pgi`（`img_indexed`）。
 * 解码只在"切到这张照片"时发生一次，轮播间隔内完全无感。
 *
 * ## RGB565 字节序（⚠️ 真机彩色的唯一开关）
 *
 * esp_jpeg 的 `flags.swap_color_bytes` 决定输出缓冲里两个字节的先后：
 *   - `false` → 输出**原生 uint16 小端**（`LOBYTE, HIBYTE`），与
 *     `img_indexed_decode8()` 写出的 `(r>>3)<<11 | ...` 完全一致；
 *   - `true`  → 输出**大端字节序**（`HIBYTE, LOBYTE`）。
 *
 * 本项目的 `disp_flush_cb()`（dashboard.ino）按 `LV_COLOR_16_SWAP` 二选一：
 *   `SWAP=1` → `draw16bitBeRGBBitmap`（面板要大端）
 *   `SWAP=0` → `draw16bitRGBBitmap`（面板要小端，当前 lv_conf 就是 0）
 * 因此**默认值直接跟随 `LV_COLOR_16_SWAP`**，而不是写死 true/false —— 这样
 * "图片管线和 JPEG 管线"永远用同一种字节序，不会出现"索引图正常、JPEG 红蓝反"
 * 这种只在真机上才看得见的问题。
 *
 * 真机若偏色（红蓝互换/灰阶反相），串口敲不了命令时改这里一行，或调用
 * `jpeg_decode_set_swap(!默认值)` 即可，调用方无需改动。
 */
#include "jpeg_decode.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "jpeg_decoder.h"

/* 取 LVGL 的字节序约定（lv_conf.h 在默认 include 路径里）*/
#if defined(__has_include)
#  if __has_include("lv_conf.h")
#    include "lv_conf.h"
#  endif
#endif

#if defined(LV_COLOR_16_SWAP) && (LV_COLOR_16_SWAP != 0)
#  define JPEG_SWAP_DEFAULT true
#else
#  define JPEG_SWAP_DEFAULT false
#endif

static const char *TAG = "jpeg";

/** 字节序开关：默认跟随 LVGL（见文件头说明）*/
static bool s_swap = JPEG_SWAP_DEFAULT;
/** 最近一次成功解码耗时（ms）*/
static uint32_t s_last_ms = 0;

void jpeg_decode_set_swap(bool swap)
{
    s_swap = swap;
}

uint32_t jpeg_decode_last_ms(void)
{
    return s_last_ms;
}

/** 单调毫秒（不依赖 Arduino 的 millis；本文件也可能被非 Arduino 目标复用）*/
static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

bool jpeg_decode_rgb565(const uint8_t *jpg, size_t len, uint16_t *out, size_t out_px,
                        uint16_t *out_w, uint16_t *out_h)
{
    if (jpg == NULL || out == NULL || out_px == 0 || len < 4) return false;

    esp_jpeg_image_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    esp_jpeg_image_output_t info;
    memset(&info, 0, sizeof(info));

    cfg.indata = (uint8_t *)jpg; /* 库只读，不改写（JDEC 的 in 回调只 memcpy 出来）*/
    cfg.indata_size = (uint32_t)len;
    cfg.outbuf = (uint8_t *)out;
    /* outbuf_size 是**字节数**：out_px 个 RGB565 像素 */
    cfg.outbuf_size = (uint32_t)(out_px * sizeof(uint16_t));
    cfg.out_format = JPEG_IMAGE_FORMAT_RGB565;
    cfg.out_scale = JPEG_IMAGE_SCALE_0; /* 不缩放：466×466 原样解 */
    cfg.flags.swap_color_bytes = s_swap ? 1 : 0;
    /*
     * 不传 advanced.working_buffer → 库内部临时 malloc 3.1 KB
     * （`heap_caps_malloc(..., MALLOC_CAP_DEFAULT)`，即内部 RAM）。
     * 只在解码期间存在、函数返回即释放，所以不需要我们常驻持有；
     * 代价是每次 malloc/free 一次（微秒级，相对 200 ms 解码可忽略）。
     */

    const uint32_t t0 = now_ms();
    esp_err_t e = esp_jpeg_decode(&cfg, &info);
    const uint32_t dt = now_ms() - t0;

    if (e != ESP_OK) {
        ESP_LOGW(TAG, "解码失败 err=%d（%u B）", (int)e, (unsigned)len);
        return false;
    }
    /* 库内部已经检查过 outsize <= outbuf_size，这里再兜一层（防御未来版本行为变化）*/
    if ((size_t)info.width * (size_t)info.height > out_px) {
        ESP_LOGW(TAG, "尺寸超缓冲：%ux%u > %u px", (unsigned)info.width,
                 (unsigned)info.height, (unsigned)out_px);
        return false;
    }

    s_last_ms = dt;
    if (out_w != NULL) *out_w = info.width;
    if (out_h != NULL) *out_h = info.height;
    return true;
}

bool jpeg_decode_probe(const uint8_t *jpg, size_t len, uint16_t *w, uint16_t *h)
{
    if (jpg == NULL || len < 5) return false;

    esp_jpeg_image_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    esp_jpeg_image_output_t info;
    memset(&info, 0, sizeof(info));

    cfg.indata = (uint8_t *)jpg;
    cfg.indata_size = (uint32_t)len;
    cfg.out_format = JPEG_IMAGE_FORMAT_RGB565;
    cfg.out_scale = JPEG_IMAGE_SCALE_0;

    /*
     * `esp_jpeg_get_image_info()` 只扫段头找 SOF0（不解像素、不分配内存），
     * 所以可以放心在提交校验里用（见 photo_rt_upload_end）。
     */
    if (esp_jpeg_get_image_info(&cfg, &info) != ESP_OK) return false;
    if (info.width == 0 || info.height == 0) return false;

    if (w != NULL) *w = info.width;
    if (h != NULL) *h = info.height;
    return true;
}
