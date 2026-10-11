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
 * ⚠️ 未在硬件上验证：本工程尚未烧录到实机（等板卡到货）。
 *    GNSS（LC76G）读取层也未接入，故速度页当前显示 `NO FIX`（属预期行为）。
 */
#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <lvgl.h>

#include "Arduino_GFX_Library.h"
#include "pin_config.h"        /* 由 libraries/Mylibrary 提供 */
#include "lv_conf.h"           /* 由 libraries/ 提供（LVGL 8.4 配置） */
#include "SensorPCF85063.hpp"
#include "TouchDrvCSTXXX.hpp"  /* SensorLib 提供的 CST9217 驱动 */

/* 共享模块（C 代码，需 extern "C") */
extern "C" {
#include "ui_pages.h"
#include "ui_speed.h"
#include "ui_timesrc.h"
}

/* ---------------- 可调参数 ---------------- */

#define EXAMPLE_LVGL_TICK_PERIOD_MS 2      /* 与官方示例一致 */
#define DEFAULT_PAGE UI_PAGE_SPEED         /* 开机默认页：速度表盘（车机主视角） */

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
  Serial.println("       help                     显示本帮助");
}

static void process_command(char *cmd)
{
  if (strcmp(cmd, "help") == 0) {
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

  lvgl_init();

  /* 时间来源：RTC（失败时界面会显示无效应有的样子） */
  ui_timesrc_set_provider(rtc_time_provider);

  /* 业务 UI：全部页面（模拟时钟 / 数字时钟 / 速度表盘） */
  ui_pages_init();
  ui_pages_show(DEFAULT_PAGE);

  /*
   * GNSS：尚未接入 LC76G 读取层（待板卡到货后按官方 09_LC76G_I2C 的 I2C 协议实现），
   * 因此速度页当前显示 NO FIX、指针归零 —— 这是「数据无效」的正确表现，不是故障。
   */
  ui_speed_set_fix(false, 0);

  Serial.printf("[UI] 页面数 %d，默认页 %d(%s)，触摸%s\n", (int)UI_PAGE_COUNT, (int)DEFAULT_PAGE,
                page_name((int)DEFAULT_PAGE), tp_ok ? "就绪" : "不可用");
  print_time();
  print_help();
}

void loop()
{
  handle_serial(); /* 串口命令：校时 / 切页 */
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
