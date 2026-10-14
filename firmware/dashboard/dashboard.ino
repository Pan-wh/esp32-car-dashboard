/**
 * ESP32 车载仪表 —— 固件主程序
 *
 * 板卡：微雪 ESP32-S3-Touch-AMOLED-1.75-G（ESP32-S3R8 / CO5300 466×466 QSPI AMOLED）
 *
 * 初始化流程改写自微雪官方示例（**保持与官方一致的引脚、总线和顺序**）：
 *   - 显示：01_HelloWorld / 03_LVGL_PCF85063_simpleTime（Arduino_ESP32QSPI + Arduino_CO5300）
 *   - LVGL：03（flush / rounder / draw_buf）与 06（esp_timer 提供 lv_tick_inc）
 *   - 触摸：06_LVGL_Widgets（TouchDrvCST92xx，I2C 0x5A）
 *   - RTC ：03（SensorPCF85063）
 *
 * UI 与数据逻辑位于共享源码（simulator/ui/），由 tools/sync_firmware.sh 同步到本目录，
 * 保证模拟器与固件使用同一份代码。
 *
 * 构建：
 *   tools/sync_firmware.sh
 *   arduino-cli compile --fqbn "esp32:esp32:esp32s3:FlashSize=16M,PartitionScheme=app3M_fat9M_16MB" \
 *     --libraries reference/ESP32-S3-Touch-AMOLED-1.75/examples/arduino/libraries firmware/dashboard
 *
 * 数据源：iPhone Core Location 经 BLE 下发（本板**无** LC76G GNSS）。
 *   未连接 / 无定位 / 数据过期时，速度页显示无效状态（不伪造旧值）。
 */
#include <Arduino.h>
#include <Preferences.h>   /* NVS：持久化屏幕方向等用户设置 */
#include <esp_heap_caps.h> /* PSRAM 分配 */
#include <Wire.h>
#include <SPI.h>
#include <lvgl.h>
#include <time.h>       /* time/gmtime/time_t */
#include <sys/time.h>   /* settimeofday/timeval */

#include "Arduino_GFX_Library.h"
#include "pin_config.h"        /* 由 libraries/Mylibrary 提供 */
#include "lv_conf.h"           /* 由 libraries/ 提供（LVGL 8.4 配置） */
#include "SensorPCF85063.hpp"
#include "TouchDrvCSTXXX.hpp"  /* SensorLib 提供的 CST9217 驱动 */

/* 共享模块（C 代码，需 extern "C") */
extern "C" {
#include "ui_face.h"
#include "face_report.h"   /* 表盘列表上报（App 据此渲染）*/
#include "ui_pages.h"
#include "ui_speed.h"
#include "ui_nav.h"      /* 导航指引卡片（阶段 1：UI 原型） */
#include "ui_baked_images.h" /* 烘焙图声明（非 const，启动时可充气）*/
#include "ui_img_inflate.h"  /* 索引图 → PSRAM RGB565 直通图 */
#include "nav.h"         /* 导航状态/动作枚举与文案（纯 C，共用） */
#include "ui_clock.h"    /* 数字时钟页（时间同步状态标签） */
#include "ui_timesrc.h"
#include "telemetry.h"   /* BLE 遥测协议解析（纯 C，模拟器/固件共用） */
}

#include "ble_telemetry.h" /* BLE Peripheral（固件侧，C++） */

/* ---------------- 可调参数 ---------------- */

#define EXAMPLE_LVGL_TICK_PERIOD_MS 2      /* 与官方示例一致 */
#define DEFAULT_PAGE UI_PAGE_SPEED         /* 开机默认页：速度表盘（车机主视角） */

/*
 * 屏幕内容旋转（整机安装方向固定，故用固定值）：
 *   0 = 不旋转   1 = 90°   2 = 180°   3 = 270°
 *
 * 实现方式（两条独立路线，可用串口命令实测对比）：
 *   A) LVGL 软件旋转 —— 官方支持，`06_LVGL_Widgets` 即用此法做四向切换。
 *      · 180°：原地首尾反转，**无需临时缓冲**，成本低（draw_buf_rotate_180）
 *      · 90°/270°：转置。本屏是**正方形**，全屏脏区正好走
 *        `draw_buf_rotate_90_sqr()` 的**原地 4 向旋转**（不分配临时缓冲）；
 *        非正方形的小脏区才用 LV_DISP_ROT_MAX_BUF(10KB) 分块转置。
 *      · 运行时可切换：`lvrot 0|1|2|3`
 *   B) 面板 MADCTL 硬件旋转 —— 若 CO5300 的 MV 位有效，则**零每帧开销**，
 *      且文字会跟着一起正确旋转（无需改界面）。库未使用该位，需实测：
 *      `madctl 60`(90°) / `C0`(180°) / `A0`(270°)，恢复 `madctl 0`
 *
 * ⚠️ 软件旋转与 full_refresh / direct_mode 不兼容（LVGL 明确禁止）。
 *    本工程为 partial 刷新模式，故可用；若日后改用全帧刷新需重新评估。
 */
#define DISPLAY_ROT 0

/* ---------------- 显示 ---------------- */

static Arduino_DataBus *bus = new Arduino_ESP32QSPI(
    LCD_CS /* CS */, LCD_SCLK /* SCK */, LCD_SDIO0 /* SDIO0 */, LCD_SDIO1 /* SDIO1 */,
    LCD_SDIO2 /* SDIO2 */, LCD_SDIO3 /* SDIO3 */);

/* QSPI 时钟（Hz）：默认 40MHz；滑页性能实验提高到 80MHz，可改回 40000000 */
#ifndef QSPI_SPEED_HZ
#define QSPI_SPEED_HZ 80000000
#endif

static Arduino_CO5300 *gfx = new Arduino_CO5300(
    bus, LCD_RESET /* RST */, 0 /* rotation */, LCD_WIDTH /* width */, LCD_HEIGHT /* height */,
    6, 0, 0, 0);

static lv_disp_draw_buf_t draw_buf;
static lv_color_t buf[LCD_WIDTH * LCD_HEIGHT / 10]; /* 与官方示例相同的缓冲大小 */

/* 性能诊断：重绘/循环速率（卡顿回归的客观指标，见 docs/troubleshooting.md）*/
static volatile uint32_t s_flush_cnt = 0;
static uint32_t s_loop_cnt = 0;
static volatile uint32_t s_render_us_max = 0; /* 窗口内单次 lv_timer_handler 最大耗时 */
static volatile uint32_t s_render_us_sum = 0;  /* 窗口内渲染总耗时（算占空比） */
static volatile uint32_t s_flush_us_sum = 0;   /* 窗口内 QSPI 传输累计耗时（从渲染耗时中区分） */
static volatile uint32_t s_flush_us_max = 0;   /* 窗口内单次 flush 最大耗时 */

/*
 * 滑动卡顿优化：全帧 PSRAM 绘制缓冲（partial 模式不变）。
 * 现状：内部 1/10 屏缓冲 → 整屏脏区被切成 ~10 块，仪表（100+ 刻度）与
 * 300×300 插画**每块都完整重算一遍**（LVGL 8 对象绘制按块执行）。
 * 全帧缓冲 → 任何脏区只算 1 块 → 渲染成本 ÷10；flush 仍只发脏区
 * （空闲小脏区行为不变，不学 direct_mode 的全屏发送）。
 * 依据：厂商 IDF BSP（03_esp-brookesia）本身就用 PSRAM 全帧缓冲；
 * 本机芯片 ESP32-S3R8 = 8MB 八线 PSRAM → 构建选项 PSRAM=opi。
 */
static lv_disp_draw_buf_t s_draw_buf_psram;
static lv_color_t *s_buf_psram = NULL;
static bool s_buf_psram_active = false;

/* ---------------- 触摸 ---------------- */

static TouchDrvCST92xx touch;
static int16_t touch_x[5], touch_y[5];
static int16_t last_x = 0, last_y = 0;

/* 触摸采样环形缓冲（诊断用：`touch` 命令打印最近若干次采样） */
#define TOUCH_LOG_N 40
typedef struct {
  uint32_t ms;
  int16_t x, y;
  uint8_t pressed;
} touch_log_t;
static touch_log_t s_tlog[TOUCH_LOG_N];
static uint8_t s_tlog_head = 0, s_tlog_cnt = 0;

/* 手势计数（诊断用：LVGL 是否识别到手势、方向为何） */
static uint16_t s_gest_left = 0, s_gest_right = 0, s_gest_top = 0, s_gest_bottom = 0;
static uint16_t s_gest_other = 0;

/* 最近一次手势的细节（持久化，便于事后读取） */
static struct {
  const char *dir;
  const char *target;  /* "screen" / "page" / "其它" */
  int page_before, page_after;
} s_last_gest = { "-", "-", -1, -1 };
static int s_page_seen = -1; /* loop 中持续记录，便于取 before */

/*
 * ---- 触摸锁存（修复「滑页无反应」）----
 * 问题：一帧渲染要 200ms 左右，LVGL 的 indev 读取被拖慢到 ~4 次/秒。
 * 若用户快速滑动（按下→移动→抬起）整个发生在一次渲染阻塞期间，LVGL 下次读
 * 到的已经是「松开」状态 → 完全感知不到这次触摸 → 表现为「操作无反应」。
 * 解决：用 esp_timer 以 100Hz 独立采样触摸（不受渲染阻塞影响），并**锁存按下**：
 * LVGL 每次读取时，若期间发生过按下，就补报一次 PRESSED，保证它一定看得见
 * 一个完整的 按下→（移动）→松开 序列。
 */
static volatile bool s_tp_pressed = false;  /* 最新采样：是否按下 */
static volatile int16_t s_tp_x = 0, s_tp_y = 0;
static volatile bool s_tp_press_latch = false; /* 自上次 LVGL 读取后发生过按下 */
/* flick（快速划动）判定：100Hz 采样能拿到完整轨迹，见 ui_pages_flick 注释 */
static volatile int8_t s_tp_flick = 0;      /* +1 下一页 / -1 上一页 / 0 无 */
static volatile bool s_tp_settle = false;   /* 抬起但未达 flick：按最近页吸附 */
static int16_t s_tp_start_x = 0, s_tp_start_y = 0;
static uint32_t s_tp_start_ms = 0;
/* 诊断：采样器共看到多少次「按下→抬起」、其中多少次判定为 flick */
static volatile uint16_t s_tp_release_cnt = 0, s_tp_flick_cnt = 0;
static volatile int16_t s_last_dx = 0, s_last_dy = 0;
static volatile uint32_t s_last_dt = 0;
static portMUX_TYPE s_tp_mux = portMUX_INITIALIZER_UNLOCKED;

/*
 * flick 方向判定的**旋转补偿**（详见 docs/rotation-tearing-fieldtest.md）。
 * 采样拿到的是**面板原始坐标**，而"左滑/右滑"应按 **LVGL 逻辑坐标**判定。
 * LVGL 自己的 indev 会对指针坐标做逆变换（lv_indev.c: indev_pointer_proc），
 * 但这条 100Hz 自建快路径绕过了它 → 90°/270° 时"左右滑"在面板上变成上下滑，
 * flick 判不出来，翻页退化成慢速原生拖拽。
 * 默认开启；串口 `flickrot 0|1` 可运行时 A/B，现场对比用。
 */
static bool s_flick_rot_fix = true;

/** 面板原始位移 → LVGL 逻辑位移（映射与 lv_indev.c 的旋转规则严格一致）*/
static void flick_delta_to_logical(int *dx, int *dy)
{
  if (!s_flick_rot_fix) return;
  lv_disp_t *d = lv_disp_get_default();
  if (d == NULL || d->driver == NULL) return;

  int rx = *dx, ry = *dy;   /* 面板原始位移 */
  switch (d->driver->rotated) {
    case LV_DISP_ROT_90:  *dx = -ry; *dy =  rx; break;  /* x'=V-y-1, y'=x */
    case LV_DISP_ROT_180: *dx = -rx; *dy = -ry; break;  /* 双轴取反 */
    case LV_DISP_ROT_270: *dx =  ry; *dy = -rx; break;  /* x'=y, y'=H-x-1 */
    default: break;                                     /* NONE：原始即逻辑 */
  }
}

static void tlog_add(int16_t x, int16_t y, bool pressed)
{
  s_tlog[s_tlog_head].ms = millis();
  s_tlog[s_tlog_head].x = x;
  s_tlog[s_tlog_head].y = y;
  s_tlog[s_tlog_head].pressed = pressed ? 1 : 0;
  s_tlog_head = (s_tlog_head + 1) % TOUCH_LOG_N;
  if (s_tlog_cnt < TOUCH_LOG_N) s_tlog_cnt++;
}

/* ---------------- BLE 遥测（iPhone Core Location 数据源）---------------- */

/*
 * 时间重校阈值：已同步后，只有偏差超过该值才重写 RTC。
 *
 * ⚠️ 曾用 2s 阈值，实测会导致时钟界面隔几秒"重置"一下：
 *    time(NULL) 只有**秒级**精度，而手机发来的是**毫秒级** UTC，
 *    两者边界错开时 diff 天然可达 ±1000ms，再叠加传输/处理延迟就逼近 2s，
 *    于是频繁触发重校 → 反复写 RTC → 时钟跳变。
 * 手机在驾驶期间持续 2Hz 发送时间戳，仪表本地走时足够准，
 * 因此改为「**首次校时 + 仅偏差过大才重校**」，正常驾驶期间零重写。
 */
#define TIME_RESYNC_THRESHOLD_MS 30000

/* 数据过期阈值：超过该时长无有效遥测 → 速度置无效（不显示旧值） */
#define BLE_STALE_MS 2500

/* 显示/存储时区：UTC+8（中国标准时间，无夏令时），第一版固定 */
#define TZ_OFFSET_HOURS 8

/* 主循环消费的遥测状态 */
static telemetry_msg_t s_tel;
static bool s_tel_valid = false; /* 最新一包是否有可用速度/定位 */
static bool s_had_data = false;  /* 是否收到过任意遥测（区分 WAIT LOC 与 STALE） */
static ui_speed_status_t s_last_status = UI_STATUS_CONNECTING;

/* 行程统计（功能④）：主循环按周期累积；纯逻辑在 telemetry.c（已单测） */
static telemetry_trip_t s_trip;
static uint32_t s_trip_last_ms = 0;
#define TRIP_STEP_MS 200 /* 每 200ms 累积一步 */

static uint32_t s_implausible = 0; /* 被丢弃的不合理包计数（§7） */
static telemetry_ramp_t s_ramp;      /* 速度线性斜坡（#2：按时间匀速逼近目标，消除跳针）*/
static telemetry_debounce_t s_deb;   /* 状态显示防抖（抑制瞬态 STALE 闪烁）*/
static const char *s_reset_reason = "未知"; /* 本次启动的复位原因（启动时记录） */
static int s_reset_reason_code = -1;        /* esp_reset_reason() 原始值 */

/* 时间同步状态 */
static bool s_time_synced = false;
static uint32_t s_last_sync_ms = 0;      /* 本机 millis，上次同步时刻 */
static int64_t s_last_sync_utc_ms = 0;   /* 上次同步的 UTC epoch 毫秒 */

/* ---------------- RTC ---------------- */

static SensorPCF85063 rtc;
static bool rtc_ok = false;

/*
 * 时间提供者：把 RTC 时间交给共享 UI 代码（ui_clock / ui_analog 不关心时间来源）。
 * RTC_DateTime::getWeek() 约定 0 = 周日，与 struct tm.tm_wday 一致。
 */
static void rtc_time_provider(struct tm *out)
{
  if (!rtc_ok) {
    /* RTC 不可用时保持 0，界面会显示无效应有的样子（不伪造时间） */
    memset(out, 0, sizeof(*out));
    return;
  }

  RTC_DateTime dt = rtc.getDateTime();
  out->tm_year = (int)dt.getYear() - 1900;
  out->tm_mon = (int)dt.getMonth() - 1;
  out->tm_mday = (int)dt.getDay();
  out->tm_hour = (int)dt.getHour();
  out->tm_min = (int)dt.getMinute();
  out->tm_sec = (int)dt.getSecond();
  out->tm_wday = (int)dt.getWeek();
}

/* ---------------- LVGL 移植 ---------------- */

/* 官方示例：把刷新区域对齐到偶数边界（CO5300 QSPI 要求） */
static void rounder_cb(struct _lv_disp_drv_t *disp_drv, lv_area_t *area)
{
  if (area->x1 % 2 != 0) area->x1--;
  if (area->y1 % 2 != 0) area->y1--;
  if (area->x2 % 2 == 0) area->x2++;
  if (area->y2 % 2 == 0) area->y2++;
}

static void disp_flush_cb(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p)
{
  uint32_t t_flush = micros();
  s_flush_cnt++;
  uint32_t w = (area->x2 - area->x1 + 1);
  uint32_t h = (area->y2 - area->y1 + 1);

#if (LV_COLOR_16_SWAP != 0)
  gfx->draw16bitBeRGBBitmap(area->x1, area->y1, (uint16_t *)&color_p->full, w, h);
#else
  gfx->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t *)&color_p->full, w, h);
#endif

  {
    uint32_t dt_flush = micros() - t_flush;
    s_flush_us_sum += dt_flush;
    if (dt_flush > s_flush_us_max) s_flush_us_max = dt_flush;
  }
  lv_disp_flush_ready(disp);
}

/*
 * 触摸读取：每帧轮询（比官方示例的「仅靠中断置位」更简单可靠，
 * 且能持续上报位置 —— LVGL 的手势（滑页）需要按住期间的连续坐标）。
 */
/* 100Hz 触摸采样（esp_timer 回调，独立于主循环/渲染） */
static void touch_sample_cb(void *)
{
  uint8_t n = touch.getPoint(touch_x, touch_y, touch.getSupportTouchPoint());
  bool was = s_tp_pressed;

  portENTER_CRITICAL(&s_tp_mux);
  if (n > 0) {
    if (!was) { /* 按下瞬间：记录起点，用于 flick 判定 */
      s_tp_start_x = touch_x[0];
      s_tp_start_y = touch_y[0];
      s_tp_start_ms = millis();
    }
    s_tp_pressed = true;
    s_tp_x = touch_x[0];
    s_tp_y = touch_y[0];
    s_tp_press_latch = true; /* 记录「期间按下过」 */
  }
  else {
    s_tp_pressed = false;
    if (was) {
      /*
       * 抬起瞬间：判定是否快速划动。
       * 阈值：时长 < 600ms、水平位移 > 50px、且明显偏水平（水平 > 1.2×垂直）。
       * ⚠️ 位移在**逻辑坐标**下判定：原始面板位移先按当前屏幕方向做逆变换，
       *    否则 90°/270° 时"左右滑"在面板上变成上下滑，flick 永远判不出来。
       * 命中则置 flick，由主循环消费（LVGL 调用必须回到主循环做）。
       */
      int raw_dx = (int)s_tp_x - (int)s_tp_start_x;
      int raw_dy = (int)s_tp_y - (int)s_tp_start_y;
      uint32_t dt = millis() - s_tp_start_ms;
      int dx = raw_dx, dy = raw_dy;
      flick_delta_to_logical(&dx, &dy);   /* 逻辑坐标（判定与翻页方向都用它）*/
      int adx = dx < 0 ? -dx : dx;
      int ady = dy < 0 ? -dy : dy;
      s_tp_release_cnt++;
      s_last_dx = (int16_t)dx;
      s_last_dy = (int16_t)dy;
      s_last_dt = dt;
      if (dt < 600 && adx > 50 && adx > (ady * 12) / 10) {
        s_tp_flick = (dx < 0) ? 1 : -1; /* 左滑 → 下一页 */
        s_tp_flick_cnt++;
        Serial.printf("[触摸] flick dx=%d dy=%d（原始 %d,%d）dt=%lu ms\n", (int)dx, (int)dy,
                      raw_dx, raw_dy, (unsigned long)dt);
      }
      else {
        s_tp_settle = true; /* 缓慢拖拽：按最近页吸附 */
        Serial.printf("[触摸] 抬起 dx=%d dy=%d（原始 %d,%d）dt=%lu ms（未达 flick 阈值→吸附）\n",
                      (int)dx, (int)dy, raw_dx, raw_dy, (unsigned long)dt);
      }
    }
  }
  portEXIT_CRITICAL(&s_tp_mux);
}

static void touchpad_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
  bool pressed, latch;
  int16_t x, y;

  portENTER_CRITICAL(&s_tp_mux);
  pressed = s_tp_pressed;
  latch = s_tp_press_latch;
  x = s_tp_x;
  y = s_tp_y;
  if (latch) s_tp_press_latch = false; /* 消费掉锁存 */
  portEXIT_CRITICAL(&s_tp_mux);

  /*
   * 只要最新采样是按下，或期间按下过（锁存），就报 PRESSED。
   * 锁存保证「快速滑动恰好落在一次渲染阻塞内」时 LVGL 仍能收到按下事件，
   * 下一次读取才报 RELEASED，从而形成完整的按下→移动→松开序列。
   */
  if (pressed || latch) {
    last_x = x;
    last_y = y;
    data->state = LV_INDEV_STATE_PRESSED;
  }
  else {
    data->state = LV_INDEV_STATE_RELEASED;
  }
  tlog_add(last_x, last_y, pressed || latch);

  data->point.x = last_x;
  data->point.y = last_y;
}

static void lvgl_tick_cb(void *arg)
{
  (void)arg;
  lv_tick_inc(EXAMPLE_LVGL_TICK_PERIOD_MS);
}

/* ---------------- 设备控制（BLE 下行：亮度 / 息屏）---------------- */

static uint8_t s_brightness = 200;   /* 当前亮度（0-255）*/
static bool s_screen_off = false;    /* 息屏状态 */
#define BRIGHTNESS_DEFAULT 200

/* 应用亮度（带限幅；0 视为最低可见亮度，见 control_clamp_brightness） */
static void apply_brightness(uint8_t v)
{
  s_brightness = v;
  gfx->setBrightness(v);
}

static void apply_screen(bool off)
{
  if (off == s_screen_off) return;
  s_screen_off = off;
  if (off) {
    gfx->displayOff();
  }
  else {
    gfx->displayOn();
    gfx->setBrightness(s_brightness); /* 唤醒后恢复亮度 */
  }
}

/* 消费一条 BLE 下行控制命令（主循环调用，BLE 回调只暂存） */
/*
 * ---------------- 屏幕方向（LVGL 软件旋转）----------------
 *
 * 说明：本屏为 1.75" **圆形** 466×466，旋转后仍是同一个圆，
 *       无长宽比/布局副作用；触摸坐标由 LVGL 自动逆变换（indev_pointer_proc）。
 *
 * 持久化：方向存入 NVS，重启后自动恢复（用户设置不应每次掉电丢失）。
 */
#define ROT_PREF_NS  "dash"
#define ROT_PREF_KEY "rot"
/*
 * 表盘记忆：断电后沿用上次的表盘与启用/隐藏状态。
 * 沿用 rot 的同一个 NVS 命名空间，便于集中管理用户设置。
 */
#define FACE_PREF_KEY  "face"  /* 当前表盘 id */
#define FMASK_PREF_KEY "fmask" /* 启用位掩码（bit i = face id i）*/

static int s_display_rot = -1; /* 当前方向（度）；-1 = 尚未设置 */

/** 角度 → lv_disp_rot_t（非 90 倍数按 0 处理；正常输入由 control.c 保证合法） */
static lv_disp_rot_t rot_deg_to_enum(int deg)
{
  switch (((deg % 360) + 360) % 360) {
    case 90:  return LV_DISP_ROT_90;
    case 180: return LV_DISP_ROT_180;
    case 270: return LV_DISP_ROT_270;
    default:  return LV_DISP_ROT_NONE;
  }
}

/* ---- 表盘记忆与列表上报 ---- */

/** 上一次观察到的表盘 id（-1 = 尚未取过），用于检测变化并持久化 */
static int s_last_face = -1;
/** 表盘列表或启用状态有变化，需要重新上报 */
static bool s_faces_dirty = false;

static void save_face_prefs(int face_id, uint32_t mask)
{
  Preferences prefs;
  if (prefs.begin(ROT_PREF_NS, false)) {
    prefs.putInt(FACE_PREF_KEY, face_id);
    prefs.putUInt(FMASK_PREF_KEY, mask);
    prefs.end();
  }
}

/**
 * 启动时恢复表盘状态（断电记忆）。
 *
 * 两件事：先恢复"哪些表盘启用/隐藏"，再恢复"当前是哪个表盘"。
 * 顺序不能反 —— 当前表盘必须是一个**已启用**的 id。
 *
 * 对 NVS 里的旧值做了防御：掩码若与当前注册表对不上（固件升级后表盘增减），
 * 或会导致一个表盘都不显示，则**整份忽略**并退回默认，而不是让仪表黑着。
 */
static void restore_face_state(void)
{
  Preferences prefs;
  int saved_face = -1;
  uint32_t saved_mask = 0;
  bool have_mask = false, have_face = false;

  if (prefs.begin(ROT_PREF_NS, true)) {
    have_mask = prefs.isKey(FMASK_PREF_KEY);
    saved_mask = prefs.getUInt(FMASK_PREF_KEY, 0);
    have_face = prefs.isKey(FACE_PREF_KEY);
    saved_face = prefs.getInt(FACE_PREF_KEY, -1);
    prefs.end();
  }

  const int n = ui_face_count();
  bool mask_ok = false;

  if (have_mask && n > 0) {
    /* 只接受落在已注册表盘范围内、且至少启用一个的掩码 */
    uint32_t valid = (n >= 32) ? 0xFFFFFFFFu : ((1u << n) - 1u);
    uint32_t use = saved_mask & valid;
    if (use != 0) {
      for (int i = 0; i < n; i++) {
        ui_face_set_enabled(i, (use & (1u << i)) != 0);
      }
      mask_ok = true;
    }
  }

  int target = -1;
  if (have_face && saved_face >= 0 && saved_face < n && ui_face_is_enabled(saved_face)) {
    target = saved_face;
  }
  if (target < 0) {
    /* 没有记录、或记录的表盘已被隐藏 → 用第一个启用的表盘 */
    for (int i = 0; i < n; i++) {
      if (ui_face_is_enabled(i)) {
        target = i;
        break;
      }
    }
  }
  if (target >= 0) ui_pages_set_face(target);

  s_last_face = ui_pages_current_face();
  Serial.printf("[表盘] 启动恢复：当前=%d(%s)，%d/%d 启用（掩码来源：%s）；表盘记忆来源：%s\n",
                s_last_face, ui_pages_current_face_name(), ui_face_enabled_count(), n,
                mask_ok ? "NVS" : "默认（无记录或与注册表不符）",
                (have_face && target == saved_face) ? "NVS（上次的表盘）" : "默认");
}

/** 生成并上报表盘列表（供 App 读取） */
static void publish_faces_report(void)
{
  static char buf[FACE_REPORT_BUF_SIZE];
  /* 告知上报模块"当前是哪个表盘"，App 据此标注选中态 */
  face_report_set_current(ui_pages_current_face());
  int len = face_report_build(buf, sizeof(buf));
  if (len <= 0) {
    Serial.printf("[表盘] 上报生成失败（len=%d）\n", len);
    return;
  }
  ble_telemetry_set_faces(buf);
  Serial.printf("[表盘] 已上报列表（%d 字节，%d 个表盘）\n", len, ui_face_count());
}

/**
 * 应用屏幕方向。
 * @param deg     0 / 90 / 180 / 270
 * @param persist 是否写入 NVS（串口调试可传 false 以免污染用户设置）
 */
static void set_display_rotation(int deg, bool persist)
{
  deg = ((deg % 360) + 360) % 360;
  lv_disp_set_rotation(NULL, rot_deg_to_enum(deg));
  s_display_rot = deg;

  if (persist) {
    Preferences prefs;
    if (prefs.begin(ROT_PREF_NS, false)) {
      prefs.putInt(ROT_PREF_KEY, deg);
      prefs.end();
    }
  }
  Serial.printf("[旋转] 屏幕方向 -> %d°%s\n", deg, persist ? "（已存 NVS）" : "（未持久化）");
}

/** 启动时恢复上次保存的方向；无记录则用编译期默认 DISPLAY_ROT */
static void restore_display_rotation(void)
{
  Preferences prefs;
  int saved = DISPLAY_ROT * 90;   /* 编译期默认 */
  bool from_nvs = false;
  if (prefs.begin(ROT_PREF_NS, true)) {
    from_nvs = prefs.isKey(ROT_PREF_KEY);   /* 先判断是否存在，才能区分来源 */
    saved = prefs.getInt(ROT_PREF_KEY, saved);
    prefs.end();
  }
  set_display_rotation(saved, false);
  Serial.printf("[旋转] 启动恢复方向 = %d°（来源：%s）\n", s_display_rot,
                from_nvs ? "NVS（上次用户设置）" : "编译期默认 DISPLAY_ROT");
}

static void control_poll(void)
{
  control_cmd_t cmd;
  if (!ble_telemetry_take_command(&cmd)) return;

  switch (cmd.kind) {
    case CONTROL_BRIGHTNESS: {
      uint8_t v = control_clamp_brightness(cmd.value);
      apply_brightness(v);
      Serial.printf("[控制] 亮度 → %u\n", (unsigned)v);
      break;
    }
    case CONTROL_SCREEN:
      apply_screen(cmd.value == 0);
      Serial.printf("[控制] %s\n", cmd.value == 0 ? "息屏" : "亮屏");
      break;
    case CONTROL_FACE: {
      /* 表盘切换（功能③）：value = face id，或负数语义 -1=下一个 / -2=上一个 */
      int want = (int)cmd.value;
      int cur = ui_pages_current_face();
      /*
       * ⚠️ 名字必须在**切换前**取好。
       * ui_pages_current_face_name() 返回的是"当前"表盘的名字，切换后再调用
       * 拿到的是新名字 —— 日志会变成 "3(nav) → 4(nav)"（两边同名的假象）。
       * 它返回的是描述符里的字符串字面量，指针稳定，提前捕获是安全的。
       */
      const char *cur_name = ui_pages_current_face_name();
      int target = want;
      if (want == -1) target = ui_face_next(cur, 1);
      else if (want == -2) target = ui_face_prev(cur, 1);

      if (ui_pages_set_face(target)) {
        ui_face_sweep_if_speed(target); /* #2b 切到速度表盘 → 自检扫表 */
        /*
         * 切完**重新上报**：上报里带当前表盘(cur)，App 据此显示"当前是哪个"。
         * 若不上报，App 只能靠本地猜测，设备因隐藏而自动切换时就会指错。
         */
        s_faces_dirty = true;
        Serial.printf("[控制] 表盘 %d(%s) → %d(%s)\n", cur, cur_name, target,
                      ui_pages_current_face_name());
      }
      else {
        Serial.printf("[控制] 表盘 id=%d 无效（共 %d 个）\n", target, ui_face_count());
      }
      break;
    }
    case CONTROL_FACE_ENABLED: {
      /*
       * 启用/隐藏表盘（App 的分组开关）。
       * "不能隐藏最后一个"的业务规则在 ui_face_set_enabled 里，这里只如实回执。
       */
      int id = (int)cmd.value;
      bool on = (cmd.value2 != 0);
      const char *nm = ui_pages_current_face_name(); /* 仅为日志占位，避免未使用告警 */
      (void)nm;

      if (!ui_face_set_enabled(id, on)) {
        Serial.printf("[控制] 表盘 %d %s 被拒绝（id 无效或不允许隐藏最后一个）\n", id,
                      on ? "启用" : "隐藏");
        break;
      }

      const ui_face_desc_t *d = ui_face_get(id);
      Serial.printf("[控制] 表盘 %d(%s) → %s；当前 %d/%d 启用\n", id,
                    (d && d->name) ? d->name : "?", on ? "启用" : "隐藏",
                    ui_face_enabled_count(), ui_face_count());

      /* 隐藏的正是当前正在显示的表盘 → 切到下一个仍启用的，否则会停在隐藏页 */
      if (!on && ui_pages_current_face() == id) {
        int nxt = ui_face_next(id, 1);
        if (nxt >= 0 && nxt != id && ui_pages_set_face(nxt)) {
          ui_face_sweep_if_speed(nxt);
          Serial.printf("[控制] 当前表盘被隐藏，已切到 %d(%s)\n", nxt,
                        ui_pages_current_face_name());
        }
      }

      save_face_prefs(ui_pages_current_face(), ui_face_mask());
      s_faces_dirty = true;
      break;
    }
    case CONTROL_ROTATE:
      /* 屏幕方向（角度由 control.c 校验为 90 的整数倍）*/
      set_display_rotation((int)cmd.value, true);
      break;
    default:
      break;
  }
}

static bool display_init(void)
{
  Wire.begin(IIC_SDA, IIC_SCL);

  /*
   * QSPI 时钟：官方默认 40MHz（ESP32QSPI_FREQUENCY）。实测滑页时全屏传输
   * 占单帧约 54ms（≈8MB/s 有效带宽），是全屏重绘的固定成本之一。
   * 这里显式提高到 80MHz 以便实测对比；若面板出现花屏/噪点，改回 40MHz 即可。
   */
  if (!gfx->begin(QSPI_SPEED_HZ)) {
    Serial.printf("[显示] gfx->begin(%d) 失败\n", (int)QSPI_SPEED_HZ);
    return false;
  }
  Serial.printf("[显示] QSPI 时钟请求 %d MHz\n", (int)(QSPI_SPEED_HZ / 1000000));
  gfx->fillScreen(RGB565_BLACK);
  gfx->setBrightness(200); /* 后续可做成夜间亮度策略 */
  return true;
}

static bool touch_init(void)
{
  pinMode(TP_RESET, OUTPUT);
  digitalWrite(TP_RESET, LOW);
  delay(30);
  digitalWrite(TP_RESET, HIGH);
  delay(50);

  touch.setPins(TP_RESET, TP_INT);
  if (!touch.begin(Wire, 0x5A, IIC_SDA, IIC_SCL)) {
    Serial.println("[触摸] 初始化失败（CST9217 不在线）");
    return false;
  }

  Serial.printf("[触摸] %s\n", touch.getModelName());
  touch.setMaxCoordinates(LCD_WIDTH, LCD_HEIGHT);
  touch.setMirrorXY(true, true); /* 与官方示例一致 */
  return true;
}

static bool rtc_init(void)
{
  if (!rtc.begin(Wire, IIC_SDA, IIC_SCL)) {
    Serial.println("[RTC] 初始化失败（PCF85063 不在线）");
    return false;
  }

  RTC_DateTime dt = rtc.getDateTime();
  Serial.printf("[RTC] %04d-%02d-%02d %02d:%02d:%02d\n", (int)dt.getYear(), (int)dt.getMonth(),
                (int)dt.getDay(), (int)dt.getHour(), (int)dt.getMinute(), (int)dt.getSecond());

  rtc_apply_build_time_if_stale();
  return true;
}

/*
 * 上电兜底：RTC 走时可信（有电池），但若它比固件编译时间还旧，
 * 说明从未校准过（或电池耗尽），此时用**编译时刻**初始化，
 * 以便串口/界面立刻有合理时间；之后可用串口命令手动校准或用 GNSS UTC 校准。
 */
static void rtc_apply_build_time_if_stale(void)
{
  static const char *mon_names[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                     "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
  char mon[4] = { 0 };
  int day = 0, year = 0, hh = 0, mm = 0, ss = 0;

  if (sscanf(__DATE__, "%3s %d %d", mon, &day, &year) != 3) return;
  if (sscanf(__TIME__, "%d:%d:%d", &hh, &mm, &ss) != 3) return;

  int month = 0;
  for (int i = 0; i < 12; i++) {
    if (strncmp(mon, mon_names[i], 3) == 0) { month = i + 1; break; }
  }
  if (month == 0) return;

  RTC_DateTime dt = rtc.getDateTime();
  if ((int)dt.getYear() >= year) return; /* RTC 不比编译时间旧 → 保留 */

  rtc.setDateTime((uint16_t)year, (uint8_t)month, (uint8_t)day, (uint8_t)hh, (uint8_t)mm,
                  (uint8_t)ss);
  Serial.printf("[RTC] RTC 时间（%04d）早于固件编译时间，已按编译时刻设为 %04d-%02d-%02d %02d:%02d:%02d\n",
                (int)dt.getYear(), year, month, day, hh, mm, ss);
}

static void lvgl_init(void)
{
  lv_init();

  lv_disp_draw_buf_init(&draw_buf, buf, NULL, LCD_WIDTH * LCD_HEIGHT / 10);

  static lv_disp_drv_t disp_drv;
  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res = LCD_WIDTH;
  disp_drv.ver_res = LCD_HEIGHT;
  disp_drv.flush_cb = disp_flush_cb;
  disp_drv.rounder_cb = rounder_cb;
  disp_drv.draw_buf = &draw_buf;

  /*
   * 软件旋转（sw_rotate）**常开**，以支持运行时切换方向（App/串口）。
   *
   * 依据：官方示例 `06_LVGL_Widgets.ino` 正是这么做的——
   *     disp_drv.sw_rotate = 1;  // add for rotation
   *   然后用 lv_disp_set_rotation() 按 IMU 姿态切换 0/90/180/270。
   * 且旋转为 NONE 时**不产生任何开销**（lv_refr.c 仅在
   * `rotated != NONE && sw_rotate` 时才走旋转路径）。
   *
   * ⚠️ 与 full_refresh / direct_mode 不兼容（LVGL 明确禁止）——
   *    本工程用 partial 刷新模式，故可用。
   *
   * ℹ️ 先前本文件曾记录「软件旋转实测不可行」，那是**配置错误**下的结论
   *    （当时开着 full_refresh，LVGL 会直接报 "cannot rotate a full refreshed
   *    display!" 并放弃旋转）。已更正；按角度分档的实际开销见
   *    docs/troubleshooting.md（180° 是原地反转，90°/270° 走原地 4 向转置）。
   */
  disp_drv.sw_rotate = 1;

  /* 初始方向（编译期默认；运行时可用 madctl / lvrot 命令或 BLE 命令修改）*/
#if DISPLAY_ROT == 1
  disp_drv.rotated = LV_DISP_ROT_90;
#elif DISPLAY_ROT == 2
  disp_drv.rotated = LV_DISP_ROT_180;
#elif DISPLAY_ROT == 3
  disp_drv.rotated = LV_DISP_ROT_270;
#else
  disp_drv.rotated = LV_DISP_ROT_NONE;
#endif

  /* 注册显示驱动（**必须先于 PSRAM 块**：注册时创建 lv_disp_t 与默认屏幕） */
  lv_disp_drv_register(&disp_drv);

  /*
   * PSRAM 全帧绘制缓冲：partial 模式下，缓冲 ≥ 脏区时 LVGL 只切 1 块。
   * bufmode 串口命令可运行时在 内部/PSRAM 间切换（A/B 实测用）。
   */
  s_buf_psram = (lv_color_t *)heap_caps_malloc(
      (size_t)LCD_WIDTH * LCD_HEIGHT * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
  if (s_buf_psram != NULL) {
    lv_disp_draw_buf_init(&s_draw_buf_psram, s_buf_psram, NULL,
                          (uint32_t)LCD_WIDTH * (uint32_t)LCD_HEIGHT);
    Serial.printf("[显示] PSRAM 全帧缓冲就绪（%u B，free PSRAM %u B）\n",
                  (unsigned)((size_t)LCD_WIDTH * LCD_HEIGHT * sizeof(lv_color_t)),
                  (unsigned)ESP.getFreePsram());
    /* 默认启用（等下通过 driver 指针切换；LVGL 8.4 的 disp->driver 指向本函数的
       静态 disp_drv，可直接改 draw_buf 字段）*/
    lv_disp_t *d = lv_disp_get_default();
    if (d != NULL) {
      d->driver->draw_buf = &s_draw_buf_psram;
      s_buf_psram_active = true;
    }
  }
  else {
    Serial.println("[显示] PSRAM 缓冲分配失败（仅内部缓冲可用）");
  }

  static lv_indev_drv_t indev_drv;
  lv_indev_drv_init(&indev_drv);
  indev_drv.type = LV_INDEV_TYPE_POINTER;
  indev_drv.read_cb = touchpad_read_cb;
  lv_indev_drv_register(&indev_drv);

  /* 触摸 100Hz 独立采样（见 touch_sample_cb 注释：防渲染阻塞导致丢触摸） */
  {
    const esp_timer_create_args_t tp_args = { .callback = &touch_sample_cb,
                                             .name = "tp_sample" };
    esp_timer_handle_t tp_timer = NULL;
    if (esp_timer_create(&tp_args, &tp_timer) == ESP_OK) {
      esp_timer_start_periodic(tp_timer, 10000); /* 10ms = 100Hz（1MHz 时基 → 微秒） */
    }
  }

  /* LVGL 心跳：务必存在，否则所有 lv_timer 不触发（时钟不走/秒针不动） */
  const esp_timer_create_args_t tick_args = { .callback = &lvgl_tick_cb, .name = "lvgl_tick" };
  esp_timer_handle_t tick_timer = NULL;
  esp_timer_create(&tick_args, &tick_timer);
  esp_timer_start_periodic(tick_timer, EXAMPLE_LVGL_TICK_PERIOD_MS * 1000);
}

/* ---------------- 串口命令（开发便利：校时 / 切页 / 查看状态） ---------------- */

static const char *page_name(int i)
{
  switch (i) {
    case UI_PAGE_ANALOG: return "模拟时钟";
    case UI_PAGE_DIGITAL: return "数字时钟";
    case UI_PAGE_SPEED: return "速度表盘";
    case UI_PAGE_COLOR_CLOCK: return "多彩时钟";
    case UI_PAGE_NAV: return "导航卡片";
    default: return "未知";
  }
}

static void print_time(void)
{
  if (!rtc_ok) {
    Serial.println("[时间] RTC 不可用");
    return;
  }
  RTC_DateTime dt = rtc.getDateTime();
  Serial.printf("[时间] %04d-%02d-%02d %02d:%02d:%02d\n", (int)dt.getYear(), (int)dt.getMonth(),
                (int)dt.getDay(), (int)dt.getHour(), (int)dt.getMinute(), (int)dt.getSecond());
}

static void print_pages(void)
{
  Serial.printf("[页面] 当前 = %d(%s)，共 %d 页\n", (int)ui_pages_current(),
                page_name((int)ui_pages_current()), (int)UI_PAGE_COUNT);
  for (int i = 0; i < (int)UI_PAGE_COUNT; i++) {
    Serial.printf("        %d = %s\n", i, page_name(i));
  }
}

static void print_help(void)
{
  Serial.println("[命令] time                     显示 RTC 时间");
  Serial.println("       set YYYY-MM-DD HH:MM:SS   手动校准 RTC 时间（例：set 2026-10-10 22:30:00）");
  Serial.println("       page N                   切换页面");
  Serial.println("       pages                    列出页面");
  Serial.println("       scan                     I2C 总线扫描（确认器件在线）");
  Serial.println("       ble                      显示 BLE 连接与遥测状态");
  Serial.println("       bufmode [psram|internal]  查看/切换绘制缓冲（滑动性能 A/B 对比）");
  Serial.println("       bright <0-255>            设置亮度（不带参数=查询）");
  Serial.println("       screen on|off             亮屏 / 息屏");
  Serial.println("       trip [reset]              行程统计：里程/最高速/均速");
  Serial.println("       face [id|next|prev]       查看/切换表盘（不带参数=列出全部）");
  Serial.println("       spd [kmh|reset]           诊断：查询/强制设置速度指针");
  Serial.println("       nav [st mv dist [spd]]   导航卡片模拟数据（阶段1）：st=0..5 mv=0..7");
  Serial.println("                                  例: nav 1 2 500 68 → 导航中·左转·500m·68km/h");
  Serial.println("       lvrot [0|1|2|3]          【运行时】LVGL 软件旋转 0/90/180/270°（不带参数=查询）");
  Serial.println("       madctl <hex>              【实验】直写面板 MADCTL(0x36) 验证硬件旋转");
  Serial.println("                                 参考: 00=0° 60=90° C0=180° A0=270°；恢复 madctl 0");
  Serial.println("       flickrot [0|1]           flick 方向判定的旋转补偿（现场 A/B，不带参数=查询）");
  Serial.println("       mem                      显示 LVGL/ESP 内存情况");
  Serial.println("       dump                     打印当前屏幕的元素树（排查元素缺失）");
  Serial.println("       touch                    打印最近触摸采样（排查坐标镜像/旋转）");
  Serial.println("       help                     显示本帮助");
}

/* I2C 总线扫描（排障：确认 LC76G 等器件是否在线） */
static void print_scan(void)
{
  Serial.println("[扫描] I2C 总线（SDA=15, SCL=14）:");
  int n = 0;
  for (uint8_t a = 1; a < 127; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      Serial.printf("        0x%02X", a);
      if (a == 0x51) Serial.print(" (RTC PCF85063)");
      if (a == 0x5A) Serial.print(" (触摸 CST9217)");
      if (a == 0x6B) Serial.print(" (IMU QMI8658)");
      if (a == 0x50) Serial.print(" (LC76G 写)");
      if (a == 0x54) Serial.print(" (LC76G 读)");
      Serial.println();
      n++;
    }
  }
  Serial.printf("[扫描] 共 %d 个设备\n", n);
}

/* 打印 BLE 遥测状态（排障用） */
static void print_ble(void)
{
  uint32_t age = ble_telemetry_age_ms();
  Serial.printf("[BLE] 连接=%s", ble_telemetry_connected() ? "已连接" : "未连接");
  if (age == UINT32_MAX) Serial.print("  数据=从未收到");
  else Serial.printf("  数据龄=%u ms", (unsigned)age);

  uint32_t rb = 0, ok = 0, pe = 0;
  ble_telemetry_stats(&rb, &ok, &pe);
  Serial.printf("  收到=%u B / 成功=%u / 解析错=%u / 不合理=%u\n",
                (unsigned)rb, (unsigned)ok, (unsigned)pe, (unsigned)s_implausible);

  if (s_tel_valid) {
    Serial.printf("   最新: v=%d 速度=%.2f m/s (%.1f km/h) 航向=%.1f° 定位有效=%d\n",
                  s_tel.v, (double)s_tel.speed_mps, (double)telemetry_speed_kmh(&s_tel),
                  (double)s_tel.course_deg, (int)s_tel.location_valid);
  }

  if (s_time_synced) {
    Serial.printf("   时间同步=已同步（UTC %lld，%.1f s 前）\n",
                  (long long)s_last_sync_utc_ms, (double)(millis() - s_last_sync_ms) / 1000.0);
  }
  else {
    Serial.println("   时间同步=未同步");
  }

  static const char *sn[] = { "BT OFF", "WAIT GPS", "NO FIX", "STALE", "LIVE" };
  int si = (s_last_status >= 0 && s_last_status <= UI_STATUS_OK) ? (int)s_last_status : 0;
  Serial.printf("   链路状态=%s\n", sn[si]);
  {
    uint32_t cok = 0, cerr = 0;
    ble_telemetry_command_stats(&cok, &cerr);
    Serial.printf("   控制命令=成功 %u / 拒绝 %u   亮度=%u 息屏=%s\n", (unsigned)cok,
                  (unsigned)cerr, (unsigned)s_brightness, s_screen_off ? "是" : "否");
  }
  Serial.printf("   本次启动复位原因=%s(码 %d)，已运行 %lu s\n", s_reset_reason,
                s_reset_reason_code, (unsigned long)(millis() / 1000));
  Serial.printf("   显示速度=%.1f km/h（目标 %.1f，原始遥测 %.1f）\n",
                (double)telemetry_ramp_value(&s_ramp),
                (double)s_ramp.target_kmh,
                (double)(s_tel_valid ? telemetry_speed_kmh(&s_tel) : 0.0f));
}

/* 打印 LVGL 内存与堆情况（排查元素缺失） */
static void print_mem(void)
{
  lv_mem_monitor_t mon;
  lv_mem_monitor(&mon);
  Serial.printf("[内存] LVGL 池: 总 %u B, 已用 %u%%, 最大空闲块 %u B, 碎片 %u%%\n",
                (unsigned)mon.total_size, (unsigned)mon.used_pct,
                (unsigned)mon.free_biggest_size, (unsigned)mon.frag_pct);
  Serial.printf("        ESP 空闲堆 %u B, 历史最低 %u B\n", (unsigned)ESP.getFreeHeap(),
                (unsigned)ESP.getMinFreeHeap());
  if (ESP.getPsramSize() > 0) {
    Serial.printf("        PSRAM 总 %u B, 空闲 %u B\n", (unsigned)ESP.getPsramSize(),
                  (unsigned)ESP.getFreePsram());
  }
  else {
    Serial.println("        PSRAM：未启用（构建选项 PSRAM=opi 未开？）");
  }
}

/* 递归打印对象树（排查页面上到底创建了哪些元素） */
static void dump_obj(lv_obj_t *o, int depth)
{
  char ind[26];
  int k = 0;
  for (int i = 0; i < depth && k < 24; i++) { ind[k++] = ' '; ind[k++] = ' '; }
  ind[k] = '\0';

  const char *cls = "obj";
  if (lv_obj_check_type(o, &lv_label_class)) cls = "label";
  else if (lv_obj_check_type(o, &lv_img_class)) cls = "img";
  else if (lv_obj_check_type(o, &lv_meter_class)) cls = "meter";
  else if (lv_obj_check_type(o, &lv_arc_class)) cls = "arc";

  Serial.printf("%s%s pos(%d,%d) size%dx%d%s%s%s\n", ind, cls, (int)lv_obj_get_x(o),
                (int)lv_obj_get_y(o), (int)lv_obj_get_width(o), (int)lv_obj_get_height(o),
                lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN) ? " [hidden]" : "",
                lv_obj_has_flag(o, LV_OBJ_FLAG_CLICKABLE) ? " [clickable]" : " [NO-click]",
                lv_obj_has_flag(o, LV_OBJ_FLAG_SCROLLABLE) ? " [scroll]" : "");

  if (lv_obj_check_type(o, &lv_label_class)) {
    Serial.printf("%s  text=\"%s\"\n", ind, lv_label_get_text(o));
  }
  else if (lv_obj_check_type(o, &lv_img_class)) {
    const void *src = lv_img_get_src(o);
    Serial.printf("%s  src=%p %s\n", ind, src,
                  src ? ((const lv_img_dsc_t *)src)->header.w ? "已设置" : "?" : "空");
  }

  uint32_t n = lv_obj_get_child_cnt(o);
  for (uint32_t i = 0; i < n; i++) dump_obj(lv_obj_get_child(o, i), depth + 1);
}

static void print_dump(void)
{
  Serial.println("[元素树] 当前屏幕对象：");
  dump_obj(lv_scr_act(), 0);
}

static void print_touch_log(void)
{
  Serial.printf("[手势-最近] 方向=%s 目标=%s 页 %d → %d\n", s_last_gest.dir, s_last_gest.target,
                s_last_gest.page_before, s_last_gest.page_after);
  Serial.printf("[采样] 抬起 %u 次，其中 flick %u 次；最近 dx=%d dy=%d dt=%lu ms\n",
                (unsigned)s_tp_release_cnt, (unsigned)s_tp_flick_cnt, (int)s_last_dx,
                (int)s_last_dy, (unsigned long)s_last_dt);
  Serial.printf("[手势] 计数 LEFT=%u RIGHT=%u TOP=%u BOTTOM=%u 其他=%u\n",
                (unsigned)s_gest_left, (unsigned)s_gest_right, (unsigned)s_gest_top,
                (unsigned)s_gest_bottom, (unsigned)s_gest_other);
  Serial.printf("[触摸] 最近 %u 次采样（x,y,pressed,时刻ms）：\n", (unsigned)s_tlog_cnt);
  uint8_t start = (s_tlog_cnt < TOUCH_LOG_N) ? 0 : s_tlog_head;
  for (uint8_t i = 0; i < s_tlog_cnt; i++) {
    touch_log_t *t = &s_tlog[(start + i) % TOUCH_LOG_N];
    Serial.printf("   %4d,%4d  %d  %u\n", t->x, t->y, t->pressed, (unsigned)t->ms);
  }
}

static void process_command(char *cmd)
{
  if (strncmp(cmd, "rot ", 4) == 0) {
    int n = atoi(cmd + 4);
    lv_disp_t *d = lv_disp_get_default();
    if (d == NULL) {
      Serial.println("[旋转] 显示未就绪");
    }
    else if (n >= 0 && n <= 3) {
      lv_disp_rot_t r = (n == 1) ? LV_DISP_ROT_90
                        : (n == 2) ? LV_DISP_ROT_180
                        : (n == 3) ? LV_DISP_ROT_270 : LV_DISP_ROT_NONE;
      lv_disp_set_rotation(d, r);
      Serial.printf("[旋转] 已设为 %d（0=0° 1=90° 2=180° 3=270°）\n", n);
    }
    else {
      Serial.println("[旋转] 参数应为 0/1/2/3");
    }
  }
  else if (strcmp(cmd, "scan") == 0) {
    print_scan();
  }
  else if (strncmp(cmd, "spd", 3) == 0) {
    /* 诊断命令：直接设置/查询速度指针（排查"速度 0 但指针不在 0"）*/
    const char *arg = cmd + 3;
    if (*arg == '\0') {
      Serial.printf("[速度] 当前=%.1f km/h 有效=%d 指针对象=%p\n",
                    (double)ui_speed_get_speed(), (int)ui_speed_get_valid(),
                    (void *)ui_speed_get_needle());
    }
    else if (strncmp(arg, " reset", 6) == 0) {
      ui_speed_set_fix(false, 0);
      Serial.println("[速度] 已置为无效（指针应归零）");
    }
    else {
      float v = (float)atof(arg);
      ui_speed_set_fix(true, 12);
      ui_speed_set_speed(v);
      Serial.printf("[速度] 强制设为 %.1f km/h\n", (double)v);
    }
  }
  else if (strncmp(cmd, "madctl", 6) == 0) {
    /*
     * 【实验命令】直接写面板 MADCTL(0x36)，验证 CO5300 是否支持**硬件旋转**。
     *
     * 为什么需要实验：
     *   - 库的 Arduino_CO5300::setRotation() 只做 X/Y 翻转，**从不使用 MV(0x20) 位**
     *     （90°/270° 旋转的关键位），源码注释也写 "CO5300 does not support rotation"；
     *   - 且 setRotation() 在本工程**从未被调用** → MADCTL 一直是面板上电默认值，
     *     其语义（含 MV 位是否有效）在本板**从未被实测**。
     *   - 若硬件支持 MV，则 90° 旋转**零性能开销**，且面板会把**文字一起正确旋转**
     *     → 无需重新设计界面（这是与"LVGL 软件旋转/重排布局"的本质区别）。
     *
     * 标准 ST7789 系取值参考（不同面板可能有差异，需实测）：
     *   0x00 = 0°    0x60 = 90°    0xC0 = 180°    0xA0 = 270°
     *   BGR 位可加 0x08 对比颜色顺序（RGB 系加 0x00）
     *
     * 恢复方式：`madctl 0`（回 0°），或按 RESET 复位（复位后回面板默认）。
     */
    const char *arg = cmd + 6;
    if (*arg == '\0') {
      Serial.println("[旋转] 用法: madctl <hex>   例: madctl 60");
      Serial.println("       参考: 00=0°  60=90°  C0=180°  A0=270°");
      Serial.println("       加 0x08 试 BGR 顺序，例: madctl 68");
      Serial.println("       恢复: madctl 0  或按 RESET");
    }
    else {
      unsigned int v = 0;
      if (sscanf(arg, "%x", &v) == 1 && v <= 0xFF) {
        bus->beginWrite();
        bus->writeC8D8(0x36, (uint8_t)v);   /* MADCTL */
        bus->endWrite();
        lv_obj_invalidate(lv_scr_act());    /* 触发整屏重绘，便于观察 */
        Serial.printf("[旋转] MADCTL <- 0x%02X；若画面异常请 `madctl 0` 或按 RESET\n", v);
      }
      else {
        Serial.println("[旋转] 参数无效，应为 1-2 位十六进制（例: 60）");
      }
    }
  }
  else if (strncmp(cmd, "nav", 3) == 0) {
    /*
     * 【阶段 1 调试命令】用模拟数据驱动导航卡片，验证 UI 而不依赖 iPhone。
     *
     * 用法：
     *   nav                      查询当前值
     *   nav <state> <maneuver> <dist_m> [speed]
     *     state    : 0=未开始 1=导航中 2=重规划 3=到达 4=过期 5=无数据
     *     maneuver : 0=未知 1=直行 2=左转 3=右转 4=掉头 5=靠左 6=靠右 7=到达
     *     dist_m   : 距离（米）；负数表示无效
     *     speed    : 可选，当前车速 km/h
     * 例：nav 1 2 500 68   → 导航中·左转·500m·68km/h
     */
    const char *arg = cmd + 3;
    while (*arg == ' ') arg++;

    if (*arg == '\0') {
      Serial.printf("[导航] 状态=%d(%s) 动作=%d(%s) 距离=%d 车速=%.0f(%s)\n",
                    ui_nav_get_state(), nav_state_name((nav_state_t)ui_nav_get_state()),
                    ui_nav_get_maneuver(), nav_maneuver_name((nav_maneuver_t)ui_nav_get_maneuver()),
                    ui_nav_get_distance(), (double)ui_nav_get_speed(),
                    ui_nav_get_speed_valid() ? "有效" : "无效");
    }
    else {
      int st = -1, mv = -1, dist = 0;
      float spd = -1.0f;
      int n = sscanf(arg, "%d %d %d %f", &st, &mv, &dist, &spd);
      if (n < 3) {
        Serial.println("[导航] 用法: nav <state> <maneuver> <dist_m> [speed]");
        Serial.println("       例: nav 1 2 500 68  （导航中·左转·500m·68km/h）");
      }
      else {
        ui_nav_set_state(st);
        ui_nav_set_maneuver(mv);
        ui_nav_set_distance(dist);
        if (n >= 4) ui_nav_set_speed(spd, true);
        /* 切到导航表盘，便于直接观察 */
        ui_pages_set_face(UI_PAGE_NAV);
        Serial.printf("[导航] 已设置 状态=%d 动作=%d 距离=%d%s\n", st, mv, dist,
                      (n >= 4) ? " 含车速" : "");
      }
    }
  }
  else if (strncmp(cmd, "lvrot", 5) == 0) {
    /*
     * 【运行时软件旋转】用 LVGL 官方的 lv_disp_set_rotation() 切换方向。
     *
     * 依据：官方示例 `06_LVGL_Widgets.ino` 正是此法（disp_drv.sw_rotate = 1 +
     * 按 IMU 姿态调 lv_disp_set_rotation），四种角度均受支持。
     *
     * 本屏为圆形，旋转后仍是同一个圆 → **无长宽比/布局副作用**。
     * 触摸坐标由 LVGL 自动做逆变换（indev_pointer_proc），无需自行处理。
     *
     * 开销按角度分档（见 lv_refr.c）：
     *   0°   —— 无开销（不进入旋转路径）
     *   180° —— 原地首尾反转，无需临时缓冲，成本低
     *   90/270° —— 转置；本屏全屏脏区为**正方形**，走原地 4 向旋转
     *              （draw_buf_rotate_90_sqr），同样不分配临时缓冲
     */
    const char *arg = cmd + 5;
    lv_disp_t *d = lv_disp_get_default();
    if (*arg == '\0') {
      Serial.printf("[旋转] 当前 LVGL 旋转=%d（%d°），sw_rotate=%d\n",
                    d ? (int)d->driver->rotated : -1,
                    d ? (int)d->driver->rotated * 90 : 0,
                    d ? (int)d->driver->sw_rotate : 0);
      Serial.println("       用法: lvrot 0|1|2|3  →  0°/90°/180°/270°");
    }
    else {
      int r = atoi(arg);
      if (r >= 0 && r <= 3) {
        /* 调试命令：**不持久化**，避免调试残留污染用户设置。
           正式用户设置走 BLE {"cmd":"rotate","value":90}，那条会写 NVS。 */
        set_display_rotation(r * 90, false);
      }
      else {
        Serial.println("[旋转] 参数应为 0-3（0°/90°/180°/270°）");
      }
    }
  }
  else if (strncmp(cmd, "flickrot", 8) == 0) {
    /*
     * 【现场 A/B】flick 方向判定是否按当前屏幕方向做坐标逆变换。
     *   0 = 旧行为：直接用面板原始坐标 → 90°/270° 下"左右滑"判不出 flick
     *   1 = 修正行为（默认）：换算到 LVGL 逻辑坐标再判定
     */
    const char *arg = cmd + 8;
    if (*arg == '\0') {
      Serial.printf("[触摸] flick 旋转补偿 = %d（0=旧行为 1=修正）\n", s_flick_rot_fix ? 1 : 0);
      Serial.println("       用法: flickrot 0|1（现场 A/B：切到 90° 后左右滑动对比）");
    }
    else {
      s_flick_rot_fix = (atoi(arg) != 0);
      Serial.printf("[触摸] flick 旋转补偿 -> %d\n", s_flick_rot_fix ? 1 : 0);
    }
  }
  else if (strncmp(cmd, "face", 4) == 0) {
    const char *arg = cmd + 4;
    if (*arg == '\0') {
      Serial.printf("[表盘] 当前=%d(%s)，共 %d 个\n", ui_pages_current_face(),
                    ui_pages_current_face_name(), ui_face_count());
      for (int i = 0; i < ui_face_count(); i++) {
        const ui_face_desc_t *f = ui_face_get(i);
        Serial.printf("       %d: %-10s %s\n", i, f->name, f->label);
      }
      Serial.println("       用法：face <id> | face next | face prev");
    }
    else if (strncmp(arg, " next", 5) == 0) {
      int t = ui_face_next(ui_pages_current_face(), 1);
      ui_pages_set_face(t);
      Serial.printf("[表盘] → %d(%s)\n", t, ui_pages_current_face_name());
    }
    else if (strncmp(arg, " prev", 5) == 0) {
      int t = ui_face_prev(ui_pages_current_face(), 1);
      ui_pages_set_face(t);
      Serial.printf("[表盘] → %d(%s)\n", t, ui_pages_current_face_name());
    }
    else {
      int t = atoi(arg);
      if (ui_pages_set_face(t)) {
        Serial.printf("[表盘] → %d(%s)\n", t, ui_pages_current_face_name());
      }
      else {
        Serial.printf("[表盘] id=%d 无效（共 %d 个）\n", t, ui_face_count());
      }
    }
  }
  else if (strncmp(cmd, "trip", 4) == 0) {
    const char *arg = cmd + 4;
    if (strncmp(arg, " reset", 6) == 0) {
      telemetry_trip_reset(&s_trip);
      ui_speed_set_trip(0.0f, 0.0f);
      Serial.println("[行程] 已清零");
    }
    else {
      Serial.printf("[行程] 里程 %.2f km，最高 %.0f km/h，均速 %.0f km/h（有效 %lu s）\n",
                    s_trip.distance_km, (double)s_trip.max_kmh,
                    (double)telemetry_trip_avg_kmh(&s_trip),
                    (unsigned long)(s_trip.time_ms / 1000));
      Serial.println("       用法：trip | trip reset");
    }
  }
  else if (strncmp(cmd, "bright", 6) == 0) {
    int v = atoi(cmd + 6);
    if (v <= 0) {
      Serial.printf("[控制] 当前亮度 %u，息屏=%s\n", (unsigned)s_brightness,
                    s_screen_off ? "是" : "否");
      Serial.println("       用法：bright <0-255>");
    }
    else {
      apply_brightness(control_clamp_brightness(v));
      Serial.printf("[控制] 亮度 → %u\n", (unsigned)s_brightness);
    }
  }
  else if (strncmp(cmd, "screen", 6) == 0) {
    const char *arg = cmd + 6;
    if (strncmp(arg, " off", 4) == 0) {
      apply_screen(true);
      Serial.println("[控制] 息屏（发送 screen on 恢复）");
    }
    else if (strncmp(arg, " on", 3) == 0) {
      apply_screen(false);
      Serial.println("[控制] 亮屏");
    }
    else {
      Serial.printf("[控制] 息屏=%s 亮度=%u（用法：screen on|off）\n",
                    s_screen_off ? "是" : "否", (unsigned)s_brightness);
    }
  }
  else if (strncmp(cmd, "bufmode", 7) == 0) {
    const char *arg = cmd + 7;
    lv_disp_t *d = lv_disp_get_default();
    if (*arg == '\0') {
      Serial.printf("[缓冲] 当前=%s（重绘/循环见心跳）\n",
                    s_buf_psram_active ? "PSRAM 全帧 434KB" : "内部 1/10 屏 43KB");
    }
    else if (d != NULL && strcmp(arg, " psram") == 0 && s_buf_psram != NULL) {
      d->driver->draw_buf = &s_draw_buf_psram;
      s_buf_psram_active = true;
      lv_obj_invalidate(lv_scr_act());
      Serial.println("[缓冲] → PSRAM 全帧（脏区单块渲染）");
    }
    else if (d != NULL && strcmp(arg, " internal") == 0) {
      d->driver->draw_buf = &draw_buf;
      s_buf_psram_active = false;
      lv_obj_invalidate(lv_scr_act());
      Serial.println("[缓冲] → 内部 1/10 屏（基线，分块渲染）");
    }
    else {
      Serial.println("[缓冲] 用法：bufmode | bufmode psram | bufmode internal");
    }
  }
  else if (strcmp(cmd, "ble") == 0) {
    print_ble();
  }
  else if (strcmp(cmd, "mem") == 0) {
    print_mem();
  }
  else if (strcmp(cmd, "dump") == 0) {
    print_dump();
  }
  else if (strcmp(cmd, "touch") == 0) {
    print_touch_log();
  }
  else if (strcmp(cmd, "help") == 0) {
    print_help();
  }
  else if (strcmp(cmd, "time") == 0) {
    print_time();
  }
  else if (strcmp(cmd, "pages") == 0) {
    print_pages();
  }
  else if (strncmp(cmd, "page ", 5) == 0) {
    int n = atoi(cmd + 5);
    if (n >= 0 && n < (int)UI_PAGE_COUNT) {
      ui_pages_show((ui_page_t)n);
      print_pages();
    }
    else {
      Serial.printf("[命令] 页码超出范围（0-%d）\n", (int)UI_PAGE_COUNT - 1);
    }
  }
  else if (strncmp(cmd, "set ", 4) == 0) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, se = 0;
    if (sscanf(cmd + 4, "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &se) == 6 && y >= 2020 &&
        mo >= 1 && mo <= 12 && d >= 1 && d <= 31 && h >= 0 && h < 24 && mi >= 0 && mi < 60 &&
        se >= 0 && se < 60) {
      if (!rtc_ok) {
        Serial.println("[RTC] 不可用，无法校准");
        return;
      }
      rtc.setDateTime((uint16_t)y, (uint8_t)mo, (uint8_t)d, (uint8_t)h, (uint8_t)mi, (uint8_t)se);
      Serial.print("[RTC] 已校准为 ");
      print_time();
    }
    else {
      Serial.println("[命令] 格式应为 set 2026-10-10 22:30:00");
    }
  }
  else {
    Serial.printf("[命令] 未知命令: %s（输入 help 查看）\n", cmd);
  }
}

/* 轮询串口输入（逐行处理） */
static void handle_serial(void)
{
  static char line[64];
  static uint8_t n = 0;

  while (Serial.available() > 0) {
    char c = (char)Serial.read();
    if (c == '\r' || c == '\n') {
      if (n > 0) {
        line[n] = '\0';
        process_command(line);
        n = 0;
      }
    }
    else if (n < sizeof(line) - 1) {
      line[n++] = c;
    }
  }
}

/*
 * 启动时把烘焙图充气到 PSRAM。
 *
 * 为什么必须做（真机实测）：8bpp 索引图每帧都要逐像素查调色板，
 * 全屏 466×466 滑页时渲染峰值从 123ms 升到 **168ms**（+45ms，+37%）。
 * 启动时解码一次成 RGB565 后恢复直接内存拷贝，速度回到 ~123ms，
 * 而 Flash 仍保持索引图的占用（省约 683 KB）。
 *
 * 内存：5 张图合计 2×434KB + 3×180KB ≈ 1.4 MB；本板 8MB PSRAM（实测空闲 7.9MB）。
 * 失败降级：分配不到 PSRAM 就保持索引图 —— **功能不受影响**，只是滑页慢约 45ms，
 *          不会白屏或崩溃（宁可慢，不可坏）。
 */
static void *psram_alloc(size_t n)
{
  return heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
}

static void inflate_baked_images(void)
{
  lv_img_dsc_t *imgs[] = {
    &dial_analog, &dial_speed,
    &driver_normal, &driver_spirited, &driver_speeding,
  };
  const int n = (int)(sizeof(imgs) / sizeof(imgs[0]));

  uint32_t before = ESP.getFreePsram();
  int ok = ui_img_inflate_all(imgs, n, psram_alloc);
  uint32_t after = ESP.getFreePsram();
  Serial.printf("[图] 充气 %d/%d 张，开销 %u KB，PSRAM 空闲 %u KB\n",
                ok, n, (unsigned)((before - after) / 1024), (unsigned)(after / 1024));
}

void setup()
{
  Serial.begin(115200);
  delay(200);
  Serial.println("\n=== ESP32 车载仪表 启动 ===");
  Serial.printf("LVGL %d.%d.%d\n", lv_version_major(), lv_version_minor(), lv_version_patch());

  /* 复位原因：便于区分上电/软件复位/崩溃/看门狗/欠压（稳定性验收要用） */
  {
    esp_reset_reason_t rr = esp_reset_reason();
    const char *rn = "未知";
    switch (rr) {
      case ESP_RST_POWERON:  rn = "上电"; break;
      case ESP_RST_SW:       rn = "软件复位(esp_restart)"; break;
      case ESP_RST_PANIC:    rn = "异常/崩溃(PANIC)"; break;
      case ESP_RST_INT_WDT:  rn = "中断看门狗"; break;
      case ESP_RST_TASK_WDT: rn = "任务看门狗"; break;
      case ESP_RST_WDT:      rn = "其他看门狗"; break;
      case ESP_RST_BROWNOUT: rn = "欠压(BROWNOUT)"; break;
      case ESP_RST_DEEPSLEEP:rn = "深睡唤醒"; break;
      case ESP_RST_EXT:      rn = "外部复位"; break;
      case ESP_RST_SDIO:     rn = "SDIO 复位"; break;
      case ESP_RST_USB:      rn = "USB 复位（烧录/USB-JTAG，正常）"; break;
      case ESP_RST_JTAG:     rn = "JTAG 复位"; break;
      case ESP_RST_UNKNOWN:  rn = "无法确定"; break;
      default: break;
    }
    s_reset_reason = rn;
    s_reset_reason_code = (int)rr;
    Serial.printf("[启动] 复位原因=%d(%s) 空闲堆=%u B\n", (int)rr, rn,
                  (unsigned)ESP.getFreeHeap());
  }

  display_init();

  bool tp_ok = touch_init();
  rtc_ok = rtc_init();

  /* BLE Peripheral：广播 CarDashboard，等待 iPhone 写入遥测 */
  Serial.printf("[BLE] 初始化 %s\n", ble_telemetry_begin() ? "成功，开始广播" : "失败");

  lvgl_init();

  /* 时间来源：RTC（失败时界面会显示无效应有的样子） */
  ui_timesrc_set_provider(rtc_time_provider);

  /* 诊断：屏幕级手势日志（若 LVGL 识别到手势但页面未切换，说明是事件目标问题） */
  lv_obj_add_event_cb(lv_scr_act(), [](lv_event_t *e) {
    (void)e;
    lv_dir_t d = lv_indev_get_gesture_dir(lv_indev_get_act());
    lv_obj_t *tgt = (lv_obj_t *)lv_event_get_target(e);
    lv_obj_t *cur = (lv_obj_t *)lv_event_get_current_target(e);
    bool tgt_is_scr = (tgt == lv_scr_act());
    bool tgt_is_page = false;
    for (int i = 0; i < (int)UI_PAGE_COUNT; i++) {
      if (tgt == ui_pages_obj((ui_page_t)i)) tgt_is_page = true;
    }
    s_last_gest.dir = (d == LV_DIR_LEFT) ? "LEFT" : (d == LV_DIR_RIGHT) ? "RIGHT"
                      : (d == LV_DIR_TOP) ? "TOP" : (d == LV_DIR_BOTTOM) ? "BOTTOM" : "?";
    s_last_gest.target = tgt_is_scr ? "screen" : tgt_is_page ? "page" : "其它";
    s_last_gest.page_before = s_page_seen;
    s_last_gest.page_after = (int)ui_pages_current();
    Serial.printf("[手势] 目标=%s current=%s 页 %d→%d\n", s_last_gest.target,
                  (cur == lv_scr_act()) ? "screen" : "其它", s_last_gest.page_before,
                  s_last_gest.page_after);

    if (d == LV_DIR_LEFT) s_gest_left++;
    else if (d == LV_DIR_RIGHT) s_gest_right++;
    else if (d == LV_DIR_TOP) s_gest_top++;
    else if (d == LV_DIR_BOTTOM) s_gest_bottom++;
    else s_gest_other++;
    const char *n = (d == LV_DIR_LEFT) ? "LEFT" : (d == LV_DIR_RIGHT) ? "RIGHT"
                    : (d == LV_DIR_TOP) ? "TOP" : (d == LV_DIR_BOTTOM) ? "BOTTOM" : "?";
    Serial.printf("[手势] 屏幕收到 %s\n", n);
  }, LV_EVENT_GESTURE, NULL);

  /* 烘焙图充气：必须在 ui_pages_init() 之前 —— 表盘构建时会 lv_img_set_src
   * 引用这些 dsc，提前改好可避免"先引用旧格式、后改写"的隐患。 */
  inflate_baked_images();

  /* 业务 UI：全部页面（模拟时钟 / 数字时钟 / 速度表盘） */
  ui_pages_init();

  /* 恢复上次保存的屏幕方向（NVS）；无记录则用编译期默认 DISPLAY_ROT */
  restore_display_rotation();

  /*
   * 恢复表盘状态（断电记忆）：先恢复启用/隐藏集合，再恢复当前表盘。
   * 必须在 ui_pages_init() 之后（注册表已就绪），且在首次 show 之前
   * —— 否则会先闪一下默认表盘再跳到记忆的表盘。
   */
  restore_face_state();
  publish_faces_report(); /* 表盘注册完成，上报名单供 App 读取 */

  ui_pages_show((ui_page_t)s_last_face);

  /*
   * 初始状态：无遥测 → 速度页显示无效（NO FIX / --），指针归零。
   * 收到有效 BLE 遥测后由 ble_poll 驱动。
   */
  telemetry_trip_reset(&s_trip);
  telemetry_ramp_reset(&s_ramp);
  telemetry_debounce_reset(&s_deb);
  s_trip_last_ms = millis();
  ui_speed_set_fix(false, 0);
  ui_speed_set_status(UI_STATUS_CONNECTING); /* 初始 BT OFF */
  ui_clock_set_time_synced(false); /* 初始 TIME SYNC，BLE 校时后转 SYNCED */

  Serial.printf("[显示] 旋转 = %d（0=0° 1=90° 2=180° 3=270°），软件旋转已开启\n", DISPLAY_ROT);
  Serial.printf("[UI] 页面数 %d，默认页 %d(%s)，触摸%s\n", (int)UI_PAGE_COUNT, (int)DEFAULT_PAGE,
                page_name((int)DEFAULT_PAGE), tp_ok ? "就绪" : "不可用");
  print_time();
  print_mem();
  print_help();
}

/*
 * 时间同步：收到有效 UTC epoch 毫秒后：
 *   1) settimeofday 校准系统时钟（UTC）；2) 写 PCF85063 RTC（本地时区）；
 *   3) 标记同步状态并更新屏幕标签 + Status 特征。
 * 避免 1-2Hz 反复写 RTC：已同步且与系统时钟偏差 ≤ 2s → 跳过。
 */
static bool time_sync_maybe(int64_t utc_ms)
{
  if (utc_ms <= 0) return false;

  /*
   * 已同步：只在偏差**明显过大**时才重校（见 TIME_RESYNC_THRESHOLD_MS 注释）。
   * 取中间时刻估算，抵消 time() 的秒级截断（±500ms）带来的固有误差。
   */
  if (s_time_synced) {
    struct timeval tv_now;
    gettimeofday(&tv_now, NULL);
    int64_t local_ms = (int64_t)tv_now.tv_sec * 1000 + tv_now.tv_usec / 1000;
    int64_t diff_ms = local_ms - utc_ms;
    if (diff_ms < 0) diff_ms = -diff_ms;
    if (diff_ms <= TIME_RESYNC_THRESHOLD_MS) return false; /* 正常：不重写 RTC */
    Serial.printf("[同步] 偏差 %lld ms 超阈值，重校\n", (long long)diff_ms);
  }

  /* 合理性校验（换算成日历后年份区间） */
  time_t utc = (time_t)(utc_ms / 1000);
  time_t local = utc + (time_t)TZ_OFFSET_HOURS * 3600;
  struct tm *lt = gmtime(&local);
  if (lt == NULL) return false;
  int y = lt->tm_year + 1900;
  if (y < 2020 || y > 2080) {
    Serial.printf("[同步] UTC 时间戳不合理（%d 年），忽略\n", y);
    return false;
  }

  /* 系统时钟（UTC） */
  struct timeval tv;
  tv.tv_sec = utc;
  tv.tv_usec = (suseconds_t)((utc_ms % 1000) * 1000);
  settimeofday(&tv, NULL);

  /* RTC：存本地时区时间（显示 provider 直接读，无需再换算） */
  if (rtc_ok) {
    rtc.setDateTime((uint16_t)y, (uint8_t)(lt->tm_mon + 1), (uint8_t)lt->tm_mday,
                    (uint8_t)lt->tm_hour, (uint8_t)lt->tm_min, (uint8_t)lt->tm_sec);
  }

  bool first = !s_time_synced;
  s_time_synced = true;
  s_last_sync_ms = millis();
  s_last_sync_utc_ms = utc_ms;
  ui_clock_set_time_synced(true);

  Serial.printf("[同步] %sBLE UTC → RTC %04d-%02d-%02d %02d:%02d:%02d（UTC+%d）\n",
                first ? "首次 " : "重校 ", y, lt->tm_mon + 1, lt->tm_mday, lt->tm_hour,
                lt->tm_min, lt->tm_sec, (int)TZ_OFFSET_HOURS);

  ble_telemetry_set_status("time-synced");
  return true;
}

/*
 * 从 BLE 取最新遥测，推给速度页。
 * 速度有效 = 协议层 speed_valid 且 speed_mps >= 0（iOS 负值表示无效）。
 * 解析/线程安全在 ble_telemetry.cpp 内部处理，此处只消费结果。
 */
/*
 * 导航消息消费（阶段 2）。
 *
 * 架构约定：BLE 回调只解析+暂存，UI 更新一律在主循环做。
 *
 * 过期语义：导航数据变化慢（一次转弯可能几分钟），故用**独立的长阈值**
 * NAV_STALE_MS（10s），而非遥测的 2.5s —— 否则会频繁误报过期。
 * 关键：一旦过期，**不得继续显示旧指引**（§6.3），由 ui_nav_set_state 落地。
 */
static void nav_poll(void)
{
  static uint32_t s_last_nav_ms = 0;
  static bool s_nav_active = false; /* 是否处于"收到过导航数据"的状态 */

  nav_msg_t nm;
  if (ble_telemetry_take_nav(&nm)) {
    s_last_nav_ms = millis();
    s_nav_active = true;

    /* 只应用**实际提供**的字段：缺字段保留旧值（不因缺字段清空显示）*/
    if (nm.has_state)    ui_nav_set_state((int)nm.state);
    if (nm.has_maneuver) ui_nav_set_maneuver((int)nm.maneuver);
    if (nm.has_dist)     ui_nav_set_distance(nm.distance_m);

    Serial.printf("[导航] state=%s maneuver=%s dist=%d\n",
                  nav_state_name(nm.has_state ? nm.state : NAV_STATE_NO_DATA),
                  nav_maneuver_name(nm.has_maneuver ? nm.maneuver : NAV_MANEUVER_UNKNOWN),
                  nm.has_dist ? nm.distance_m : -1);
    return;
  }

  /* 无新数据：检查是否过期 */
  if (!s_nav_active) return;

  uint32_t age = millis() - s_last_nav_ms;

  /* BLE 断开 → 立即进入"无数据"（不等超时）*/
  if (!ble_telemetry_connected()) {
    if (ui_nav_get_state() != NAV_STATE_NO_DATA) {
      ui_nav_set_state(NAV_STATE_NO_DATA);
      s_nav_active = false;
      Serial.println("[导航] BLE 断开 → 无数据");
    }
    return;
  }

  /* 超时 → 过期（**不显示旧指引**）*/
  if (age > NAV_STALE_MS && ui_nav_get_state() == NAV_STATE_ACTIVE) {
    ui_nav_set_state(NAV_STATE_STALE);
    Serial.printf("[导航] 数据过期（%u ms 无更新）→ 停止显示旧指引\n", (unsigned)age);
  }
}

static void ble_poll(void)
{
  nav_poll(); /* 导航消息（与遥测同一特征，先生效以便状态切换）*/

  telemetry_msg_t m;
  uint32_t rx_ms = 0;

  if (!ble_telemetry_fetch(&m, &rx_ms)) return; /* 无新数据 */

  /* §7 兜底：超范围/自相矛盾的包直接丢弃（iOS 侧另有精度质量门槛，见 §5.4） */
  if (!telemetry_plausible(&m)) {
    s_implausible++;
    Serial.printf("[BLE] 丢弃不合理数据（累计 %u）：速度=%.1f km/h 航向=%.0f°\n",
                  (unsigned)s_implausible, (double)telemetry_speed_kmh(&m), (double)m.course_deg);
    return;
  }

  s_tel = m;
  s_had_data = true;

  /* 时间同步：带 ts_utc_ms 的包触发（首次/漂移 >2s 才真正写 RTC） */
  if (m.has_ts) time_sync_maybe(m.ts_utc_ms);

  bool speed_ok = m.speed_valid && m.speed_mps >= 0.0f;
  s_tel_valid = m.location_valid || speed_ok;

  Serial.printf("[BLE] v=%d 速度=%.2f m/s(%.1f km/h) valid=%d 航向=%.1f° 定位=%d\n",
                m.v, (double)m.speed_mps, (double)telemetry_speed_kmh(&m), (int)m.speed_valid,
                (double)m.course_deg, (int)m.location_valid);
}

/*
 * 五态状态机（每循环执行，AI_TASK §6.4）：
 *   未连接 → BT OFF；已连接未收到 → WAIT GPS；收到但无效 → NO FIX；
 *   数据超时 → STALE；新鲜有效 → LIVE。
 * 同时驱动指针/数字：只有 LIVE 才显示速度，其余状态指针归零、不残留旧值。
 */
static void ble_update(void)
{
  /*
   * UI 驱动限频 50Hz：手机遥测本身 2Hz，更高频率的调用在 setter 的变更检测
   * 下虽不会触发重绘，但限频可进一步压低空转成本（状态切换延迟 ≤20ms，
   * 相对 2.5s 超时阈值可忽略）。
   */
  static uint32_t last_ui_ms = 0;
  uint32_t now_ms = millis();
  if (now_ms - last_ui_ms < 20) return;
  last_ui_ms = now_ms;

  /*
   * 状态判定用共享的纯 C 状态机（simulator/ui/telemetry.c），
   * 已有 11 项单元测试覆盖（五态 + 并发 HOLD + 阈值边界），
   * 避免"固件里一份、测试里一份"的双份逻辑漂移。
   * 枚举值刻意与 ui_speed_status_t 对齐。
   */
  telemetry_link_t link = telemetry_link_state(ble_telemetry_connected(), s_had_data,
                                               ble_telemetry_age_ms(), s_tel_valid,
                                               BLE_STALE_MS, (telemetry_link_t)s_last_status);

  /*
   * ⚠️ 两个状态**必须分开**（安全优先原则）：
   *
   *   link_raw —— 未经防抖的真实判定，用于**指针与数据有效性**决策：
   *               数据一过期就归零，绝不因防抖而用旧值冒充当前车速（§6.3）。
   *   link_disp —— 加了防抖、**仅用于屏幕状态标签**：
   *               室内静止时 iOS 定位降到约 1.25Hz，偶发延迟会擦到 2.5s
   *               阈值，标签闪一下 STALE（真机长稳实测）；防抖消除该闪烁，
   *               而真实断流仍在连续 3 次判定后照常上报 STALE。
   */
  telemetry_link_t link_raw = (link == LINK_HOLD) ? (telemetry_link_t)s_last_status : link;
  telemetry_link_t link_disp = telemetry_debounce_apply(&s_deb, link_raw);
  if (link_disp == LINK_HOLD) link_disp = (telemetry_link_t)s_last_status;

  ui_speed_status_t st_disp = (ui_speed_status_t)link_disp;
  ui_speed_set_status(st_disp);
  s_last_status = st_disp;

  if (link_raw == LINK_OK) {
    bool speed_ok = s_tel.speed_valid && s_tel.speed_mps >= 0.0f;
    ui_speed_set_fix(true, 0);
    if (speed_ok) {
      /*
       * 只设定**目标值**；指针由主循环的斜坡按时间匀速推进（#2 修跳针）。
       * 原先在这里直接 EMA 出显示值并写入：数据 2Hz 而 UI 50Hz，
       * 指针呈"每 500ms 走一段就停"的阶梯运动，观感即跳针。
       */
      telemetry_ramp_set_target(&s_ramp, telemetry_speed_kmh(&s_tel), true);
      if (s_tel.has_course) ui_speed_set_heading((int)(s_tel.course_deg + 0.5f));
    }
    else {
      telemetry_ramp_set_target(&s_ramp, 0.0f, false); /* 速度不可信：目标归零 */
    }
  }
  else {
    telemetry_ramp_reset(&s_ramp); /* 失效/超时/断连：立即重置 */
    ui_speed_set_fix(false, 0);    /* 指针归零，不显示旧值 */
  }
}

void loop()
{
  s_loop_cnt++;                /* 性能：循环速率 */
  handle_serial();            /* 串口命令：校时 / 切页 */
  control_poll();             /* BLE 下行控制：亮度 / 息屏 / 表盘 */

  /*
   * 表盘变化检测：**滑动、串口命令、BLE 命令**都会改变当前表盘，
   * 与其在每处分别持久化（容易漏），不如在循环里统一比对一次。
   * 这样"断电记忆"对所有切换途径都生效。
   */
  {
    int cur = ui_pages_current_face();
    if (cur != s_last_face) {
      s_last_face = cur;
      save_face_prefs(cur, ui_face_mask());
      Serial.printf("[表盘] 当前 → %d(%s)（已存 NVS，断电后沿用）\n", cur,
                    ui_pages_current_face_name());
    }
    /* App 改动过启用/隐藏状态 → 重新上报，保持两端一致 */
    if (s_faces_dirty) {
      s_faces_dirty = false;
      publish_faces_report();
    }
  }

  /* 速度指针斜坡推进（#2）：按实际经过时间匀速逼近目标，与帧率无关 */
  {
    static uint32_t last_ramp_ms = 0;
    uint32_t now_r = millis();
    uint32_t dt_r = now_r - last_ramp_ms;
    if (dt_r >= 20) { /* 50Hz 推进 */
      last_ramp_ms = now_r;
      float shown = telemetry_ramp_step(&s_ramp, dt_r);
      ui_speed_set_speed(shown);
    }
  }

  /* 行程统计（功能④）：仅在链路 LIVE 且速度有效时累积，失效不计入 */
  {
    uint32_t now_trip = millis();
    uint32_t dt_trip = now_trip - s_trip_last_ms;
    if (dt_trip >= TRIP_STEP_MS) {
      s_trip_last_ms = now_trip;
      bool ok = (s_last_status == UI_STATUS_OK) && telemetry_plausible(&s_tel) &&
                telemetry_speed_kmh(&s_tel) >= 0.0f;
      telemetry_trip_step(&s_trip, telemetry_speed_kmh(&s_tel), ok, dt_trip);
      ui_speed_set_trip((float)s_trip.distance_km, s_trip.max_kmh);
    }
  }
  ble_poll();                 /* 取 BLE 遥测 + 校时（无新数据直接返回） */
  ble_update();               /* 五态状态机，每循环驱动速度页 */
  /* 消费 flick：快速划动直接翻页（100Hz 采样判定，见 touch_sample_cb） */
  if (s_tp_flick != 0) {
    int8_t dir = s_tp_flick;
    s_tp_flick = 0;
    s_tp_settle = false; /* flick 优先，避免再吸附一次 */
    int before = (int)ui_pages_current();
    ui_pages_flick(dir);
    int after = (int)ui_pages_current();
    Serial.printf("[翻页] flick dir=%d：页 %d → %d%s\n", (int)dir, before, after,
                  before == after ? "（已到边界）" : "");
  }
  else if (s_tp_settle) {
    s_tp_settle = false;
    ui_pages_settle();
  }

  s_page_seen = (int)ui_pages_current(); /* 记录当前页，供手势诊断取 before */
  {
    uint32_t t_render = micros();
    lv_timer_handler();
    uint32_t dt = micros() - t_render;
    if (dt > s_render_us_max) s_render_us_max = dt;
    s_render_us_sum += dt;
  }
  delay(5);

  /* 心跳日志：每 5 秒打印运行状态 + 重绘/循环速率（性能回归监测）*/
  static uint32_t last = 0;
  static uint32_t last_flush = 0, last_loop = 0;
  if (millis() - last > 5000) {
    uint32_t now = millis();
    uint32_t dt = now - last;
    last = now;
    uint32_t f = s_flush_cnt, l = s_loop_cnt;
    Serial.printf("[运行] %lu s  空闲堆 %u B  循环 %u/s  重绘 %u/s  缓冲=%s\n",
                  (unsigned long)(now / 1000), (unsigned)ESP.getFreeHeap(),
                  (unsigned)((l - last_loop) * 1000UL / dt),
                  (unsigned)((f - last_flush) * 1000UL / dt),
                  s_buf_psram_active ? "PSRAM全帧" : "内部1/10");
    Serial.printf("       渲染最大 %lu ms（占空 %u%%）｜QSPI 传输 合计 %lu ms 单次最大 %lu ms\n",
                  (unsigned long)(s_render_us_max / 1000),
                  (unsigned)(s_render_us_sum / (dt * 10)),
                  (unsigned long)(s_flush_us_sum / 1000),
                  (unsigned long)(s_flush_us_max / 1000));
    s_render_us_max = 0;
    s_render_us_sum = 0;
    s_flush_us_sum = 0;
    s_flush_us_max = 0;
    last_flush = f;
    last_loop = l;
  }
}
