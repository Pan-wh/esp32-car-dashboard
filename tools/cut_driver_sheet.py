#!/usr/bin/env python3
"""
切图工具：把「驾驶员表情组图」自动切成独立的圆形头像 PNG

适用：一张图上排布若干圆形角色（白底，圆内是角色）。
自动识别每个圆的圆心与半径（用行/列最大跨度定位），裁成正方形并把圆外置为
纯黑（与仪表页背景一致），缩放到目标尺寸输出。

用法：
  python3 tools/cut_driver_sheet.py <组图.png> [--size 200] [--outdir design/assets/images]

默认输出（按位置命名，供 make_images.py 映射到状态）：
  上排单个      → driver_shout.png    （咬牙瞪眼）
  下排左        → driver_frown.png    （皱眉）
  下排右        → driver_calm.png     （平静）

依赖：numpy + Pillow（DSH 运行环境已内置）
"""
import argparse
import os
import sys

import numpy as np
from PIL import Image, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# 位置 → 输出文件名（改这里即可调整命名）
POSITION_NAMES = {
    "top": "driver_shout.png",
    "bottom_left": "driver_frown.png",
    "bottom_right": "driver_calm.png",
}


def find_circles(mask: np.ndarray):
    """返回 [(name, cx, cy, r), ...]，按 上排 / 下排左 / 下排右 识别。"""
    H, W = mask.shape

    # 上下分界：找中间一段「整行无像素」的空隙
    rows = mask.sum(axis=1)
    gaps = np.where(rows == 0)[0]
    mid = gaps[(gaps > H * 0.25) & (gaps < H * 0.8)]
    split = int((mid.min() + mid.max()) // 2) if len(mid) else H // 2

    def circle_of(sub, x_off, y_off, name):
        row_ext = []
        for y in range(sub.shape[0]):
            xs = np.where(sub[y])[0]
            row_ext.append((xs.max() - xs.min() + 1) if len(xs) else 0)
        col_ext = []
        for x in range(sub.shape[1]):
            ys = np.where(sub[:, x])[0]
            col_ext.append((ys.max() - ys.min() + 1) if len(ys) else 0)
        cy = int(np.argmax(row_ext))
        cx = int(np.argmax(col_ext))
        d_row, d_col = max(row_ext), max(col_ext)
        r = (d_row + d_col) / 4.0
        return (name, x_off + cx, y_off + cy, r)

    out = []
    # 上排
    out.append(circle_of(mask[:split, :], 0, 0, "top"))
    # 下排：找中间的竖直空隙分成左右
    lower = mask[split:, :]
    cols = lower.sum(axis=0)
    cgaps = np.where(cols == 0)[0]
    cmid = cgaps[(cgaps > W * 0.3) & (cgaps < W * 0.7)]
    xsplit = int((cmid.min() + cmid.max()) // 2) if len(cmid) else W // 2
    out.append(circle_of(lower[:, :xsplit], 0, split, "bottom_left"))
    out.append(circle_of(lower[:, xsplit:], xsplit, split, "bottom_right"))
    return out


def cut_one(im: Image.Image, cx: float, cy: float, r: float, size: int, ss: int = 4) -> Image.Image:
    """裁出圆并置于黑底，输出 size×size。"""
    half = r + 2.0                      # 含描边，留 2px 余量
    box = (int(round(cx - half)), int(round(cy - half)),
           int(round(cx + half)), int(round(cy + half)))
    crop = im.crop(box).convert("RGB")

    # 圆外遮罩（放大 ss 倍做抗锯齿边缘）
    S = size * ss
    big = crop.resize((S, S), Image.LANCZOS)
    m = Image.new("L", (S, S), 0)
    ImageDraw.Draw(m).ellipse([0, 0, S - 1, S - 1], fill=255)
    black = Image.new("RGB", (S, S), (0, 0, 0))
    big = Image.composite(big, black, m)

    return big.resize((size, size), Image.LANCZOS)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("sheet", help="组图 PNG 路径")
    ap.add_argument("--size", type=int, default=200, help="输出边长（默认 200）")
    ap.add_argument("--outdir", default=os.path.join(ROOT, "design", "assets", "images"))
    args = ap.parse_args()

    im = Image.open(args.sheet).convert("RGB")
    a = np.asarray(im).astype(np.int16)
    mask = a.sum(axis=2) < 3 * 240        # 非白即前景
    print(f"组图 {im.size[0]}×{im.size[1]}，前景像素 {int(mask.sum())}")

    circles = find_circles(mask)
    os.makedirs(args.outdir, exist_ok=True)
    for name, cx, cy, r in circles:
        out = cut_one(im, cx, cy, r, args.size)
        fname = POSITION_NAMES[name]
        path = os.path.join(args.outdir, fname)
        out.save(path)
        print(f"  {name:<12} 圆心({cx:>4},{cy:>4}) 半径 {r:6.1f} → {fname} ({args.size}×{args.size})")

    print(f"\n完成，输出目录: {args.outdir}")
    print("接下来：确认表情与状态的对应关系后运行 python3 tools/make_images.py 转成 LVGL 数组。")


if __name__ == "__main__":
    main()
