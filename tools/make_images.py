#!/usr/bin/env python3
"""
图片资产工具：PNG → LVGL C 数组（CF_TRUE_COLOR / RGB565）+ 生成驾驶员临时底图

用法：
  python3 tools/make_images.py            # 生成临时底图（若不存在）并转换为 C
  python3 tools/make_images.py --force    # 强制重画临时底图（覆盖 design/assets/images/driver_*.png）

替换为真实素材：把你画的 PNG（建议正方形、黑底或纯色底，尺寸 180×180 或 2 的幂）
放到 design/assets/images/ 并保持文件名 driver_normal.png / driver_spirited.png /
driver_speeding.png，然后重跑本脚本即可。

依赖：Pillow（DSH 运行环境已内置）
输出：simulator/ui/ui_driver_images.c（三个 lv_img_dsc_t：driver_normal / driver_spirited / driver_speeding）
"""
import argparse
import os
import struct

try:
    from PIL import Image, ImageDraw
except ImportError:  # pragma: no cover
    raise SystemExit("需要 Pillow：请用 DSH 运行环境的 python 执行本脚本")

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
IMG_DIR = os.path.join(ROOT, "design", "assets", "images")
OUT_C = os.path.join(ROOT, "simulator", "ui", "ui_driver_images.c")

WHITE = (245, 245, 247)
ACCENTS = {
    "normal": (10, 132, 255),     # 蓝：正常驾驶
    "spirited": (48, 209, 88),    # 绿：激烈驾驶
    "speeding": (255, 149, 0),    # 橙：超速
}

SS = 4  # 超采样倍数（先放大画，再缩小，得到平滑线条）


def draw_driver(state: str, size: int = 180) -> Image.Image:
    """画一个极简扁平风格的驾驶员（手绘线条 + 状态表情），风格与仪表一致。"""
    S = size * SS
    img = Image.new("RGB", (S, S), (0, 0, 0))
    d = ImageDraw.Draw(img)
    accent = ACCENTS[state]

    def sc(v):
        return int(round(v * SS))

    lw = sc(5)          # 主线宽
    lw_thin = sc(3)     # 细线宽

    cx = size * 0.5

    # ---- 方向盘（在驾驶员身前，遮住躯干下部）----
    wheel_box = [sc(cx - 62), sc(112), sc(cx + 62), sc(156)]
    d.ellipse(wheel_box, outline=WHITE, width=lw)
    # 轮毂
    hub_r = 9
    d.ellipse([sc(cx - hub_r), sc(134 - hub_r), sc(cx + hub_r), sc(134 + hub_r)],
              outline=WHITE, width=lw_thin)
    # 两根辐条（轮毂 → 轮缘）
    d.line([sc(cx), sc(134), sc(cx - 44), sc(122)], fill=WHITE, width=lw_thin)
    d.line([sc(cx), sc(134), sc(cx + 44), sc(122)], fill=WHITE, width=lw_thin)

    # ---- 躯干/肩（在方向盘之后）----
    d.arc([sc(cx - 46), sc(78), sc(cx + 46), sc(170)], start=180, end=360,
          fill=WHITE, width=lw)
    # 脖子
    d.line([sc(cx - 7), sc(78), sc(cx - 7), sc(88)], fill=WHITE, width=lw_thin)
    d.line([sc(cx + 7), sc(78), sc(cx + 7), sc(88)], fill=WHITE, width=lw_thin)

    # ---- 头 ----
    head_cy, head_r = 56, 27
    if state == "speeding":
        head_cy = 53  # 略微前倾
    d.ellipse([sc(cx - head_r), sc(head_cy - head_r),
               sc(cx + head_r), sc(head_cy + head_r)], outline=WHITE, width=lw)

    # ---- 表情 ----
    eye_dx, eye_dy = 11, -4
    if state == "normal":
        for sx in (-1, 1):
            d.ellipse([sc(cx + sx * eye_dx - 3.2), sc(head_cy + eye_dy - 3.2),
                       sc(cx + sx * eye_dx + 3.2), sc(head_cy + eye_dy + 3.2)], fill=WHITE)
        d.arc([sc(cx - 9), sc(head_cy + 4), sc(cx + 9), sc(head_cy + 16)],
              start=15, end=165, fill=WHITE, width=lw_thin)          # 微笑
    elif state == "spirited":
        for sx in (-1, 1):
            d.ellipse([sc(cx + sx * eye_dx - 3.2), sc(head_cy + eye_dy - 3.2),
                       sc(cx + sx * eye_dx + 3.2), sc(head_cy + eye_dy + 3.2)], fill=WHITE)
        # 皱起的眉毛
        d.line([sc(cx - 18), sc(head_cy - 15), sc(cx - 6), sc(head_cy - 10)],
               fill=WHITE, width=lw_thin)
        d.line([sc(cx + 18), sc(head_cy - 15), sc(cx + 6), sc(head_cy - 10)],
               fill=WHITE, width=lw_thin)
        d.line([sc(cx - 8), sc(head_cy + 12), sc(cx + 8), sc(head_cy + 12)],
               fill=WHITE, width=lw_thin)                            # 抿嘴
    else:  # speeding
        for sx in (-1, 1):  # 瞪大的眼
            d.ellipse([sc(cx + sx * eye_dx - 6), sc(head_cy + eye_dy - 6),
                       sc(cx + sx * eye_dx + 6), sc(head_cy + eye_dy + 6)],
                      outline=WHITE, width=lw_thin)
        d.ellipse([sc(cx - 8), sc(head_cy + 6), sc(cx + 8), sc(head_cy + 20)],
                  fill=accent)                                       # 张嘴喊
        # 两侧速度线
        for i, dy in enumerate((-14, 0, 14)):
            x1 = cx - 74 - i * 2
            d.line([sc(x1), sc(head_cy + dy), sc(x1 - 14), sc(head_cy + dy)],
                   fill=accent, width=lw_thin)
            x2 = cx + 74 + i * 2
            d.line([sc(x2), sc(head_cy + dy), sc(x2 + 14), sc(head_cy + dy)],
                   fill=accent, width=lw_thin)

    # 激烈驾驶：加两道小速度线
    if state == "spirited":
        for dy in (-10, 8):
            d.line([sc(cx + 68), sc(head_cy + dy), sc(cx + 82), sc(head_cy + dy)],
                   fill=accent, width=lw_thin)

    return img.resize((size, size), Image.LANCZOS)


def png_to_lvgl_c(path_png: str, name: str) -> str:
    """PNG → LVGL C 数组源码片段（CF_TRUE_COLOR，RGB565，小端）"""
    im = Image.open(path_png).convert("RGB")
    w, h = im.size
    px = im.load()
    data = bytearray()
    for y in range(h):
        for x in range(w):
            r, g, b = px[x, y]
            c = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
            data += struct.pack("<H", c)

    lines = []
    lines.append(f"/* {name}: {w}x{h} RGB565, 由 tools/make_images.py 生成，请勿手改 */")
    lines.append(f"const LV_ATTRIBUTE_MEM_ALIGN uint8_t {name}_map[] = {{")
    for i in range(0, len(data), 16):
        chunk = data[i:i + 16]
        lines.append("    " + " ".join(f"0x{b:02X}," for b in chunk))
    lines.append("};")
    lines.append("")
    lines.append(f"const lv_img_dsc_t {name} = {{")
    lines.append("    .header.cf = LV_IMG_CF_TRUE_COLOR,")
    lines.append("    .header.always_zero = 0,")
    lines.append("    .header.reserved = 0,")
    lines.append(f"    .header.w = {w},")
    lines.append(f"    .header.h = {h},")
    lines.append(f"    .data_size = {w * h * 2},")
    lines.append(f"    .data = {name}_map,")
    lines.append("};")
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--force", action="store_true", help="强制重画临时底图")
    args = ap.parse_args()

    os.makedirs(IMG_DIR, exist_ok=True)
    states = ["normal", "spirited", "speeding"]

    for st in states:
        p = os.path.join(IMG_DIR, f"driver_{st}.png")
        if args.force or not os.path.exists(p):
            draw_driver(st).save(p)
            print(f"已生成临时底图: {p}")
        else:
            print(f"已存在（用 --force 可重画）: {p}")

    header = f"""/**
 * ui_driver_images.c —— 驾驶员插画（LVGL 图像资源）
 *
 * 由 tools/make_images.py 从 design/assets/images/driver_*.png 生成。
 * 替换真实素材：覆盖同名 PNG 后重跑该脚本。
 * 格式：LV_IMG_CF_TRUE_COLOR（RGB565，无 alpha，底色即页面的纯黑）
 */
#ifdef LV_LVGL_H_INCLUDE_SIMPLE
#include "lvgl.h"
#else
#include "lvgl/lvgl.h"
#endif

#ifndef LV_ATTRIBUTE_MEM_ALIGN
#define LV_ATTRIBUTE_MEM_ALIGN
#endif

"""
    parts = [header]
    for st in states:
        parts.append(png_to_lvgl_c(os.path.join(IMG_DIR, f"driver_{st}.png"), f"driver_{st}"))
        parts.append("")

    with open(OUT_C, "w", encoding="utf-8") as f:
        f.write("\n".join(parts))

    size_kb = sum(os.path.getsize(os.path.join(IMG_DIR, f"driver_{s}.png")) for s in states) / 1024
    print(f"已生成 {OUT_C}")
    print(f"提示：3 张图在 Flash 中约 {3 * 180 * 180 * 2 / 1024:.0f} KB（180×180 RGB565）")


if __name__ == "__main__":
    main()
