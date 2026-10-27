/**
 * photo_rt.cpp —— 照片运行时（固件专用）：SD 扫描、播放、解码、上传落盘、清单/状态上报
 *
 * 分工（见 `photo_rt.h` 的文件头）：
 *   - 共享纯逻辑：`pgj.c`（格式）、`photo_list.c`（清单）、`photo_cmd.c`（命令解析）、
 *     `photo_sync.c`（接收状态机）—— 都在 `simulator/ui/`，PC 可测。
 *   - **本模块**：把上面四块接到 SD / LVGL / BLE 命令上。
 *   - `ble_photo.cpp`：只做 GATT/JSON 编解码，业务全部 via `photo_rt.h`。
 *
 * ## 线程模型（⚠️ 改这里之前先读）
 *
 * | 函数 | 运行在 | 说明 |
 * |------|--------|------|
 * | `photo_rt_upload_chunk()` | **BLE host 任务（core 0）** | 只做 offset 校验 + memcpy 进 PSRAM 环形缓冲；**不写盘、不 malloc、不打印** |
 * | 其余全部 | 主循环（core 1） | 包括写盘（每拍 ≤4 KB）、提交校验、播放 |
 *
 * 共享状态 = `photo_sync_t s_sync` + 环形缓冲的 `head/tail` 两个单调计数，
 * 一律在 `s_up_mux` 临界区里读写（SPSC：单生产者 core0 / 单消费者 core1）。
 * 数据本身放在 PSRAM 环形缓冲，临界区里只做 memcpy(≤178 B) 和索引加减。
 *
 * ## 内存（内部 RAM 只多几 KB，PSRAM 约 1.4 MB）
 *
 * | 用途 | 大小 | 位置 | 分配时机 |
 * |------|------|------|---------|
 * | 显示双缓冲 | 2 × 434 KB | PSRAM | 第一次进照片表盘 |
 * | 暂存（.pgj 整文件 / .pgi 一帧） | ~512 KB | PSRAM | 同上 |
 * | 上传环形缓冲 | 32 KB | PSRAM | 第一次 begin |
 * | 清单 | 128 × 76 B ≈ 9.7 KB | **PSRAM** | 第一次扫描 |
 * | 状态机/计数器 | < 2 KB | 内部 RAM（静态） | 常驻 |
 *
 * 内部 RAM 是唯一的真墙（Wi-Fi 时代只剩 22 KB，见
 * `docs/photo-wireless-design.md` §3.1）—— 所以清单也放 PSRAM，而不是 `static`。
 */
#include <Arduino.h>
#include <Preferences.h>
#include <SD_MMC.h>
#include <driver/sdmmc_host.h>
#include <esp_heap_caps.h>
#include <esp_vfs_fat.h>
#include <sdmmc_cmd.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "photo_rt.h"

#include "img_indexed.h"
#include "jpeg_decode.h"
#include "pgj.h"
#include "pgi.h"
#include "photo_list.h"
#include "photo_slide.h"
#include "photo_sync.h"
#include "ui_pages.h"
#include "ui_photo.h"

/* ═══════════════════════════════════════════════════════════════════════
 * dashboard.ino 提供的桥（跨编译单元）
 *
 * 这三件事都在 .ino 里（面板/背光/SD 引脚定义也在那里），本模块不重复实现：
 *   - `dash_sd_mount()`      ：懒挂载 TF 卡（失败可重试；启动时没卡也能后插）
 *   - `dash_brightness_cap()`：照片表盘的亮度封顶（烧屏防护第 3 道）
 *   - `dash_screen_off()`    ：自动息屏（第 4 道）
 * ═══════════════════════════════════════════════════════════════════════ */
bool dash_sd_mount(void);
void dash_sd_reset(void);
void dash_brightness_cap(uint8_t v);
void dash_screen_off(void);

/* ---------------- 可调参数（每个数字都有出处，改前先看注释）---------------- */

/** 照片目录（与 docs/photo-protocol.md §9 一致）*/
#define PHOTO_DIR "/photos"
/** 上传临时目录（.part + .meta；不参与扫描，永远不会被显示）*/
#define PHOTO_INCOMING_DIR "/photos/.incoming"

/** 每拍最多读多少字节（沿用旧值：实测 32 KB 读约 14 ms，主循环不会卡）*/
#define PHOTO_READ_CHUNK (32u * 1024u)
/** 每拍最多写多少字节到 SD（设计 §9.2：每拍 +25 ms，上传期间可接受）*/
#define PHOTO_DRAIN_MAX (4u * 1024u)
/** 每写这么多字节 flush 一次（保证断连后 .part 大小可信，见设计 §8.4）*/
/*
 * 上传中**不要**周期性 flush SD：
 * 真机实测每 16 KB 一次 flush 会把 BLE 链路带停（flush 仅 15 ms，但之后
 * 写入/确认会随机停 10 s 以上，触发设备超时暂停）。改为：
 *   - 传输中只 write，不 flush；
 *   - 暂停/断连（photo_rt_upload_pause）和提交（upload_end 前）统一 flush。
 * 代价：掉电最多丢最后一段未 flush 数据，重连后按文件实际大小续传，
 * CRC 兜底校验（失败会整份重传）。对 512 KB 上限的静态图可接受。
 */
#define PHOTO_FLUSH_EVERY (4u * 1024u * 1024u) /* 仅兜底，实际不会在单张图内触发 */
/** 上传环形缓冲（PSRAM）。窗口 4096 B，这里留 8 倍余量吸收 BLE 突发 */
#define PHOTO_RING_CAP (32u * 1024u)
/** 单帧像素上限（与屏幕一致）*/
#define PHOTO_MAX_FRAME_PX (466u * 466u)
/** `.pgi` 单文件上限（历史素材；`.pgj` 的上限在 pgj.h 里）*/
#define PHOTO_PGI_FILE_MAX (8u * 1024u * 1024u)
/** 暂存区容量：`.pgj` 上限 = 32 B 头 + 512 KB 载荷；`.pgi` 一帧只要 218 KB，共用这块 */
#define PHOTO_STAGE_CAP (PGJ_HEADER_BYTES + PGJ_MAX_JPEG_BYTES)
/** 协议硬约束：所有 JSON ≤ 180 B（iOS 单次 Read/Notify 约 184 B）*/
#define PHOTO_JSON_MAX 180

/** 照片表盘下的亮度封顶（比正常暗一档，显著延缓 AMOLED 老化）*/
#define PHOTO_BRIGHTNESS_CAP 180
/** 像素位移相位推进周期：2 分钟换一档，四档循环 */
#define PHOTO_SHIFT_PERIOD_MS 120000u
/** 接收进度的 UI 刷新周期（5 Hz；再快是白烧 BLE/CPU）*/
#define PHOTO_PROGRESS_MS 200u
/** 清单为空（可能刚插卡/刚清空）时的重扫节流 */
#define PHOTO_RESCAN_MS 5000u
/** 实测动图帧率写 NVS 的最小间隔（NVS 写多了既磨损又会有 flash 停顿）*/
#define PHOTO_FPS_SAVE_MIN_MS 60000u
/** `.incoming` 残留清理阈值（设计 §8.4：超过 7 天的残留清掉）*/
#define PHOTO_PART_MAX_AGE_S (7u * 24u * 3600u)

/* NVS（命名空间与旧版一致，老用户设置无缝继承）*/
#define PHOTO_PREF_NS "photo"
#define PK_SLIDE "slide"
#define PK_SHIFT "shift"
#define PK_SOFF "soff"
#define PK_SOFFMS "soffms"
#define PK_INTV "intv"
#define PK_SEQ "seq"     /* 下一个设备分配序号 */
#define PK_FPS "fpsmeas" /* 实测动图帧率 */
#define PK_DEC "decms"   /* 最近一次 JPEG 解码耗时 */

/* ---------------- 状态 ---------------- */

/** 清单（PSRAM 懒分配；`photo_list_t` 128 项 ≈ 9.7 KB，内部 RAM 放不起）*/
static photo_list_t *s_list = NULL;
static bool s_scanned = false;      /* 是否至少扫过一次（false → 首次用时扫）*/
static uint32_t s_scan_ms = 0;      /* 上次扫描时刻（空清单时用来节流重扫）*/

/** 轮播状态机（纯逻辑，见 photo_slide.c）*/
static photo_slide_t s_slide;
/**
 * slide 的 64 项清单窗口在总清单里的起始下标。
 *
 * `photo_slide.h` 的 `PHOTO_SLIDE_MAX_FILES` 是 64，而清单最多 128 项。
 * 为了让"清单下标"和"播放下标"在任何时候都有一一对应关系（否则 play(i) 会
 * 播到别的图），这里用一个**滑动窗口**：默认从 0 开始，play 到 64 以后的
 * 项时把窗口平移过去（见 `slide_rebuild`）。
 */
static int s_view_off = 0;

/** 显示双缓冲（PSRAM；434 KB × 2）*/
static uint16_t *s_fb[2] = { NULL, NULL };
static int s_fb_show = 0;          /* 当前正在显示的那块 */
static uint8_t *s_stage = NULL;    /* 暂存：.pgj 整文件 / .pgi 一帧 */
static size_t s_stage_cap = 0;
static bool s_buf_ready = false;

/** 当前打开的文件与读取进度 */
static char s_cur_name[PHOTO_NAME_MAX];
static uint8_t s_cur_kind = PHOTO_KIND_PGJ;
static uint16_t s_cur_w = 0, s_cur_h = 0;
static pgi_header_t s_cur_pgi;      /* 仅 .pgi 有效 */
static File s_file;
static bool s_file_open = false;
static int s_load_frame = -1;       /* 正在读的帧号；-1 = 空闲 */
static size_t s_got = 0, s_need = 0;
static uint32_t s_frame_off = 0;

/** 当前已显示的内容（判断"帧没变就别重读重解"）*/
static int s_shown_file = -1;       /* 清单下标（含窗口偏移）*/
static int s_shown_frame = -1;

/** 页面生命周期 / 位移相位 */
static bool s_page_active = false;
static uint32_t s_shift_ms = 0;
static int s_shift_phase = 0;

/** 动图实测帧率窗口（播完一段动图结算一次，见 `fps_finalize`）*/
static int s_anim_file = -1;        /* 正在统计的清单下标；-1 = 没有 */
static uint32_t s_anim_t0 = 0;
static uint32_t s_anim_frames = 0;
static uint32_t s_fps_save_ms = 0;

/** NVS 缓存（避免每次上报都读 NVS，见设计 §7.2）*/
static uint32_t s_fps_meas = 0;
static uint32_t s_dec_ms_nvs = 0;

/**
 * 存储是否就绪的**缓存**。
 *
 * 为什么要缓存而不是每次都 `SD_MMC.begin()`：`photo_rt_state_json()` 等上报函数
 * 可能被 GATT Read 回调（BLE 任务）调用，那里**不允许**做挂载这种重 IO；
 * 而"没卡时每拍 begin 一次"也很贵。规则：
 *   - 主循环（init/轮询/命令/上传）负责 `storage_probe()` 刷新本缓存；
 *   - 上报路径只读缓存。
 */
static bool s_stor_ok = false;
static uint32_t s_stor_probe_ms = 0;

/** 上传会话（`s_sync` 与环形缓冲指针受 `s_up_mux` 保护）*/
static photo_sync_t s_sync;
static portMUX_TYPE s_up_mux = portMUX_INITIALIZER_UNLOCKED;
static uint8_t *s_ring = NULL;      /* PSRAM，懒分配 */
static volatile uint32_t s_ring_head = 0; /* 生产者（core0）写入的单调计数 */
static volatile uint32_t s_ring_tail = 0; /* 消费者（core1）读出的单调计数 */
static File s_part;                 /* .part 文件句柄（仅主循环用）*/
static bool s_part_open = false;
static char s_part_path[96];
static char s_meta_path[96];
static char s_up_name[PHOTO_NAME_MAX];
static uint32_t s_up_flush = 0;     /* 距上次 flush 的落盘字节 */
static uint32_t s_up_seq = 0;       /* 下一个设备分配序号（NVS 缓存）*/

static uint32_t s_up_seq_used = 0;  /* 本次 begin 实际占用的序号（提交后 +1 存 NVS）*/
static bool s_up_new_seq = false;   /* 本次 begin 是否占用了新序号 */
static uint32_t s_progress_ms = 0;
static bool s_progress_on = false;

/** 事件监听（ble_photo 注册；只在主循环里调用）*/
static photo_rt_listener_t s_listener;

/* ---------------- 小工具 ---------------- */

static void emit(const char *json)
{
    if (s_listener.on_event != NULL) s_listener.on_event(json);
}

static void emit_changed(void)
{
    emit("{\"t\":\"pe\",\"ev\":\"changed\"}");
}

static void emit_info(void)
{
    emit("{\"t\":\"pe\",\"ev\":\"info\"}");
}

/** 小端 32 位（.pgi 帧索引表项）*/
static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

/**
 * 从极简 JSON 里取一个整数字段（`.meta` 只有我们自己写的 7 个字段）。
 * 找不到返回 false。
 */
static bool json_int_field(const char *json, const char *key, uint32_t *out)
{
    char pat[24];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *p = strstr(json, pat);
    if (p == NULL) return false;
    p += strlen(pat);
    if (*p < '0' || *p > '9') return false;
    *out = (uint32_t)strtoul(p, NULL, 10);
    return true;
}

/**
 * 流式 CRC-32（标准 IEEE 802.3，与 `pgj_crc32()` 完全一致）。
 *
 * 为什么要第二份实现：`pgj_crc32()` 只接受"整块在内存里"的缓冲，而 `.pgi`
 * 上传可能到 8 MB —— 不可能为了算 CRC 一次读进 PSRAM。这里用同一张表逐块喂，
 * 结果与 `pgj_crc32()` 逐位一致（算法：反射 poly 0xEDB88320，初值与终值异或
 * 0xFFFFFFFF，即 zlib `crc32()`）。表在第一次使用时生成（1 KB 静态）。
 */
static uint32_t s_crc_tab[256];
static bool s_crc_tab_ready = false;

static void crc32_tab_init(void)
{
    if (s_crc_tab_ready) return;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) {
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        s_crc_tab[i] = c;
    }
    s_crc_tab_ready = true;
}

static uint32_t crc32_update(uint32_t crc, const uint8_t *buf, size_t len)
{
    crc32_tab_init();
    for (size_t i = 0; i < len; i++) {
        crc = s_crc_tab[(crc ^ buf[i]) & 0xFFu] ^ (crc >> 8);
    }
    return crc;
}

/* ---------------- NVS ---------------- */

static void prefs_save_settings(void)
{
    Preferences p;
    if (!p.begin(PHOTO_PREF_NS, false)) return;
    p.putBool(PK_SLIDE, s_slide.settings.slideshow);
    p.putBool(PK_SHIFT, s_slide.settings.shift);
    p.putBool(PK_SOFF, s_slide.settings.screen_off);
    p.putUInt(PK_SOFFMS, s_slide.settings.screen_off_ms);
    p.putUInt(PK_INTV, s_slide.settings.interval_ms);
    p.end();
}

static void prefs_save_meas(void)
{
    Preferences p;
    if (!p.begin(PHOTO_PREF_NS, false)) return;
    p.putUInt(PK_FPS, s_fps_meas);
    p.putUInt(PK_DEC, s_dec_ms_nvs);
    p.end();
}

static void prefs_save_seq(void)
{
    Preferences p;
    if (!p.begin(PHOTO_PREF_NS, false)) return;
    p.putUInt(PK_SEQ, s_up_seq);
    p.end();
}

static void prefs_load(void)
{
    photo_settings_t d = photo_settings_default();
    Preferences p;
    if (p.begin(PHOTO_PREF_NS, true)) {
        if (p.isKey(PK_SLIDE)) d.slideshow = p.getBool(PK_SLIDE, true);
        if (p.isKey(PK_SHIFT)) d.shift = p.getBool(PK_SHIFT, true);
        if (p.isKey(PK_SOFF)) d.screen_off = p.getBool(PK_SOFF, true);
        if (p.isKey(PK_SOFFMS)) d.screen_off_ms = p.getUInt(PK_SOFFMS, d.screen_off_ms);
        if (p.isKey(PK_INTV)) d.interval_ms = p.getUInt(PK_INTV, d.interval_ms);
        s_up_seq = p.getUInt(PK_SEQ, 0);
        s_fps_meas = p.getUInt(PK_FPS, 0);
        s_dec_ms_nvs = p.getUInt(PK_DEC, 0);
        p.end();
    }
    photo_settings_sanitize(&d);
    photo_slide_init(&s_slide, d);
}

/* ---------------- 存储 ---------------- */

/** 主循环专用：真去挂载（可重试）并刷新缓存 */
static bool storage_probe(void)
{
    s_stor_ok = dash_sd_mount();
    s_stor_probe_ms = millis();
    return s_stor_ok;
}

bool photo_rt_storage_ready(void)
{
    /* 上报路径可能调用 → 只读缓存（由主循环刷新，见 s_stor_ok 的说明）*/
    return s_stor_ok;
}

uint32_t photo_rt_free_bytes(void)
{
    if (!s_stor_ok) return 0;
    /* totalBytes/usedBytes 读的是挂载时缓存的容量信息，不做 IO */
    const uint64_t total = SD_MMC.totalBytes();
    const uint64_t used = SD_MMC.usedBytes();
    if (total <= used) return 0;
    const uint64_t freeb = total - used;
    return (freeb > 0xFFFFFFFFull) ? 0xFFFFFFFFu : (uint32_t)freeb;
}

/* ---------------- PSRAM 缓冲 ---------------- */

/** 分配显示双缓冲 + 暂存区（只做一次；失败则不启用照片表盘）*/
static bool ensure_buffers(void)
{
    if (s_buf_ready) return true;
    const size_t px = PHOTO_MAX_FRAME_PX;
    const size_t fb_bytes = px * sizeof(uint16_t);
    s_fb[0] = (uint16_t *)ps_malloc(fb_bytes);
    s_fb[1] = (uint16_t *)ps_malloc(fb_bytes);
    s_stage_cap = PHOTO_STAGE_CAP;
    s_stage = (uint8_t *)ps_malloc(s_stage_cap);
    if (s_fb[0] == NULL || s_fb[1] == NULL || s_stage == NULL) {
        Serial.printf("[照片] PSRAM 分配失败（需要约 %u KB，当前 PSRAM 空闲 %u KB）\n",
                      (unsigned)((fb_bytes * 2u + s_stage_cap) / 1024u),
                      (unsigned)(ESP.getFreePsram() / 1024u));
        free(s_fb[0]); s_fb[0] = NULL;
        free(s_fb[1]); s_fb[1] = NULL;
        free(s_stage); s_stage = NULL;
        s_stage_cap = 0;
        return false;
    }
    s_buf_ready = true;
    Serial.printf("[照片] 缓冲就绪：2×%u KB 显示 + %u KB 暂存（PSRAM 空闲 %u KB）\n",
                  (unsigned)(fb_bytes / 1024u), (unsigned)(s_stage_cap / 1024u),
                  (unsigned)(ESP.getFreePsram() / 1024u));
    return true;
}

/* ---------------- 清单扫描 ---------------- */

static void close_file(void);

/**
 * 扫描 `/photos/`，用 `photo_list_*` 建清单并排序。
 *
 * 关键点（设计 §6.2 / §9.5）：
 *   - **只读文件头**：`.pgj` 32 B、`.pgi` 32 B + 帧表（≤4 KB），不解码；
 *   - **坏文件不进清单**：头解析失败、或"文件大小与头不自洽"（被截断）直接跳过，
 *     从源头消灭历史 `.pgi` 那种 `LOAD ERROR`；
 *   - 排序 = 文件名升序（新文件是 `p%05u`，序号即顺序；历史素材也稳定）。
 */
static void scan_dir(void)
{
    const uint32_t now = millis();
    s_scan_ms = now;
    s_scanned = true;
    s_view_off = 0;

    if (s_list == NULL) {
        s_list = (photo_list_t *)ps_malloc(sizeof(photo_list_t));
        if (s_list == NULL) {
            Serial.println("[照片] 清单分配失败（PSRAM 不足）");
            return;
        }
    }
    photo_list_init(s_list);

    if (!storage_probe()) {
        Serial.println("[照片] 未检测到存储卡，清单为空");
        return;
    }

    SD_MMC.mkdir(PHOTO_DIR); /* 已存在返回 false，无妨 */

    File dir = SD_MMC.open(PHOTO_DIR);
    if (!dir || !dir.isDirectory()) {
        Serial.println("[照片] 打不开 /photos/");
        return;
    }

    /*
     * 读文件头的缓冲：
     *   .pgj 只要 32 B；
     *   .pgi 要 32 B 头 + 帧表（pgi_parse_header 会校验帧表也在范围内，
     *   所以不能只给 32 B —— 旧代码这里踩过坑，见 photo_scan_dir 注释）。
     */
    static uint8_t hdr[PGI_HEADER_BYTES + PGI_MAX_FRAMES * PGI_FRAME_ENTRY_BYTES];

    int bad = 0;
    int seen = 0;
    for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
        if (f.isDirectory()) continue;

        const char *nm = f.name();
        const char *base = strrchr(nm, '/');
        if (base != NULL) nm = base + 1;

        const size_t len = strlen(nm);
        const bool is_pgj = (len >= 4 && strcasecmp(nm + len - 4, ".pgj") == 0);
        const bool is_pgi = (len >= 4 && strcasecmp(nm + len - 4, ".pgi") == 0);
        if (!is_pgj && !is_pgi) continue;
        seen++;

        photo_item_t it;
        memset(&it, 0, sizeof(it));
        snprintf(it.name, sizeof(it.name), "%s", nm);
        if (!photo_name_valid(it.name)) { bad++; continue; }

        const size_t fsz = (size_t)f.size();
        it.bytes = (uint32_t)fsz;
        it.kind = is_pgj ? PHOTO_KIND_PGJ : PHOTO_KIND_PGI;

        f.seek(0);
        if (is_pgj) {
            int got = f.read(hdr, PGJ_HEADER_BYTES);
            pgj_header_t h;
            if (got != (int)PGJ_HEADER_BYTES || !pgj_parse_header(hdr, (size_t)got, &h)) {
                bad++;
                continue;
            }
            /* 大小必须与头自洽：32 + jpeg_bytes（截断文件在这里被挡掉）*/
            if ((uint32_t)fsz != PGJ_HEADER_BYTES + h.jpeg_bytes) { bad++; continue; }
            it.frames = 1;
            it.fps = 0;
            it.width = h.width;
            it.height = h.height;
        }
        else {
            int got = f.read(hdr, sizeof(hdr));
            pgi_header_t h;
            if (got < (int)PGI_HEADER_BYTES || !pgi_parse_header(hdr, (size_t)got, &h)) {
                bad++;
                continue;
            }
            const uint32_t frame_bytes = (uint32_t)PGI_PALETTE_BYTES +
                                         (uint32_t)h.width * (uint32_t)h.height;
            const uint32_t expect = h.data_off + frame_bytes * (uint32_t)h.frame_count;
            if ((uint32_t)fsz != expect) { bad++; continue; }
            it.frames = h.frame_count;
            it.fps = h.fps;
            it.width = h.width;
            it.height = h.height;
        }

        if (!photo_list_add(s_list, &it)) {
            /* 满（128）或同名/字段非法：满之后不再计数，避免刷屏 */
            if (s_list->count >= PHOTO_MAX_FILES) { bad++; continue; }
            bad++;
        }
    }

    photo_list_sort(s_list);
    Serial.printf("[照片] 扫描完成：%d 个可用，%d 个跳过（共看到 %d 个 .pgj/.pgi）\n",
                  s_list->count, bad, seen);
    if (s_list->count > PHOTO_SLIDE_MAX_FILES) {
        Serial.printf("[照片] ⚠️ 超过 %d 个文件：清单全列出，但轮播窗口只有 %d 项\n",
                      (int)PHOTO_SLIDE_MAX_FILES, (int)PHOTO_SLIDE_MAX_FILES);
    }
}

/** 首次使用时扫描（其余地方只管读 `s_list`）*/
static void ensure_scan(void)
{
    if (!s_scanned) scan_dir();
}

/* ---------------- slide 窗口 ---------------- */

/**
 * 按清单重建轮播状态机的文件窗口。
 *
 * @param focus 想立刻显示的**清单下标**；-1 = 不动当前内容
 *
 * `photo_slide.h` 的窗口是 64 项，清单可到 128 项 —— 所以窗口要能平移：
 * focus 落在窗口外时把窗口整体挪过去，保证 `manifest[focus]` 就是窗口的第 0 项。
 */
static void slide_rebuild(int focus)
{
    const int n = (s_list != NULL) ? s_list->count : 0;
    if (n <= 0) {
        photo_slide_set_files(&s_slide, 0, NULL);
        s_view_off = 0;
        return;
    }

    int off = 0;
    if (focus >= PHOTO_SLIDE_MAX_FILES) {
        /* 让 focus 尽量落在窗口末尾，这样往后翻仍有内容 */
        off = focus - (PHOTO_SLIDE_MAX_FILES - 1);
    }
    if (off > n - PHOTO_SLIDE_MAX_FILES) off = n - PHOTO_SLIDE_MAX_FILES;
    if (off < 0) off = 0;

    int cnt = n - off;
    if (cnt > PHOTO_SLIDE_MAX_FILES) cnt = PHOTO_SLIDE_MAX_FILES;

    photo_file_meta_t metas[PHOTO_SLIDE_MAX_FILES];
    for (int i = 0; i < cnt; i++) {
        metas[i].frame_count = s_list->items[off + i].frames;
        metas[i].fps = s_list->items[off + i].fps;
    }
    photo_slide_set_files(&s_slide, cnt, metas);
    s_view_off = off;

    if (focus >= off && focus < off + cnt) {
        const int k = focus - off;
        s_slide.cur_file = k;
        s_slide.cur_frame = 0;
        s_slide.loops_done = 0;
        s_slide.pinned_file = k;
        /* 从现在开始计时：这一张会立刻被加载并完整停留一个 interval */
        s_slide.seg_start_ms = millis();
    }
}

/** 当前正在显示的清单下标（-1 = 没有）*/
static int cur_manifest_index(void)
{
    if (s_list == NULL || s_list->count <= 0) return -1;
    if (s_slide.file_count <= 0) return -1;
    const int man = s_view_off + s_slide.cur_file;
    if (man < 0 || man >= s_list->count) return -1;
    return man;
}

/**
 * 清单变化后重建（上传提交/删除）：尽量停在原来那张。
 *
 * 重置 `s_shown_*` 是必须的 —— 下标含义变了，不重载就会把"新清单第 k 项"
 * 当成"旧清单第 k 项"继续显示。
 */
static void list_rebuild_keeping(const char *keep_name)
{
    int focus = 0;
    if (keep_name != NULL && keep_name[0] != '\0' && s_list != NULL) {
        const int f = photo_list_find(s_list, keep_name);
        if (f >= 0) focus = f;
    }
    slide_rebuild(focus);
    s_shown_file = -1;
    s_shown_frame = -1;
    s_load_frame = -1;
}

/* ---------------- 文件打开 / 帧读取 ---------------- */

static void close_file(void)
{
    if (s_file_open) {
        s_file.close();
        s_file_open = false;
    }
    s_load_frame = -1;
    s_cur_name[0] = '\0';
}

/**
 * 打开清单项对应的文件并校验头。
 *
 * 播放前**再校验一次头**（而不是只信扫描结果）：卡可能在扫描后被换掉/写坏，
 * 宁可显示 `LOAD ERROR` 也不要读越界。
 */
static bool open_file(const photo_item_t *it)
{
    close_file();

    char path[96];
    snprintf(path, sizeof(path), PHOTO_DIR "/%s", it->name);
    s_file = SD_MMC.open(path, FILE_READ);
    if (!s_file) {
        Serial.printf("[照片] 打不开 %s\n", it->name);
        return false;
    }

    if (it->kind == PHOTO_KIND_PGJ) {
        static uint8_t hdr[PGJ_HEADER_BYTES];
        s_file.seek(0);
        const int got = s_file.read(hdr, PGJ_HEADER_BYTES);
        pgj_header_t h;
        if (got != (int)PGJ_HEADER_BYTES || !pgj_parse_header(hdr, (size_t)got, &h)) {
            s_file.close();
            return false;
        }
        s_cur_w = h.width;
        s_cur_h = h.height;
    }
    else {
        static uint8_t hdr[PGI_HEADER_BYTES + PGI_MAX_FRAMES * PGI_FRAME_ENTRY_BYTES];
        s_file.seek(0);
        const int got = s_file.read(hdr, sizeof(hdr));
        if (got < (int)PGI_HEADER_BYTES || !pgi_parse_header(hdr, (size_t)got, &s_cur_pgi)) {
            s_file.close();
            return false;
        }
        s_cur_w = s_cur_pgi.width;
        s_cur_h = s_cur_pgi.height;
    }

    s_cur_kind = it->kind;
    snprintf(s_cur_name, sizeof(s_cur_name), "%s", it->name);
    s_file_open = true;
    s_load_frame = -1;
    return true;
}

/* ---------------- 动图实测帧率 ---------------- */

/**
 * 结算"刚播完的这一段动图"的真实帧率。
 *
 * 为什么必须实测：旧代码推算是 10 fps，实测只有 3~4 fps（读 SD + 解码 + flush
 * 三段耗时都被低估）。这个数字就是 `PhotoInfo.anim.fps_meas`，App 据此决定
 * 要不要给动图入口（设计 §7.1 的三层门控之二）。
 */
static void fps_finalize(uint32_t now)
{
    if (s_anim_file < 0) return;
    const uint32_t elapsed = now - s_anim_t0;
    const uint32_t frames = s_anim_frames;
    s_anim_file = -1;
    s_anim_frames = 0;
    s_anim_t0 = now;

    /* 太短的窗口不可信（比如刚进页面就切走）*/
    if (frames < 2 || elapsed < 1000u) return;

    uint32_t fps = (uint32_t)((uint64_t)frames * 1000u / elapsed);
    if (fps == 0) fps = 1;
    if (fps > 60u) fps = 60u; /* 异常数据保护 */
    if (fps == s_fps_meas) return;

    const bool first = (s_fps_meas == 0);
    s_fps_meas = fps;
    Serial.printf("[照片] 动图实测 %u fps（%u 帧 / %u ms）%s\n", (unsigned)fps,
                  (unsigned)frames, (unsigned)elapsed, first ? " ← 首次" : "");
    /* 写 NVS 节流：flash 写入会短暂停 cache，不能在每段动图后都写 */
    if (first || (uint32_t)(now - s_fps_save_ms) >= PHOTO_FPS_SAVE_MIN_MS ||
        s_fps_save_ms == 0) {
        s_fps_save_ms = now;
        prefs_save_meas();
    }
}

/* ---------------- 播放 ---------------- */

/** 主循环：照片表盘的轮播/读帧/解码/显示（只在页面可见时被调用）*/
static void playback_poll(uint32_t now)
{
    if (!ensure_buffers()) return;
    ensure_scan();

    if (s_list == NULL || s_list->count == 0) {
        /* 存储没挂上 → "未检测到存储卡"；挂上了但空的 → "还没有照片" */
        ui_photo_set_placeholder(s_stor_ok ? UI_PHOTO_PLACEHOLDER_EMPTY
                                          : UI_PHOTO_PLACEHOLDER_NO_SD);
        return;
    }

    /* 自动息屏（烧屏防护第 4 道）：独立于轮播，触摸会重置计时 */
    if (photo_slide_should_screen_off(&s_slide, now)) {
        dash_screen_off();
        return;
    }

    /* 推进轮播（到达切换时刻才真正换帧/换图）*/
    photo_slide_tick(&s_slide, now);

    photo_frame_t f;
    photo_slide_state(&s_slide, now, &f);
    if (f.file_index < 0) {
        ui_photo_set_placeholder(UI_PHOTO_PLACEHOLDER_EMPTY);
        return;
    }

    const int man = s_view_off + f.file_index;
    if (man < 0 || man >= s_list->count) {
        ui_photo_set_placeholder(UI_PHOTO_PLACEHOLDER_ERROR);
        return;
    }

    /* 位移相位：每 2 分钟换一档（只挪画面，进度条不动）*/
    if ((uint32_t)(now - s_shift_ms) > PHOTO_SHIFT_PERIOD_MS) {
        s_shift_ms = now;
        s_shift_phase = (s_shift_phase + 1) & 3;
    }
    ui_photo_set_shift(f.shift, s_shift_phase);

    /* 帧没变 → 什么都不用做（旧代码就是这里漏了，静态图也每秒重读重解一遍）*/
    if (s_shown_file == man && s_shown_frame == f.frame_index && ui_photo_has_frame()) {
        return;
    }

    /* 换图 → 结算上一段动图的 fps */
    if (s_anim_file >= 0 && s_anim_file != man) fps_finalize(now);

    const photo_item_t *it = &s_list->items[man];

    /* 换文件 → 重开（.pgj / .pgi 同一套打开逻辑，差异只在解码）*/
    if (!s_file_open || strcmp(it->name, s_cur_name) != 0 || s_cur_kind != it->kind) {
        if (!open_file(it)) {
            ui_photo_set_placeholder(UI_PHOTO_PLACEHOLDER_ERROR);
            return;
        }
    }

    /* 还没开始读这一帧：算文件内偏移与长度 */
    if (s_load_frame < 0) {
        uint32_t off = 0, need = 0;
        if (it->kind == PHOTO_KIND_PGJ) {
            /* 整文件读进来（含 32 B 头）；JPEG 必须整幅在内存，见 esp_jpeg 的说明 */
            off = 0;
            need = it->bytes;
            if (need < PGJ_HEADER_BYTES || need > s_stage_cap) {
                Serial.printf("[照片] %s 体积 %u 超出暂存区\n", it->name, (unsigned)need);
                ui_photo_set_placeholder(UI_PHOTO_PLACEHOLDER_ERROR);
                return;
            }
        }
        else {
            /* .pgi：直接从帧索引表取（表项 8 B：off u32 + size u32）*/
            uint8_t ent[PGI_FRAME_ENTRY_BYTES];
            const uint32_t ent_off = s_cur_pgi.frame_table_off +
                                     (uint32_t)f.frame_index * PGI_FRAME_ENTRY_BYTES;
            s_file.seek(ent_off);
            if (s_file.read(ent, sizeof(ent)) != (int)sizeof(ent)) {
                ui_photo_set_placeholder(UI_PHOTO_PLACEHOLDER_ERROR);
                return;
            }
            off = le32(ent);
            need = le32(ent + 4);
            const uint32_t expect = (uint32_t)PGI_PALETTE_BYTES +
                                    (uint32_t)s_cur_w * (uint32_t)s_cur_h;
            if (need != expect || need > s_stage_cap) {
                Serial.printf("[照片] 帧长度不符：%u != %u\n", (unsigned)need, (unsigned)expect);
                ui_photo_set_placeholder(UI_PHOTO_PLACEHOLDER_ERROR);
                return;
            }
        }
        s_frame_off = off;
        s_need = need;
        s_got = 0;
        s_load_frame = f.frame_index;
        s_file.seek(off);
    }

    /* 分片读（每拍 ≤32 KB，分摊到多个主循环，避免卡住渲染）*/
    if (s_got < s_need) {
        size_t chunk = s_need - s_got;
        if (chunk > PHOTO_READ_CHUNK) chunk = PHOTO_READ_CHUNK;
        const int r = s_file.read(s_stage + s_got, chunk);
        if (r <= 0) {
            Serial.printf("[照片] 读取失败（%s 帧 %d）\n", s_cur_name, s_load_frame);
            ui_photo_set_placeholder(UI_PHOTO_PLACEHOLDER_ERROR);
            s_load_frame = -1;
            return;
        }
        s_got += (size_t)r;
        if (s_got < s_need) return; /* 还没读完，下次继续 */
    }

    /* 读满 → 解码到**后台**缓冲，成功才交换（避免半张图抖出来）*/
    const int back = 1 - s_fb_show;
    bool decoded = false;
    int out_w = (int)s_cur_w, out_h = (int)s_cur_h;

    if (it->kind == PHOTO_KIND_PGJ) {
        uint16_t jw = 0, jh = 0;
        decoded = jpeg_decode_rgb565(s_stage + PGJ_HEADER_BYTES,
                                     s_need - PGJ_HEADER_BYTES, s_fb[back],
                                     PHOTO_MAX_FRAME_PX, &jw, &jh);
        if (decoded) {
            out_w = (int)jw;
            out_h = (int)jh;
            s_cur_w = jw;
            s_cur_h = jh;
            /* 顺手把解码耗时带给 PhotoInfo.bench.dec_ms（NVS 只在 bench 时写）*/
            s_dec_ms_nvs = jpeg_decode_last_ms();
        }
        else {
            Serial.printf("[照片] JPEG 解码失败：%s（%u B）\n", s_cur_name,
                          (unsigned)(s_need - PGJ_HEADER_BYTES));
        }
    }
    else {
        /* 暂存区布局就是 [调色板 1 KB][索引 w*h]，img_indexed 直接吃 */
        decoded = img_indexed_decode8(s_stage, (int)s_cur_w, (int)s_cur_h, s_fb[back],
                                      PHOTO_MAX_FRAME_PX);
    }

    if (!decoded) {
        ui_photo_set_placeholder(UI_PHOTO_PLACEHOLDER_ERROR);
        s_load_frame = -1;
        return;
    }

    s_fb_show = back;
    ui_photo_show_frame(s_fb[s_fb_show], out_w, out_h);
    s_shown_file = man;
    s_shown_frame = f.frame_index;
    s_load_frame = -1;

    /* 动图帧率统计：窗口 = 这张动图从第一帧到换图 */
    if (it->frames > 1) {
        if (s_anim_file != man) {
            s_anim_file = man;
            s_anim_t0 = now;
            s_anim_frames = 0;
        }
        s_anim_frames++;
    }
}

/* ---------------- 上传：会话管理（主循环侧） ---------------- */

/** 会话是否占着上传通道（WAIT_DATA / RECEIVING / COMMITTING），带临界区 */
static bool upload_busy(void)
{
    portENTER_CRITICAL(&s_up_mux);
    const bool busy = photo_sync_busy(&s_sync);
    portEXIT_CRITICAL(&s_up_mux);
    return busy;
}

static bool upload_paused(void)
{
    portENTER_CRITICAL(&s_up_mux);
    const bool paused = (s_sync.state == PHOTO_SYNC_PAUSED);
    portEXIT_CRITICAL(&s_up_mux);
    return paused;
}

/** 环形缓冲里未落盘的字节数（临界区）*/
static uint32_t ring_pending(void)
{
    portENTER_CRITICAL(&s_up_mux);
    const uint32_t n = s_ring_head - s_ring_tail;
    portEXIT_CRITICAL(&s_up_mux);
    return n;
}

/** 读 `.part` 的 meta（bytes/crc/kind），用于续传匹配 */
static bool meta_read(const char *path, uint8_t *kind, uint32_t *bytes, uint32_t *crc)
{
    File f = SD_MMC.open(path, FILE_READ);
    if (!f) return false;
    char buf[192];
    const int n = f.read((uint8_t *)buf, sizeof(buf) - 1);
    f.close();
    if (n <= 0) return false;
    buf[n] = '\0';

    uint32_t kb = 0, kb2 = 0, kb3 = 0;
    if (!json_int_field(buf, "bytes", &kb)) return false;
    if (!json_int_field(buf, "crc", &kb2)) return false;
    if (!json_int_field(buf, "kind", &kb3)) return false;
    if (kb3 > 1) return false;
    *bytes = kb;
    *crc = kb2;
    *kind = (uint8_t)kb3;
    return true;
}

static bool meta_write(const char *path, uint8_t kind, uint32_t bytes, uint32_t crc,
                       uint16_t w, uint16_t h, uint16_t frames, uint8_t fps)
{
    char buf[192];
    const int n = snprintf(buf, sizeof(buf),
                           "{\"bytes\":%u,\"crc\":%u,\"kind\":%u,\"w\":%u,\"h\":%u,"
                           "\"frames\":%u,\"fps\":%u}",
                           (unsigned)bytes, (unsigned)crc, (unsigned)kind, (unsigned)w,
                           (unsigned)h, (unsigned)frames, (unsigned)fps);
    if (n <= 0 || n >= (int)sizeof(buf)) return false;
    File f = SD_MMC.open(path, FILE_WRITE);
    if (!f) return false;
    const size_t wr = f.write((const uint8_t *)buf, (size_t)n);
    f.close();
    return wr == (size_t)n;
}

/**
 * 找一个可续传的 `.part`：meta 里 kind/bytes/crc 与本次 begin 完全一致才算。
 *
 * 不一致的（例如 App 重新编码过，crc 变了）**不能续传** —— 已收到的字节
 * 属于另一个文件，继续写就成了"拼接"出来的坏文件。
 */
static bool find_resume(uint8_t kind, uint32_t bytes, uint32_t crc, char *out_name,
                        size_t name_cap, uint32_t *out_off)
{
    File dir = SD_MMC.open(PHOTO_INCOMING_DIR);
    if (!dir || !dir.isDirectory()) return false;

    bool found = false;
    for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
        if (f.isDirectory()) continue;
        const char *nm = f.name();
        const char *base = strrchr(nm, '/');
        if (base != NULL) nm = base + 1;

        const size_t len = strlen(nm);
        if (len < 5 || strcasecmp(nm + len - 5, ".part") != 0) continue;

        char cand[PHOTO_NAME_MAX];
        if (len - 5 >= sizeof(cand)) continue;
        memcpy(cand, nm, len - 5);
        cand[len - 5] = '\0';
        if (!photo_name_valid(cand)) continue;

        char mp[128];
        snprintf(mp, sizeof(mp), PHOTO_INCOMING_DIR "/%s.meta", cand);
        uint8_t mk = 0;
        uint32_t mb = 0, mc = 0;
        if (!meta_read(mp, &mk, &mb, &mc)) continue;
        if (mk != kind || mb != bytes || mc != crc) continue;

        snprintf(out_name, name_cap, "%s", cand);
        *out_off = (uint32_t)f.size();
        Serial.printf("[照片] 发现可续传：%s（已落盘 %u / %u B）\n", cand,
                      (unsigned)*out_off, (unsigned)bytes);
        found = true;
        f.close();
        break;
    }
    dir.close();
    return found;
}

/** 分配一个新的设备文件名（`p%05u.<ext>`），跳过已存在的 */
static bool alloc_name(uint8_t kind, char *out, size_t cap, uint32_t *used_seq)
{
    for (uint32_t i = 0; i < (uint32_t)PHOTO_MAX_FILES + 1u; i++) {
        const uint32_t seq = s_up_seq + i;
        char cand[PHOTO_NAME_MAX];
        if (!photo_make_name(cand, sizeof(cand), seq, (photo_kind_t)kind)) return false;
        char path[96];
        snprintf(path, sizeof(path), PHOTO_DIR "/%s", cand);
        char ppath[112];
        snprintf(ppath, sizeof(ppath), PHOTO_INCOMING_DIR "/%s.part", cand);
        if (SD_MMC.exists(path) || SD_MMC.exists(ppath)) continue;
        snprintf(out, cap, "%s", cand);
        *used_seq = seq;
        return true;
    }
    return false;
}

/** 删掉本次上传的临时文件（失败路径统一走这里）*/
static void discard_part(void)
{
    if (s_part_open) {
        s_part.close();
        s_part_open = false;
    }
    if (s_part_path[0] != '\0') SD_MMC.remove(s_part_path);
    if (s_meta_path[0] != '\0') SD_MMC.remove(s_meta_path);
    s_part_path[0] = '\0';
    s_meta_path[0] = '\0';
    s_up_name[0] = '\0';
}

/** 上传失败：清临时文件 + 状态机转 ERROR + 进度条收起 */
static void upload_fail(const char *code)
{
    discard_part();
    portENTER_CRITICAL(&s_up_mux);
    photo_sync_commit_fail(&s_sync, code);
    s_ring_head = 0;
    s_ring_tail = 0;
    portEXIT_CRITICAL(&s_up_mux);
    if (s_progress_on) {
        ui_photo_set_progress(false, 0);
        s_progress_on = false;
    }
    Serial.printf("[照片] 上传失败：%s\n", code != NULL ? code : "?");
}

/** 每拍推进接收进度的 UI（5 Hz）*/
static void progress_update(uint32_t now, bool force)
{
    if (!upload_busy()) {
        if (s_progress_on) {
            ui_photo_set_progress(false, 0);
            s_progress_on = false;
        }
        return;
    }
    if (!force && (uint32_t)(now - s_progress_ms) < PHOTO_PROGRESS_MS) return;
    s_progress_ms = now;

    portENTER_CRITICAL(&s_up_mux);
    const uint32_t total = s_sync.total_bytes;
    const uint32_t recv = s_sync.received_bytes;
    portEXIT_CRITICAL(&s_up_mux);

    int pct = 0;
    if (total > 0) pct = (int)((uint64_t)recv * 100u / total);
    ui_photo_set_progress(true, pct);
    s_progress_on = true;
}

/* ---------------- 上传：JSON 上报 ---------------- */

static int json_finish(char *out, size_t cap, int n)
{
    if (n <= 0) return 0;
    if ((size_t)n >= cap || n > PHOTO_JSON_MAX) {
        /* 协议硬约束：>180 B 会被 iOS 侧截断成坏 JSON，宁可不发 */
        Serial.printf("[照片] ⚠️ JSON 超限（%d B，上限 %u B）已丢弃\n", n,
                      (unsigned)PHOTO_JSON_MAX);
        if (cap > 0) out[0] = '\0';
        return 0;
    }
    return n;
}

int photo_rt_info_json(char *out, size_t cap)
{
    if (out == NULL || cap == 0) return 0;
    /* ⚠️ 上报路径**不做 I/O**：它可能被 GATT Read 回调（BLE 任务）调用 */

    /*
     * capacity 位（协议 §2）：
     *   bit0 静态 JPEG  ✓（本模块实现）
     *   bit1 动图 .pgi  ✓（沿用索引解码）
     *   bit2 删除       ✓
     *   bit3 内置 FFat  ✗（v1 只有 SD）
     * → cap = 7
     */
    const uint32_t fps = photo_rt_playback_fps();
    const int tier = (fps >= 8u) ? 2 : 1;
    uint32_t dec = jpeg_decode_last_ms();
    if (dec == 0) dec = s_dec_ms_nvs;

    const int n = snprintf(out, cap,
                           "{\"t\":\"pi\",\"v\":1,\"tier\":%d,\"cap\":7,\"px\":%u,\"max_kb\":%u,"
                           "\"anim\":{\"px\":300,\"fps\":8,\"frames\":12,\"max_kb\":2048,"
                           "\"fps_meas\":%u},\"bench\":{\"dec_ms\":%u}}",
                           tier, (unsigned)PGJ_MAX_DIM,
                           (unsigned)(PGJ_MAX_JPEG_BYTES / 1024u), (unsigned)fps,
                           (unsigned)dec);
    return json_finish(out, cap, n);
}

int photo_rt_state_json(char *out, size_t cap)
{
    if (out == NULL || cap == 0) return 0;
    /* ⚠️ 同上：只用缓存，不碰 SD */

    const bool ready = s_stor_ok; /* 上报路径不做 IO */
    const int nn = (s_list != NULL) ? s_list->count : 0;
    const int cur = (nn > 0) ? cur_manifest_index() : -1;
    const uint32_t free_kb = photo_rt_free_bytes() / 1024u;

    int slide = 0, shift = 0;
    uint32_t intv = 0, soff_s = 0;
    photo_settings_t st = s_slide.settings;
    slide = st.slideshow ? 1 : 0;
    shift = st.shift ? 1 : 0;
    intv = st.interval_ms / 1000u;
    soff_s = st.screen_off ? (st.screen_off_ms / 1000u) : 0u;

    const int n = snprintf(out, cap,
                           "{\"t\":\"ps\",\"cur\":%d,\"n\":%d,\"stor\":%d,\"free_kb\":%u,"
                           "\"slide\":%d,\"intv\":%u,\"shift\":%d,\"soff_s\":%u,\"rx\":%d}",
                           cur, nn, ready ? 1 : 0, (unsigned)free_kb, slide,
                           (unsigned)intv, shift, (unsigned)soff_s,
                           photo_rt_is_uploading() ? 1 : 0);
    return json_finish(out, cap, n);
}

int photo_rt_count(void)
{
    /* ⚠️ 上报路径不做 I/O：清单由 init/轮询/命令维护，这里只读缓存 */
    return (s_list != NULL) ? s_list->count : 0;
}

int photo_rt_manifest_item_json(int index, char *out, size_t cap)
{
    if (out == NULL || cap == 0) return 0;
    if (s_list == NULL || index < 0 || index >= s_list->count) return 0;

    /* 每项用共享实现（与测试同一份代码），再统一卡 ≤180 B */
    const int n = photo_list_item_json(s_list, index, out, cap);
    return json_finish(out, cap, n);
}

/* ═══════════════════════════════════════════════════════════════════════
 * 上传：开始 / 收分片 / 写盘 / 提交
 * ═══════════════════════════════════════════════════════════════════════ */

photo_rt_begin_t photo_rt_upload_begin(uint8_t kind, uint32_t bytes, uint32_t crc,
                                       uint16_t w, uint16_t h, uint16_t frames, uint8_t fps,
                                       uint32_t *out_offset, char *out_name, size_t name_cap,
                                       const char **err)
{
    if (err != NULL) *err = NULL;
    if (out_offset != NULL) *out_offset = 0;
    if (out_name != NULL && name_cap > 0) out_name[0] = '\0';

    /* ── 参数与能力校验（业务判断在这里，语法判断在 photo_cmd.c）── */
    if (kind > 1) {
        if (err) *err = "bad_arg";
        return PHOTO_RT_BEGIN_BAD_ARG;
    }
    const uint32_t max_bytes = (kind == 0)
                                   ? (uint32_t)(PGJ_HEADER_BYTES + PGJ_MAX_JPEG_BYTES)
                                   : PHOTO_PGI_FILE_MAX;
    if (bytes == 0) {
        if (err) *err = "bad_arg";
        return PHOTO_RT_BEGIN_BAD_ARG;
    }
    if (bytes > max_bytes) {
        if (err) *err = "too_large";
        return PHOTO_RT_BEGIN_UNSUPPORTED;
    }
    if (kind == 0) {
        if (w == 0 || h == 0 || w > PGJ_MAX_DIM || h > PGJ_MAX_DIM) {
            if (err) *err = "bad_arg";
            return PHOTO_RT_BEGIN_BAD_ARG;
        }
        frames = 1; /* 静态照片恒为 1 帧 */
        fps = 0;
    }
    else {
        if (frames == 0 || frames > PGI_MAX_FRAMES || w == 0 || h == 0 ||
            w > PGI_MAX_DIMENSION || h > PGI_MAX_DIMENSION) {
            if (err) *err = "bad_arg";
            return PHOTO_RT_BEGIN_BAD_ARG;
        }
        if (frames > 1 && fps == 0) {
            if (err) *err = "bad_arg";
            return PHOTO_RT_BEGIN_BAD_ARG;
        }
    }

    /* ── 同一时刻只允许一个上传（设计 §9.3）── */
    if (upload_busy()) {
        if (err) *err = "busy";
        return PHOTO_RT_BEGIN_BUSY;
    }
    /* PAUSED 的旧会话在 begin 时直接作废（它的 .part 如果匹配会被续传逻辑捡起来）*/
    portENTER_CRITICAL(&s_up_mux);
    if (s_sync.state == PHOTO_SYNC_PAUSED || s_sync.state == PHOTO_SYNC_ERROR) {
        photo_sync_init(&s_sync);
    }
    s_ring_head = 0;
    s_ring_tail = 0;
    portEXIT_CRITICAL(&s_up_mux);

    if (!storage_probe()) {
        if (err) *err = "not_ready";
        return PHOTO_RT_BEGIN_IO;
    }
    SD_MMC.mkdir(PHOTO_DIR);
    SD_MMC.mkdir(PHOTO_INCOMING_DIR);

    /* ── 空间校验：还要留 1 MB 余量，避免把卡写满（PHOTO_SYNC_SPACE_RESERVE）── */
    const uint32_t free_b = photo_rt_free_bytes();
    /* photo_sync 内部要求 free_bytes **严格大于** bytes + 1 MB，这里同步口径 */
    if (free_b <= bytes + PHOTO_SYNC_SPACE_RESERVE) {
        if (err) *err = "no_space";
        Serial.printf("[照片] 空间不足：可用 %u KB，需要 %u KB\n", (unsigned)(free_b / 1024u),
                      (unsigned)((bytes + PHOTO_SYNC_SPACE_RESERVE) / 1024u));
        return PHOTO_RT_BEGIN_NO_SPACE;
    }

    /* ── 续传 or 新文件 ── */
    char name[PHOTO_NAME_MAX] = { 0 };
    uint32_t resume_off = 0;
    bool resume = find_resume(kind, bytes, crc, name, sizeof(name), &resume_off);
    uint32_t used_seq = 0;
    s_up_new_seq = false;

    if (resume) {
        if (resume_off >= bytes) {
            /* .part 已经收齐但没提交（例如上次 end 前掉电）→ 直接当成"待提交" */
            resume_off = bytes;
        }
    }
    else {
        if (!alloc_name(kind, name, sizeof(name), &used_seq)) {
            if (err) *err = "io";
            return PHOTO_RT_BEGIN_IO;
        }
        s_up_seq_used = used_seq;
        s_up_new_seq = true;
    }

    /* ── 打开 .part（续传必须追加写，不能用 FILE_WRITE —— 那会清空已收字节）── */
    snprintf(s_part_path, sizeof(s_part_path), PHOTO_INCOMING_DIR "/%s.part", name);
    snprintf(s_meta_path, sizeof(s_meta_path), PHOTO_INCOMING_DIR "/%s.meta", name);
    s_part = SD_MMC.open(s_part_path, resume ? FILE_APPEND : FILE_WRITE);
    if (!s_part) {
        discard_part();
        if (err) *err = "io";
        return PHOTO_RT_BEGIN_IO;
    }
    s_part_open = true;
    s_up_flush = 0;
    snprintf(s_up_name, sizeof(s_up_name), "%s", name);

    /* meta 先写：断电后重连 begin 靠它判断"这份 .part 是哪个文件" */
    if (!meta_write(s_meta_path, kind, bytes, crc, w, h, frames, fps)) {
        discard_part();
        if (err) *err = "io";
        return PHOTO_RT_BEGIN_IO;
    }

    /* 环形缓冲懒分配（只有用得上照片上传的人才占这 32 KB PSRAM）*/
    if (s_ring == NULL) {
        s_ring = (uint8_t *)ps_malloc(PHOTO_RING_CAP);
        if (s_ring == NULL) {
            discard_part();
            if (err) *err = "io";
            return PHOTO_RT_BEGIN_IO;
        }
    }

    portENTER_CRITICAL(&s_up_mux);
    const photo_sync_result_t r = photo_sync_begin(
        &s_sync, kind, bytes, crc, w, h, frames, fps, free_b, resume_off, name, millis());
    portEXIT_CRITICAL(&s_up_mux);

    if (r != PHOTO_SYNC_OK) {
        discard_part();
        /*
         * `photo_sync_begin()` 的枚举比协议 code 粗（没有 TOO_LARGE 项），
         * 具体原因放在 `s_sync.err` 里（例如超上限时是 "too_large"）。
         * 优先用它，App 才能显示"文件太大"而不是笼统的 bad_arg。
         */
        /* 错误码要活到调用方用完 → 放静态缓冲（主循环单线程，不会互相踩）*/
        static char s_begin_err[16];
        s_begin_err[0] = '\0';
        portENTER_CRITICAL(&s_up_mux);
        snprintf(s_begin_err, sizeof(s_begin_err), "%s", s_sync.err);
        portEXIT_CRITICAL(&s_up_mux);

        const char *fallback = "io";
        photo_rt_begin_t rc = PHOTO_RT_BEGIN_IO;
        switch (r) {
            case PHOTO_SYNC_BUSY:        fallback = "busy";        rc = PHOTO_RT_BEGIN_BUSY; break;
            case PHOTO_SYNC_NO_SPACE:    fallback = "no_space";    rc = PHOTO_RT_BEGIN_NO_SPACE; break;
            case PHOTO_SYNC_UNSUPPORTED: fallback = "unsupported"; rc = PHOTO_RT_BEGIN_UNSUPPORTED; break;
            case PHOTO_SYNC_BAD_ARG:     fallback = "bad_arg";     rc = PHOTO_RT_BEGIN_BAD_ARG; break;
            default:                     fallback = "io";          rc = PHOTO_RT_BEGIN_IO; break;
        }
        if (err) *err = (s_begin_err[0] != '\0') ? s_begin_err : fallback;
        return rc;
    }

    if (out_offset != NULL) *out_offset = resume_off;
    if (out_name != NULL && name_cap > 0) snprintf(out_name, name_cap, "%s", name);

    /*
     * 上传期间暂停照片读帧：这里把正在播的那张"钉住"，
     * 并结算动图 fps（否则统计窗口会跨过整个上传，数字失真）。
     */
    fps_finalize(millis());
    progress_update(millis(), true);

    Serial.printf("[照片] 开始接收 %s：%u B，kind=%u %ux%u %u 帧%s\n", name,
                  (unsigned)bytes, (unsigned)kind, (unsigned)w, (unsigned)h,
                  (unsigned)frames, resume ? "（续传）" : "");
    return resume ? PHOTO_RT_BEGIN_RESUME : PHOTO_RT_BEGIN_OK;
}

bool photo_rt_upload_chunk(uint32_t offset, const uint8_t *data, uint32_t len,
                           uint32_t *want, const char **err)
{
    /*
     * ⚠️ **本函数运行在 BLE host 任务（core 0）**，只允许：
     *   临界区里校验 + memcpy 进环形缓冲。
     * 绝对不能：malloc / 写 SD / Serial.printf（会拖死 BLE 栈）。
     */
    if (data == NULL || len == 0) {
        if (want != NULL) *want = 0;
        if (err != NULL) *err = "bad_arg";
        return false;
    }

    bool ok = false;
    portENTER_CRITICAL(&s_up_mux);

    if (!photo_sync_active(&s_sync)) {
        /* COMMITTING（end 已开始）→ busy；其它（IDLE/PAUSED/ERROR）→ offset，让 App 重 begin */
        if (want != NULL) *want = s_sync.received_bytes;
        if (err != NULL) {
            *err = (s_sync.state == PHOTO_SYNC_COMMITTING) ? "busy" : "offset";
        }
        portEXIT_CRITICAL(&s_up_mux);
        return false;
    }

    /* 环形缓冲余量：窗口 4 KB + 缓冲 32 KB，正常永远不会撞上；撞上就是 App 没守流控 */
    const uint32_t used = s_ring_head - s_ring_tail;
    if (s_ring == NULL || used + len > PHOTO_RING_CAP) {
        if (want != NULL) *want = s_sync.received_bytes;
        if (err != NULL) *err = "busy";
        portEXIT_CRITICAL(&s_up_mux);
        return false;
    }

    uint32_t want_off = 0;
    if (!photo_sync_accept_chunk(&s_sync, offset, len, &want_off, err, millis())) {
        if (want != NULL) *want = want_off;
        portEXIT_CRITICAL(&s_up_mux);
        return false;
    }

    /* 写进环形缓冲（可能跨界，拆两段）
     * ⚠️ 写指针是 **head**、读指针是 tail。曾误用 tail 当写位置：
     *    环形里有未读数据时新数据会覆盖"最老但尚未读走"的字节 → 文件内容错乱
     *    （表现：长度对、CRC 对不上，2026-10-27 真机抓到）。*/
    const uint32_t pos = s_ring_head % PHOTO_RING_CAP;
    uint32_t first = PHOTO_RING_CAP - pos;
    if (first > len) first = len;
    memcpy(s_ring + pos, data, first);
    if (len > first) memcpy(s_ring, data + first, len - first);
    s_ring_head += len;
    ok = true;

    portEXIT_CRITICAL(&s_up_mux);
    return ok;
}

uint32_t photo_rt_upload_drain(void)
{
    if (!s_part_open) return 0;

    /* 从环形缓冲取一块（临界区里只做 memcpy + 索引）*/
    static uint8_t io[PHOTO_DRAIN_MAX];
    uint32_t n = 0;
    portENTER_CRITICAL(&s_up_mux);
    const uint32_t avail = s_ring_head - s_ring_tail;
    if (avail > 0) {
        n = (avail > PHOTO_DRAIN_MAX) ? PHOTO_DRAIN_MAX : avail;
        const uint32_t pos = s_ring_tail % PHOTO_RING_CAP;
        uint32_t first = PHOTO_RING_CAP - pos;
        if (first > n) first = n;
        memcpy(io, s_ring + pos, first);
        if (n > first) memcpy(io + first, s_ring, n - first);
        s_ring_tail += n;
    }
    portEXIT_CRITICAL(&s_up_mux);

    if (n == 0) return 0;

    const size_t wr = s_part.write(io, n);
    if (wr != n) {
        upload_fail("io"); /* 写盘失败：删 .part，别留下半截文件 */
        return 0;
    }

    /* 每 16 KB flush 一次：保证断连后续传偏移可信（设计 §8.4）*/
    s_up_flush += (uint32_t)wr;
    if (s_up_flush >= PHOTO_FLUSH_EVERY) {
        s_part.flush();
        s_up_flush = 0;
    }

    portENTER_CRITICAL(&s_up_mux);
    photo_sync_on_committed(&s_sync, s_sync.committed_bytes + (uint32_t)wr);
    portEXIT_CRITICAL(&s_up_mux);
    return (uint32_t)wr;
}

/**
 * 校验 `.part`：整文件 CRC32 + 格式头/尺寸。
 *
 * @param out 成功时填好清单项（name 由调用方补）
 */
static bool verify_part(const char **err, photo_item_t *out)
{
    if (err != NULL) *err = "io";

    portENTER_CRITICAL(&s_up_mux);
    const uint32_t total = s_sync.total_bytes;
    const uint32_t crc_expect = s_sync.crc_expect;
    const uint8_t kind = s_sync.kind;
    portEXIT_CRITICAL(&s_up_mux);

    File f = SD_MMC.open(s_part_path, FILE_READ);
    if (!f) return false;
    const size_t fsz = (size_t)f.size();
    if ((uint32_t)fsz != total) {
        f.close();
        if (err) *err = "bad_arg";
        return false;
    }

    uint32_t crc = 0xFFFFFFFFu;
    memset(out, 0, sizeof(*out));
    out->kind = kind;
    out->bytes = (uint32_t)fsz;

    if (kind == PHOTO_KIND_PGJ) {
        /* .pgj ≤ 512 KB：一次读进暂存区，CRC 与头校验都用这块（省一次读盘）*/
        if (!ensure_buffers() || fsz > s_stage_cap) {
            f.close();
            if (err) *err = "no_space";
            return false;
        }
        size_t got = 0;
        while (got < fsz) {
            const int r = f.read(s_stage + got, fsz - got);
            if (r <= 0) break;
            got += (size_t)r;
        }
        f.close();
        if (got != fsz) return false;

        /*
         * ⚠️ **CRC 口径**（与 iOS `makePGJ()` 逐字节对齐）：
         * `begin.crc == header.crc32 == crc32(header 把 [16..19] 置 0 ‖ payload)`。
         *
         * 也就是说算 CRC 时要把头里的 `crc32` 字段**当 0** —— 否则"自指"了：
         * 写进头里的值会参与自己的计算，两边永远对不上（实测会让每一张
         * `.pgj` 上传都报 `crc`）。暂存区可写，所以保存→清零→算→恢复。
         */
        uint8_t crc_field[4];
        memcpy(crc_field, s_stage + 16, sizeof(crc_field));
        memset(s_stage + 16, 0, sizeof(crc_field));
        crc = pgj_crc32(s_stage, fsz); /* pgj_crc32() 返回值就是标准 CRC32（含初值/终值）*/
        memcpy(s_stage + 16, crc_field, sizeof(crc_field));

        if (crc != crc_expect) {
            if (err) *err = "crc";
            Serial.printf("[照片] CRC 不符：收到 %08x，声明 %08x\n", (unsigned)crc,
                          (unsigned)crc_expect);
            return false;
        }

        pgj_header_t h;
        const char *e2 = NULL;
        if (!pgj_validate_file(s_stage, fsz, &h, &e2)) {
            if (err) *err = (e2 != NULL) ? e2 : "unsupported";
            Serial.printf("[照片] .pgj 校验失败：%s\n", e2 != NULL ? e2 : "?");
            return false;
        }
        /* 头里的 crc32 也应等于实测值（协议 §7）；不相等只警告，以 begin 声明为准 */
        if (h.crc32 != crc) {
            Serial.printf("[照片] ⚠️ 头内 crc32 %08x 与实测 %08x 不一致\n",
                          (unsigned)h.crc32, (unsigned)crc);
        }
        out->frames = 1;
        out->fps = 0;
        out->width = h.width;
        out->height = h.height;
    }
    else {
        /* .pgi 可能到 8 MB：流式算 CRC，再单独读一次头 */
        static uint8_t buf[4096];
        size_t got = 0;
        while (got < fsz) {
            size_t want = fsz - got;
            if (want > sizeof(buf)) want = sizeof(buf);
            const int r = f.read(buf, want);
            if (r <= 0) break;
            crc = crc32_update(crc, buf, (size_t)r);
            got += (size_t)r;
        }
        if (got != fsz) {
            f.close();
            return false;
        }
        crc ^= 0xFFFFFFFFu;
        if (crc != crc_expect) {
            f.close();
            if (err) *err = "crc";
            Serial.printf("[照片] CRC 不符：收到 %08x，声明 %08x\n", (unsigned)crc,
                          (unsigned)crc_expect);
            return false;
        }

        static uint8_t hdr[PGI_HEADER_BYTES + PGI_MAX_FRAMES * PGI_FRAME_ENTRY_BYTES];
        f.seek(0);
        const int hg = f.read(hdr, sizeof(hdr));
        f.close();
        pgi_header_t h;
        if (hg < (int)PGI_HEADER_BYTES || !pgi_parse_header(hdr, (size_t)hg, &h)) {
            if (err) *err = "unsupported";
            return false;
        }
        const uint32_t frame_bytes = (uint32_t)PGI_PALETTE_BYTES +
                                     (uint32_t)h.width * (uint32_t)h.height;
        if ((uint32_t)fsz != h.data_off + frame_bytes * (uint32_t)h.frame_count) {
            if (err) *err = "bad_arg";
            return false;
        }
        out->frames = h.frame_count;
        out->fps = h.fps;
        out->width = h.width;
        out->height = h.height;
    }

    return true;
}

/**
 * 收到 end：排空缓冲 → 关文件 → 读回算 CRC32 + 校验格式头/尺寸 → rename → 插入清单。
 *
 * 失败一律删 `.part`（**绝不进清单**，设计 §8.4）；成功才推 `changed`。
 */
bool photo_rt_upload_end(bool *committed, const char **err)
{
    if (committed != NULL) *committed = false;
    if (err != NULL) *err = "bad_arg";

    if (!s_part_open) return false;

    /* 1) 先把 PSRAM 环形缓冲排空（App 发完时最后几片可能还在路上）*/
    for (int guard = 0; guard < 16 && ring_pending() > 0; guard++) {
        if (photo_rt_upload_drain() == 0) break;
    }
    if (ring_pending() > 0) {
        if (err) *err = "io";
        upload_fail("io");
        return false;
    }

    /*
     * 2) "end 先于最后几片到达" 的时序保护。
     *
     * WRITE_NR（Data）与 WRITE（Ctl）是**两条通路**，极端时序下 Ctl 的 end 可能
     * 先被主循环取到。这时**绝不能**把 .part 删掉 —— 那等于一次抖动丢整份文件。
     * 做法：保留会话与 .part，回 `err="offset"`；调用方（ble_photo）用
     * `photo_rt_upload_received()` 当 `want` 告诉 App "从这儿续发"，App 继续发
     * 最后几片后会再发一次 end，这次就能提交。
     */
    portENTER_CRITICAL(&s_up_mux);
    const uint32_t recv = s_sync.received_bytes;
    const uint32_t total = s_sync.total_bytes;
    portEXIT_CRITICAL(&s_up_mux);
    if (recv < total) {
        if (err != NULL) *err = "offset";
        Serial.printf("[照片] end 早于数据（%u / %u B）→ 保留 .part，等 App 续发\n",
                      (unsigned)recv, (unsigned)total);
        return false;
    }

    /* 3) 状态机转 COMMITTING（此时 received == total，正常不会失败）*/
    portENTER_CRITICAL(&s_up_mux);
    const bool req = photo_sync_request_end(&s_sync, err);
    portEXIT_CRITICAL(&s_up_mux);
    if (!req) {
        /* 真正失败只回 bad_arg，**不删** .part（还能重试 end）*/
        if (err != NULL && *err == NULL) *err = "bad_arg";
        return false;
    }

    s_part.flush();
    s_part.close();
    s_part_open = false;

    /* 4) 读回全文件校验（CRC + 头），失败**才**删 .part —— 绝不进清单 */
    photo_item_t it;
    const char *e2 = NULL;
    if (!verify_part(&e2, &it)) {
        if (err != NULL) *err = (e2 != NULL) ? e2 : "io";
        upload_fail((e2 != NULL) ? e2 : "io");
        return false;
    }
    snprintf(it.name, sizeof(it.name), "%s", s_up_name);

    /* 5) 原子提交：rename 到 /photos/<name>，删 meta */
    char final_path[96];
    snprintf(final_path, sizeof(final_path), PHOTO_DIR "/%s", s_up_name);
    if (SD_MMC.exists(final_path)) SD_MMC.remove(final_path); /* 极端残留（同名）*/
    if (!SD_MMC.rename(s_part_path, final_path)) {
        if (err != NULL) *err = "io";
        upload_fail("io");
        return false;
    }
    SD_MMC.remove(s_meta_path);
    s_part_path[0] = '\0';
    s_meta_path[0] = '\0';

    /* 6) 序号持久化（下次分配不会重名）*/
    if (s_up_new_seq) {
        /* 用**实际占用**的序号 +1（中途可能跳过已被占用的名字），而不是盲目 ++ */
        s_up_seq = s_up_seq_used + 1u;
        prefs_save_seq();
        s_up_new_seq = false;
    }

    /* 7) 更新清单：优先自己插入（不必重扫整目录），然后重建滑窗 */
    if (s_list == NULL) {
        s_list = (photo_list_t *)ps_malloc(sizeof(photo_list_t));
        if (s_list != NULL) photo_list_init(s_list);
    }
    if (s_list != NULL) {
        if (!photo_list_add(s_list, &it)) {
            /* 插不进去（满了/同名）→ 老老实实重扫一遍 */
            scan_dir();
        }
        else {
            photo_list_sort(s_list);
        }
        /*
         * 重建滑窗时尽量停在**正在看的那张**（上传不该把画面甩回第一张）；
         * 那张要是不存在（第一次上传）就退回清单第 0 项。
         */
        {
            char keep[PHOTO_NAME_MAX];
            snprintf(keep, sizeof(keep), "%s", s_cur_name);
            list_rebuild_keeping(keep);
        }
    }

    portENTER_CRITICAL(&s_up_mux);
    photo_sync_commit_ok(&s_sync);
    s_ring_head = 0;
    s_ring_tail = 0;
    portEXIT_CRITICAL(&s_up_mux);

    if (s_progress_on) {
        ui_photo_set_progress(false, 0);
        s_progress_on = false;
    }

    Serial.printf("[照片] ✓ 已提交 %s（%u B，%u 帧，%ux%u）\n", it.name, (unsigned)it.bytes,
                  (unsigned)it.frames, (unsigned)it.width, (unsigned)it.height);
    emit_changed();

    if (committed != NULL) *committed = true;
    if (err != NULL) *err = NULL;
    return true;
}

void photo_rt_upload_abort(void)
{
    if (s_part_open || s_part_path[0] != '\0') {
        discard_part();
        Serial.println("[照片] 上传已取消");
    }
    portENTER_CRITICAL(&s_up_mux);
    if (photo_sync_busy(&s_sync) || s_sync.state == PHOTO_SYNC_PAUSED) {
        photo_sync_commit_fail(&s_sync, "abort"); /* 转 ERROR（非 busy）*/
    }
    s_ring_head = 0;
    s_ring_tail = 0;
    portEXIT_CRITICAL(&s_up_mux);

    if (s_progress_on) {
        ui_photo_set_progress(false, 0);
        s_progress_on = false;
    }
}

/**
 * 递归清空目录内容（保留目录本身）。用"反复重开目录取第一个条目"的办法，
 * 不依赖一次性把上千个文件名放内存；顺序删除后目录会逐步变空。
 */
static void wipe_tree(const char *path)
{
    for (int guard = 0; guard < 4096; guard++) {
        File dir = SD_MMC.open(path);
        if (!dir || !dir.isDirectory()) {
            if (dir) dir.close();
            return;
        }
        File f = dir.openNextFile();
        if (!f) { /* 目录空了 */
            dir.close();
            return;
        }
        char name[80];
        snprintf(name, sizeof(name), "%s", f.name());
        const bool isDir = f.isDirectory();
        f.close();
        dir.close();

        const char *base = strrchr(name, '/');
        if (base != NULL) memmove(name, base + 1, strlen(base));
        if (name[0] == '\0' || strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;

        char child[128];
        if (snprintf(child, sizeof(child), "%s/%s", path, name) >= (int)sizeof(child)) continue;
        if (isDir) {
            wipe_tree(child);
            SD_MMC.rmdir(child);
        }
        else {
            SD_MMC.remove(child);
        }
    }
}

bool photo_rt_format_card(const char **err)
{
    if (err != NULL) *err = NULL;
    if (photo_rt_upload_active() || s_sync.state == PHOTO_SYNC_COMMITTING) {
        if (err != NULL) *err = "busy";
        return false;
    }

    Serial.println("[照片] 开始清空/重置 SD 卡…");

    /* 先停播放、关掉所有文件句柄，否则 unmount 会失败/丢数据 */
    close_file();
    s_shown_file = -1;
    s_shown_frame = -1;
    s_load_frame = -1;
    s_anim_file = -1;
    s_anim_frames = 0;
    s_page_active = false;

    SD_MMC.end();
    dash_sd_reset();

    /*
     * 重新挂载。`format_if_mount_failed=true` 只影响**挂载失败**的卡：
     * 卡的文件系统坏了（上次格式化被打断/脏分区）时由 SD 驱动重建，用户点了
     * "格式化"才允许走到这里，正常启动路径仍是 false（绝不悄悄格式化）。
     * 卡能正常挂载时不重建，只做下面的"清空所有内容"。
     */
    bool mounted = SD_MMC.begin("/sdcard", true, /*format_if_mount_failed=*/true,
                                SDMMC_FREQ_HIGHSPEED, 5);
    if (!mounted) {
        Serial.println("[照片] 存储卡无法挂载/重建");
        if (err != NULL) *err = "io";
        return false;
    }
    /* 让 dashboard 的挂载缓存与真实状态一致（后续 sd_mount 直接返回 true） */
    dash_sd_reset();
    if (!dash_sd_mount()) {
        if (err != NULL) *err = "io";
        return false;
    }

    /* 清空根目录下所有内容（照片、残留 .part/.meta、其它脏文件） */
    wipe_tree("/sdcard");
    SD_MMC.mkdir(PHOTO_DIR);
    SD_MMC.mkdir(PHOTO_INCOMING_DIR);

    s_scanned = false;
    scan_dir(); /* 重建空清单，并确保 /photos 存在 */
    slide_rebuild(0);
    s_shown_file = -1;
    s_shown_frame = -1;
    s_load_frame = -1;
    emit_changed();
    Serial.println("[照片] SD 卡已清空/重置（目录已重建）");
    return true;
}

void photo_rt_upload_pause(void)
{
    /*
     * 断连：**保留 .part**，下次 begin 同 bytes+crc 就能续传（设计 §8.4）。
     * 先把已收字节全部落盘再关文件 —— 否则 ring 里的数据丢了，
     * 而 photo_sync 记的 received 却是"收到过"，续传偏移会错。
     */
    for (int guard = 0; guard < 16 && ring_pending() > 0; guard++) {
        if (photo_rt_upload_drain() == 0) break;
    }
    if (s_part_open) {
        s_part.flush();
        s_part.close();
        s_part_open = false;
    }

    portENTER_CRITICAL(&s_up_mux);
    photo_sync_pause(&s_sync);
    s_ring_head = 0;
    s_ring_tail = 0;
    portEXIT_CRITICAL(&s_up_mux);

    if (s_progress_on) {
        ui_photo_set_progress(false, 0);
        s_progress_on = false;
    }
    Serial.printf("[照片] 上传暂停（保留 %s，可续传）\n",
                  s_up_name[0] != '\0' ? s_up_name : ".part");
}

bool photo_rt_upload_active(void)
{
    return upload_busy();
}

uint32_t photo_rt_upload_received(void)
{
    portENTER_CRITICAL(&s_up_mux);
    const uint32_t v = s_sync.received_bytes;
    portEXIT_CRITICAL(&s_up_mux);
    return v;
}

uint32_t photo_rt_upload_total(void)
{
    portENTER_CRITICAL(&s_up_mux);
    const uint32_t v = s_sync.total_bytes;
    portEXIT_CRITICAL(&s_up_mux);
    return v;
}

uint32_t photo_rt_upload_committed(void)
{
    portENTER_CRITICAL(&s_up_mux);
    const uint32_t v = s_sync.committed_bytes;
    portEXIT_CRITICAL(&s_up_mux);
    return v;
}

bool photo_rt_is_uploading(void)
{
    return upload_busy();
}

uint32_t photo_rt_playback_fps(void)
{
    return s_fps_meas;
}

/* ═══════════════════════════════════════════════════════════════════════
 * 命令：play / del / set / bench
 * ═══════════════════════════════════════════════════════════════════════ */

bool photo_rt_play(int index)
{
    if (!storage_probe()) return false;
    ensure_scan();
    const int n = (s_list != NULL) ? s_list->count : 0;
    if (n <= 0) {
        Serial.println("[照片] 清单为空，无法播放");
        return false;
    }
    /* 越界夹取（语义同旧 photo_play：n<0→0，n>=count→count-1）*/
    if (index < 0) index = 0;
    if (index >= n) index = n - 1;

    slide_rebuild(index);
    /* 钉住：关闭轮播时停在这张，符合用户"我点了这张就停在这张"的预期 */
    if (s_slide.file_count > 0) s_slide.pinned_file = s_slide.cur_file;
    s_slide.seg_start_ms = millis();

    /* 强制下一次 poll 重新加载（下标含义可能已经变了）*/
    s_shown_file = -1;
    s_shown_frame = -1;
    s_load_frame = -1;
    fps_finalize(millis());

    Serial.printf("[照片] 切到第 %d 张（共 %d 张）：%s\n", index + 1, n,
                  s_list->items[index].name);
    emit_changed();
    return true;
}

bool photo_rt_delete(const char *name, const char **err)
{
    if (err != NULL) *err = NULL;
    if (name == NULL || !photo_name_valid(name)) {
        if (err) *err = "bad_arg";
        return false;
    }
    if (!storage_probe()) {
        if (err) *err = "not_ready";
        return false;
    }

    char path[96];
    snprintf(path, sizeof(path), PHOTO_DIR "/%s", name);
    if (!SD_MMC.exists(path)) {
        if (err) *err = "bad_arg";
        return false;
    }

    /* 删的是正在播的那张：先关句柄（否则某些实现会因文件被占用而删失败）*/
    if (strcmp(name, s_cur_name) == 0) close_file();

    /* 记住"接下来该停在哪张"：下标夹取要在删除**后**做 */
    const int old_cur = cur_manifest_index();

    if (!SD_MMC.remove(path)) {
        if (err) *err = "io";
        Serial.printf("[照片] 删除失败：%s\n", path);
        return false;
    }

    ensure_scan();
    if (s_list != NULL) photo_list_remove(s_list, name);

    const int n = (s_list != NULL) ? s_list->count : 0;
    int focus = old_cur;
    if (focus < 0) focus = 0;
    if (n <= 0) focus = -1;
    else if (focus >= n) focus = n - 1;

    slide_rebuild(focus);
    s_shown_file = -1;
    s_shown_frame = -1;
    s_load_frame = -1;

    Serial.printf("[照片] 已删除 %s（剩余 %d 张）\n", name, n);
    emit_changed();
    return true;
}

bool photo_rt_apply_settings(bool has_slide, int slide, bool has_intv, int intv,
                             bool has_shift, int shift, bool has_soff, int soff_s)
{
    if (!has_slide && !has_intv && !has_shift && !has_soff) return false;

    photo_settings_t t = s_slide.settings;
    if (has_slide) t.slideshow = (slide != 0);
    if (has_intv) {
        if (intv < 1) intv = 1;
        if (intv > 3600) intv = 3600;
        t.interval_ms = (uint32_t)intv * 1000u;
    }
    if (has_shift) t.shift = (shift != 0);
    if (has_soff) {
        if (soff_s <= 0) {
            t.screen_off = false; /* 0 = 关（保留原时长，App 再开时沿用）*/
        }
        else {
            t.screen_off = true;
            t.screen_off_ms = (uint32_t)soff_s * 1000u;
        }
    }

    /* photo_slide_set_settings() 内部 sanitize，并处理"关轮播 → 钉住当前张" */
    photo_slide_set_settings(&s_slide, t);
    prefs_save_settings();
    /* 用户刚动过设置 → 自动息屏重新计时（否则可能立刻黑屏）*/
    photo_slide_note_touch(&s_slide, millis());

    Serial.printf("[照片] 设置：轮播=%d 间隔=%us 位移=%d 息屏=%d(%us)\n",
                  (int)s_slide.settings.slideshow,
                  (unsigned)(s_slide.settings.interval_ms / 1000u),
                  (int)s_slide.settings.shift, (int)s_slide.settings.screen_off,
                  (unsigned)(s_slide.settings.screen_off_ms / 1000u));
    emit_changed();
    return true;
}

void photo_rt_bench(void)
{
    if (upload_busy()) {
        Serial.println("[照片] 自检跳过：正在接收上传");
        return;
    }
    ensure_scan();
    if (!ensure_buffers()) return;

    /* 找清单里第一张 .pgj，读整文件 → 解码一次 → 记录耗时 */
    int target = -1;
    if (s_list != NULL) {
        for (int i = 0; i < s_list->count; i++) {
            if (s_list->items[i].kind == PHOTO_KIND_PGJ) {
                target = i;
                break;
            }
        }
    }
    if (target < 0) {
        Serial.printf("[照片] 自检：清单里没有 .pgj（dec_ms 沿用 NVS 里的 %u ms）\n",
                      (unsigned)s_dec_ms_nvs);
        emit_info();
        return;
    }

    const photo_item_t *it = &s_list->items[target];
    char path[96];
    snprintf(path, sizeof(path), PHOTO_DIR "/%s", it->name);
    const size_t need = it->bytes;
    if (need == 0 || need > s_stage_cap) {
        Serial.println("[照片] 自检：文件体积超出暂存区");
        return;
    }

    File f = SD_MMC.open(path, FILE_READ);
    if (!f) {
        Serial.println("[照片] 自检：打不开文件");
        return;
    }
    const uint32_t t_read0 = millis();
    size_t got = 0;
    while (got < need) {
        const int r = f.read(s_stage + got, need - got);
        if (r <= 0) break;
        got += (size_t)r;
    }
    f.close();
    const uint32_t read_ms = millis() - t_read0;
    if (got != need) {
        Serial.println("[照片] 自检：读文件失败");
        return;
    }

    /* 解到**后台**缓冲：不交换，画面不动 */
    const int back = 1 - s_fb_show;
    uint16_t jw = 0, jh = 0;
    const bool ok = jpeg_decode_rgb565(s_stage + PGJ_HEADER_BYTES, need - PGJ_HEADER_BYTES,
                                       s_fb[back], PHOTO_MAX_FRAME_PX, &jw, &jh);
    if (!ok) {
        Serial.println("[照片] 自检：JPEG 解码失败");
        return;
    }

    const uint32_t dec_ms = jpeg_decode_last_ms();
    s_dec_ms_nvs = dec_ms;
    prefs_save_meas(); /* dec_ms 写 NVS（bench 是低频操作，不怕写）*/

    Serial.printf("[照片] 自检 %s：读 %u B / %u ms，解码 %u ms（%ux%u），"
                  "fps_meas=%u tier=%d\n",
                  it->name, (unsigned)need, (unsigned)read_ms, (unsigned)dec_ms,
                  (unsigned)jw, (unsigned)jh, (unsigned)photo_rt_playback_fps(),
                  (photo_rt_playback_fps() >= 8u) ? 2 : 1);
    emit_info();
}

/* ═══════════════════════════════════════════════════════════════════════
 * 生命周期
 * ═══════════════════════════════════════════════════════════════════════ */

/**
 * 清理 `.incoming/` 里的残留：只删**超过 7 天**的（设计 §8.4）。
 *
 * 为什么要判年龄而不是全删：断连/掉电留下的 `.part` 是合法续传数据，
 * 开机全删等于把"续传"这个功能取消掉。只有 RTC 时间可信时才按年龄删。
 */
static void cleanup_incoming(void)
{
    if (!storage_probe()) return;
    File dir = SD_MMC.open(PHOTO_INCOMING_DIR);
    if (!dir || !dir.isDirectory()) return;

    const time_t now = time(NULL);
    const bool time_ok = (now > 1600000000); /* 2020 之后才算 RTC 已校准 */

    char victims[16][72];
    int nv = 0;
    int total = 0;
    for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
        if (f.isDirectory()) continue;
        total++;
        if (!time_ok || nv >= (int)(sizeof(victims) / sizeof(victims[0]))) continue;
        const time_t mt = f.getLastWrite();
        if (mt == 0) continue;
        if ((double)(now - mt) < (double)PHOTO_PART_MAX_AGE_S) continue;

        const char *nm = f.name();
        const char *base = strrchr(nm, '/');
        if (base != NULL) nm = base + 1;
        snprintf(victims[nv], sizeof(victims[0]), "%s", nm);
        nv++;
    }
    dir.close();

    for (int i = 0; i < nv; i++) {
        char p[112];
        snprintf(p, sizeof(p), PHOTO_INCOMING_DIR "/%s", victims[i]);
        SD_MMC.remove(p);
        Serial.printf("[照片] 清理过期残留 %s\n", victims[i]);
    }
    if (total > nv) {
        Serial.printf("[照片] .incoming 里有 %d 个待续传/未完成文件（保留）\n", total - nv);
    }
}

void photo_rt_init(void)
{
    memset(&s_listener, 0, sizeof(s_listener));
    memset(&s_sync, 0, sizeof(s_sync));
    photo_sync_init(&s_sync);

    s_list = NULL;
    s_scanned = false;
    s_scan_ms = 0;
    s_view_off = 0;
    s_fb[0] = s_fb[1] = NULL;
    s_stage = NULL;
    s_stage_cap = 0;
    s_buf_ready = false;
    s_fb_show = 0;
    close_file();
    s_shown_file = -1;
    s_shown_frame = -1;
    s_page_active = false;
    s_shift_ms = millis();
    s_shift_phase = 0;
    s_anim_file = -1;
    s_anim_frames = 0;
    s_anim_t0 = 0;
    s_fps_save_ms = 0;
    s_ring = NULL;
    s_ring_head = 0;
    s_ring_tail = 0;
    s_part_open = false;
    s_part_path[0] = '\0';
    s_meta_path[0] = '\0';
    s_up_name[0] = '\0';
    s_up_flush = 0;
    s_up_new_seq = false;
    s_progress_on = false;
    s_progress_ms = 0;

    prefs_load();
    /*
     * **启动时就扫一次**（而不是等第一次进照片页）：
     *   - 上报路径（PhotoInfo/State/清单）不允许做 I/O，必须有现成清单；
     *   - 代价是开机多 ~50 次小读（实测几十 ms），可接受。
     * 没卡的时候扫出来是空清单，之后轮询里会按 5 s 节流重扫。
     */
    scan_dir();
    cleanup_incoming();

    Serial.printf("[照片] 运行时初始化：轮播=%d 间隔=%us 息屏=%d(%umin) 位移=%d "
                  "序号=%u fps_meas=%u dec_ms=%u\n",
                  (int)s_slide.settings.slideshow,
                  (unsigned)(s_slide.settings.interval_ms / 1000u),
                  (int)s_slide.settings.screen_off,
                  (unsigned)(s_slide.settings.screen_off_ms / 60000u),
                  (int)s_slide.settings.shift, (unsigned)s_up_seq, (unsigned)s_fps_meas,
                  (unsigned)s_dec_ms_nvs);
}

void photo_rt_set_listener(const photo_rt_listener_t *listener)
{
    if (listener != NULL) {
        s_listener = *listener;
    }
    else {
        memset(&s_listener, 0, sizeof(s_listener));
    }
}

void photo_rt_poll(uint32_t now_ms)
{
    /* ── 1) 上传：写盘 + 超时/断连处理 + 进度 UI ──
     * 放在最前面：写盘 4 KB 是"必须按时做"的事，不能因为渲染拖后。 */
    const bool was_busy = upload_busy();
    if (was_busy) {
        photo_rt_upload_drain();
    }

    /* 超时/断连推进：10 s 无数据 → PAUSED（.part 保留，可续传）*/
    portENTER_CRITICAL(&s_up_mux);
    photo_sync_tick(&s_sync, now_ms);
    const bool paused_now = (s_sync.state == PHOTO_SYNC_PAUSED);
    portEXIT_CRITICAL(&s_up_mux);

    if (paused_now && s_part_open) {
        /* 先 flush 再关：.part 大小是可续传偏移的唯一依据 */
        s_part.flush();
        s_part.close();
        s_part_open = false;
        Serial.printf("[照片] 上传超时/断连 → 已暂停（%s 保留，可续传）\n",
                      s_up_name[0] != '\0' ? s_up_name : ".part");
    }
    progress_update(now_ms, false);

    /* ── 2) 上传占着通道时：暂停读帧（画面冻结，但触摸/滑页不受影响）── */
    if (upload_busy()) return;

    /* ── 3) 只有照片表盘可见时才工作（切走即停，省电省 CPU）── */
    if (ui_pages_current() != UI_PAGE_PHOTO) {
        if (s_page_active) {
            s_page_active = false;
            fps_finalize(now_ms); /* 离开页面时结算动图 fps，别把窗口带到下次 */
        }
        return;
    }

    if (!s_page_active) {
        s_page_active = true;
        /* 亮度封顶（烧屏防护第 3 道）：每次**进入**照片页设一次，不在每拍比对 */
        dash_brightness_cap(PHOTO_BRIGHTNESS_CAP);
        /* 进页面重新开始息屏计时（否则刚切过来就可能立刻黑屏）*/
        photo_slide_note_touch(&s_slide, now_ms);
        s_shift_ms = now_ms;
        /* 清单为空时允许周期性重扫：刚插卡/刚清空都能自己恢复 */
        ensure_scan();
    }
    else if ((s_list == NULL || s_list->count == 0) &&
             (uint32_t)(now_ms - s_scan_ms) >= PHOTO_RESCAN_MS) {
        if (storage_probe()) scan_dir();
    }

    /* 没就绪（没插卡）时每 5 s 重试一次挂载：插卡后能自己恢复，不用重启 */
    if (!s_stor_ok && (uint32_t)(now_ms - s_stor_probe_ms) >= PHOTO_RESCAN_MS) {
        if (storage_probe() && !s_scanned) scan_dir();
    }

    playback_poll(now_ms);
}
