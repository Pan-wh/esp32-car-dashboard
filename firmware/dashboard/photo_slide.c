/**
 * photo_slide.c —— 轮播状态机实现（纯逻辑）
 *
 * 时间单位统一 **毫秒**；所有 `now_ms` 都是调用方传入的单调时钟
 * （真机用 `millis()`，PC 测试用任意递增数）。**本模块不取系统时间**，
 * 所以测试可以精确控制"过了多久"。
 *
 * 溢出处理：所有时间差都用 `uint32_t` 减法，依赖无符号回绕语义 ——
 * `now - start` 即使跨越 2^32 也仍然正确（只要两次间隔 < 49.7 天）。
 */
#include "photo_slide.h"

#include <string.h>

photo_settings_t photo_settings_default(void)
{
    photo_settings_t s;
    s.slideshow     = true;
    s.interval_ms   = 30000u;
    s.screen_off    = true;
    s.screen_off_ms = 30u * 60u * 1000u; /* 30 分钟 */
    s.shift         = true;
    return s;
}

void photo_settings_sanitize(photo_settings_t *s)
{
    if (s == NULL) return;

    if (s->interval_ms < PHOTO_SLIDE_INTERVAL_MIN_MS) s->interval_ms = PHOTO_SLIDE_INTERVAL_MIN_MS;
    if (s->interval_ms > PHOTO_SLIDE_INTERVAL_MAX_MS) s->interval_ms = PHOTO_SLIDE_INTERVAL_MAX_MS;

    if (s->screen_off_ms < PHOTO_SCREEN_OFF_MIN_MS) s->screen_off_ms = PHOTO_SCREEN_OFF_MIN_MS;
    if (s->screen_off_ms > PHOTO_SCREEN_OFF_MAX_MS) s->screen_off_ms = PHOTO_SCREEN_OFF_MAX_MS;
}

void photo_slide_init(photo_slide_t *s, photo_settings_t settings)
{
    if (s == NULL) return;
    memset(s, 0, sizeof(*s));
    photo_settings_sanitize(&settings);
    s->settings    = settings;
    s->file_count  = 0;
    s->cur_file    = -1;
    s->cur_frame   = 0;
    s->loops_done  = 0;
    s->seg_start_ms = 0;
    s->pinned_file = -1;
    s->last_touch_ms = 0;
}

int photo_slide_set_files(photo_slide_t *s, int count, const photo_file_meta_t *metas)
{
    if (s == NULL) return 0;

    int n = count;
    if (n < 0) n = 0;
    if (n > PHOTO_SLIDE_MAX_FILES) n = PHOTO_SLIDE_MAX_FILES;

    for (int i = 0; i < n; i++) {
        photo_file_meta_t m;
        m.frame_count = metas ? metas[i].frame_count : 0;
        m.fps         = metas ? metas[i].fps : 0;
        /* 帧数为 0 的文件无法显示 → 当作 1 帧静态，让上层画出"坏图"占位，
         * 而不是让下标算出 -1 或除以 0。 */
        if (m.frame_count == 0) m.frame_count = 1;
        s->files[i] = m;
    }

    s->file_count = n;
    s->cur_file   = (n > 0) ? 0 : -1;
    s->cur_frame  = 0;
    s->loops_done = 0;
    s->pinned_file = (n > 0) ? 0 : -1;
    return n;
}

void photo_slide_set_settings(photo_slide_t *s, photo_settings_t settings)
{
    if (s == NULL) return;
    photo_settings_sanitize(&settings);
    bool was_slideshow = s->settings.slideshow;
    s->settings = settings;

    /*
     * 轮播从"开"变"关"：把当前文件钉住（用户希望停在眼前这张）。
     * 反过来从"关"变"开"：从钉住的那张继续往后播，而不是跳回第一张。
     */
    if (was_slideshow && !settings.slideshow) {
        if (s->cur_file >= 0) s->pinned_file = s->cur_file;
    } else if (!was_slideshow && settings.slideshow) {
        if (s->pinned_file >= 0 && s->pinned_file < s->file_count) {
            s->cur_file = s->pinned_file;
        }
    }
}

void photo_slide_note_touch(photo_slide_t *s, uint32_t now_ms)
{
    if (s == NULL) return;
    s->last_touch_ms = now_ms;
}

bool photo_slide_eff_shift(const photo_slide_t *s)
{
    if (s == NULL) return false;
    /* 不轮播 → 强制位移（用户要求）。轮播时按用户设置。 */
    if (!s->settings.slideshow) return true;
    return s->settings.shift;
}

/** 当前文件的段时长（一帧动图 or 一张静态图）*/
static uint32_t seg_duration_ms(const photo_slide_t *s, int file)
{
    const photo_file_meta_t *m = &s->files[file];

    if (m->frame_count <= 1) {
        return s->settings.interval_ms; /* 静态：停留 = 轮播间隔 */
    }
    if (m->fps == 0) {
        /* 多帧却没 fps：无法播放，当作静态图停留（并会被上层跳过）*/
        return s->settings.interval_ms;
    }
    uint32_t per = 1000u / (uint32_t)m->fps;
    if (per == 0) per = 1;
    return per;
}

void photo_slide_state(const photo_slide_t *s, uint32_t now_ms, photo_frame_t *out)
{
    if (out == NULL) return;
    out->file_index  = -1;
    out->frame_index = 0;
    out->shift       = false;

    if (s == NULL || s->file_count <= 0) return;

    int file = s->cur_file;
    if (file < 0 || file >= s->file_count) {
        /* 轮播关闭时钉住的那个；再不行退回 0 */
        file = (s->pinned_file >= 0 && s->pinned_file < s->file_count) ? s->pinned_file : 0;
    }

    out->file_index  = file;
    out->frame_index = s->cur_frame;
    out->shift       = photo_slide_eff_shift(s);

    /* 帧下标越界保护（元信息可能被换过）*/
    if (out->frame_index < 0 || out->frame_index >= (int)s->files[file].frame_count) {
        out->frame_index = 0;
    }
    (void)now_ms; /* 状态查询本身不依赖时间；推进在 tick 里做 */
}

bool photo_slide_tick(photo_slide_t *s, uint32_t now_ms)
{
    if (s == NULL || s->file_count <= 0) return false;

    if (s->cur_file < 0 || s->cur_file >= s->file_count) {
        s->cur_file   = 0;
        s->cur_frame  = 0;
        s->loops_done = 0;
        s->seg_start_ms = now_ms;
        return true;
    }

    const photo_file_meta_t *m = &s->files[s->cur_file];
    uint32_t seg = seg_duration_ms(s, s->cur_file);
    uint32_t elapsed = now_ms - s->seg_start_ms;

    if (elapsed < seg) return false; /* 还没到切换时刻 */

    s->seg_start_ms = now_ms;

    /* ── 动图：先推进帧 ── */
    if (m->frame_count > 1 && m->fps > 0) {
        s->cur_frame++;
        if (s->cur_frame < (int)m->frame_count) return true;

        /* 一轮播完 */
        s->cur_frame = 0;
        s->loops_done++;
        if (s->loops_done < PHOTO_SLIDE_LOOP_COUNT) return true;
    }

    /* ── 该切下一个文件了（静态图也走这里）── */
    s->loops_done = 0;
    s->cur_frame  = 0;

    if (!s->settings.slideshow) {
        /*
         * 不轮播：停在当前文件。静态图没有"下一帧"可动，
         * 但动图仍应继续循环播放（"不轮播"是"不换照片"，不是"冻结画面"）。
         * 所以这里只把计时重置，不改文件 —— 返回 false 表示画面内容未变。
         */
        s->pinned_file = s->cur_file;
        return false;
    }

    s->cur_file = (s->cur_file + 1) % s->file_count;
    s->pinned_file = s->cur_file;
    return true;
}

bool photo_slide_should_screen_off(const photo_slide_t *s, uint32_t now_ms)
{
    if (s == NULL) return false;
    if (!s->settings.screen_off) return false;
    return (uint32_t)(now_ms - s->last_touch_ms) >= s->settings.screen_off_ms;
}

void photo_slide_restart(photo_slide_t *s, uint32_t now_ms)
{
    if (s == NULL) return;
    if (s->cur_file < 0 && s->file_count > 0) s->cur_file = 0;
    s->cur_frame    = 0;
    s->loops_done   = 0;
    s->seg_start_ms = now_ms;
    s->last_touch_ms = now_ms;
}
