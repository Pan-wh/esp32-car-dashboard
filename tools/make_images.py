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
    """卡通驾驶员（圆脸 + 卷发 + 双手扶方向盘），三种状态表情。

    风格参考用户提供的车载集群截图：扁平色块 + 粗描边 + 圆润造型。
    注意：不复刻任何品牌标识（AI_CONTEXT 第 3 节要求）。
    底色纯黑（与页面背景一致，使用 CF_TRUE_COLOR 无透明通道）。
    """
    import math

    S = size * SS
    img = Image.new("RGB", (S, S), (0, 0, 0))
    d = ImageDraw.Draw(img)

    SKIN = (243, 198, 163)
    SKIN_D = (222, 172, 136)
    HAIR = (38, 38, 43)
    SHIRT = (76, 111, 168)
    SHIRT_D = (58, 88, 138)
    WHEEL = (56, 56, 62)
    LINE = (28, 28, 32)
    accent = ACCENTS[state]

    def sc(v):
        return int(round(v * SS))

    def E(box, **kw):
        d.ellipse([sc(box[0]), sc(box[1]), sc(box[2]), sc(box[3])], **kw)

    cx = size * 0.5
    head_cy = 62
    head_rx, head_ry = 36, 34

    # ---------- 躯干 ----------
    d.polygon([(sc(cx - 64), sc(size)), (sc(cx - 50), sc(106)),
               (sc(cx + 50), sc(106)), (sc(cx + 64), sc(size))], fill=SHIRT)
    d.polygon([(sc(cx - 16), sc(106)), (sc(cx), sc(124)), (sc(cx + 16), sc(106))], fill=SKIN_D)

    # ---------- 脖子 ----------
    d.rectangle([sc(cx - 12), sc(86), sc(cx + 12), sc(112)], fill=SKIN_D)

    # ---------- 头 + 耳朵 ----------
    E((cx - head_rx, head_cy - head_ry, cx + head_rx, head_cy + head_ry), fill=SKIN)
    E((cx - head_rx - 5, head_cy - 5, cx - head_rx + 5, head_cy + 11), fill=SKIN)
    E((cx + head_rx - 5, head_cy - 5, cx + head_rx + 5, head_cy + 11), fill=SKIN)

    # ---------- 卷发（沿颅顶排一圈小球） ----------
    for i in range(9):
        t = math.pi * (1.0 - i / 8.0)
        hx = cx + (head_rx - 3) * math.cos(t)
        hy = head_cy - (head_ry - 7) * math.sin(t) - 3
        r = 11 if 2 <= i <= 6 else 9
        E((hx - r, hy - r, hx + r, hy + r), fill=HAIR)
    # 发顶加厚（只盖到眉毛之上）
    E((cx - 27, head_cy - head_ry - 11, cx + 27, head_cy - head_ry + 16), fill=HAIR)
    # 鬓角
    E((cx - head_rx - 1, head_cy - 15, cx - head_rx + 11, head_cy + 7), fill=HAIR)
    E((cx + head_rx - 11, head_cy - 15, cx + head_rx + 1, head_cy + 7), fill=HAIR)

    # ---------- 表情 ----------
    ex_l, ex_r = cx - 13, cx + 13
    ey = head_cy + 3

    if state == "normal":
        for ex in (ex_l, ex_r):
            E((ex - 4.5, ey - 5.5, ex + 4.5, ey + 5.5), fill=LINE)
            E((ex - 1.6, ey - 3.6, ex + 1.4, ey - 0.6), fill=(255, 255, 255))
        d.line([sc(ex_l - 9), sc(ey - 16), sc(ex_l + 8), sc(ey - 14)], fill=HAIR, width=sc(3))
        d.line([sc(ex_r - 8), sc(ey - 14), sc(ex_r + 9), sc(ey - 16)], fill=HAIR, width=sc(3))
        d.arc([sc(cx - 11), sc(head_cy + 13), sc(cx + 11), sc(head_cy + 27)],
              start=20, end=160, fill=LINE, width=sc(2.6))
    elif state == "spirited":
        for ex in (ex_l, ex_r):
            E((ex - 4, ey - 4, ex + 4, ey + 4), fill=LINE)
            E((ex - 1.4, ey - 2.8, ex + 1.4, ey - 0.4), fill=(255, 255, 255))
        d.line([sc(ex_l - 10), sc(ey - 19), sc(ex_l + 9), sc(ey - 12)], fill=HAIR, width=sc(3.4))
        d.line([sc(ex_r - 9), sc(ey - 12), sc(ex_r + 10), sc(ey - 19)], fill=HAIR, width=sc(3.4))
        d.line([sc(cx - 10), sc(head_cy + 21), sc(cx + 10), sc(head_cy + 21)],
               fill=LINE, width=sc(2.6))
        E((cx + 30, head_cy - 22, cx + 38, head_cy - 7), fill=accent)
    else:  # speeding
        for ex in (ex_l, ex_r):
            E((ex - 6.5, ey - 7, ex + 6.5, ey + 7), fill=(255, 255, 255))
            E((ex - 3.4, ey - 3.8, ex + 3.4, ey + 3.8), fill=LINE)
        d.line([sc(ex_l - 10), sc(ey - 21), sc(ex_l + 9), sc(ey - 16)], fill=HAIR, width=sc(3))
        d.line([sc(ex_r - 9), sc(ey - 16), sc(ex_r + 10), sc(ey - 21)], fill=HAIR, width=sc(3))
        E((cx - 10, head_cy + 14, cx + 10, head_cy + 31), fill=(118, 38, 38))
        E((cx - 6, head_cy + 25, cx + 6, head_cy + 31), fill=(232, 124, 124))
        E((cx + 30, head_cy - 24, cx + 37, head_cy - 10), fill=accent)
        E((cx + 39, head_cy - 6, cx + 45, head_cy + 6), fill=accent)
        for dy in (-16, 0, 16):
            d.line([sc(cx - 86), sc(head_cy + dy), sc(cx - 68), sc(head_cy + dy)],
                   fill=accent, width=sc(3))
            d.line([sc(cx + 86), sc(head_cy + dy), sc(cx + 104), sc(head_cy + dy)],
                   fill=accent, width=sc(3))

    # ---------- 鼻子 ----------
    d.arc([sc(cx - 6), sc(head_cy + 7), sc(cx + 6), sc(head_cy + 16)],
          start=200, end=340, fill=SKIN_D, width=sc(2.4))

    # ---------- 手臂 ----------
    for sx in (-1, 1):
        d.line([sc(cx + sx * 57), sc(122), sc(cx + sx * 44), sc(134)],
               fill=SHIRT_D, width=sc(13))

    # ---------- 方向盘（先画轮，再画手 → 手在轮缘之上） ----------
    d.arc([sc(cx - 62), sc(124), sc(cx + 62), sc(180)], start=185, end=355,
          fill=WHEEL, width=sc(10))
    E((cx - 9, 144, cx + 9, 162), fill=WHEEL)

    # ---------- 手 ----------
    for sx in (-1, 1):
        E((cx + sx * 44 - 11, 134 - 11, cx + sx * 44 + 11, 134 + 11), fill=SKIN)

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
