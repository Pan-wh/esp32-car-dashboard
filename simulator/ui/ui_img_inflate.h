/**
 * ui_img_inflate.h —— 把 8bpp 索引图"充气"成 RGB565 直通图
 *
 * ## 为什么需要它
 *
 * 为省 Flash（应用分区仅 3 MB），烘焙图以 **8bpp 索引色** 存储，尺寸减半。
 * 但 LVGL 的内置解码器对索引图是**逐行解码**的：每个像素都要查一次调色板。
 * 全屏 466×466 = 21.7 万次查表，滑页时两页同绘 → 每帧 43 万次。
 *
 * 真机实测代价（连续切页、7 个心跳窗口取峰值）：
 *   RGB565 直通图 : 123 ms
 *   8bpp 索引图   : **168 ms**（+45 ms，+37%）
 *
 * ## 解决方式
 *
 * 启动时**解码一次**到 PSRAM 得到 RGB565 缓冲，再把 `lv_img_dsc_t` 就地改成
 * `LV_IMG_CF_TRUE_COLOR` 指向该缓冲。之后绘制走直接内存拷贝，速度恢复。
 *
 *   收益：Flash 仍是索引图的占用（省 683 KB），滑页恢复 ~123 ms。
 *   代价：每张图在 PSRAM 占用 w*h*2 字节（本项目的 5 张图共约 1.4 MB；
 *         本板 8 MB PSRAM，实测空闲 7.9 MB，余量充足）。
 *
 * ## 为什么 decode 是纯逻辑
 *
 * 纯解码逻辑放在 **img_indexed.h**（不依赖 LVGL），因此可在 PC 上单测
 * （含边界/越界/非法参数）。本文件只负责 `lv_img_dsc_t` 的就地改造。
 * PSRAM 分配由平台层负责（模拟器 malloc、真机 ps_malloc），
 * 本模块不碰内存分配策略。
 */
#ifndef UI_IMG_INFLATE_H
#define UI_IMG_INFLATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 判断某张图是否需要充气（只有 8bpp 索引图需要）。
 */
bool ui_img_needs_inflate(const lv_img_dsc_t *dsc);

/*
 * 就地充气：把 dsc 从 INDEXED_8BIT 改为 TRUE_COLOR，数据指向 out_buf。
 *
 * @param out_buf      由调用方分配的缓冲（真机应为 PSRAM）
 * @param out_bytes    out_buf 的字节数
 * @return true 表示已充气；false 表示无需充气/参数非法/缓冲不足（此时 dsc 不变）
 */
bool ui_img_inflate(lv_img_dsc_t *dsc, void *out_buf, size_t out_bytes);

/** 充气一张图所需的字节数（w*h*2）；参数非法返回 0。 */
size_t ui_img_inflate_size(const lv_img_dsc_t *dsc);

/*
 * 批量充气。分配策略由平台提供（真机用 PSRAM，模拟器用 malloc），
 * 这样真机与模拟器走**同一条渲染路径**，像素回归才有意义。
 *
 * 单张失败不影响其它图：分配不到就保持索引图（功能不受影响，只是绘制变慢）。
 *
 * @param imgs  图像描述符数组
 * @param n     数量
 * @param alloc 分配函数；返回 NULL 表示分配失败
 * @return 成功充气的张数
 *
 * 注：早期还有一个 `names` 参数自称"仅用于日志"，但**两个调用处都传 NULL、
 *     函数体也从未使用**，属死参数，已删除。
 */
int ui_img_inflate_all(lv_img_dsc_t *const *imgs, int n, void *(*alloc)(size_t));

/*
 * 把已充气的 RGB565 图片左上角底色原地改为 LVGL 色键并启用透明。
 * 适合驾驶员这类“纯色背景 + 前景”的图片：不增加 Flash/PSRAM，也不改前景像素。
 * 未充气的索引图不会修改并返回 false。
 */
bool ui_img_make_bg_transparent(lv_img_dsc_t *dsc);

#ifdef __cplusplus
}
#endif

#endif /* UI_IMG_INFLATE_H */
