/**
 * test_photo_slide.c —— 照片表盘轮播状态机的单测（纯逻辑，不需要硬件）
 *
 * 这个状态机最容易出错的地方都在"边界与开关组合"上，所以测试重点：
 *   - 静态与动图**混排**时的时间推进；
 *   - 动图**播满 LOOP_COUNT 轮**才切下一个文件；
 *   - **不轮播**时：静态图不动、动图仍继续循环、且**像素位移被强制为真**；
 *   - 开关**来回切**：关时钉住当前文件，再开时从钉住的那张继续；
 *   - 自动息屏：计时、触摸重置、开关关闭后永不触发；
 *   - 设置越界值被收敛；空清单/坏元信息不崩、不除零。
 */
#include "../simulator/ui/photo_slide.h"

#include <stdio.h>
#include <string.h>

static int g_pass, g_fail;

#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        if (cond) {                                                            \
            g_pass++;                                                          \
        } else {                                                               \
            g_fail++;                                                          \
            printf("  ✗ %s:%d  %s\n", __FILE__, __LINE__, msg);                \
        }                                                                      \
    } while (0)

/* ── 辅助：造一个静态文件 + 一个动图文件的清单 ─────────────── */
static photo_file_meta_t g_metas[4];

static void setup_static_static(photo_slide_t *s)
{
    g_metas[0].frame_count = 1;
    g_metas[0].fps = 0;
    g_metas[1].frame_count = 1;
    g_metas[1].fps = 0;
    photo_slide_set_files(s, 2, g_metas);
}

/* ── 1. 空清单：应报告"没有内容"，且不崩 ───────────────────── */

static void test_empty(void)
{
    photo_slide_t s;
    photo_slide_init(&s, photo_settings_default());

    photo_frame_t f;
    photo_slide_state(&s, 0, &f);
    CHECK(f.file_index == -1, "空清单应返回 file_index=-1（调用方画占位）");
    CHECK(!photo_slide_tick(&s, 1000), "空清单 tick 应返回 false");
    CHECK(&s != NULL, "空清单不应崩溃");
}

/* ── 2. 静态图：按 interval 切下一个，并回绕 ───────────────── */

static void test_static_advance(void)
{
    photo_slide_t s;
    photo_slide_init(&s, photo_settings_default());
    setup_static_static(&s);
    photo_slide_restart(&s, 0);

    photo_frame_t f;
    photo_slide_state(&s, 0, &f);
    CHECK(f.file_index == 0 && f.frame_index == 0, "初始应显示第 0 个文件第 0 帧");

    /* 还没到 30s → 不动 */
    CHECK(!photo_slide_tick(&s, 29999), "29.999s 不应切换");
    photo_slide_state(&s, 29999, &f);
    CHECK(f.file_index == 0, "29.999s 仍是第 0 个");

    /* 到 30s → 切 */
    CHECK(photo_slide_tick(&s, 30000), "30s 应切换");
    photo_slide_state(&s, 30000, &f);
    CHECK(f.file_index == 1, "30s 后应是第 1 个");

    /* 再到 60s → 回绕到第 0 个 */
    CHECK(photo_slide_tick(&s, 60000), "60s 应再切换");
    photo_slide_state(&s, 60000, &f);
    CHECK(f.file_index == 0, "两个文件应回绕到第 0 个");
}

/* ── 3. 动图：按 fps 逐帧，播满 LOOP_COUNT 轮才换文件 ──────── */

static void test_animated_frames(void)
{
    photo_slide_t s;
    photo_settings_t cfg = photo_settings_default();
    photo_slide_init(&s, cfg);

    g_metas[0].frame_count = 4; /* 4 帧 @10fps → 每帧 100ms */
    g_metas[0].fps = 10;
    g_metas[1].frame_count = 1;
    g_metas[1].fps = 0;
    photo_slide_set_files(&s, 2, g_metas);
    photo_slide_restart(&s, 0);

    photo_frame_t f;
    photo_slide_state(&s, 0, &f);
    CHECK(f.file_index == 0 && f.frame_index == 0, "动图应从第 0 帧开始");

    /* 每 100ms 前进一帧 */
    CHECK(!photo_slide_tick(&s, 99), "99ms 不应换帧");
    CHECK(photo_slide_tick(&s, 100), "100ms 应换到第 1 帧");
    photo_slide_state(&s, 100, &f);
    CHECK(f.frame_index == 1, "100ms 后应是第 1 帧");

    CHECK(photo_slide_tick(&s, 200), "200ms → 第 2 帧");
    CHECK(photo_slide_tick(&s, 300), "300ms → 第 3 帧");

    /* 400ms：第 1 轮播完，还有第 2 轮（LOOP_COUNT=2）→ 仍留在文件 0 */
    CHECK(photo_slide_tick(&s, 400), "400ms 应回到第 0 帧（第 2 轮）");
    photo_slide_state(&s, 400, &f);
    CHECK(f.file_index == 0, "第 2 轮仍应在文件 0");
    CHECK(f.frame_index == 0, "第 2 轮应从第 0 帧开始");

    /* 800ms：第 2 轮也播完 → 切到文件 1 */
    for (uint32_t t = 500; t <= 700; t += 100) {
        photo_slide_tick(&s, t);
    }
    CHECK(photo_slide_tick(&s, 800), "800ms 应切到下一个文件");
    photo_slide_state(&s, 800, &f);
    CHECK(f.file_index == 1, "两轮播完后应切到文件 1");
}

/* ── 4. 不轮播：静态不动；动图仍循环；位移强制为真 ─────────── */

static void test_no_slideshow(void)
{
    photo_slide_t s;
    photo_settings_t cfg = photo_settings_default();
    cfg.slideshow = false;
    cfg.shift = false; /* 用户把位移也关了 */
    photo_slide_init(&s, cfg);

    g_metas[0].frame_count = 1;
    g_metas[0].fps = 0;
    g_metas[1].frame_count = 1;
    g_metas[1].fps = 0;
    photo_slide_set_files(&s, 2, g_metas);
    photo_slide_restart(&s, 0);

    photo_frame_t f;
    photo_slide_state(&s, 0, &f);
    CHECK(f.file_index == 0, "不轮播时显示第 0 个");
    CHECK(f.shift == true, "**不轮播时位移必须被强制为真**（用户要求）");
    CHECK(photo_slide_eff_shift(&s) == true, "eff_shift 在不轮播时应为 true");

    /* 过了很久也不换文件 */
    CHECK(!photo_slide_tick(&s, 30000), "不轮播：静态图不应切换");
    photo_slide_state(&s, 30000, &f);
    CHECK(f.file_index == 0, "不轮播：30s 后仍是同一个文件");
    CHECK(!photo_slide_tick(&s, 300000), "不轮播：300s 后仍不切换");
    photo_slide_state(&s, 300000, &f);
    CHECK(f.file_index == 0, "不轮播：长时间后仍停在同一文件");
}

/* ── 5. 不轮播但当前是动图：动图应继续循环 ─────────────────── */

static void test_no_slideshow_animation_still_plays(void)
{
    photo_slide_t s;
    photo_settings_t cfg = photo_settings_default();
    cfg.slideshow = false;
    photo_slide_init(&s, cfg);

    g_metas[0].frame_count = 3;
    g_metas[0].fps = 10;
    photo_slide_set_files(&s, 1, g_metas);
    photo_slide_restart(&s, 0);

    photo_frame_t f;
    photo_slide_tick(&s, 100);
    photo_slide_state(&s, 100, &f);
    CHECK(f.frame_index == 1, "不轮播时动图仍应逐帧播放（100ms → 第1帧）");

    photo_slide_tick(&s, 200);
    photo_slide_state(&s, 200, &f);
    CHECK(f.frame_index == 2, "200ms → 第2帧");

    /* 300ms 一轮结束 → 回到第 0 帧继续 */
    photo_slide_tick(&s, 300);
    photo_slide_state(&s, 300, &f);
    CHECK(f.file_index == 0, "不轮播：永远停在同一个文件");
    CHECK(f.frame_index == 0, "一轮结束回到第 0 帧继续播");
}

/* ── 6. 开关来回切：关时钉住，再开时从钉住处继续 ───────────── */

static void test_toggle_pins_and_resumes(void)
{
    photo_slide_t s;
    photo_slide_init(&s, photo_settings_default());
    setup_static_static(&s);
    photo_slide_restart(&s, 0);

    /* 走到文件 1 */
    photo_slide_tick(&s, 30000);
    photo_frame_t f;
    photo_slide_state(&s, 30000, &f);
    CHECK(f.file_index == 1, "应已到文件 1");

    /* 关轮播 → 钉住文件 1 */
    photo_settings_t off = s.settings;
    off.slideshow = false;
    off.shift = false;
    photo_slide_set_settings(&s, off);
    CHECK(photo_slide_eff_shift(&s), "关轮播后位移应被强制为真");
    photo_slide_state(&s, 60000, &f);
    CHECK(f.file_index == 1, "关轮播后应停在文件 1");

    /* 再开轮播 → 从文件 1 继续（而不是跳回 0）*/
    photo_settings_t on = s.settings;
    on.slideshow = true;
    photo_slide_set_settings(&s, on);

    photo_slide_restart(&s, 100000);
    photo_slide_tick(&s, 130000); /* 30s 后切换 */
    photo_slide_state(&s, 130000, &f);
    CHECK(f.file_index == 0, "再开轮播后应从文件 1 往后切到文件 0（回绕）");
}

/* ── 7. 自动息屏：计时、触摸重置、开关关闭 ─────────────────── */

static void test_screen_off(void)
{
    photo_slide_t s;
    photo_slide_init(&s, photo_settings_default());
    setup_static_static(&s);

    const uint32_t k30min = 30u * 60u * 1000u;
    photo_slide_restart(&s, 1000);
    CHECK(!photo_slide_should_screen_off(&s, 1000), "刚开始不应息屏");
    CHECK(!photo_slide_should_screen_off(&s, 1000 + k30min - 1), "差 1ms 不应息屏");
    CHECK(photo_slide_should_screen_off(&s, 1000 + k30min), "满 30min 应息屏");

    /* 触摸重置 */
    photo_slide_note_touch(&s, 1000 + k30min);
    CHECK(!photo_slide_should_screen_off(&s, 1000 + k30min + 1000), "触摸后重新计时");

    /* 关掉自动息屏 → 永不触发 */
    photo_settings_t off = s.settings;
    off.screen_off = false;
    photo_slide_set_settings(&s, off);
    CHECK(!photo_slide_should_screen_off(&s, 0xFFFFFFFFu),
          "关闭自动息屏后，无论多久都不应息屏");
}

/* ── 8. 设置收敛：越界值被夹回合法范围 ─────────────────────── */

static void test_sanitize(void)
{
    photo_settings_t d = photo_settings_default();
    CHECK(d.slideshow && d.screen_off && d.shift, "默认三个开关都是开");
    CHECK(d.interval_ms == 30000u, "默认轮播间隔 30s");
    CHECK(d.screen_off_ms == 30u * 60u * 1000u, "默认息屏 30min");

    photo_settings_t s;
    s.slideshow = true;
    s.interval_ms = 1;            /* 太小 */
    s.screen_off = true;
    s.screen_off_ms = 1;          /* 太小 */
    s.shift = true;
    photo_settings_sanitize(&s);
    CHECK(s.interval_ms == PHOTO_SLIDE_INTERVAL_MIN_MS, "间隔过小应夹到下限 10s");
    CHECK(s.screen_off_ms == PHOTO_SCREEN_OFF_MIN_MS, "息屏过小应夹到下限 1min");

    s.interval_ms = 999999u;
    s.screen_off_ms = 0xFFFFFFFFu;
    photo_settings_sanitize(&s);
    CHECK(s.interval_ms == PHOTO_SLIDE_INTERVAL_MAX_MS, "间隔过大应夹到上限 300s");
    CHECK(s.screen_off_ms == PHOTO_SCREEN_OFF_MAX_MS, "息屏过大应夹到上限 24h");
}

/* ── 9. 坏元信息：帧数为 0 时不崩、不除零 ──────────────────── */

static void test_bad_meta(void)
{
    photo_slide_t s;
    photo_slide_init(&s, photo_settings_default());

    g_metas[0].frame_count = 0; /* 损坏的文件 */
    g_metas[0].fps = 0;
    photo_slide_set_files(&s, 1, g_metas);

    photo_frame_t f;
    photo_slide_state(&s, 0, &f);
    CHECK(f.file_index == 0, "坏文件仍占据下标（上层画占位）");
    CHECK(f.frame_index == 0, "坏文件帧下标应为 0，不应为负");

    /* 多帧但 fps=0：按静态处理，不应除零 */
    g_metas[0].frame_count = 5;
    g_metas[0].fps = 0;
    photo_slide_set_files(&s, 1, g_metas);
    photo_slide_restart(&s, 0);
    CHECK(!photo_slide_tick(&s, 1000), "fps=0 的多帧文件应像静态图一样按 interval 处理");
    photo_slide_state(&s, 1000, &f);
    CHECK(f.file_index == 0, "不崩且仍在文件 0");
}

/* ── 10. 时间回绕：unsigned 减法应与真实间隔一致 ────────────── */

static void test_time_wrap(void)
{
    photo_slide_t s;
    photo_slide_init(&s, photo_settings_default());
    setup_static_static(&s);

    uint32_t start = 0xFFFFFF00u; /* 距回绕只差 256ms */
    photo_slide_restart(&s, start);

    /* 回绕后再过 30s → 应正常切换（依赖 uint32 减法语义）*/
    uint32_t later = start + 30000u; /* 自动回绕 */
    CHECK(photo_slide_tick(&s, later), "跨 2^32 回绕后 30s 仍应正常切换");
    photo_frame_t f;
    photo_slide_state(&s, later, &f);
    CHECK(f.file_index == 1, "回绕后应切到文件 1");
}

int main(void)
{
    printf("=== test_photo_slide（照片表盘轮播状态机）===\n");

    test_empty();
    test_static_advance();
    test_animated_frames();
    test_no_slideshow();
    test_no_slideshow_animation_still_plays();
    test_toggle_pins_and_resumes();
    test_screen_off();
    test_sanitize();
    test_bad_meta();
    test_time_wrap();

    printf("结果: %d 通过, %d 失败\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
