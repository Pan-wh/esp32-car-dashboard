/**
 * nmea_replay.c —— NMEA 日志回放实现（仅模拟器用）
 */
#include "nmea_replay.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui/gnss_nmea.h"
#include "ui/ui_speed.h"

#define MAX_GROUPS   2048
#define MAX_LINE_LEN 160

/* 每组语句（一组 = 一条 RMC 起的若干句） */
typedef struct {
    int first; /* 在 line_offsets 中的起始下标 */
    int count;
} group_t;

static char *s_buf;               /* 整个文件内容 */
static size_t s_buf_len;
static char *s_lines[MAX_GROUPS]; /* 行指针（仅存指针，行内已用 \0 分隔） */
static group_t s_groups[MAX_GROUPS];
static int s_line_count;
static int s_group_count;

static bool s_active;
static float s_speedup = 1.0f;
static uint32_t s_start_ms;
static int s_cur_group = -1;

static gnss_ctx_t s_ctx;

bool nmea_replay_load(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) return false;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) {
        fclose(f);
        return false;
    }

    s_buf = (char *)malloc((size_t)sz + 1);
    if (s_buf == NULL) {
        fclose(f);
        return false;
    }
    s_buf_len = fread(s_buf, 1, (size_t)sz, f);
    s_buf[s_buf_len] = '\0';
    fclose(f);

    /* 切行 */
    s_line_count = 0;
    char *p = s_buf;
    while (*p != '\0' && s_line_count < MAX_GROUPS) {
        char *nl = strchr(p, '\n');
        if (nl != NULL) *nl = '\0';
        /* 去掉行尾 \r */
        size_t n = strlen(p);
        if (n > 0 && p[n - 1] == '\r') p[n - 1] = '\0';
        if (p[0] == '$') s_lines[s_line_count++] = p;
        if (nl == NULL) break;
        p = nl + 1;
    }

    /* 分组：遇到 RMC 开新组 */
    s_group_count = 0;
    for (int i = 0; i < s_line_count; i++) {
        const char *line = s_lines[i];
        /* 语句形如 $GNRMC,... → 以 RMC 作为每组的起点（后续 GGA/GSA/VTG 归入同组） */
        bool is_rmc = (line[0] == '$') && (strstr(line, "RMC,") != NULL);

        if (is_rmc || s_group_count == 0) {
            s_groups[s_group_count].first = i;
            s_groups[s_group_count].count = 0;
            s_group_count++;
            if (s_group_count >= MAX_GROUPS) break;
        }
        s_groups[s_group_count - 1].count++;
    }

    gnss_init(&s_ctx);
    s_active = false;
    s_cur_group = -1;
    return s_group_count > 0;
}

void nmea_replay_start(float speedup)
{
    if (s_group_count <= 0) return;
    s_speedup = (speedup > 0.05f) ? speedup : 1.0f;
    s_start_ms = 0; /* 首次 update 时设定 */
    s_active = true;
    s_cur_group = -1;
}

/* 打印解析结果（NMEA_REPLAY_VERBOSE=1 时启用，便于核对链路） */
static void report(void)
{
    if (getenv("NMEA_REPLAY_VERBOSE") == NULL) return;
    printf("[replay] 组 %3d/%d  有效=%d  速度=%6.2f km/h  航向=%5.1f  卫星=%2d  定位=%s\n",
           s_cur_group, s_group_count, (int)s_ctx.data.valid, (double)s_ctx.data.speed_kmh,
           (double)s_ctx.data.course_deg, s_ctx.data.sats_used,
           s_ctx.data.fix == GNSS_FIX_3D ? "3D" : (s_ctx.data.fix == GNSS_FIX_2D ? "2D" : "无"));
    fflush(stdout);
}

/* 把解析结果推给速度页 */
static void push_to_ui(void)
{
    bool usable = gnss_is_usable(&s_ctx);
    ui_speed_set_speed(usable ? s_ctx.data.speed_kmh : 0.0f);
    ui_speed_set_fix(usable, s_ctx.data.sats_used);
    ui_speed_set_heading((int)(s_ctx.data.course_deg + 0.5f));
}

void nmea_replay_update(uint32_t now_ms)
{
    if (!s_active) return;
    if (s_start_ms == 0) s_start_ms = now_ms;

    gnss_set_time(&s_ctx, now_ms);

    uint32_t elapsed = now_ms - s_start_ms;
    int target = (int)((float)elapsed * s_speedup / 1000.0f);

    if (s_group_count > 0) target = target % s_group_count; /* 循环回放 */

    /* 推进并喂入新组（跨圈时重置解析器，避免沿用上一圈数据） */
    if (target < s_cur_group) {
        gnss_init(&s_ctx);
        s_cur_group = -1;
    }

    int guard = 0;
    while (s_cur_group < target && guard++ < s_group_count) {
        s_cur_group++;
        const group_t *g = &s_groups[s_cur_group];
        for (int i = 0; i < g->count; i++) {
            gnss_parse_line(&s_ctx, s_lines[g->first + i]);
        }
        push_to_ui();
        report();
    }
}

bool nmea_replay_active(void)
{
    return s_active;
}

int nmea_replay_group(void)
{
    return s_cur_group;
}

int nmea_replay_group_count(void)
{
    return s_group_count;
}
