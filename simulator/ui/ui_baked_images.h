/**
 * ui_baked_images.h —— 烘焙图资源声明（表盘底图 + 驾驶员插画）
 *
 * ⚠️ 这里用的是 **非 const** 的 `extern lv_img_dsc_t`，**不能用 `LV_IMG_DECLARE`**：
 *    LV_IMG_DECLARE 展开为 `extern const lv_img_dsc_t`，而真机启动时要把
 *    索引图"充气"成 RGB565 直通图（就地改写 header.cf / data，见 ui_img_inflate.h），
 *    必须是可写对象。
 *
 * 新增烘焙图时，除了在生成脚本里加，**也要在此登记**，否则固件无法在启动时充气
 * （会退化为每帧查表解码，滑页慢约 45 ms）。
 */
#ifndef UI_BAKED_IMAGES_H
#define UI_BAKED_IMAGES_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* tools/make_dial_faces.py 生成 */
extern lv_img_dsc_t dial_analog;
extern lv_img_dsc_t dial_speed;

/* tools/make_images.py 生成 */
extern lv_img_dsc_t driver_normal;
extern lv_img_dsc_t driver_spirited;
extern lv_img_dsc_t driver_speeding;

#ifdef __cplusplus
}
#endif

#endif /* UI_BAKED_IMAGES_H */
