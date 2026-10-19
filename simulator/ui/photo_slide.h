/**
 * photo_slide.h —— 照片表盘的**轮播状态机**（纯逻辑，不依赖 LVGL/SD/时间源）
 *
 * 这是照片表盘的"大脑"：给定**文件清单**与**当前时间**，算出"此刻应当显示哪一帧"。
 * 不碰文件、不碰屏幕、不取系统时间 —— 因此可以在 PC 上把边界情况全测一遍。
 *
 * ## 它要同时处理两种内容
 *
 * `.pgi` 的**帧数就是类型**（见 pgi.h）：
 *   - `frame_count == 1` → 静态照片：显示 `interval_ms` 后切下一个文件；
 *   - `frame_count > 1`  → 动图：按 `fps` 逐帧播放，播 `loop_count` 轮后切下一个文件。
 *
 * ## 三个开关（用户要求，App 可手动控制）
 *
 * | 开关 | 关掉后 |
 * |------|--------|
 * | `slideshow` | **停在 `pinned_file` 不动**（记住最后显示的那个文件）；⚠️ 此时 `shift` 强制为真 |
 * | `screen_off` | 调用方不再自动息屏 |
 * | `shift`（像素位移）| 画面完全静止（烧屏风险最高，用户已知晓）|
 *
 * > **不轮播时强制位移**：这是用户明确要求的（"不轮播时候就是像素位移的方法会开启"）。
 * > 注意是"强制为真"，不是"忽略设置"——所以 `eff_shift()` 有一处 `||`。
 */
#ifndef PHOTO_SLIDE_H
#define PHOTO_SLIDE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 最多跟踪多少个文件（超出部分忽略；SD 目录里成千上万个也没意义）*/
#define PHOTO_SLIDE_MAX_FILES 64

/** 一轮动图最多循环几次（避免一张动图霸屏）*/
#define PHOTO_SLIDE_LOOP_COUNT 2

/** 轮播间隔的可配范围（毫秒）*/
#define PHOTO_SLIDE_INTERVAL_MIN_MS 10000u
#define PHOTO_SLIDE_INTERVAL_MAX_MS 300000u

/** 自动息屏时长的可配范围（毫秒）*/
#define PHOTO_SCREEN_OFF_MIN_MS (60u * 1000u)
#define PHOTO_SCREEN_OFF_MAX_MS (24u * 3600u * 1000u)

/** 用户可配的设置（会写 NVS 持久化）*/
typedef struct {
    bool     slideshow;         /* 轮播开关 */
    uint32_t interval_ms;       /* 静态图的停留时间 */
    bool     screen_off;        /* 自动息屏开关 */
    uint32_t screen_off_ms;     /* 无交互多久后息屏 */
    bool     shift;             /* 像素位移开关 */
} photo_settings_t;

/** 单个文件的元信息（从 .pgi 文件头读出来的）*/
typedef struct {
    uint16_t frame_count;       /* 1 = 静态 */
    uint8_t  fps;               /* 动图帧率；静态为 0 */
} photo_file_meta_t;

/** 状态机 */
typedef struct {
    photo_settings_t settings;

    /* 文件清单 */
    int file_count;
    photo_file_meta_t files[PHOTO_SLIDE_MAX_FILES];

    /* 运行态 */
    int cur_file;               /* 当前文件下标；-1 = 还没有 */
    int cur_frame;              /* 当前帧下标 */
    int loops_done;             /* 当前文件已播放的整轮数 */
    uint32_t seg_start_ms;      /* 当前"段"（一帧 or 一张静态图）的起始时间 */
    int pinned_file;            /* 关闭轮播时停在哪个文件；-1 = 未定 */

    /* 交互（用于息屏判定）*/
    uint32_t last_touch_ms;
} photo_slide_t;

/** 一份"默认设置"：轮播开、30s、息屏开、30min、位移开 */
photo_settings_t photo_settings_default(void);

/**
 * 把设置夹到合法范围（间隔 10~300s、息屏 1min~24h）。
 * App 可能发来任意数字，**在这里统一收敛**，不要在别处各自 clamp。
 */
void photo_settings_sanitize(photo_settings_t *s);

/** 初始化（不传文件清单；调用 set_files 之后再 tick）*/
void photo_slide_init(photo_slide_t *s, photo_settings_t settings);

/**
 * 设置文件清单。
 *
 * @param count 文件个数（<= PHOTO_SLIDE_MAX_FILES）
 * @param metas 元信息数组
 * @return 实际接受的个数
 */
int photo_slide_set_files(photo_slide_t *s, int count, const photo_file_meta_t *metas);

/** 改设置；会顺带把越界值夹回合法范围 */
void photo_slide_set_settings(photo_slide_t *s, photo_settings_t settings);

/** 记录一次用户交互（触摸/按键），用于自动息屏计时 */
void photo_slide_note_touch(photo_slide_t *s, uint32_t now_ms);

/** 像素位移的**最终生效值**：不轮播时强制为真 */
bool photo_slide_eff_shift(const photo_slide_t *s);

/** 此刻应显示的帧 */
typedef struct {
    int file_index;             /* -1 = 没有可显示的内容（调用方画占位）*/
    int frame_index;
    bool shift;                 /* 这一帧是否要做像素位移 */
} photo_frame_t;

/**
 * **不修改状态**地算出此刻应显示什么。
 *
 * 调用方在每帧渲染前调用它即可；真正的"推进"由 `photo_slide_tick()` 完成。
 * 拆成两个函数，是因为渲染可能比 tick 更频繁（且 tick 可能被节流）。
 */
void photo_slide_state(const photo_slide_t *s, uint32_t now_ms, photo_frame_t *out);

/**
 * 推进状态机（到达切换时刻就前进）。
 *
 * @return true = 显示内容变了（调用方需要重新解码/重绘）
 */
bool photo_slide_tick(photo_slide_t *s, uint32_t now_ms);

/** 是否该自动息屏了（关闭自动息屏时永远返回 false）*/
bool photo_slide_should_screen_off(const photo_slide_t *s, uint32_t now_ms);

/** 从这一刻重新开始计时（例如息屏唤醒、刚进入该表盘）*/
void photo_slide_restart(photo_slide_t *s, uint32_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* PHOTO_SLIDE_H */
