/**
 * lv_conf.h —— macOS 模拟器配置
 *
 * 原则：渲染相关参数与真机（官方 examples/arduino/libraries/lv_conf.h）保持一致，
 *       资源类参数（内存）按 PC 放宽。真机编译时使用官方 lv_conf.h，不用本文件。
 */
#ifndef LV_CONF_H
#define LV_CONF_H

/* ==== 与真机一致的关键项 ==== */
#define LV_COLOR_DEPTH  16      /* 真机：RGB565，AMOLED 16 位色 */
#define LV_COLOR_16_SWAP 0      /* 真机：0 */

/* ==== 模拟器放宽项 ==== */
#define LV_MEM_CUSTOM   0
#define LV_MEM_SIZE     (512U * 1024U)  /* 真机 48KB；PC 放宽以免布局试验受限 */

/* ==== 字体：内置 Montserrat 系列（后续主数字换用 lv_font_conv 生成的大号字体） ==== */
#define LV_FONT_MONTSERRAT_12 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_MONTSERRAT_48 1
#define LV_FONT_DEFAULT &lv_font_montserrat_16

/* 其余未列出的选项均采用 LVGL 8.4 内置默认值 */

#endif /* LV_CONF_H */
