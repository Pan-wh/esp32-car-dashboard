/**
 * main.c —— ESP32 车载仪表 macOS 模拟器入口
 *
 * 与真机一致的参数：466×466、RGB565（16bit）、LVGL 8.4.0。
 * 圆形遮罩用于还原 1.75" 圆屏的物理可视区域（角部为设备边框区）。
 *
 * 用法：
 *   ./dashboard_sim                                  交互模式（ESC 退出，S 键截图）
 *   ./dashboard_sim --scale 2                         窗口放大 2 倍（默认 2）
 *   ./dashboard_sim --offscreen --ms 1500 --shot out.bmp
 *                                                    无窗口自动运行截图（验证用）
 */
#include <SDL.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl.h"
#include "ui_pages.h"
#include "ui_timesrc.h"

#define SIM_W 466
#define SIM_H 466

/* ---- LVGL 显示/输入绑定 ---- */
static lv_disp_draw_buf_t g_draw_buf;
static lv_color_t g_buf1[SIM_W * SIM_H];
static lv_color_t g_buf2[SIM_W * SIM_H];
static lv_color_t g_frame[SIM_W * SIM_H]; /* 完整帧缓存：呈现与截图共用 */

static void flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *colors)
{
    int32_t w = area->x2 - area->x1 + 1;
    for (int32_t y = area->y1; y <= area->y2; y++) {
        memcpy(&g_frame[y * SIM_W + area->x1], colors, (size_t)w * sizeof(lv_color_t));
        colors += w;
    }
    lv_disp_flush_ready(drv);
}

static int32_t g_mouse_x, g_mouse_y;
static bool g_mouse_down;

static void mouse_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    (void)drv;
    data->point.x = g_mouse_x;
    data->point.y = g_mouse_y;
    data->state = g_mouse_down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

/* ---- 圆形可视区遮罩（ARGB8888）---- */
static Uint32 *g_mask;

static void build_round_mask(void)
{
    g_mask = malloc(SIM_W * SIM_H * sizeof(Uint32));
    float cx = (SIM_W - 1) / 2.0f, cy = (SIM_H - 1) / 2.0f;
    float r = SIM_W / 2.0f;
    for (int y = 0; y < SIM_H; y++) {
        for (int x = 0; x < SIM_W; x++) {
            float dx = (float)x - cx, dy = (float)y - cy;
            float d = sqrtf(dx * dx + dy * dy);
            Uint32 px;
            if (d >= r - 1.5f && d <= r + 1.5f)
                px = 0xFF9AA3AD;  /* 圆形可视边界描边（浅灰） */
            else if (d > r + 1.5f)
                px = 0xFF060708;  /* 圆外：近似设备边框的近黑 */
            else
                px = 0x00000000; /* 圆内：完全透明 */
            g_mask[y * SIM_W + x] = px;
        }
    }
}

/* ---- 24bit BMP 截图（含圆形遮罩，圆外置黑）---- */
static int save_bmp(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;

    int row = SIM_W * 3;
    int pad = (4 - (row % 4)) % 4;
    int stride = row + pad;
    uint32_t data_size = (uint32_t)stride * SIM_H;

    uint8_t fh[14] = { 'B', 'M', 0, 0, 0, 0, 0, 0, 0, 0, 54, 0, 0, 0 };
    uint32_t file_size = 54 + data_size;
    memcpy(fh + 2, &file_size, 4);
    fwrite(fh, 1, 14, f);

    uint8_t ih[40] = { 0 };
    int32_t w32 = SIM_W, h32 = SIM_H;
    int32_t hdr_size = 40;
    memcpy(ih + 0, &hdr_size, 4); /* biSize */
    memcpy(ih + 4, &w32, 4);
    memcpy(ih + 8, &h32, 4);
    ih[12] = 1;  /* planes */
    ih[14] = 24; /* bpp */
    memcpy(ih + 20, &data_size, 4);
    fwrite(ih, 1, 40, f);

    for (int y = SIM_H - 1; y >= 0; y--) {
        for (int x = 0; x < SIM_W; x++) {
            uint16_t c = g_frame[y * SIM_W + x].full;
            uint8_t bgr[3];
            if (g_mask[y * SIM_W + x] >> 24) {
                /* 圆外取遮罩颜色（描边灰/边框黑），ARGB -> BGR */
                Uint32 m = g_mask[y * SIM_W + x];
                bgr[0] = (uint8_t)(m);        /* B */
                bgr[1] = (uint8_t)(m >> 8);   /* G */
                bgr[2] = (uint8_t)(m >> 16);  /* R */
            } else {
                /* RGB565 -> BGR888（BMP 为 BGR 行序） */
                bgr[0] = (uint8_t)(((c >> 0) & 0x1F) << 3);   /* B */
                bgr[1] = (uint8_t)(((c >> 5) & 0x3F) << 2);   /* G */
                bgr[2] = (uint8_t)(((c >> 11) & 0x1F) << 3);  /* R */
            }
            fwrite(bgr, 1, 3, f);
        }
        for (int p = 0; p < pad; p++) fputc(0, f);
    }
    fclose(f);
    printf("已保存截图: %s\n", path);
    return 0;
}

int main(int argc, char **argv)
{
    int scale = 2;
    bool offscreen = false;
    const char *shot = NULL;
    Uint32 run_ms = 0;
    int start_page = 0;
    int fh = -1, fm = 0, fs = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--scale") && i + 1 < argc) scale = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--offscreen")) offscreen = true;
        else if (!strcmp(argv[i], "--shot") && i + 1 < argc) shot = argv[++i];
        else if (!strcmp(argv[i], "--ms") && i + 1 < argc) run_ms = (Uint32)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--page") && i + 1 < argc) start_page = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--time") && i + 1 < argc) {
            if (sscanf(argv[++i], "%d:%d:%d", &fh, &fm, &fs) < 2) fh = -1;
        }
    }

    /* 冻结时间（截图验证/设计比对用）；未指定则用系统时间 */
    if (fh >= 0) ui_timesrc_freeze(fh, fm, fs);

    if (offscreen) SDL_SetHint(SDL_HINT_VIDEODRIVER, "dummy");
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init 失败: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Window *window = NULL;
    SDL_Renderer *renderer = NULL;
    SDL_Surface *off_surf = NULL;

    if (!offscreen) {
        window = SDL_CreateWindow("ESP32 车载仪表模拟器 466x466 RGB565",
                                  SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                  SIM_W * scale, SIM_H * scale, 0);
        renderer = SDL_CreateRenderer(window, -1,
                                      SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    } else {
        off_surf = SDL_CreateRGBSurfaceWithFormat(0, SIM_W * scale, SIM_H * scale,
                                                  32, SDL_PIXELFORMAT_RGB888);
        renderer = SDL_CreateSoftwareRenderer(off_surf);
    }
    if (!renderer) {
        fprintf(stderr, "创建渲染器失败: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Texture *frame_tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB565,
                                               SDL_TEXTUREACCESS_STREAMING, SIM_W, SIM_H);
    build_round_mask();
    SDL_Surface *mask_surf = SDL_CreateRGBSurfaceWithFormatFrom(
        g_mask, SIM_W, SIM_H, 32, SIM_W * 4,
        SDL_PIXELFORMAT_ARGB8888);
    SDL_Texture *mask_tex = SDL_CreateTextureFromSurface(renderer, mask_surf);
    SDL_SetTextureBlendMode(mask_tex, SDL_BLENDMODE_BLEND);

    /* LVGL 初始化（与官方示例一致的注册流程） */
    lv_init();
    lv_disp_draw_buf_init(&g_draw_buf, g_buf1, g_buf2, SIM_W * SIM_H);

    lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = SIM_W;
    disp_drv.ver_res = SIM_H;
    disp_drv.flush_cb = flush_cb;
    disp_drv.draw_buf = &g_draw_buf;
    lv_disp_drv_register(&disp_drv);

    lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = mouse_read_cb;
    lv_indev_drv_register(&indev_drv);

    /* 业务 UI：页面管理（模拟时钟 / 数字时钟） */
    ui_pages_init();
    ui_pages_show((ui_page_t)start_page);

    Uint32 deadline = run_ms ? SDL_GetTicks() + run_ms : 0;

    for (;;) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) goto done;
            if (e.type == SDL_KEYDOWN) {
                if (e.key.keysym.sym == SDLK_ESCAPE) goto done;
                if (e.key.keysym.sym == SDLK_s) save_bmp("screenshot.bmp");
                /* 方向键 / A、D 切页（仅模拟器便利键，设备上靠滑动手势） */
                if (e.key.keysym.sym == SDLK_RIGHT || e.key.keysym.sym == SDLK_d)
                    ui_pages_show((ui_page_t)((ui_pages_current() + 1) % UI_PAGE_COUNT));
                if (e.key.keysym.sym == SDLK_LEFT || e.key.keysym.sym == SDLK_a)
                    ui_pages_show((ui_page_t)((ui_pages_current() + UI_PAGE_COUNT - 1) % UI_PAGE_COUNT));
            }
            if (e.type == SDL_MOUSEMOTION) {
                g_mouse_x = e.motion.x / scale;
                g_mouse_y = e.motion.y / scale;
            }
            if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == 1) g_mouse_down = true;
            if (e.type == SDL_MOUSEBUTTONUP && e.button.button == 1) g_mouse_down = false;
        }

        lv_timer_handler();

        SDL_UpdateTexture(frame_tex, NULL, g_frame, SIM_W * (int)sizeof(lv_color_t));
        SDL_SetRenderDrawColor(renderer, 6, 7, 8, 255);
        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, frame_tex, NULL, NULL);
        SDL_RenderCopy(renderer, mask_tex, NULL, NULL);
        SDL_RenderPresent(renderer);

        if (deadline && SDL_TICKS_PASSED(SDL_GetTicks(), deadline)) {
            if (shot) save_bmp(shot);
            break;
        }
        SDL_Delay(5);
    }

done:
    if (shot && !deadline) save_bmp(shot);
    SDL_DestroyTexture(frame_tex);
    SDL_DestroyTexture(mask_tex);
    SDL_FreeSurface(mask_surf);
    if (off_surf) SDL_FreeSurface(off_surf);
    if (renderer) SDL_DestroyRenderer(renderer);
    if (window) SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
