/**
 * face_report.c —— 表盘列表上报（纯 C，模拟器/固件共用）
 *
 * 只依赖 ui_face 注册表，不做任何硬件调用，因此可在 PC 上单测。
 * 详见 face_report.h 的格式说明。
 */
#include "face_report.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "ui_face.h"

/* ---- 极简带边界检查的字符串拼接 ---- */

typedef struct {
    char *buf;
    size_t cap; /* 含结尾 NUL */
    size_t len; /* 已写入（不含 NUL）*/
    bool overflow;
} sb_t;

/** 当前表盘 id（由 face_report_set_current 设置；-1 表示未知）*/
static int s_current = -1;

void face_report_set_current(int face_id)
{
    s_current = face_id;
}

/*
 * 设备显示状态（亮度/方向/息屏）。
 *
 * 各字段负数 = "尚未设置" → 上报时**省略**该字段，而不是发一个假值。
 * 这一点很重要：App 收到省略的字段会保持原值，而收到 0 会显示成 0
 * —— 那正是"亮度被凭空改掉"的 bug 形态。
 */
static face_report_display_t s_display = { -1, -1, -1, -1, -1, -1, -1 };

void face_report_set_display(const face_report_display_t *d)
{
    if (d == NULL) {
        s_display.brightness = -1;
        s_display.rotation = -1;
        s_display.screen_on = -1;
        s_display.trip_distance_km = -1;
        s_display.trip_max_kmh = -1;
        s_display.trip_avg_kmh = -1;
        s_display.trip_time_s = -1;
        return;
    }
    s_display = *d;
}

face_report_display_t face_report_get_display(void)
{
    return s_display;
}

void face_report_reset_for_test(void)
{
    s_current = -1;
    face_report_set_display(NULL);
}

static void sb_init(sb_t *sb, char *buf, size_t cap)
{
    sb->buf = buf;
    sb->cap = cap;
    sb->len = 0;
    sb->overflow = false;
    if (cap > 0) buf[0] = '\0';
}

/** 追加一个字符；空间不足则置 overflow 并丢弃（不写坏缓冲）*/
static void sb_putc(sb_t *sb, char c)
{
    if (sb->len + 1 >= sb->cap) {
        sb->overflow = true;
        return;
    }
    sb->buf[sb->len++] = c;
    sb->buf[sb->len] = '\0';
}

static void sb_puts(sb_t *sb, const char *s)
{
    if (s == NULL) return;
    for (; *s != '\0'; s++) sb_putc(sb, *s);
}

/** 追加 JSON 字符串（转义 " 与 \，其余按原样；表盘名是 ASCII 标识符）*/
static void sb_puts_json(sb_t *sb, const char *s)
{
    if (s == NULL) {
        sb_puts(sb, "\"\"");
        return;
    }
    sb_putc(sb, '"');
    for (; *s != '\0'; s++) {
        if (*s == '"' || *s == '\\') {
            sb_putc(sb, '\\');
            sb_putc(sb, *s);
        }
        else if ((unsigned char)*s < 0x20) {
            /* 控制字符：JSON 里不合法，统一转义 */
            sb_putc(sb, '?');
        }
        else {
            sb_putc(sb, *s);
        }
    }
    sb_putc(sb, '"');
}

static void sb_printf(sb_t *sb, const char *fmt, ...)
{
    /*
     * ⚠️ 这个缓冲必须容得下**最长的单段格式串**（含展开后的数字）。
     * 曾经取 64，而截断分支的表头展开后约 70 字符 → 被误判为"溢出"，
     * 导致表盘多时整份报告返回 -1（而不是按设计截断）。已由
     * tests/test_face_report.c 的截断用例抓出。
     */
    char tmp[192];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0) {
        sb->overflow = true;
        return;
    }
    if ((size_t)n >= sizeof(tmp)) {
        /* 单段超长说明有 bug；保守地标记溢出 */
        sb->overflow = true;
        return;
    }
    sb_puts(sb, tmp);
}

/**
 * 追加"设备显示状态"字段（只追加已设置的非负字段）。
 *
 * 抽成函数是因为**完整报告与截断报告的表头必须一致** —— 两处各写一遍
 * 迟早会改漏一处（截断路径较少被走到）。
 *
 * ⚠️ 定义位置必须在 sb_printf 之后（它要用到）。
 */
static void sb_put_display(sb_t *sb)
{
    if (s_display.brightness >= 0) {
        sb_printf(sb, ",\"bright\":%d", s_display.brightness);
    }
    if (s_display.rotation >= 0) {
        sb_printf(sb, ",\"rot\":%d", s_display.rotation);
    }
    if (s_display.screen_on >= 0) {
        sb_printf(sb, ",\"screen\":%d", s_display.screen_on ? 1 : 0);
    }
    /*
     * 行程统计（P1-1）。用 %.2f 控制体积：里程/速度保留两位小数足够
     * （里程 0.01km=10m 分辨率，速度 0.01km/h 远超仪表显示精度）。
     * ⚠️ 这几个字段会让载荷变长，故**放在最后** —— 一旦触到上限，
     *    被牺牲的是行程数字而不是表盘列表本身。
     */
    if (s_display.trip_distance_km >= 0) {
        sb_printf(sb, ",\"trip_km\":%.2f", s_display.trip_distance_km);
    }
    if (s_display.trip_max_kmh >= 0) {
        sb_printf(sb, ",\"trip_max\":%.2f", s_display.trip_max_kmh);
    }
    if (s_display.trip_avg_kmh >= 0) {
        sb_printf(sb, ",\"trip_avg\":%.2f", s_display.trip_avg_kmh);
    }
    if (s_display.trip_time_s >= 0) {
        sb_printf(sb, ",\"trip_s\":%ld", s_display.trip_time_s);
    }
}

/** 分组枚举 → ASCII 名（App 据此本地化分组标题）*/
/*
 * 表盘的 `group` 字段已经**不再上报**（分组改由 App 按 name 映射），
 * 原来的 `group_key()` 因此成了死代码 —— 这里删掉，避免 -Wall 报 unused。
 */

int face_report_build(char *buf, size_t cap)
{
    if (buf == NULL || cap == 0) return -1;

    const int n = ui_face_count();
    const uint32_t mask = ui_face_mask();
    /*
     * 当前表盘由调用方通过 face_report_set_current() 告知。
     * 不直接依赖 ui_pages（那会把 ui_face 的纯注册表逻辑拖进页面层依赖），
     * 也便于单测构造任意"当前"值。
     */
    const int current = s_current;

    /* ---- 先算"完整"报告需要多少，决定是否需要截断 ---- */
    char probe[FACE_REPORT_BUF_SIZE];
    sb_t full;
    sb_init(&full, probe, sizeof(probe));
    sb_printf(&full,
              "{\"t\":\"f\",\"c\":%d,\"m\":%u,\"u\":%d",
              n, (unsigned)mask, current);
    sb_put_display(&full);
    /*
     * ⚠️ **紧凑数组**：只出名字（顺序即 id）。
     *
     * 原因见文件头注释 —— iOS 单次 BLE 读取只有 ~184 字节，
     * 带 label/group 的对象格式 6 个表盘就要 371 字节，必然被截断，
     * 结果是**排在最后的照片表盘消失**（用户"看不到照片表盘"的真因）。
     *
     * 现在：`"f":["analog","digital","speed","color","nav","photo"]`
     * → `id` = 下标、`group` 由名字映射、`on` 走 `mask` —— App 自己推导。
     */
    sb_puts(&full, ",\"f\":[");
    for (int i = 0; i < n; i++) {
        const ui_face_desc_t *d = ui_face_get(i);
        if (d == NULL) continue;
        if (full.len > 0 && full.buf[full.len - 1] == '}') sb_putc(&full, ',');
        /*
         * ⚠️ **必须压到 ~184 字节以内**（iOS 的 BLE 单次读取上限 ≈ MTU 185）。
         *
         * 原来每条带 `label`（"Analog" 之类），6 个表盘一共 476 字节 ——
         * 超出一次读取能拿到的量，App 拿到残段 → 判定"列表不完整" →
         * **排在最后的照片表盘直接消失**。用户反复反馈"看不到照片表盘"就是这个。
         *
         * 现在：**去掉 label**（App 本来就有 `?? name` 兜底 + 本地化表），
         * 字段名也缩短。6 个表盘约 150 字节，稳在 184 以内。
         */
        if (i > 0) sb_putc(&full, ',');
        sb_puts_json(&full, d->name);
    }
    sb_puts(&full, "]}");
    const size_t full_len = full.len;
    const bool need_truncate = (full_len > FACE_REPORT_MAX_PAYLOAD) || full.overflow;

    /* ---- 实际输出：完整放得下就直接用 ---- */
    if (!need_truncate) {
        if (full_len + 1 > cap) return -1; /* 连完整报告都放不进调用方缓冲 */
        memcpy(buf, probe, full_len + 1);
        return (int)full_len;
    }

    /* ---- 需要截断：逐条加，直到超过载荷上限 ---- */
    sb_t sb;
    sb_init(&sb, buf, cap);
    sb_printf(&sb,
              "{\"t\":\"f\",\"c\":%d,\"m\":%u,\"u\":%d,\"trunc\":1",
              n, (unsigned)mask, current);
    /*
     * ⚠️ 显示状态放在表头之后、faces 之前，且**截断时也必须带出** ——
     *    否则表盘一多，App 就拿不到亮度/方向，又退化回"默认值覆盖设备"的坑。
     */
    sb_put_display(&sb);
    /*
     * ⚠️ 截断分支也必须是**紧凑名字数组**，与完整分支保持一致。
     *
     * 曾经这里漏改：完整报文用了 `"f":[名字...]`，截断时却仍输出旧的
     * `"faces":[{id,name,label,group,on}...]`。后果是**一旦触发截断**：
     *   - 报文体里没有 `f` 键 → App 走旧格式分支；
     *   - 而 App 的旧分支只认 `truncated`/`cur`，不认新的 `trunc`/`u`
     *     → **截断标志和当前表盘一起丢失**。
     * 截断只在表盘极多时发生（32 个才会触发），所以一直没在真机上暴露 ——
     * 但这是实打实的 bug，不能留着。
     */
    sb_puts(&sb, ",\"f\":[");

    bool first = true;
    for (int i = 0; i < n; i++) {
        const ui_face_desc_t *d = ui_face_get(i);
        if (d == NULL) continue;

        /* 先拼到临时串，判断加进去会不会超上限 */
        char item[96];
        sb_t it;
        sb_init(&it, item, sizeof(item));
        sb_puts_json(&it, d->name);
        if (it.overflow) continue;

        const size_t extra = it.len + (first ? 0 : 1) + 2 /* ]} */;
        if (sb.len + extra > FACE_REPORT_MAX_PAYLOAD) break;

        if (!first) sb_putc(&sb, ',');
        sb_puts(&sb, item);
        first = false;
    }
    sb_puts(&sb, "]}");

    if (sb.overflow) return -1;
    return (int)sb.len;
}
