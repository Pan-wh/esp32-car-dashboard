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
#include <SD_MMC.h>
#include <nvs.h>
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
#include "nav_map.h"    /* 周边地图窗口的接收缓冲（阶段 4）*/
#include "ui_nav_map.h" /* 导航地图层（阶段 4）*/
#include "ui_baked_images.h" /* 烘焙图声明（非 const，启动时可充气）*/
#include "ui_img_inflate.h"  /* 索引图 → PSRAM RGB565 直通图 */
#include "nav.h"         /* 导航状态/动作枚举与文案（纯 C，共用） */
#include "ui_clock.h"    /* 数字时钟页（时间同步状态标签） */
#include "ui_timesrc.h"
#include "auto_bright.h"   /* 亮度曲线：按时段自动调 */
#include "bright_parse.h"   /* 亮度曲线包解析（App → 设备）*/
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
 * 0°/180° 使用 LVGL 低开销路径；90°/270° 使用 CO5300 MADCTL 硬件旋转。
 * 默认不旋转。完整取舍与真机数据见 docs/rotation-hardware-vs-software.md。
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

/* Arduino_GFX 会随 setRotation() 改写地址偏移；厂商 BSP 要求始终保持 (6,0)。 */
class DashboardCO5300 : public Arduino_CO5300 {
public:
  DashboardCO5300(Arduino_DataBus *b, int8_t rst, uint8_t r, int16_t w, int16_t h,
                  uint8_t c1, uint8_t r1, uint8_t c2, uint8_t r2)
      : Arduino_CO5300(b, rst, r, w, h, c1, r1, c2, r2) {}

  void setGap(uint8_t x, uint8_t y)
  {
    _xStart = x;
    _yStart = y;
    _currentX = -1; /* 强制下一帧重设 CASET/PASET */
    _currentY = -1;
  }
};

/*
 * 构造参数末尾 4 个是 col_offset1 / row_offset1 / col_offset2 / row_offset2。
 * 官方 BSP 的做法是 `esp_lcd_panel_set_gap(panel, 0x06, 0)` —— **固定 (6, 0)**，
 * 旋转时只改 MADCTL、不动 gap。本工程照此对齐：(6, 0, 0, 0)。
 */
static DashboardCO5300 *gfx = new DashboardCO5300(
    bus, LCD_RESET /* RST */, 0 /* rotation */, LCD_WIDTH /* width */, LCD_HEIGHT /* height */,
    6, 0, 0, 0);

/* ══════════════ 旋转路径 ══════════════
 *
 * 0°/180° 使用 LVGL 的低开销路径；90°/270° 按厂商 BSP 使用 CO5300
 * MADCTL 硬件旋转。硬件旋转在本屏会留下少量边缘黑缺口，但能保持秒针、
 * 指针和滑页的实时性。软件 flush 转置虽能消除缺口，真机实测会明显卡顿，
 * 已明确弃用；完整数据见 docs/rotation-hardware-vs-software.md。
 */
static uint8_t s_touch_rot = 0;

/*
 * 面板的 466x466 可见窗口使用固定地址偏移 (6,0)。厂商 ESP-IDF BSP 在
 * MADCTL 0x00/0x60/0xC0/0xA0 四种方向下都保留这个 gap；MADCTL 自己负责
 * 扫描方向，主控不应再把 gap 换轴或按 480x480 GRAM 反推第二次。
 */
/*
 * ── 调试输出闸门（装车运行的关键保护）──────────────────────────────
 *
 * ESP32-S3 的 USB-CDC `Serial.printf()` 在**没有主机接收**时会阻塞
 * （等待 TX FIFO 空位）。实测单次可阻塞近 1 秒 —— 主循环被卡住的期间
 * LVGL 定时器持续积压，于是下一次 `lv_timer_handler()` 要跑近 1 秒。
 *
 * 装车后设备不接电脑，若仍无条件打印就会表现为
 * **"跑一段时间就很卡"**（每次周期打印都卡一下）。
 * 这同时解释了"接着电脑调试时又不卡"——有主机接收，写入不阻塞。
 *
 * 故所有**周期性**打印都必须先过这道闸门：
 *   - 主机连着（USB-CDC 的 DTR 有效，即有人开着串口）→ 正常输出
 *   - 独立运行（车里没接电脑）→ 直接跳过，绝不阻塞
 * 串口命令的应答**不需要**闸门：能发命令时主机必然连着。
 * `verbose on|off` 可强制打开（以防某平台 DTR 检测不可靠）。
 */
static bool s_dbg_force = false;
/**
 * 是否允许打印**周期性**调试信息（BLE 2Hz 行、5 秒心跳、触摸日志）。
 *
 * ⚠️ 除了"有没有串口"，还必须检查**发送缓冲余量** —— 这是踩过的坑：
 *
 * 控制台走 USB CDC（`CDCOnBoot=cdc`）。当**没有读端**时（没插电脑、
 * 或串口监视器/日志记录器没在跑），CDC 的发送缓冲会填满，
 * 此时 `Serial.printf` **会阻塞**，把主循环（甚至 BLE 回调）卡死 ——
 * 现象是设备"活着但什么都不打印、也不响应命令"，只能复位。
 *
 * 所以周期性日志一律"写不下就跳过这一条"：丢一行日志远比卡死设备划算。
 * 按需输出（命令响应）不受此限，因为那本身就意味着有人在交互。
 */
static inline bool dbg_on(void)
{
  if (!(s_dbg_force || (bool)Serial)) return false;
  /* 256 B 余量：够放最长的一条心跳（约 200 B），避免 printf 中途被卡住 */
  return Serial.availableForWrite() >= 256;
}

/*
 * ── 中断间隔诊断（区分"中断被关"与"任务被抢占"）────────────────────
 *
 * 主循环偶发停滞约 1 秒（实测 lv_timer_handler 单次耗时 1003~1035ms）。
 * 两种可能必须区分开，否则会修错地方：
 *   A) **全局中断被关闭**（flash 写入 / cache 禁用）→ 连硬件中断也停
 *   B) **主循环任务被更高优先级任务抢占** → 中断照常，只是任务拿不到 CPU
 *
 * 做法：1kHz 硬件定时器中断只做"打时间戳"，记录相邻两次中断的最大间隔。
 *   - 该间隔 ≈ 1 秒 → 情况 A（中断被关）
 *   - 该间隔很小    → 情况 B（任务被抢占）
 */
/*
 * ── 主循环分阶段计时（定位"~1 秒停滞"落在哪一段）──────────────────
 *
 * 已知：停滞时硬件中断间隔最大仅 0us（中断正常），且提高 loop 优先级会显著恶化
 * → 是**阻塞等待**而非被抢占。那就必须测出"哪一段调用阻塞了"。
 */
enum { PH_SERIAL = 0, PH_CONTROL, PH_FACE, PH_RAMP, PH_TRIP, PH_POLL, PH_TOUCH, PH_RENDER, PH_N };
static const char *const PH_NAME[PH_N] = { "串口", "BLE控制", "表盘检测", "斜坡",
                                           "行程", "BLE轮询", "触摸", "渲染" };
static uint32_t s_ph_max_us[PH_N] = { 0 };
static uint32_t s_ph_t0 = 0;
static inline void ph_begin(void) { s_ph_t0 = micros(); }
static inline void ph_end(int i)
{
  uint32_t d = micros() - s_ph_t0;
  if (d > s_ph_max_us[i]) s_ph_max_us[i] = d;
}

/*
 * 单次 lv_timer_handler() 里的 flush 次数。
 * 停滞时若该值极大（几十上百），说明是"一次调用内反复重绘"
 * （渲染循环 / 对象自失效），而非某一次写入慢 —— 这与
 * "中断正常 + QSPI 单次仅 7ms"的观测吻合。
 */
static uint32_t s_flush_in_call = 0;
static uint32_t s_flush_per_call_max = 0;

static volatile uint32_t s_isr_gap_us_max = 0;
static volatile uint32_t s_isr_cnt = 0;
static uint32_t s_isr_last_us = 0;

static void IRAM_ATTR isr_gap_cb(void)
{
  uint32_t now = micros();
  if (s_isr_last_us != 0) {
    uint32_t gap = now - s_isr_last_us;
    /* 只统计异常间隔，避免常态抖动把最大值抬高 */
    if (gap > 2000 && gap > s_isr_gap_us_max) s_isr_gap_us_max = gap;
  }
  s_isr_last_us = now;
  s_isr_cnt++;
}

/*
 * ── TF / microSD 卡 ────────────────────────────────────────────────
 *
 * 引脚取自厂商 BSP（`esp32_s3_touch_amoled_1_75.c`）与硬件参考手册：
 *   SD_CLK = GPIO2、SD_CMD = GPIO1、SD_D0 = GPIO3（**1-bit SDMMC**）
 * （SD_D3/CS = GPIO41 虽在 PCB 上连着，但 1-bit 模式不用它。）
 *
 * 为什么现在只挂载、不做别的：先把"卡在不在、多大、能不能读写"确认下来，
 * 后续导航的地图包/离线资源要放这里（见 docs/nav-backend.md 的待办）。
 *
 * ⚠️ 用 `SD_MMC`（原生 SDMMC 控制器）而不是 `SD`(SPI)：本板就是原生接法，
 *    走 SPI 需要占用其它 GPIO 且速率低得多。
 */
#define SD_CLK_PIN 2
#define SD_CMD_PIN 1
#define SD_D0_PIN  3

static bool s_sd_ok = false;
static uint32_t s_sd_retry_ms = 0;

/** 挂载 TF 卡（1-bit SDMMC）。可重复调用；失败不 format（绝不能悄悄格式化用户的卡）。 */
static bool sd_mount(void)
{
  if (s_sd_ok) return true;
  SD_MMC.setPins(SD_CLK_PIN, SD_CMD_PIN, SD_D0_PIN);
  /* begin(mountpoint, mode1bit, format_if_mount_failed, max_files) */
  if (!SD_MMC.begin("/sdcard", true, false, 5)) {
    s_sd_ok = false;
    return false;
  }
  s_sd_ok = true;
  return true;
}

/** 打印卡信息。`verbose=false` 时只报"在/不在"，供周期性状态行使用。 */
static void sd_report(bool verbose)
{
  if (!sd_mount()) {
    Serial.println("[SD] 未检测到 TF 卡（或未格式化）");
    return;
  }
  const uint8_t type = SD_MMC.cardType();
  const char *typeName = (type == CARD_MMC)  ? "MMC"
                         : (type == CARD_SD) ? "SDSC"
                         : (type == CARD_SDHC) ? "SDHC"
                                               : "未知";
  const uint64_t total = SD_MMC.totalBytes();
  const uint64_t used = SD_MMC.usedBytes();
  Serial.printf("[SD] 已挂载：类型=%s 容量=%.1f GB 已用=%.2f GB 可用=%.2f GB\n",
                typeName, (double)total / 1073741824.0, (double)used / 1073741824.0,
                (double)(total - used) / 1073741824.0);
  if (!verbose) return;

  /* 列根目录（最多 20 项，避免刷屏） */
  File root = SD_MMC.open("/");
  if (!root || !root.isDirectory()) {
    Serial.println("[SD] 打不开根目录");
    return;
  }
  Serial.println("[SD] 根目录：");
  int n = 0;
  for (File f = root.openNextFile(); f && n < 20; f = root.openNextFile(), n++) {
    if (f.isDirectory()) {
      Serial.printf("       <DIR>  %s\n", f.name());
    }
    else {
      Serial.printf("       %8u B  %s\n", (unsigned)f.size(), f.name());
    }
  }
  if (n == 0) Serial.println("       （空）");
  if (n >= 20) Serial.println("       …（仅列前 20 项）");
}

#define PANEL_NATIVE_GAP_X 6
#define PANEL_NATIVE_GAP_Y 0


static lv_disp_draw_buf_t draw_buf;
static lv_color_t buf[LCD_WIDTH * LCD_HEIGHT / 10]; /* 与官方示例相同的缓冲大小 */

/* 性能诊断：重绘/循环速率（卡顿回归的客观指标，见 docs/troubleshooting.md）*/
static volatile uint32_t s_flush_cnt = 0;
static uint32_t s_loop_cnt = 0;
static volatile uint32_t s_render_us_max = 0; /* 窗口内单次 lv_timer_handler 最大耗时 */
static volatile uint32_t s_render_us_sum = 0;  /* 窗口内渲染总耗时（算占空比） */
static volatile uint32_t s_flush_us_sum = 0;   /* 窗口内 QSPI 传输累计耗时（从渲染耗时中区分） */
static volatile uint32_t s_flush_us_max = 0;   /* 窗口内单次 flush 最大耗时 */
static volatile uint32_t s_flush_area_max = 0; /* 窗口内单次 flush 最大面积（像素）*/
static volatile uint32_t s_flush_area_sum = 0; /* 窗口内 flush 面积累计 */

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
static portMUX_TYPE s_tp_mux = portMUX_INITIALIZER_UNLOCKED;

/*
 * flick 方向判定的旋转补偿（详见 docs/rotation-hardware-vs-software.md）。
 * 采样拿到的是**面板原始坐标**，而"左滑/右滑"应按 **LVGL 逻辑坐标**判定。
 * LVGL 自己的 indev 会对指针坐标做逆变换（lv_indev.c: indev_pointer_proc），
 * 但这条 100Hz 自建快路径绕过了它 → 90°/270° 时"左右滑"在面板上变成上下滑，
 * flick 判不出来，翻页会退化成慢速原生拖拽。
 */
/** 面板原始位移 → LVGL 逻辑位移（映射与 lv_indev.c 的旋转规则严格一致）*/
static void flick_delta_to_logical(int *dx, int *dy)
{
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
  /*
   * ⚠️ 星期必须钳制到 0..6。
   * PCF85063 的星期寄存器异常时可能给出 7+，而 UI 侧用
   * WD[tm_wday] 索引 7 元素数组 —— 越界取野指针后会崩在 snprintf 的 %s 上
   * （实测 Core 1 LoadProhibited，backtrace 见 ui_clock.c 的同名注释）。
   * 两处都设防：这里治本，UI 侧兜底。
   */
  int wd = (int)dt.getWeek();
  if (wd < 0 || wd > 6) wd = 0;
  out->tm_wday = wd;
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
  s_flush_in_call++;
  uint32_t w = (area->x2 - area->x1 + 1);
  uint32_t h = (area->y2 - area->y1 + 1);
  {
    uint32_t a = w * h;
    s_flush_area_sum += a;
    if (a > s_flush_area_max) s_flush_area_max = a;
  }

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
static esp_timer_handle_t s_tp_timer = NULL; /* 供 `tp` 命令停/起，用于定位 I2C 锁竞争 */
static void touch_sample_cb(void *)
{
  uint8_t n = touch.getPoint(touch_x, touch_y, touch.getSupportTouchPoint());
  bool was = s_tp_pressed;

  portENTER_CRITICAL(&s_tp_mux);
  if (n > 0) {
      /* 面板原生坐标 → 显示坐标（硬件旋转时两者相差一个旋转）*/
      int16_t tx = touch_x[0], ty = touch_y[0];
      touch_apply_rot(&tx, &ty);
    if (!was) { /* 按下瞬间：记录起点，用于 flick 判定 */
      s_tp_start_x = tx;
      s_tp_start_y = ty;
      s_tp_start_ms = millis();
    }
    s_tp_pressed = true;
    s_tp_x = tx;
    s_tp_y = ty;
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
      if (dt < 600 && adx > 50 && adx > (ady * 12) / 10) {
        s_tp_flick = (dx < 0) ? 1 : -1; /* 左滑 → 下一页 */
      }
      else {
        s_tp_settle = true; /* 缓慢拖拽：按最近页吸附 */
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

/*
 * 前置声明：定义在下面（那里能用到 s_display_rot / s_faces_dirty）。
 * C++ 要求先声明后使用，故在此声明、在下方定义。
 */
static void sync_display_report_state(void);

/**
 * 亮度曲线状态（App 经 BLE 下发，见 bright_parse.h）。
 *
 * 为什么由 **App** 决定曲线而不是设备自己存：曲线是"用户偏好"，
 * 而 App 才是用户改设置的地方；设备只负责按时段应用。
 * 每次连接 App 都会重发，所以设备**不需要**持久化（也避免两边不一致）。
 *
 * ⚠️ 声明必须放在 `apply_brightness_user()` **之前**（C++ 先声明后使用）。
 */
static auto_bright_config_t s_bright_cfg;
static bool s_bright_cfg_valid = false; /* 是否收到过 App 的曲线 */
/**
 用户是否手动调过亮度（**手动优先**）。

 一旦为 true，自动曲线就**不再改亮度**，直到：
   - App 重发曲线（用户在 App 里重新打开自动开关），或
   - 设备重启
 否则会出现"我调好了它又给我改回去"的经典烦躁体验。
 */
static bool s_bright_user_override = false;
/** 上次应用过的亮度值（避免每帧重复写面板命令）*/
static int s_bright_last_applied = -1;
/**
 本次开机是否已成功同步过一次曲线。

 用于区分"开机后首次收到曲线"与"蓝牙重连重发"：
   - 首次：必须应用（否则曲线永远不生效）
   - 重连且内容相同：**不应用**（否则会覆盖用户手动调的亮度，
     表现为"亮度突然像被重置"）
 */
static bool s_bright_first_sync = false;
/**
 请求亮度曲线**立即**重算（跳过 20 秒节流）。
 
 由 `time_sync_maybe()` 在校时成功后置位：RTC 时间可能大幅跳变
 （如开机时 RTC 无效、App 连上才校到真实时间），若仍等满 20 秒，
 屏幕会在这段时间里停在按 00:00 算出的夜间亮度上。
 
 必须声明在此处（apply_brightness / time_sync_maybe 都会用到）。
 */
static volatile bool s_bright_recalc_now = false;

/**
 亮度**平滑过渡**（目标值），-1 表示无过渡在进行。
 
 ## 为什么需要
 
 App 拖动滑块时以 20Hz 连发亮度，但固件对 BLE 命令做的是 **latest-wins 合并**
 （`s_pending_cmd` 只有一个槽位）。多条命令在同一个主循环帧之前到达时，
 中间值被丢弃 → 屏幕**跳变**到最终值，而不是逐格跟随。
 用户形容就是"发个包重置的感觉"。
 
 ## 做法
 
 命令只设置**目标值**，主循环每帧朝目标推进一小步（`BRIGHT_STEP_PER_FRAME`）。
 这样：
   - 不依赖命令到达频率（丢了多少中间值都无所谓，最终会走到目标）
   - 视觉上是连续渐变，不会"啪"地跳
 */
static int s_bright_target = -1;

/** 每帧亮度最多变化多少（255 满量程约 1.3 秒走完，与拖动速度匹配）*/
#define BRIGHT_STEP_PER_FRAME 3

/**
 应用亮度（底层，**不要直接调用**）。
 
 ⚠️ 请改用下面两个语义明确的入口：
   - `apply_brightness_user()`  —— 用户在 App/串口上手动调的
   - `apply_brightness_auto()`  —— 自动曲线调的
 用两个入口而不是一个，是为了让"手动优先"这条规则**由类型区分**，
 而不是靠调用方记得去置标志位（那样迟早会漏一处，
 表现为"用户调完亮度又被自动改回去"，且极难复现）。
 */
static void apply_brightness(uint8_t v)
{
  s_brightness = v;
  gfx->setBrightness(v);
  sync_display_report_state();
}

/**
 用户**手动**改亮度 → 置"手动优先"，自动曲线从此让位。
 直到 App 重发曲线（用户在 App 里重开自动）或设备重启。
 */
static void apply_brightness_user(uint8_t v)
{
  apply_brightness(v);
  /*
   * 只有"自动本来是开着的"才需要置覆盖标志：
   * 若自动本就没开，用户拖亮度是常规操作，不该被记成"覆盖"。
   */
  if (s_bright_cfg_valid && s_bright_cfg.enabled) {
    if (!s_bright_user_override) {
      Serial.println("[亮度] 用户手动调节 → 自动曲线暂停（重开自动或重启后恢复）");
    }
    s_bright_user_override = true;
  }
  /* 让自动曲线知道"当前值就是它算的那个"，避免下一帧又写一次 */
  s_bright_last_applied = (int)v;
}

/** 自动曲线应用亮度（不置覆盖标志）*/
static void apply_brightness_auto(uint8_t v)
{
  apply_brightness(v);
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
  sync_display_report_state();
}

/* 消费一条 BLE 下行控制命令（主循环调用，BLE 回调只暂存） */
/*
 * ---------------- 屏幕方向 ----------------
 *
 * 说明：本屏为 1.75" **圆形** 466×466，旋转后仍是同一个圆，
 *       无长宽比/布局副作用；触摸坐标由 LVGL 自动逆变换（indev_pointer_proc）。
 *
 * 持久化：方向存入 NVS，重启后自动恢复（用户设置不应每次掉电丢失）。
 */
#define ROT_PREF_NS  "dash"
#define ROT_PREF_KEY "rot"
#define ROT_POLICY_PREF_KEY "rot_policy"
#define ROT_POLICY_VERSION  2
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
/** 表盘列表、启用状态或显示状态（亮度/方向/息屏）有变化，需要重新上报 */
static bool s_faces_dirty = false;

/**
 * 把"设备显示状态"（亮度/方向/息屏）同步进上报表，供 App 显示**真值**。
 *
 * 为什么要有：这三项是**设备的属性**，不是 App 的设置。用 App 调好亮度后
 * 断掉 App、带着仪表上车，亮度仍是那个值；下次单独给仪表上电（压根没开 App）
 * 也应保持。所以**绝不能**在连接时由 App 下发一份"它记得的亮度"，那会把
 * 用户在设备上设好的值悄悄冲掉。正确方向是：设备上报 → App 显示。
 *
 * ⚠️ 定义放在这里（而非 apply_brightness 旁边）是因为要用到上面这两个变量，
 *    C++ 要求先声明后使用（上面已有前置声明供 apply_* 调用）。
 */
static void sync_display_report_state(void)
{
  face_report_display_t d;
  d.brightness = (int)s_brightness;
  d.rotation = s_display_rot; /* -1 = 尚未设置 → 上报时省略该字段 */
  d.screen_on = s_screen_off ? 0 : 1;
  /*
   * 行程统计（P1-1）：由**设备**累积并上报，App 只显示。
   *
   * ⚠️ 每个字段都必须显式赋值 —— C 的结构体局部变量**不会自动清零**，
   *    漏掉的字段会带上栈上的垃圾值（可能被当成合法里程发出去）。
   */
  d.trip_distance_km = s_trip.distance_km;
  d.trip_max_kmh = (double)s_trip.max_kmh;
  d.trip_avg_kmh = (double)telemetry_trip_avg_kmh(&s_trip);
  d.trip_time_s = (long)(s_trip.time_ms / 1000);
  face_report_set_display(&d);
  s_faces_dirty = true; /* 让主循环立刻重新上报 */
}

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

  const bool hw = (deg == 90 || deg == 270);
  if (hw) {
    /*
     * 厂商 BSP：方向 1/3 分别写 MADCTL 0x60/0xA0，并始终保留 gap=(6,0)。
     * 本项目的视觉角度命名与面板扫描方向相反，因此 270° 对应 0x60。
     * 先调用库旋转以同步内部地址状态，再恢复厂商固定 gap，最后覆盖 MADCTL。
     */
    const uint8_t mad = (deg == 270) ? 0x60 : 0xA0;
    const uint8_t librot = (deg == 270) ? 3 : 1;
    gfx->setRotation(librot);
    gfx->setGap(PANEL_NATIVE_GAP_X, PANEL_NATIVE_GAP_Y);
    bus->beginWrite();
    bus->writeC8D8(0x36, mad);
    bus->endWrite();

    s_touch_rot = (deg == 270) ? 3 : 1; /* 270° 已现场标定；90° 按镜像推得，未逐一标定 */
    lv_disp_set_rotation(NULL, LV_DISP_ROT_NONE);
    s_display_rot = deg;
    lv_obj_invalidate(lv_scr_act());
  }
  else {
    /* 0°/180° 的 LVGL 路径完整且开销低；面板恢复原生方向。 */
    gfx->setRotation(0); /* 内部会写 MADCTL=0 */
    gfx->setGap(PANEL_NATIVE_GAP_X, PANEL_NATIVE_GAP_Y);
    s_touch_rot = 0;
    lv_disp_set_rotation(NULL, rot_deg_to_enum(deg));
    s_display_rot = deg;
    lv_obj_invalidate(lv_scr_act());
  }

  if (persist) {
    Preferences prefs;
    if (prefs.begin(ROT_PREF_NS, false)) {
      prefs.putInt(ROT_PREF_KEY, deg);
      prefs.end();
    }
  }
  /*
   * 方向变了 → 通知 App 重新上报（方向同样是"设备属性、设备上报"）。
   * 放在两个分支之后统一调用，避免只在某一条路径上同步而漏掉另一条。
   */
  sync_display_report_state();
  Serial.printf("[旋转] 屏幕方向 -> %d°（%s）%s\n", deg,
                hw ? "CO5300 硬件旋转" : "LVGL 低开销路径",
                persist ? "（已存 NVS）" : "（未持久化）");
}

/** 启动时恢复方向；旋转策略升级时先回到编译期默认，之后继续保存用户设置。 */
static void restore_display_rotation(void)
{
  Preferences prefs;
  int saved = DISPLAY_ROT * 90;   /* 编译期默认 */
  bool from_nvs = false;
  bool policy_reset = false;
  if (prefs.begin(ROT_PREF_NS, false)) {
    const uint8_t policy = prefs.getUChar(ROT_POLICY_PREF_KEY, 0);
    if (policy != ROT_POLICY_VERSION) {
      /* 软件转置试验曾保存过旋转值；升级后按需求一次性恢复默认 0°。 */
      prefs.putInt(ROT_PREF_KEY, saved);
      prefs.putUChar(ROT_POLICY_PREF_KEY, ROT_POLICY_VERSION);
      policy_reset = true;
    }
    else {
      from_nvs = prefs.isKey(ROT_PREF_KEY);
      saved = prefs.getInt(ROT_PREF_KEY, saved);
    }
    prefs.end();
  }
  set_display_rotation(saved, false);
  Serial.printf("[旋转] 启动恢复方向 = %d°（来源：%s）\n", s_display_rot,
                policy_reset ? "策略升级，重置为默认" :
                from_nvs ? "NVS（上次用户设置）" : "编译期默认 DISPLAY_ROT");
}

/**
 * 触摸坐标变换（面板原生 → 显示坐标）。
 *
 * 面板的 MADCTL 会把**画面**旋转，但 CST9217 的触摸坐标固定在**物理面板**上，
 * 两者相差一个旋转。所以在采样处就变换成显示坐标 —— 这样 LVGL 的 indev 与
 * flick 手势判定**都自动正确**，不必各自打补丁。
 *
 * @param x,y 原地变换
 */
static void touch_apply_rot(int16_t *x, int16_t *y)
{
  if (s_touch_rot == 0) return;
  const int16_t W = LCD_WIDTH - 1, H = LCD_HEIGHT - 1;
  const int16_t ix = *x, iy = *y;
  switch (s_touch_rot) {
    case 1: *x = (int16_t)(H - iy); *y = ix; break;                 /* 顺时针 90° */
    case 2: *x = (int16_t)(W - ix); *y = (int16_t)(H - iy); break;  /* 180° */
    case 3: *x = iy; *y = (int16_t)(W - ix); break;                 /* 逆时针 90°（= 顺时针 270°）*/
    default: break;
  }
}

/*
 * ── 方向切换的"合并"（避免连点导致画面长时间卡住）────────────────
 *
 * 背景：每次切换屏幕方向都必须**整屏重绘**（实测渲染 101ms + QSPI 44ms）
 * 并写一次 NVS。App 的方向选择器每点一下发一条命令，连点 7 次就会排队
 * 执行 7 次 → 约 1.5 秒画面像冻住（用户报"点旋转后卡死了"）。
 *
 * 做法：收到命令只记下**目标值**，等 ROT_COALESCE_MS 内没有新命令才真正执行。
 * 连点任意多次只会执行最后一次，代价从 N×200ms 降到 1×200ms。
 */
#define ROT_COALESCE_MS 300
static int s_pending_rot = -1; /* <0 = 无待执行 */
static uint32_t s_pending_rot_ms = 0;

/* 主循环调用：安静了才真正切方向 */
static void rotation_poll(void)
{
  if (s_pending_rot < 0) return;
  if ((uint32_t)(millis() - s_pending_rot_ms) < ROT_COALESCE_MS) return;
  const int deg = s_pending_rot;
  s_pending_rot = -1;
  set_display_rotation(deg, true);
}

static void control_poll(void)
{
  control_cmd_t cmd;
  if (!ble_telemetry_take_command(&cmd)) return;

  switch (cmd.kind) {
    case CONTROL_BRIGHTNESS: {
      uint8_t v = control_clamp_brightness(cmd.value);
      /*
       * ⚠️ 只设**目标值**，不直接写面板。
       *
       * 原因：App 拖动滑块时以 20Hz 连发，而固件对命令是 latest-wins 合并 ——
       * 直接应用会让屏幕**跳变**到最终值（用户形容"像发个包重置"）。
       * 设目标后由主循环逐帧逼近，视觉上是连续渐变。
       *
       * 同时置"手动优先"（用户明确调了亮度）。
       */
      s_bright_target = (int)v;
      if (s_bright_cfg_valid && s_bright_cfg.enabled) {
        if (!s_bright_user_override) {
          Serial.println("[亮度] 用户手动调节 → 自动曲线暂停（重开自动或重启后恢复）");
        }
        s_bright_user_override = true;
      }
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
    case CONTROL_DEMO_SPEED: {
      /*
       * 演示速度（App 的"模拟速度测试"滑块）。value < 0 = 退出演示。
       *
       * 安全约定：**不以 App 的退出命令为准** —— 收到真实遥测或 BLE 断开时，
       * 固件自己退出（见 telemetry_apply 与 ble 断开处理）。否则命令一丢，
       * 演示速度就会在真正开车时继续显示，那是危险的。
       */
      const int32_t v = cmd.value;
      ui_speed_set_demo(v < 0 ? -1.0f : (float)v);
      /* 注意把数值也打出来：第一版格式串写成 "%s%s" 却只传了方向字符串，
         日志变成"[演示] 速度 →"没有数值，真机排查时看不出到底设了多少 */
      if (v < 0) {
        Serial.println("[演示] 退出（→ 状态行交还链路状态）");
      } else {
        Serial.printf("[演示] 速度 → %ld km/h（状态行显示 DEMO）\n", (long)v);
      }
      break;
    }
    case CONTROL_ROTATE: {
      /*
       * 只暂存目标值（合并连点），真正执行交给 rotation_poll()。
       * 已经是当前方向则直接忽略，连一次重绘都不做。
       */
      const int deg = (int)cmd.value;
      if (deg == s_display_rot) break;
      s_pending_rot = deg;
      s_pending_rot_ms = millis();
      break;
    }
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

  /* 初始方向（编译期默认；运行时可用 lvrot 命令或 BLE 命令修改）*/
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

  /* PSRAM 全帧绘制缓冲：partial 模式下，缓冲 ≥ 脏区时 LVGL 只切 1 块。 */
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
    if (esp_timer_create(&tp_args, &s_tp_timer) == ESP_OK) {
      esp_timer_start_periodic(s_tp_timer, 10000); /* 10ms = 100Hz（1MHz 时基 → 微秒） */
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
  Serial.println("       map                      地图窗口诊断（收到多少/能否画/自车偏移）");
  Serial.println("       sd [list]                TF 卡状态（`sd list` 连根目录一起列）");
  Serial.println("       scan                     I2C 总线扫描（确认器件在线）");
  Serial.println("       ble                      显示 BLE 连接与遥测状态");
  Serial.println("       bright <0-255>            设置亮度（不带参数=查询）");
  Serial.println("       screen on|off             亮屏 / 息屏");
  Serial.println("       trip [reset]              行程统计：里程/最高速/均速");
  Serial.println("       face [id|next|prev]       查看/切换表盘（不带参数=列出全部）");
  Serial.println("       nav [st mv dist [spd]]   导航卡片模拟数据（阶段1）：st=0..5 mv=0..7");
  Serial.println("                                  例: nav 1 2 500 68 → 导航中·左转·500m·68km/h");
  Serial.println("       demo [kmh|off]           演示速度（App 滑块的调试入口；状态行显示 DEMO）");
  Serial.println("       lvrot [0|1|2|3]          临时切换 0/90/180/270°（不写入 NVS）");
  Serial.println("       mem                      显示 LVGL/ESP 内存情况");
  Serial.println("       dump                     打印当前屏幕的元素树（排查元素缺失）");
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
  /* 上界用 UI_STATUS_COUNT：见 ui_speed.h 里关于"往枚举末尾加值"的说明 */
  int si = (s_last_status >= 0 && s_last_status < UI_STATUS_COUNT) ? (int)s_last_status : 0;
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

static void process_command(char *cmd)
{
  if (strcmp(cmd, "map") == 0) {
    /*
     * 地图窗口诊断：看"有没有收到、够不够画、自车在哪儿"。
     * 排查链路（手机 → BLE → 解析 → 渲染）时先看这里。
     */
    int ox = 0, oy = 0;
    const bool has_off = nav_map_get_vehicle_offset(&ox, &oy);
    const nav_map_window_t *w = nav_map_active();
    Serial.printf("[地图] 消息=%u 窗口=%u 丢弃=%u\n",
                  (unsigned)nav_map_msg_count(), (unsigned)nav_map_window_count(),
                  (unsigned)nav_map_drop_count());
    if (w == NULL) {
      Serial.println("[地图] 当前无可用窗口（未收到 / 已过期）");
    }
    else {
      int rp = 0, bp = 0;
      for (int i = 0; i < w->n_roads; i++) rp += w->roads[i].n;
      for (int i = 0; i < w->n_bldgs; i++) bp += w->bldgs[i].n;
      Serial.printf("[地图] 窗口：道路 %d 条/%d 点，建筑 %d 栋/%d 点，路线 %d 点\n",
                    w->n_roads, rp, w->n_bldgs, bp, w->n_route);
      if (w->n_roads > 0) {
        Serial.printf("[地图] 第 0 条道路：等级=%d 点数=%d 首点=(%d,%d)\n",
                      w->roads[0].cls, w->roads[0].n,
                      w->roads[0].pts[0].x, w->roads[0].pts[0].y);
      }
    }
    {
      Serial.printf("[地图] 提交瞬间 build=%u/%u/%u → active=%u/%u/%u\n",
                    (unsigned)g_nav_map_dbg_build[0], (unsigned)g_nav_map_dbg_build[1],
                    (unsigned)g_nav_map_dbg_build[2],
                    (unsigned)g_nav_map_dbg_commit[0], (unsigned)g_nav_map_dbg_commit[1],
                    (unsigned)g_nav_map_dbg_commit[2]);
      const uint8_t *ord = NULL;
      const uint32_t on = nav_map_order(&ord);
      if (on > 0) {
        Serial.print("[地图] 消息顺序：");
        for (uint32_t i = 0; i < on; i++) {
          Serial.print((char)('0' + ord[i])); /* 1=mw 2=mr 3=mb 4=mp 5=me */
          if (i + 1 < on) Serial.print(' ');
        }
        Serial.println();
      }
      uint32_t cb, cr, cbld, crt, ce, rfull, rpts, bfull, bpts;
      nav_map_counts(&cb, &cr, &cbld, &crt, &ce);
      nav_map_rejects(&rfull, &rpts, &bfull, &bpts);
      Serial.printf("[地图] 收到分类：mw=%u mr=%u mb=%u mp=%u me=%u\n",
                    (unsigned)cb, (unsigned)cr, (unsigned)cbld, (unsigned)crt, (unsigned)ce);
      Serial.printf("[地图] 被拒：道路满=%u 道路点数=%u 建筑满=%u 建筑点数=%u\n",
                    (unsigned)rfull, (unsigned)rpts, (unsigned)bfull, (unsigned)bpts);
    }
    Serial.printf("[地图] 地图层重算=%u 次\n", (unsigned)ui_nav_map_redraw_count());
    if (has_off) {
      Serial.printf("[地图] 自车偏移=(%d,%d) 米\n", ox, oy);
    }
    else {
      Serial.println("[地图] 自车偏移：未设置（导航消息未带 ox/oy）");
    }
  }
  else if (strncmp(cmd, "sd", 2) == 0) {
    /* TF 卡检测：`sd` 看状态，`sd list` 连目录一起列 */
    const char *arg = cmd + 2;
    while (*arg == ' ') arg++;
    sd_report(strcmp(arg, "list") == 0);
  }
  else if (strcmp(cmd, "nvs") == 0) {
    /*
     * 【诊断】NVS 分区用量与 GC 统计。
     * 动机：停滞时"中断正常但 lv_timer_handler 耗时 1 秒、且 flush 次数不变"，
     * 指向 flash 操作（cache 被禁用会让所有走 cache 的访问停顿），
     * 而 NVS 垃圾回收正是"约 1 秒"量级。若 NVS 接近写满，GC 会频繁触发。
     */
    nvs_stats_t st;
    esp_err_t e1 = nvs_get_stats(NULL, &st);
    if (e1 != ESP_OK) {
      Serial.printf("[NVS] 取统计失败: %d\n", (int)e1);
    }
    else {
      Serial.printf("[NVS] 命名空间 %u｜已用条目 %u｜空闲条目 %u｜可用条目 %u｜总条目 %u\n",
                    (unsigned)st.namespace_count, (unsigned)st.used_entries,
                    (unsigned)st.free_entries, (unsigned)st.available_entries,
                    (unsigned)st.total_entries);
    }
  }
  else if (strncmp(cmd, "bufmode", 7) == 0) {
    /*
     * 【诊断】切换 LVGL 绘制缓冲：psram（全帧）/ internal（1/10 屏）。
     *
     * 用途：验证"~1 秒停滞"是否来自 **PSRAM 带宽争抢**。
     * 全帧缓冲下，flush 的逐像素搬移要从 PSRAM 读 → 与 BLE 栈争带宽；
     * 内部缓冲则把读写都放在内部 RAM（代价是脏区大时 LVGL 要切更多块）。
     */
    const char *arg = cmd + 7;
    while (*arg == ' ') arg++;
    lv_disp_t *d = lv_disp_get_default();
    if (d == NULL || d->driver == NULL) {
      Serial.println("[显示] 显示驱动未就绪");
    }
    else if (strncmp(arg, "internal", 8) == 0) {
      d->driver->draw_buf = &draw_buf;
      s_buf_psram_active = false;
      lv_obj_invalidate(lv_scr_act());
      Serial.println("[显示] 缓冲 -> 内部 RAM（1/10 屏）");
    }
    else if (strncmp(arg, "psram", 5) == 0) {
      if (s_buf_psram == NULL) {
        Serial.println("[显示] PSRAM 缓冲不可用");
      }
      else {
        d->driver->draw_buf = &s_draw_buf_psram;
        s_buf_psram_active = true;
        lv_obj_invalidate(lv_scr_act());
        Serial.println("[显示] 缓冲 -> PSRAM 全帧");
      }
    }
    else {
      Serial.printf("[显示] 缓冲 = %s\n", s_buf_psram_active ? "PSRAM全帧" : "内部1/10");
    }
  }
  else if (strncmp(cmd, "tp", 2) == 0) {
    /*
     * 【诊断】停/起 100Hz 触摸采样。
     * 用途：定位"~1 秒停滞"是否来自 I2C 锁竞争
     * （触摸采样在 esp_timer 回调里做 I2C，主循环读 RTC 用同一条总线）。
     */
    const char *arg = cmd + 2;
    while (*arg == ' ') arg++;
    if (s_tp_timer == NULL) {
      Serial.println("[触摸] 采样定时器未创建");
    }
    else if (strncmp(arg, "off", 3) == 0) {
      esp_timer_stop(s_tp_timer);
      Serial.println("[触摸] 采样已停（仅用于诊断，触摸将无响应）");
    }
    else if (strncmp(arg, "on", 2) == 0) {
      esp_timer_start_periodic(s_tp_timer, 10000);
      Serial.println("[触摸] 采样已恢复 100Hz");
    }
    else {
      Serial.printf("[触摸] 采样=%s\n",
                    esp_timer_is_active(s_tp_timer) ? "运行中(100Hz)" : "已停止");
    }
  }
  else if (strncmp(cmd, "verbose", 7) == 0) {
    /* 调试输出闸门的手动开关（默认随 USB 主机连接状态自动判定） */
    const char *arg = cmd + 7;
    while (*arg == ' ') arg++;
    if (strncmp(arg, "on", 2) == 0) s_dbg_force = true;
    else if (strncmp(arg, "off", 3) == 0) s_dbg_force = false;
    Serial.printf("[日志] 周期打印 = %s（主机连接=%d）\n",
                  dbg_on() ? "开" : "关", (int)(bool)Serial);
  }
  else if (strcmp(cmd, "scan") == 0) {
    print_scan();
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
        /*
         * 走与 BLE 完全相同的路径（含"上升沿自动跳转"）——
         * 这样串口命令既能驱动界面，也能**验证**真机上的跳转规则。
         * 若要强制跳到导航页做界面调试，用 `page 4`。
         */
        nav_apply_state((nav_state_t)st);
        ui_nav_set_maneuver(mv);
        ui_nav_set_distance(dist);
        if (n >= 4) ui_nav_set_speed(spd, true);
        Serial.printf("[导航] 已设置 状态=%d 动作=%d 距离=%d%s（当前表盘=%d）\n",
                      st, mv, dist, (n >= 4) ? " 含车速" : "",
                      (int)ui_pages_current());
      }
    }
  }
  else if (strncmp(cmd, "demo", 4) == 0) {
    /*
     * 【调试命令】演示速度（等同 App 的"模拟速度测试"滑块）。
     *   demo           查询
     *   demo <kmh>     进入演示并设定车速
     *   demo off       退出演示
     */
    const char *arg = cmd + 4;
    while (*arg == ' ') arg++;
    if (*arg == '\0') {
      /* 顺便报出**斜坡当前值**：这是"命令是否真的驱动了指针"的直接证据
         （指针由斜坡驱动，演示值只是斜坡的目标）。 */
      Serial.printf("[演示] %s（演示值=%.0f，斜坡当前=%.1f，目标=%.1f）\n",
                    ui_speed_demo_active() ? "进行中" : "未开启",
                    (double)ui_speed_demo_kmh(),
                    (double)telemetry_ramp_value(&s_ramp),
                    (double)s_ramp.target_kmh);
    } else if (strncmp(arg, "off", 3) == 0) {
      ui_speed_set_demo(-1.0f);
      Serial.println("[演示] 已退出");
    } else {
      float v = atof(arg);
      ui_speed_set_demo(v);
      Serial.printf("[演示] 车速 → %.0f km/h（状态行应显示 DEMO）\n", (double)v);
    }
  }
  else if (strncmp(cmd, "lvrot", 5) == 0) {
    /* 临时切换正式旋转策略，不写 NVS，便于开发期验证。 */
    const char *arg = cmd + 5;
    if (*arg == '\0') {
      Serial.printf("[旋转] 当前=%d°（0/180°=LVGL，90/270°=MADCTL）\n", s_display_rot);
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
      /* 串口也是"用户手动调" → 同样置手动优先，保持行为一致 */
      apply_brightness_user(control_clamp_brightness(v));
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
  else if (strcmp(cmd, "ble") == 0) {
    print_ble();
  }
  else if (strcmp(cmd, "mem") == 0) {
    print_mem();
  }
  else if (strcmp(cmd, "dump") == 0) {
    print_dump();
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

  /*
   * ⚠️ 实测记录：**不要提高 loop 任务的优先级**。
   *
   * 曾把 loop 从默认优先级 1 提到 3，想解决"~1 秒停滞"
   * （停滞窗口里渲染最大 1003~1035ms，而同时段硬件中断间隔最大仅 0us，
   *   说明中断正常、不是 flash/cache 关中断）。
   *
   * 结果**显著恶化**：停滞从"约每 100 秒 1 次"变成"每 14 秒 1 次"
   * （79 个窗口里 28 次）。单纯的"被饿死"提优先级只会改善、绝不会恶化，
   * 故这是**优先级反转**的特征 —— 主循环在等一个由更低优先级任务持有的锁，
   * 提优先级后持有者反而拿不到 CPU 去释放，于是等到约 1 秒的超时。
   *
   * 结论：保持默认优先级，转而去查"主循环持有哪些锁/资源"。
   */

  /* 中断间隔诊断：1kHz，仅打时间戳（见 s_isr_gap_us_max 的说明） */
  {
    /* Arduino-ESP32 3.x 的定时器 API：timerBegin(频率Hz)，tick 即 1/频率 秒 */
    hw_timer_t *t = timerBegin(1000); /* 1kHz → 每 tick = 1ms */
    if (t != NULL) {
      timerAttachInterrupt(t, &isr_gap_cb);
      timerAlarm(t, 1, true, 0); /* 每 1 个 tick 触发一次 = 1kHz */
    }
  }

  /* 烘焙图充气：必须在 ui_pages_init() 之前 —— 表盘构建时会 lv_img_set_src
   * 引用这些 dsc，提前改好可避免"先引用旧格式、后改写"的隐患。 */
  inflate_baked_images();

  /* 业务 UI：全部页面（模拟时钟 / 数字时钟 / 速度表盘） */
  ui_pages_init();

  /* 恢复上次保存的屏幕方向（NVS）；无记录则用编译期默认 DISPLAY_ROT */
  restore_display_rotation();

  /* TF 卡：启动时尝试挂载一次并报告（失败不影响启动） */
  sd_report(false);

  /* 地图窗口：清空接收缓冲（等待手机下发） */
  nav_map_init();

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

  /*
   * 时间刚被校准 → 让亮度曲线立刻重算一次。
   * 典型场景：开机时 RTC 无效（或电池掉电后时间归零），App 连上校时
   * 到真实时间。若不等这一步，屏幕会在最长 20 秒里停在按 00:00 算出的
   * 夜间亮度上。
   */
  s_bright_recalc_now = true;
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
/**
 * 应用一个导航状态，并在**上升沿**自动跳到导航表盘。
 *
 * 抽成函数的原因：BLE 路径（nav_poll）与串口调试命令（nav ...）都要用同一套
 * 跳转规则。原先串口命令是**无条件** `ui_pages_set_face(UI_PAGE_NAV)` ——
 * 那样就绕过了"只在开始时跳一次"的判定，也没法用它验证真机行为。
 */
static void nav_apply_state(nav_state_t now)
{
  const nav_state_t prev = (nav_state_t)ui_nav_get_state();
  ui_nav_set_state((int)now);

  if (nav_should_auto_jump(prev, now)) {
    if (ui_pages_show_nav_if_enabled()) {
      Serial.printf("[导航] 开始导航 → 自动跳到导航表盘（%s → %s）\n",
                    nav_state_name(prev), nav_state_name(now));
    } else {
      Serial.println("[导航] 开始导航，但导航表盘已被禁用 → 不跳转");
    }
  }
}

static void nav_poll(void)
{
  static uint32_t s_last_nav_ms = 0;
  static bool s_nav_active = false; /* 是否处于"收到过导航数据"的状态 */

  nav_msg_t nm;
  if (ble_telemetry_take_nav(&nm)) {
    s_last_nav_ms = millis();
    s_nav_active = true;

    /* 只应用**实际提供**的字段：缺字段保留旧值（不因缺字段清空显示）*/
    if (nm.has_state) nav_apply_state((nav_state_t)nm.state);
    if (nm.has_maneuver) ui_nav_set_maneuver((int)nm.maneuver);
    if (nm.has_dist)     ui_nav_set_distance(nm.distance_m);
    /* 自车在窗口坐标系中的偏移：地图层据此平移地图，使车永远在中心箭头上 */
    if (nm.has_offset)   nav_map_set_vehicle_offset(nm.offset_x_m, nm.offset_y_m);

    Serial.printf("[导航] state=%s maneuver=%s dist=%d offset=%d,%d\n",
                  nav_state_name(nm.has_state ? nm.state : NAV_STATE_NO_DATA),
                  nav_maneuver_name(nm.has_maneuver ? nm.maneuver : NAV_MANEUVER_UNKNOWN),
                  nm.has_dist ? nm.distance_m : -1,
                  nm.has_offset ? nm.offset_x_m : -9999,
                  nm.has_offset ? nm.offset_y_m : -9999);
    return;
  }

  /* 无新数据：检查是否过期 */
  if (!s_nav_active) return;

  uint32_t age = millis() - s_last_nav_ms;

  /* BLE 断开 → 立即进入"无数据"（不等超时）*/
  if (!ble_telemetry_connected()) {
    /* BLE 断开：演示数据也失去意义，一并退出（App 可能已经走了）*/
    if (ui_speed_demo_active()) {
      ui_speed_set_demo(-1.0f);
      Serial.println("[演示] BLE 断开 → 自动退出演示");
    }
    if (ui_nav_get_state() != NAV_STATE_NO_DATA) {
      nav_apply_state(NAV_STATE_NO_DATA);
      s_nav_active = false;
      Serial.println("[导航] BLE 断开 → 无数据");
    }
    return;
  }

  /* 超时 → 过期（**不显示旧指引**）*/
  if (age > NAV_STALE_MS && ui_nav_get_state() == NAV_STATE_ACTIVE) {
    nav_apply_state(NAV_STATE_STALE);
    Serial.printf("[导航] 数据过期（%u ms 无更新）→ 停止显示旧指引\n", (unsigned)age);
  }
}

/**
 * 消费 App 下发的亮度曲线包（主循环调用，BLE 回调只暂存）。
 *
 * 状态变量（s_bright_cfg / s_bright_user_override 等）声明在文件上方
 * `apply_brightness_user()` 之前 —— C++ 要求先声明后使用。
 */
static void bright_poll(void)
{
  /*
   * ── 亮度平滑过渡 ──────────────────────────────
   *
   * 每帧朝 s_bright_target 推进一步。放在最前面，因为它是**每帧**都要做的
   * （而曲线重算受 20 秒节流限制）。
   */
  if (s_bright_target >= 0) {
    const int cur = (int)s_brightness;
    if (cur == s_bright_target) {
      s_bright_target = -1; /* 到位 */
    }
    else {
      int step = s_bright_target - cur;
      if (step > BRIGHT_STEP_PER_FRAME) step = BRIGHT_STEP_PER_FRAME;
      else if (step < -BRIGHT_STEP_PER_FRAME) step = -BRIGHT_STEP_PER_FRAME;
      /*
       * ⚠️ 直接写面板，**不走** apply_brightness() ——
       * 后者会调 sync_display_report_state() → 置 s_faces_dirty → 触发 BLE 上报。
       * 过渡期间每帧都上报会把链路刷爆（这正是之前"卡死"那类问题的形态）。
       * 只更新 s_brightness 与面板，上报留到**到位那一帧**做一次。
       */
      s_brightness = (uint8_t)(cur + step);
      gfx->setBrightness(s_brightness);
      if (s_brightness == s_bright_target) {
        s_bright_target = -1;
        sync_display_report_state(); /* 到位：同步一次给 App */
      }
      return; /* 过渡中：本帧不再处理曲线 */
    }
  }

  bright_packet_t pkt;
  if (ble_telemetry_take_bright(&pkt)) {
    if (!s_bright_cfg_valid) {
      s_bright_cfg = auto_bright_default_config();
      s_bright_cfg_valid = true;
    }

    /*
     * ⚠️ 先记下旧配置，用来判断"这次收到的曲线**内容**是否真的变了"。
     *
     * 为什么必须判断：App 在**每次 BLE 连接**都会重发曲线（这是刻意的，
     * 因为设备不持久化）。若收到包就无条件解除"手动优先"，那么蓝牙任何
     * 一次抖动重连都会把用户手动调好的亮度**打回曲线值** ——
     * 实测现象就是"亮度突然像被重置了一样，一下子变得很暗"
     * （用户手动调到 220，重连后曲线在夜间时段 → 骤降到 40）。
     *
     * 所以规则改为：
     *   - 曲线**内容变了** → 用户在 App 里改过设置 → 解除手动优先（正确意图）
     *   - 曲线**内容没变** → 只是重连重发 → **保留**手动优先，不动亮度
     */
    const auto_bright_config_t before = s_bright_cfg;
    bright_apply_packet(&s_bright_cfg, &pkt);

    const bool changed =
        (before.enabled != s_bright_cfg.enabled) ||
        (before.day_start_min != s_bright_cfg.day_start_min) ||
        (before.day_end_min != s_bright_cfg.day_end_min) ||
        (before.day_level != s_bright_cfg.day_level) ||
        (before.night_level != s_bright_cfg.night_level) ||
        (before.ramp_min != s_bright_cfg.ramp_min);

    /*
     * 曲线内容变了 = 用户在 App 里**明确表达**了意图 → 解除手动覆盖。
     * 这条是"手动优先"的出口：否则用户拖过滑块后，就再也没有办法
     * 让自动重新生效（只能重启设备）。
     *
     * 若内容没变（重连重发）则**不解除** —— 见上面的说明。
     */
    /*
     * `reset` = App 明确请求"恢复自动调节"（用户在 App 里点「按时段自动」）。
     *
     * 为什么不能只靠 changed 判断：切回自动时曲线内容**往往没变**
     * （还是原来那些值），会被当成"重连重发"而保持手动亮度 ——
     * 用户点了"自动"却没反应。
     */
    const bool want_reset = (pkt.has_reset && pkt.reset != 0);

    if (changed || want_reset) {
      s_bright_first_sync = true; /* ⚠️ 必须在这里也置位：否则重发时会被误判成"首次" */
      s_bright_user_override = false;
      /*
       * 立刻按新曲线应用一次。
       *
       * ⚠️ 除了清 last_applied，还必须置 s_bright_recalc_now ——
       *    否则会被 20 秒的 RTC 读取节流挡住，用户改完设置要等最多
       *    20 秒才看到变化（实测确认：改完亮度立刻查询仍是旧值，
       *    等 22 秒后才正确）。
       */
      s_bright_last_applied = -1;
      s_bright_recalc_now = true;

      Serial.printf("[亮度] 曲线更新：%s 白天 %02d:%02d-%02d:%02d 亮度 %d/%d 过渡 %d 分\n",
                    s_bright_cfg.enabled ? "启用" : "关闭",
                    s_bright_cfg.day_start_min / 60, s_bright_cfg.day_start_min % 60,
                    s_bright_cfg.day_end_min / 60, s_bright_cfg.day_end_min % 60,
                    s_bright_cfg.day_level, s_bright_cfg.night_level, s_bright_cfg.ramp_min);
    }
    else if (!s_bright_first_sync) {
      s_bright_first_sync = true;
      /* 首次收到（本次开机还没收到过）→ 应用一次，让曲线立刻生效 */
      s_bright_user_override = false;
      s_bright_last_applied = -1;
      s_bright_recalc_now = true; /* 同上：跳过 20 秒节流 */

      Serial.printf("[亮度] 曲线首次同步：%s 白天 %02d:%02d-%02d:%02d 亮度 %d/%d 过渡 %d 分\n",
                    s_bright_cfg.enabled ? "启用" : "关闭",
                    s_bright_cfg.day_start_min / 60, s_bright_cfg.day_start_min % 60,
                    s_bright_cfg.day_end_min / 60, s_bright_cfg.day_end_min % 60,
                    s_bright_cfg.day_level, s_bright_cfg.night_level, s_bright_cfg.ramp_min);
    }
    else {
      /* 重连重发且内容相同：什么都不做，保住用户手动调的亮度 */
      Serial.println("[亮度] 收到相同曲线（重连重发）→ 保持当前亮度，不覆盖");
    }
    return;
  }

  /*
   * 每帧都算一次其实很便宜（几次整数比较），但**写面板命令**不便宜，
   * 且会打断显示。所以只在"算出来的值与上次不同"时才写。
   *
   * ⚠️ 必须用 RTC 时间而不是 millis()：曲线是按"钟点"而非"开机时长"。
   */
  if (!s_bright_cfg_valid || !s_bright_cfg.enabled) return;
  if (s_bright_user_override) return;

  /*
   * ⚠️ 性能关键：**不要每帧读 RTC**。
   *
   * `ui_timesrc_now()` 在真机上会通过 I2C 读 PCF85063 的 7 字节寄存器。
   * 主循环约 100-200 次/秒，每帧读一次 I2C 会把循环频率**腰斩**
   * （实测：198 次/秒 → 78~97 次/秒），并间接拖慢渲染
   * （渲染最大耗时从 4ms 涨到 57ms）。
   *
   * 而亮度曲线只需要"分钟"级精度 —— 所以按 millis() 节流，
   * 每 20 秒才真正读一次 RTC。
   *
   * 用 `!=` 比较而不是"每次加 20 秒"，是为了让 millis() 回绕时
   * 也能自然恢复（回绕后差值仍可能满足条件，不会永久卡死）。
   */
  static uint32_t s_last_rtc_ms = 0;
  static bool s_rtc_read_once = false;
  const uint32_t now_ms = millis();
  /*
   * s_bright_recalc_now 由 time_sync_maybe() 置位：校时后时间可能大跳，
   * 必须立刻重算，不能等满 20 秒（否则屏幕在这段时间里是错的亮度）。
   */
  if (!s_bright_recalc_now) {
    if (s_rtc_read_once && (now_ms - s_last_rtc_ms) < 20000u) return;
  }
  s_bright_recalc_now = false;
  s_last_rtc_ms = now_ms;
  s_rtc_read_once = true;

  /*
   * ★ RTC 无效时**不要**按曲线调亮度。
   *
   * `rtc_time_provider()` 在 rtc_ok=false 时把 tm 清零（"不伪造时间"），
   * 于是 now_min 恒为 0（00:00），曲线会一直判定为夜间 →
   * **白天也把屏幕调到夜间亮度**，在阳光下几乎看不清。
   *
   * 这比"不调亮度"更糟：用户会觉得设备坏了。所以 RTC 不可用时
   * 直接放弃自动调节，保持当前亮度不动（用户仍可手动调）。
   *
   * 何时会发生：RTC 芯片焊接不良 / 纽扣电池耗尽 / I2C 异常。
   * 本机 rtc_init() 失败时会置 rtc_ok=false（见 print_time 的同名判断）。
   */
  if (!rtc_ok) return;

  struct tm now;
  ui_timesrc_now(&now);
  /*
   * 再兜一层：即使 rtc_ok=true，个别情况下读回的日期也可能是 0
   * （例如电池刚掉、寄存器复位）。年份 < 2020 视为"时间不可信"，
   * 同样不据此调亮度。
   */
  if (now.tm_year + 1900 < 2020) return;

  const int now_min = auto_bright_minutes_of_day(now.tm_hour, now.tm_min);

  const int want = auto_bright_level_at(now_min, &s_bright_cfg);
  if (want == s_bright_last_applied) return;

  apply_brightness_auto((uint8_t)want);
  s_bright_last_applied = want;
}

static void ble_poll(void)
{
  bright_poll(); /* 亮度曲线：先应用（与会话/时序无关，尽快生效）*/
  nav_poll();
  nav_map_tick(millis()); /* 地图窗口：累积超时与过期检查 */
  ui_nav_map_update();  /* 地图层：按航向/位移节流重算（无窗口时自动隐藏）*/
  /*
   * 有地图时切紧凑布局：把大图标与 150px 距离收起来，主体让给地图。
   * 无地图（未连接/无网/过期）自动回到大字卡片，保证指引依然清晰可读。
   */
  ui_nav_set_map_mode(ui_nav_map_has_data()); /* 导航消息（与遥测同一特征，先生效以便状态切换）*/

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

  /*
   * ⚠️ **安全兜底：收到真实遥测就退出演示**。
   *
   * 不能依赖 App 一定发出退出命令（命令可能丢、App 可能被杀）。
   * 演示速度显示得像真实车速是危险的 —— 所以只要真实数据来了，
   * 演示立刻让位。这也是"点击开始驾驶自动退出演示"在**固件侧**的保证：
   * 开始驾驶后遥测就开始发，第一条到达即退出。
   */
  if (ui_speed_demo_active()) {
    ui_speed_set_demo(-1.0f);
    Serial.println("[演示] 收到真实遥测 → 自动退出演示");
  }

  s_tel = m;
  s_had_data = true;

  /* 时间同步：带 ts_utc_ms 的包触发（首次/漂移 >2s 才真正写 RTC） */
  if (m.has_ts) time_sync_maybe(m.ts_utc_ms);

  bool speed_ok = m.speed_valid && m.speed_mps >= 0.0f;
  s_tel_valid = m.location_valid || speed_ok;

  /* ⚠️ 2Hz 高频打印：必须过闸门，否则装车后会周期性阻塞主循环 */
  uint32_t t_pkt0 = micros();
  if (dbg_on()) {
    Serial.printf("[BLE] v=%d 速度=%.2f m/s(%.1f km/h) valid=%d 航向=%.1f° 定位=%d\n",
                  m.v, (double)m.speed_mps, (double)telemetry_speed_kmh(&m), (int)m.speed_valid,
                  (double)m.course_deg, (int)m.location_valid);
  }
  {
    uint32_t dtp = micros() - t_pkt0;
    if (dtp > 30000) {
      Serial.printf("[诊断] 遥测打印阻塞 %lu ms\n", (unsigned long)(dtp / 1000));
    }
  }
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

  /*
   * ⚠️ **演示模式下遥测路径不得碰速度斜坡**。
   *
   * 这是"滑动速度条完全没反应"的真正原因：演示时没有真实遥测，
   * link_raw != LINK_OK，于是下面 else 分支每帧都 `telemetry_ramp_reset()`，
   * 而主循环的斜坡块刚设好的演示目标**下一帧就被清零** —— 目标恒为 0、
   * 指针纹丝不动，而状态行的 DEMO 又显示正常，所以看起来"命令到了但没动画"。
   *
   * 演示期间速度目标**只由主循环的斜坡块**设置（见 PH_RAMP 段）。
   */
  if (ui_speed_demo_active()) {
    ui_speed_set_fix(true, 0); /* 让指针区域按"有效"渲染 */
  }
  else if (link_raw == LINK_OK) {
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
      /* 地图层也按同一航向做"航向朝上"旋转（无 course 时内部保持上一次）*/
      if (s_tel.has_course) ui_nav_map_set_heading(s_tel.course_deg);
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
  ph_begin();
  handle_serial();            /* 串口命令：校时 / 切页 */
  ph_end(PH_SERIAL);
  ph_begin();
  control_poll();             /* BLE 下行控制：亮度 / 息屏 / 表盘 */
  rotation_poll();            /* 合并后的方向切换（见 s_pending_rot 说明）*/

  /*
   * ── 硬件旋转的"显示不全"：**问题一直存在，旋转并不能修复** ──────────
   *
   * 曾误以为"开机即 270° 有黑边，切到别的角度再切回 270° 就正常"，
   * 并据此实现过"开机自动切走再切回"（两版：一版没渲染、一版用
   * lv_refr_now 强制两次真实渲染）—— **两版都无效**。
   *
   * 用户最终查明真实原因：**那个"旋转后就好了"是误判**。早前会话加过一个
   * **白色描边的遮挡块**，手动旋转屏幕会触发它显示出来，于是看起来"变好了"。
   * 该显示不全**与旋转无关，一直存在**。
   *
   * 现状：用户表示"没法修复就先继续这样，有点显示不全也无所谓" —— 暂不处理。
   * 详见 docs/rotation-hardware-vs-software.md 第十二节。
   */  ph_end(PH_CONTROL);

  /*
   * 表盘变化检测：**滑动、串口命令、BLE 命令**都会改变当前表盘，
   * 与其在每处分别持久化（容易漏），不如在循环里统一比对一次。
   * 这样"断电记忆"对所有切换途径都生效。
   */
  ph_begin();
  {
    int cur = ui_pages_current_face();
    if (cur != s_last_face) {
      s_last_face = cur;
      save_face_prefs(cur, ui_face_mask());
      Serial.printf("[表盘] 当前 → %d(%s)（已存 NVS，断电后沿用）\n", cur,
                    ui_pages_current_face_name());
    }
    /*
     * App 改动过启用/隐藏状态 → 重新上报，保持两端一致。
     *
     * ⚠️ **必须节流**，否则会引发"同步好几次才对"和卡死：
     *
     * `sync_display_report_state()` 会置 s_faces_dirty，而它被
     *   - 每次亮度变化（自动曲线 / 用户调）
     *   - 每 10 秒的行程统计
     * 调用。若不节流，整份表盘列表（约 460 字节）就会被反复推给 App，
     * App 每次都要重新 Read → 又触发一轮同步 → 雪崩。
     *
     * 实测症状（2026-10-19）：表盘列表在 1 秒内上报 3 次、之后每 13 秒一次，
     * 循环频率从 198 次/秒掉到 78 次/秒，最终卡死。
     *
     * 现在限制为**最快 2 秒一次**。表盘列表本身几乎不变，延迟 2 秒
     * 用户完全无感；而状态字段（亮度/方向/行程）由 App 周期 Read 获取，
     * 不依赖这条通知。
     */
    if (s_faces_dirty) {
      static uint32_t s_last_faces_report_ms = 0;
      const uint32_t now_f = millis();
      /* 首次（s_last_faces_report_ms==0）立即上报，之后按 2 秒节流 */
      if (s_last_faces_report_ms == 0 || (now_f - s_last_faces_report_ms) >= 2000u) {
        s_last_faces_report_ms = now_f;
        s_faces_dirty = false;
        publish_faces_report();
      }
    }
    /*
     * 行程统计周期性上报（P1-1）。
     *
     * ⚠️ **必须节流**：行程每帧都在变，若跟着主循环上报会以数百 Hz 刷 BLE，
     *    把遥测和地图消息挤掉（地图队列本来就对带宽敏感）。
     *    10 秒一次足够 —— 用户看的是"跑了多少公里"，不是秒级精度。
     *
     * 只在**有效行驶**（trip.time_ms 在增长）时才上报，停车时不必刷。
     */
    {
      static uint32_t last_trip_report_ms = 0;
      static uint32_t last_trip_time_ms = 0;
      uint32_t now_t = millis();
      if (s_trip.time_ms != last_trip_time_ms &&
          now_t - last_trip_report_ms >= 10000) {
        last_trip_report_ms = now_t;
        last_trip_time_ms = s_trip.time_ms;
        sync_display_report_state(); /* 会把 s_faces_dirty 置位，下一帧真正上报 */
      }
    }
  }
  ph_end(PH_FACE);

  /* 速度指针斜坡推进（#2）：按实际经过时间匀速逼近目标，与帧率无关 */
  ph_begin();
  {
    static uint32_t last_ramp_ms = 0;
    uint32_t now_r = millis();
    uint32_t dt_r = now_r - last_ramp_ms;
    if (dt_r >= 20) { /* 50Hz 推进 */
      last_ramp_ms = now_r;
      /*
       * 演示模式**仍然走同一条斜坡**，只是把目标换成演示值。
       *
       * 为什么不是"跳过斜坡直接写指针"：那样指针只会瞬间跳变，看不出动画，
       * 而演示的唯一目的就是看动画。让演示值与真实数据走**完全相同的路径**，
       * 演示才有意义（也顺带验证了斜坡逻辑本身）。
       *
       * 斜坡是写指针的**唯一**驱动者，所以两者不会互相打架。
       */
      if (ui_speed_demo_active()) {
        telemetry_ramp_set_target(&s_ramp, ui_speed_demo_kmh(), true);
      }
      float shown = telemetry_ramp_step(&s_ramp, dt_r);
      ui_speed_set_speed(shown);
    }
  }
  ph_end(PH_RAMP);

  /* 行程统计（功能④）：仅在链路 LIVE 且速度有效时累积，失效不计入 */
  ph_begin();
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
  ph_end(PH_TRIP);
  ph_begin();
  ble_poll();                 /* 取 BLE 遥测 + 校时（无新数据直接返回） */
  ble_telemetry_poll();       /* 断连后延迟重启广播（见 ble_telemetry.cpp 说明） */
  ble_update();               /* 五态状态机，每循环驱动速度页 */
  ph_end(PH_POLL);
  ph_begin();
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
  ph_end(PH_TOUCH);

  {
    ph_begin();
    uint32_t t_render = micros();
    s_flush_in_call = 0;
    lv_timer_handler();
    if (s_flush_in_call > s_flush_per_call_max) s_flush_per_call_max = s_flush_in_call;
    uint32_t dt = micros() - t_render;
    if (dt > s_render_us_max) s_render_us_max = dt;
    s_render_us_sum += dt;
  }
  ph_end(PH_RENDER);
  delay(5);

  /* 心跳日志：每 5 秒打印运行状态 + 重绘/循环速率（性能回归监测）*/
  static uint32_t last = 0;
  static uint32_t last_flush = 0, last_loop = 0;
  if (millis() - last > 5000) {
    uint32_t now = millis();
    uint32_t dt = now - last;
    last = now;
    uint32_t f = s_flush_cnt, l = s_loop_cnt;
    /*
     * 【诊断】测出这几条 printf 的真实耗时。
     * 目的是用数据判定"串口输出是否会阻塞主循环"这一假设，
     * 而不是靠推测 —— 若耗时只有几百微秒，则该假设不成立。
     */
    uint32_t t_print0 = micros();
    /* ⚠️ 这 3 条 printf 实测就是 ~1 秒阻塞的来源，必须过闸门 */
    if (dbg_on()) {
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
      Serial.printf("       脏区：单次最大 %lu px（占整屏 %u%%），窗口累计 %lu px（整屏 217156）\n",
                    (unsigned long)s_flush_area_max,
                    (unsigned)(s_flush_area_max * 100UL / 217156UL),
                    (unsigned long)s_flush_area_sum);
      /*
       * 中断间隔：这是判断"停滞根因"的关键指标。
       * 若该值也接近 1 秒 → 全局中断被关闭（flash/cache 路径）；
       * 若该值很小而渲染最大仍是 1 秒 → 中断正常，是任务被抢占。
       */
      Serial.printf("       中断间隔最大 %lu us（中断次数 %lu）\n",
                    (unsigned long)s_isr_gap_us_max, (unsigned long)s_isr_cnt);
      /* 各阶段最大耗时：停滞时看哪一段数值爆掉，即可锁定阻塞点 */
      Serial.printf("       单次渲染内 flush 次数 max=%lu\n",
                    (unsigned long)s_flush_per_call_max);
      Serial.printf("       阶段max(ms):");
      for (int i = 0; i < PH_N; i++) {
        Serial.printf(" %s=%lu", PH_NAME[i], (unsigned long)(s_ph_max_us[i] / 1000));
      }
      Serial.println();
    }
    {
      uint32_t dtp = micros() - t_print0;
      if (dtp > 30000) {
        Serial.printf("[诊断] 心跳打印阻塞 %lu ms（窗口 %lu ms）\n",
                      (unsigned long)(dtp / 1000), (unsigned long)dt);
      }
    }
    /* 统计清零与是否打印无关，照常执行 */
    s_render_us_max = 0;
    s_render_us_sum = 0;
    s_flush_us_sum = 0;
    s_flush_us_max = 0;
    s_flush_area_max = 0;
    s_flush_area_sum = 0;
    s_isr_gap_us_max = 0;
    s_flush_per_call_max = 0;
    for (int i = 0; i < PH_N; i++) s_ph_max_us[i] = 0;
    last_flush = f;
    last_loop = l;
  }
}
