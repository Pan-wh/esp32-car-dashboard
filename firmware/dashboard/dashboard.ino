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
#include "ui_pages.h"
#include "ui_speed.h"
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
 * ⚠️ 90°/270° 实测**不可用**，保持 0（本机安装方向已按此调整）：
 *   - CO5300 驱动无法用寄存器做 90° 旋转（Arduino_CO5300::setRotation 只做 X/Y 翻转，
 *     源码注释 "CO5300 does not support rotation"；上游 master 同样如此）
 *   - 退而用 LVGL 软件旋转（sw_rotate）时，466×466 下每帧都要经 10KB 临时缓冲旋转，
 *     实测明显卡顿；分页滚动时旋转条带次序错乱，出现撕裂/果冻效应
 *     （LVGL 源码 lv_refr.c 亦明确禁止 full_refresh + sw_rotate）
 *   - 结论：本屏在 466×466 分辨率下不做整屏旋转；若日后确需竖屏，
 *     应**按目标方向重新排版界面**，而非旋转像素
 *
 * 仅供调试保留：串口命令 `rot 0/1/2/3` 可运行时切换（会带来上述性能代价）。
 */
#define DISPLAY_ROT 0

/* ---------------- 显示 ---------------- */

static Arduino_DataBus *bus = new Arduino_ESP32QSPI(
    LCD_CS /* CS */, LCD_SCLK /* SCK */, LCD_SDIO0 /* SDIO0 */, LCD_SDIO1 /* SDIO1 */,
    LCD_SDIO2 /* SDIO2 */, LCD_SDIO3 /* SDIO3 */);

static Arduino_CO5300 *gfx = new Arduino_CO5300(
    bus, LCD_RESET /* RST */, 0 /* rotation */, LCD_WIDTH /* width */, LCD_HEIGHT /* height */,
    6, 0, 0, 0);

static lv_disp_draw_buf_t draw_buf;
static lv_color_t buf[LCD_WIDTH * LCD_HEIGHT / 10]; /* 与官方示例相同的缓冲大小 */

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

/* 数据过期阈值：超过该时长无有效遥测 → 速度置无效（不显示旧值） */
#define BLE_STALE_MS 2500

/* 显示/存储时区：UTC+8（中国标准时间，无夏令时），第一版固定 */
#define TZ_OFFSET_HOURS 8

/* 主循环消费的遥测状态 */
static telemetry_msg_t s_tel;
static bool s_tel_valid = false; /* 最新一包是否有可用速度/定位 */
static bool s_had_data = false;  /* 是否收到过任意遥测（区分 WAIT LOC 与 STALE） */
static ui_speed_status_t s_last_status = UI_STATUS_CONNECTING;
static uint32_t s_implausible = 0; /* 被丢弃的不合理包计数（§7） */

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
  uint32_t w = (area->x2 - area->x1 + 1);
  uint32_t h = (area->y2 - area->y1 + 1);

#if (LV_COLOR_16_SWAP != 0)
  gfx->draw16bitBeRGBBitmap(area->x1, area->y1, (uint16_t *)&color_p->full, w, h);
#else
  gfx->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t *)&color_p->full, w, h);
#endif

  lv_disp_flush_ready(disp);
}

/*
 * 触摸读取：每帧轮询（比官方示例的「仅靠中断置位」更简单可靠，
 * 且能持续上报位置 —— LVGL 的手势（滑页）需要按住期间的连续坐标）。
 */
static void touchpad_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
  uint8_t n = touch.getPoint(touch_x, touch_y, touch.getSupportTouchPoint());

  if (n > 0) {
    last_x = touch_x[0];
    last_y = touch_y[0];
    data->state = LV_INDEV_STATE_PRESSED;
  }
  else {
    data->state = LV_INDEV_STATE_RELEASED;
  }
  tlog_add(last_x, last_y, n > 0);

  data->point.x = last_x;
  data->point.y = last_y;
}

static void lvgl_tick_cb(void *arg)
{
  (void)arg;
  lv_tick_inc(EXAMPLE_LVGL_TICK_PERIOD_MS);
}

static bool display_init(void)
{
  Wire.begin(IIC_SDA, IIC_SCL);

  if (!gfx->begin()) {
    Serial.println("[显示] gfx->begin() 失败");
    return false;
  }
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
   * 软件旋转（sw_rotate）默认为关：实测在 466×466 全屏刷新下明显卡顿，
   * 且滚动分页会出现撕裂/果冻效应（LVGL 官方也禁止 full_refresh + sw_rotate）。
   * CO5300 无法用寄存器做 90° 旋转，故 90°/270° 不作为常规方案；
   * 如确需 90°，应改用「界面按旋转后重新布局」而不是整屏软件旋转。
   */
#if DISPLAY_ROT != 0
  disp_drv.sw_rotate = 1;
#endif
#if DISPLAY_ROT == 1
  disp_drv.rotated = LV_DISP_ROT_90;
#elif DISPLAY_ROT == 2
  disp_drv.rotated = LV_DISP_ROT_180;
#elif DISPLAY_ROT == 3
  disp_drv.rotated = LV_DISP_ROT_270;
#else
  disp_drv.rotated = LV_DISP_ROT_NONE;
#endif

  lv_disp_drv_register(&disp_drv);

  static lv_indev_drv_t indev_drv;
  lv_indev_drv_init(&indev_drv);
  indev_drv.type = LV_INDEV_TYPE_POINTER;
  indev_drv.read_cb = touchpad_read_cb;
  lv_indev_drv_register(&indev_drv);

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
  Serial.println("       rot 0|1|2|3              屏幕内容旋转（0=0° 1=90° 2=180° 3=270°）");
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

void setup()
{
  Serial.begin(115200);
  delay(200);
  Serial.println("\n=== ESP32 车载仪表 启动 ===");
  Serial.printf("LVGL %d.%d.%d\n", lv_version_major(), lv_version_minor(), lv_version_patch());

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

  /* 业务 UI：全部页面（模拟时钟 / 数字时钟 / 速度表盘） */
  ui_pages_init();
  ui_pages_show(DEFAULT_PAGE);

  /*
   * 初始状态：无遥测 → 速度页显示无效（NO FIX / --），指针归零。
   * 收到有效 BLE 遥测后由 ble_poll 驱动。
   */
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

  /* 已同步且偏差小：不重写（手机 1-2Hz 连续发同一时间） */
  time_t now = time(NULL);
  int64_t diff_ms = (int64_t)now * 1000 - utc_ms;
  if (s_time_synced && diff_ms >= -2000 && diff_ms <= 2000) return false;

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
static void ble_poll(void)
{
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
  ui_speed_status_t st;

  if (!ble_telemetry_connected()) {
    st = UI_STATUS_CONNECTING;
  }
  else {
    uint32_t age = ble_telemetry_age_ms();
    if (!s_had_data)       st = UI_STATUS_WAIT_LOC;   /* 已连接，还没收到任何包 */
    else if (age == UINT32_MAX || age > BLE_STALE_MS) st = UI_STATUS_STALE;  /* 超时 */
    else if (!s_tel_valid) st = UI_STATUS_INVALID;    /* 收到但无效 */
    else                   st = UI_STATUS_OK;         /* 正常 */
  }

  ui_speed_set_status(st);
  s_last_status = st;

  if (st == UI_STATUS_OK) {
    bool speed_ok = s_tel.speed_valid && s_tel.speed_mps >= 0.0f;
    ui_speed_set_fix(true, 0);
    if (speed_ok) {
      ui_speed_set_speed(telemetry_speed_kmh(&s_tel));
      if (s_tel.has_course) ui_speed_set_heading((int)(s_tel.course_deg + 0.5f));
    }
  }
  else {
    ui_speed_set_fix(false, 0); /* 指针归零，不显示旧值 */
  }
}

void loop()
{
  handle_serial();            /* 串口命令：校时 / 切页 */
  ble_poll();                 /* 取 BLE 遥测 + 校时（无新数据直接返回） */
  ble_update();               /* 五态状态机，每循环驱动速度页 */
  s_page_seen = (int)ui_pages_current(); /* 记录当前页，供手势诊断取 before */
  lv_timer_handler();
  delay(5);

  /* 心跳日志：每 5 秒打印一次运行状态，便于串口排查 */
  static uint32_t last = 0;
  if (millis() - last > 5000) {
    last = millis();
    Serial.printf("[运行] %lu s  空闲堆 %u B\n", (unsigned long)(millis() / 1000),
                  (unsigned)ESP.getFreeHeap());
  }
}
