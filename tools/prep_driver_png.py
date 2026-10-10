#!/usr/bin/env python3
"""
驾驶员素材预处理：单张 PNG（可带透明通道）→ 统一尺寸 + 指定底色

适用于美术逐张交付的素材（相比 cut_driver_sheet.py 的「一张组图切多张」）。

用法：
  python3 tools/prep_driver_png.py --size 230 --bg FFFFFF \
      normal=design/assets/images/source/driver_calm_src.png \
      spirited=design/assets/images/source/driver_frown_src.png \
      speeding=design/assets/images/source/driver_angry_src.png

  # 保留透明通道（改用 CF_TRUE_COLOR_ALPHA，体积 +50%，可叠任意底色）
  python3 tools/prep_driver_png.py --keep-alpha ...

参数：
  --size N     输出边长（默认 230；内容圆半径 ≈ N/2，需满足 24 + N/2 < 158 才不会碰到刻度数字）
  --bg RRGGBB  合成底色（默认 FFFFFF 白，与白底表盘无缝）
  --keep-alpha 保留透明通道（输出 RGBA PNG，需在 make_images.py 中同步改用 alpha 格式）

输出：design/assets/images/driver_calm.png / driver_frown.png / driver_shout.png
（文件名与 make_images.py 的 STATE_FILES 对应）
"""
import argparse
import os
import sys

from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_DIR = os.path.join(ROOT, "design", "assets", "images")

# 状态 → 输出文件名（与 make_images.py 的 STATE_FILES 保持一致）
OUT_NAMES = {
    "normal": "driver_calm.png",
    "spirited": "driver_frown.png",
    "speeding": "driver_shout.png",
}


def parse_hex(s: str, default=(255, 255, 255)):
    s = (s or "").strip().lstrip("#")
    if len(s) != 6:
        return default
    try:
        return tuple(int(s[i:i + 2], 16) for i in (0, 2, 4))
    except ValueError:
        return default


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pairs", nargs="+", help="状态=源PNG，如 normal=xxx.png")
    ap.add_argument("--size", type=int, default=230)
    ap.add_argument("--bg", default="FFFFFF", help="合成底色 RRGGBB（默认白）")
    ap.add_argument("--keep-alpha", action="store_true", help="保留透明通道")
    args = ap.parse_args()

    bg = parse_hex(args.bg)
    os.makedirs(OUT_DIR, exist_ok=True)
    print(f"目标尺寸 {args.size}×{args.size}，底色 {'透明(RGBA)' if args.keep_alpha else '#%02X%02X%02X' % bg}")

    for pair in args.pairs:
        if "=" not in pair:
            sys.exit(f"参数格式应为 状态=源PNG，收到: {pair}")
        state, src = pair.split("=", 1)
        if state not in OUT_NAMES:
            sys.exit(f"未知状态 {state}（可用: {', '.join(OUT_NAMES)}）")
        if not os.path.exists(src):
            sys.exit(f"源文件不存在: {src}")

        im = Image.open(src).convert("RGBA")
        # 等比缩放到目标边长（源为正方形；若略扁则先居中裁成正方形）
        w, h = im.size
        if w != h:
            side = min(w, h)
            im = im.crop(((w - side) // 2, (h - side) // 2,
                          (w - side) // 2 + side, (h - side) // 2 + side))
        im = im.resize((args.size, args.size), Image.LANCZOS)

        out_path = os.path.join(OUT_DIR, OUT_NAMES[state])
        if args.keep_alpha:
            im.save(out_path)
            alpha_pct = 100.0 * sum(1 for p in im.getdata() if p[3] == 0) / (args.size ** 2)
            print(f"  {state:<9} → {OUT_NAMES[state]}  (RGBA，透明像素 {alpha_pct:.1f}%)")
        else:
            flat = Image.new("RGB", im.size, bg)
            flat.paste(im, mask=im.split()[3])
            flat.save(out_path)
            print(f"  {state:<9} → {OUT_NAMES[state]}  (合成到 #{args.bg})")

    print("\n完成。接下来运行 python3 tools/make_images.py 生成 LVGL C 数组。")


if __name__ == "__main__":
    main()
