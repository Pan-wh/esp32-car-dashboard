/**
 * ui_photo.h —— 照片表盘（静态图片 + 动图 GIF）
 *
 * **本模块只负责"怎么显示"**，不碰 SD 卡、不解析 `.pgi`、不管轮播逻辑。
 * 分工：
 *   - `pgi.c`           —— 文件格式的解析与解码（纯逻辑，PC 可测）
 *   - `photo_slide.c`   —— 轮播状态机（纯逻辑，PC 可测）
 *   - **本模块**        —— 把一帧 RGB565 画到屏幕上 + 占位状态 + 像素位移
 *   - 固件 `dashboard.ino` —— SD 读取、分片预读、NVS、BLE
 *
 * 这样"能不能画对"可以在模拟器里截图验证，不需要真机与 SD 卡。
 *
 * ## 像素位移（防 AMOLED 烧屏）
 *
 * 每隔一段时间把画面整体平移 ±`UI_PHOTO_SHIFT_MAX` 像素。圆屏半径远大于位移量，
 * 所以四边不会被看出来；位移时**不做插值**，直接改 `lv_obj` 的位置。
 * 必须配合"图片比屏幕大 2×shift"或者接受边缘 3px 露出底色 —— 这里选后者：
 * 底色用纯黑（AMOLED 黑像素不发光，正好不引人注意）。
 */
#ifndef UI_PHOTO_H
#define UI_PHOTO_H

#include "lvgl.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 像素位移的最大幅度（像素）*/
#define UI_PHOTO_SHIFT_MAX 3

/** 占位状态：没有可显示的内容时，告诉用户"为什么" */
typedef enum {
    UI_PHOTO_PLACEHOLDER_NO_SD = 0,   /* 未检测到 SD 卡 */
    UI_PHOTO_PLACEHOLDER_EMPTY,       /* 目录为空 */
    UI_PHOTO_PLACEHOLDER_ERROR,       /* 文件损坏/格式不支持 */
    UI_PHOTO_PLACEHOLDER_LOADING,     /* 正在读第一帧 */
} ui_photo_placeholder_t;

/** 构建表盘内容（注册表回调签名一致：注册时转型为 void(*)(void*)）*/
lv_obj_t *ui_photo_create(lv_obj_t *parent);

/**
 * 显示一帧。
 *
 * @param src       RGB565 像素（**调用方持有**；本模块只读它，不会释放）
 * @param w,h       像素尺寸
 *
 * ⚠️ `src` 必须在下次 `ui_photo_show_frame()` 之前保持有效。固件用双缓冲，
 * 传给这里的指针是"当前显示的那块"，另一块在后台读下一帧。
 */
void ui_photo_show_frame(const uint16_t *src, int w, int h);

/** 显示占位提示（会自动隐藏图片）*/
void ui_photo_set_placeholder(ui_photo_placeholder_t kind);

/**
 * 像素位移开关 + 相位。
 *
 * @param enable 是否位移（关闭时立即归中）
 * @param phase  0..3 四档位移（由固件按时间推进，本模块不自己计时）
 *
 * 高频调用安全：只在相位变化时才动坐标。
 */
void ui_photo_set_shift(bool enable, int phase);

/** 当前是否正在显示实图（false = 占位）*/
bool ui_photo_has_frame(void);

#ifdef __cplusplus
}
#endif

#endif /* UI_PHOTO_H */
