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
#include "ui_baked_images.h"
#include "ui_img_inflate.h"
#include "ui_nav.h"
#include "ui_nav_map.h"
#include "nav_map.h"
#include "ui_pages.h"
#include "ui_speed.h"
#include "ui_timesrc.h"

#define SIM_W 466
#define SIM_H 466

/* ---- LVGL 显示/输入绑定 ---- */
static lv_disp_draw_buf_t g_draw_buf;
static lv_color_t g_buf1[SIM_W * SIM_H];
static lv_color_t g_buf2[SIM_W * SIM_H];
static lv_color_t g_frame[SIM_W * SIM_H]; /* 完整帧缓存：呈现与截图共用 */

static uint32_t s_flush_cnt = 0; /* 性能诊断：flush 次数（无效化风暴的客观指标） */

static void flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *colors)
{
    s_flush_cnt++;
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

/* 诊断：打印对象类别名 */
static const char *obj_cls_name(lv_obj_t *o)
{
    if (o == NULL) return "null";
    if (lv_obj_check_type(o, &lv_label_class)) return "label";
    if (lv_obj_check_type(o, &lv_img_class)) return "img";
    if (lv_obj_check_type(o, &lv_meter_class)) return "meter";
    if (lv_obj_check_type(o, &lv_arc_class)) return "arc";
    if (lv_obj_check_type(o, &lv_btn_class)) return "btn";
    return "obj";
}

/* 诊断：页面容器收到手势时的日志（--gesture-test 用） */
static void page_gesture_probe_cb(lv_event_t *e)
{
    (void)e;
    printf("[自测] 页面收到手势 dir=%d → 当前页=%d\n",
           (int)lv_indev_get_gesture_dir(lv_indev_get_act()), (int)ui_pages_current());
    fflush(stdout);
}

/*
 * --map-demo：注入一个**合成的地图窗口**用于离线验证渲染。
 *
 * 为什么需要：地图链路的真实数据要手机+网关+BLE 三者齐备才能跑通，
 * 而渲染层（航向朝上变换、道路等级线宽、建筑轮廓、路线）可以在模拟器里
 * 先用合成数据看效果 —— 这样"画得对不对"与"数据通不通"能分开排查。
 *
 * 造一个规则路网 + 少量建筑 + 一条向北偏东的路线，覆盖 ±300m 可见范围。
 */
static void inject_map_demo(void)
{
    nav_map_init();
    const char *begin = "{\"t\":\"mw\"}";
    nav_map_handle(begin, strlen(begin));

    char buf[512];
    /* 横向道路（东西向）：y 固定，x 从 -320 到 320 */
    const int ys[] = { -240, -160, -80, 0, 80, 160, 240 };
    const int ycls[] = { 2, 3, 1, 0, 1, 3, 2 };
    for (int i = 0; i < 7; i++) {
        snprintf(buf, sizeof(buf), "{\"t\":\"mr\",\"c\":%d,\"p\":[[-320,%d],[-160,%d],[0,%d],[160,%d],[320,%d]]}",
                 ycls[i], ys[i], ys[i], ys[i], ys[i], ys[i]);
        nav_map_handle(buf, strlen(buf));
    }
    /* 纵向道路（南北向）：x 固定，y 从 -320 到 320 */
    const int xs[] = { -240, -160, -80, 0, 80, 160, 240 };
    const int xcls[] = { 3, 2, 1, 0, 1, 2, 3 };
    for (int i = 0; i < 7; i++) {
        snprintf(buf, sizeof(buf), "{\"t\":\"mr\",\"c\":%d,\"p\":[[%d,-320],[%d,-160],[%d,0],[%d,160],[%d,320]]}",
                 xcls[i], xs[i], xs[i], xs[i], xs[i], xs[i]);
        nav_map_handle(buf, strlen(buf));
    }
    /* 建筑：每个街区一个方块 */
    const int bx[] = { -220, -140, -60, 20, 100, 180 };
    const int by[] = { -220, -140, -60, 20, 100, 180 };
    for (int i = 0; i < 6; i++) {
        for (int k = 0; k < 3; k++) {
            const int x0 = bx[i] + k * 8;
            const int y0 = by[(i + k) % 6];
            snprintf(buf, sizeof(buf), "{\"t\":\"mb\",\"p\":[[%d,%d],[%d,%d],[%d,%d],[%d,%d]]}",
                     x0, y0, x0 + 46, y0, x0 + 46, y0 + 46, x0, y0 + 46);
            nav_map_handle(buf, strlen(buf));
        }
    }
    /* 路线：从自车向前（北）再右转（东），模拟"前方右转" */
    const char *route = "{\"t\":\"mp\",\"p\":[[0,0],[0,60],[14,120],[46,166],[96,188],[170,196],[250,196]]}";
    nav_map_handle(route, strlen(route));

    const char *end = "{\"t\":\"me\"}";
    nav_map_handle(end, strlen(end));

    /* 自车放在窗口中心 */
    nav_map_set_vehicle_offset(0, 0);
}


int main(int argc, char **argv)
{
    int scale = 2;
    bool offscreen = false;
    const char *shot = NULL;
    Uint32 run_ms = 0;
    int start_page = 0;
    int fh = -1, fm = 0, fs = 0;
    int status_state = -1; /* --status N：设置五态链路状态（用于安全区/文案验证） */
    float speed_kmh = 0.0f;
    int rot_deg = 0;              /* LVGL 软件旋转角度（0/90/180/270）*/
    /* 导航卡片（阶段 1）模拟数据：-1 表示不覆盖，保持表盘默认 */
    int nav_state = -1, nav_maneuver = -1, nav_dist = -1;
    float nav_speed = -1.0f;
    int nofix = 0;
    int driver_state = UI_DRIVER_AUTO;
    int gesture_test = 0;
    int sweep_test = 0;
    int ble_sim = 0;
    int map_demo = 0;          /* --map-demo：注入合成地图窗口 */
    float demo_speed = -1.0f;  /* --demo-speed：演示速度（-1 = 不用）*/
    float map_heading = 0.0f;  /* --map-heading：初始航向 */
    /* 复刻固件 ble_update 每帧调用 setter 的行为（性能回归复现） */

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--scale") && i + 1 < argc) scale = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--offscreen")) offscreen = true;
        else if (!strcmp(argv[i], "--shot") && i + 1 < argc) shot = argv[++i];
        else if (!strcmp(argv[i], "--ms") && i + 1 < argc) run_ms = (Uint32)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--page") && i + 1 < argc) start_page = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--speed") && i + 1 < argc) speed_kmh = (float)atof(argv[++i]);
        /* --rot N：启用 LVGL 软件旋转（0/90/180/270），用于验证旋转正确性与布局 */
        else if (!strcmp(argv[i], "--rot") && i + 1 < argc) rot_deg = atoi(argv[++i]);
        /* 导航卡片模拟数据（阶段 1 验证用）*/
        else if (!strcmp(argv[i], "--nav-state") && i + 1 < argc) nav_state = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--nav-maneuver") && i + 1 < argc) nav_maneuver = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--nav-dist") && i + 1 < argc) nav_dist = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--nav-speed") && i + 1 < argc) nav_speed = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--nofix")) nofix = 1;
        else if (!strcmp(argv[i], "--status") && i + 1 < argc) status_state = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--driver") && i + 1 < argc) driver_state = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--gesture-test")) gesture_test = 1;
        else if (!strcmp(argv[i], "--sweep")) sweep_test = 1;
        else if (!strcmp(argv[i], "--ble-sim")) ble_sim = 1;
        else if (!strcmp(argv[i], "--map-demo")) map_demo = 1;
        /*
         * --demo-speed <kmh>：进入演示模式，并**持续模拟固件的周期状态刷新**
         *   （每帧调一次 ui_speed_set_status(LIVE/STALE/...)）。
         *
         * 这是"演示状态会不会被冲掉"的回归测试：固件每 20ms 刷新状态行，
         * 若 ui_speed_set_status 没有把演示期间的状态让给演示，界面会显示
         * LIVE/STALE 而不是 DEMO —— 曾经就是这样，用户看到的是"状态一直在变、
         * 演示速度卡顿"。
         */
        else if (!strcmp(argv[i], "--demo-speed") && i + 1 < argc) demo_speed = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--map-heading") && i + 1 < argc) map_heading = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--time") && i + 1 < argc) {
            if (sscanf(argv[++i], "%d:%d:%d", &fh, &fm, &fs) < 2) fh = -1;
        }
    }

    /* 冻结时间（截图验证/设计比对用）；未指定则用系统时间 */
    if (fh >= 0) ui_timesrc_freeze(fh, fm, fs);

    /* 速度表盘预置状态（GNSS 未接入前由命令行模拟数据） */
    ui_speed_set_speed(speed_kmh);
    if (demo_speed >= 0.0f) ui_speed_set_demo(demo_speed);
    ui_speed_set_fix(nofix ? false : true, 12);
    if (status_state >= 0) ui_speed_set_status((ui_speed_status_t)status_state);
    ui_speed_set_driver_state(driver_state);

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
    /*
     * 软件旋转（与真机固件同一份 LVGL 代码）：
     *   真机固件已把 sw_rotate 常开，这里保持一致，便于在 PC 上验证
     *   旋转后的画面是否正确（真机无法回读面板内容）。
     *   注意：与 full_refresh / direct_mode 不兼容，本模拟器为 partial 模式。
     */
    disp_drv.sw_rotate = 1;
    switch (rot_deg) {
        case 90:  disp_drv.rotated = LV_DISP_ROT_90;  break;
        case 180: disp_drv.rotated = LV_DISP_ROT_180; break;
        case 270: disp_drv.rotated = LV_DISP_ROT_270; break;
        default:  disp_drv.rotated = LV_DISP_ROT_NONE; break;
    }
    if (rot_deg != 0) printf("已启用软件旋转: %d°\n", rot_deg);
    lv_disp_drv_register(&disp_drv);

    lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = mouse_read_cb;
    lv_indev_drv_register(&indev_drv);

    /* 业务 UI：页面管理（模拟时钟 / 数字时钟 / 速度表盘） */
    /*
     * 与真机走同一条渲染路径：把索引图充气成 RGB565 直通图。
     * 若只在真机充气，像素回归测的就是"索引解码渲染"，
     * 与真机实际渲染路径不一致，回归会失去意义。
     */
    {
        lv_img_dsc_t *imgs[] = { &dial_analog, &dial_speed,
                                 &driver_normal, &driver_spirited, &driver_speeding };
        ui_img_inflate_all(imgs, 5, malloc);
    }

    ui_pages_init();

    /*
     * #2b 扫表动画：必须在 ui_pages_init() **之后**调用——
     * 表盘与指针是在页面构建时创建的，之前调用 s_meter 仍为 NULL 会直接返回。
     */
    if (sweep_test) ui_speed_sweep();

    /* 导航卡片模拟数据：必须在 ui_pages_init() 之后（表盘此时才建好）*/
    if (nav_state >= 0)    ui_nav_set_state(nav_state);
    if (nav_maneuver >= 0) ui_nav_set_maneuver(nav_maneuver);
    if (nav_dist >= 0)     ui_nav_set_distance(nav_dist);
    if (nav_speed >= 0.0f) ui_nav_set_speed(nav_speed, true);

    /* 地图层：合成窗口（--map-demo）用于离线验证渲染；航向可选 */
    if (map_demo) {
        inject_map_demo();
        ui_nav_map_set_heading(map_heading);
        ui_nav_map_invalidate();
    }

    ui_pages_show((ui_page_t)start_page);

    if (gesture_test) {
        /* 诊断：确认手势事件是否真的送到页面容器 */
        for (int i = 0; i < UI_PAGE_COUNT; i++) {
            lv_obj_t *pg = ui_pages_obj((ui_page_t)i);
            if (pg) {
                lv_obj_add_event_cb(pg, page_gesture_probe_cb, LV_EVENT_GESTURE, NULL);
            }
        }
    }

    Uint32 deadline = run_ms ? SDL_GetTicks() + run_ms : 0;
    Uint32 last_tick = SDL_GetTicks();

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

        /*
         * 喂 LVGL 心跳：LV_TICK_CUSTOM=0 时必须周期性调用 lv_tick_inc()，
         * 否则 LVGL 认为时间没有流逝，所有 lv_timer 都不会触发
         * （表现为时钟不走、秒针不动）。真机上由硬件定时器做同样的事。
         */
        Uint32 now_tick = SDL_GetTicks();
        if (now_tick != last_tick) {
            lv_tick_inc(now_tick - last_tick);
            last_tick = now_tick;
        }

        /*
         * 复刻固件行为（--ble-sim）：dashboard.ino 的 ble_update() 每圈主循环
         * 都调用 ui_speed_set_fix()/set_speed()。真机上这曾是无效化风暴的
         * 根因（setter 不做变更检测 → LVGL 无条件重绘）。此模式用于回归复现。
         */
        if (ble_sim) {
            ui_speed_set_fix(false, 0); /* 模拟 BT OFF 分支的每帧调用 */
        }

        /*
         * 手势自测（--gesture-test）：直接驱动 LVGL 输入，验证「按下→拖动→抬起」
         * 能否被判定为手势并切页。用于在没有触摸屏时回归测试切页链路。
         */
        if (gesture_test) {
            Uint32 t = SDL_GetTicks();
            if (t > 300 && t < 900) {
                g_mouse_down = true;
                g_mouse_y = 233;
                g_mouse_x = 33 + (int32_t)((t - 300) * 400 / 600); /* 33 → 433（右拖 400px）*/
            }
            else if (t >= 900 && t < 1400) {
                g_mouse_down = false;
                /* 拖拽结束后打印 LVGL 输入内部状态（诊断手势为何未触发） */
                static int printed = 0;
                if (printed < 1) {
                    printed = 1;
                    lv_indev_t *ind = lv_indev_get_next(NULL);
                    if (ind) {
                        printf("[自测] indev状态: act=(%d,%d) last=(%d,%d) act_obj=%p "
                               "scroll_obj=%p gesture_sent=%d gesture_sum=(%d,%d)\n",
                               (int)ind->proc.types.pointer.act_point.x,
                               (int)ind->proc.types.pointer.act_point.y,
                               (int)ind->proc.types.pointer.last_point.x,
                               (int)ind->proc.types.pointer.last_point.y,
                               (void *)ind->proc.types.pointer.act_obj,
                               (void *)ind->proc.types.pointer.scroll_obj,
                               (int)ind->proc.types.pointer.gesture_sent,
                               (int)ind->proc.types.pointer.gesture_sum.x,
                               (int)ind->proc.types.pointer.gesture_sum.y);
                        /* 追查 act_obj 到底是哪个对象 */
                        lv_obj_t *ao = ind->proc.types.pointer.act_obj;
                        printf("[自测] act_obj=%p (%s)", (void *)ao, obj_cls_name(ao));
                        for (int i = 0; i < UI_PAGE_COUNT; i++) {
                            if (ao == ui_pages_obj((ui_page_t)i)) printf(" == 页面%d", i);
                        }
                        printf("  父链: ");
                        lv_obj_t *pp = ao;
                        for (int d = 0; d < 5 && pp; d++) {
                            printf("%s(%p) ", obj_cls_name(pp), (void *)pp);
                            pp = lv_obj_get_parent(pp);
                        }
                        printf("\n");
                        fflush(stdout);
                    }
                }
            }
            else if (t >= 1400 && t < 1500) {
                static int p1 = 0;
                if (!p1) { p1 = 1; printf("[自测] 右拖 400px 后，当前页 = %d\n", (int)ui_pages_current()); fflush(stdout); }
            }
            else if (t >= 1500 && t < 2100) {
                g_mouse_down = true;
                g_mouse_y = 233;
                g_mouse_x = 433 - (int32_t)((t - 1500) * 400 / 600); /* 433 → 33（左拖 400px）*/
            }
            else if (t >= 2100 && t < 2600) {
                g_mouse_down = false;
            }
            else if (t >= 2600 && t < 2700) {
                static int p2 = 0;
                if (!p2) { p2 = 1; printf("[自测] 左拖 400px 后，当前页 = %d\n", (int)ui_pages_current()); fflush(stdout); }
                /* 命中探测：看页面容器是否可被触摸命中 */
                lv_obj_t *scr = lv_scr_act();
                static int probed = 0;
                if (!probed) {
                probed = 1;
                lv_point_t pt = { 233, 233 };
                printf("[自测] 命中探测 (%d,%d)：屏幕子对象 %d 个\n", (int)pt.x, (int)pt.y,
                       (int)lv_obj_get_child_cnt(scr));
                for (uint32_t i = 0; i < lv_obj_get_child_cnt(scr); i++) {
                    lv_obj_t *c = lv_obj_get_child(scr, i);
                    printf("   #%u hit=%d clickable=%d hidden=%d scrollable=%d scroll_chain=%d\n",
                           (unsigned)i, (int)lv_obj_hit_test(c, &pt),
                           (int)lv_obj_has_flag(c, LV_OBJ_FLAG_CLICKABLE),
                           (int)lv_obj_has_flag(c, LV_OBJ_FLAG_HIDDEN),
                           (int)lv_obj_has_flag(c, LV_OBJ_FLAG_SCROLLABLE),
                           (int)lv_obj_has_flag(c, LV_OBJ_FLAG_SCROLL_CHAIN));
                }
                fflush(stdout);
                }
            }
        }

        /*
         * 与固件一致：每帧都尝试刷新一次链路状态。
         * 演示模式下这**不能**把 DEMO 冲掉（见 ui_speed_set_status 的说明）——
         * 这里刻意用不同的状态轮流灌，最大化暴露该问题。
         */
        if (demo_speed >= 0.0f) {
            static const ui_speed_status_t kCycle[] = {
                UI_STATUS_OK, UI_STATUS_STALE, UI_STATUS_CONNECTING, UI_STATUS_WAIT_LOC
            };
            static int k = 0;
            ui_speed_set_status(kCycle[k++ & 3]);
        }

        ui_nav_map_update(); /* 与固件一致：主循环刷新地图层（内部按航向/位移节流）*/
        ui_nav_set_map_mode(ui_nav_map_has_data());
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
    if (ble_sim || s_flush_cnt > 0) {
        Uint32 elapsed = SDL_GetTicks();
        printf("[性能] flush 总次数=%u，平均 %.1f 次/秒（--ble-sim=%d）\n",
               (unsigned)s_flush_cnt, elapsed ? (double)s_flush_cnt * 1000.0 / elapsed : 0.0, ble_sim);
    }
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
