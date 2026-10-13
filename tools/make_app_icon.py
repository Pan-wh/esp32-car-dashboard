#!/usr/bin/env python3
"""
App 图标生成：与仪表盘 UI 风格一致的「速度表 + 指针」图标。

设计依据（与 simulator/ui/ui_speed.c 的 V5 表盘同源）：
  - 白底圆盘 + 深灰/橙/红渐变刻度环（与 dial_speed 一致）
  - 橙色指针（C_WARN = 0xFF9500），指向中高速区
  - 底部小字 km/h，呼应仪表底部信息
iOS 图标要求：不透明、无 alpha、直角（系统自动圆角），故导出纯 RGB。

用法：python3 tools/make_app_icon.py
输出：ios/CarDashboardPhone/CarDashboardPhone/Assets.xcassets/AppIcon.appiconset/ 下的
      多尺寸 PNG（含 1024×1024 主图）+ Contents.json
"""
import json
import math
import os

from PIL import Image, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ICON_DIR = os.path.join(
    ROOT, "ios", "CarDashboardPhone", "CarDashboardPhone",
    "Assets.xcassets", "AppIcon.appiconset",
)

S = 1024          # 主图尺寸
SS = 4            # 超采样
BG = (255, 255, 255)
TRACK_DIM = (232, 232, 236)
C_NEEDLE = (255, 149, 0)
C_TEXT = (60, 60, 66)

# 与固件 speed_color() 一致的渐变：深灰 → 橙 → 红
def speed_color(t):
    """t ∈ [0,1] 表示表盘量程比例（0=起点, 1=满量程）"""
    a = (74, 74, 80)       # 深灰（低速）
    b = (255, 149, 0)      # 橙（阈值 120/200 = 0.6）
    c = (255, 59, 48)      # 红（满量程）
    if t <= 0.6:
        k = t / 0.6
        p, q = a, b
    else:
        k = (t - 0.6) / 0.4
        p, q = b, c
    return tuple(int(p[i] + (q[i] - p[i]) * k) for i in range(3))


def render():
    size = S * SS
    img = Image.new("RGB", (size, size), BG)
    d = ImageDraw.Draw(img)
    cx = cy = size / 2.0

    # 刻度：贴着外圈、短而朝内（仪表直觉）；中间留白给指针
    R_OUT = size * 0.470         # 刻度外端（留出图标安全边距）
    R_TICK = size * 0.045        # 刻度长度（短促，避免"毛刺"感）
    R_IN = R_OUT - R_TICK        # 刻度内端
    R_NEEDLE = size * 0.325      # 指针长度（明显短于刻度内圈，读数清晰）

    sweep_start = 135.0          # 与固件一致：135° 起（左下）
    sweep = 270.0

    # 底环：位于刻度内端稍内侧，作为"表盘边界"
    rr0 = R_IN - size * 0.014
    d.ellipse([cx - rr0, cy - rr0, cx + rr0, cy + rr0],
              outline=TRACK_DIM, width=int(size * 0.010))

    # 41 个小刻度，按量程渐变着色（40 段）
    N = 41
    for i in range(N):
        t = i / (N - 1)
        ang = math.radians(sweep_start + t * sweep)
        major = (i % 6 == 0)                       # 每 6 格一个大刻度（共 7 个）
        r_in = R_IN - (size * 0.014 if major else 0.0)
        w = size * 0.021 if major else size * 0.009
        col = speed_color(t)
        x1, y1 = cx + math.cos(ang) * r_in, cy + math.sin(ang) * r_in
        x2, y2 = cx + math.cos(ang) * R_OUT, cy + math.sin(ang) * R_OUT
        d.line([(x1, y1), (x2, y2)], fill=col, width=int(w))
        rr = w / 2.0
        for (px, py) in ((x1, y1), (x2, y2)):
            d.ellipse([px - rr, py - rr, px + rr, py + rr], fill=col)

    # 指针：指向约 62% 量程（视觉上偏右，像"行驶中"）
    ang = math.radians(sweep_start + 0.62 * sweep)
    nx, ny = cx + math.cos(ang) * R_NEEDLE, cy + math.sin(ang) * R_NEEDLE
    inner = size * 0.055  # 内端留出中心点
    ix, iy = cx + math.cos(ang) * inner, cy + math.sin(ang) * inner
    d.line([(ix, iy), (nx, ny)], fill=C_NEEDLE, width=int(size * 0.026))
    rr = size * 0.015
    d.ellipse([nx - rr, ny - rr, nx + rr, ny + rr], fill=C_NEEDLE)

    # 中心圆点
    rc = size * 0.030
    d.ellipse([cx - rc, cy - rc, cx + rc, cy + rc], fill=C_NEEDLE)
    rc2 = size * 0.013
    d.ellipse([cx - rc2, cy - rc2, cx + rc2, cy + rc2], fill=(255, 255, 255))

    return img.resize((S, S), Image.LANCZOS)


# iOS 单尺寸图标清单（iPhone + iPad 通知/设置/Spotlight/App）
SPECS = [
    ("iphone", "20x20", 2, 40), ("iphone", "20x20", 3, 60),
    ("iphone", "29x29", 2, 58), ("iphone", "29x29", 3, 87),
    ("iphone", "40x40", 2, 80), ("iphone", "40x40", 3, 120),
    ("iphone", "60x60", 2, 120), ("iphone", "60x60", 3, 180),
    ("ipad", "20x20", 1, 20), ("ipad", "20x20", 2, 40),
    ("ipad", "29x29", 1, 29), ("ipad", "29x29", 2, 58),
    ("ipad", "40x40", 1, 40), ("ipad", "40x40", 2, 80),
    ("ipad", "76x76", 1, 76), ("ipad", "76x76", 2, 152),
    ("ipad", "83.5x83.5", 2, 167),
    ("ios-marketing", "1024x1024", 1, 1024),
]


def main():
    os.makedirs(ICON_DIR, exist_ok=True)
    base = render()

    images = []
    for idiom, size_str, scale, px in SPECS:
        name = f"icon-{size_str.replace('.', '_')}@{scale}x.png"
        base.resize((px, px), Image.LANCZOS).save(os.path.join(ICON_DIR, name))
        images.append({"idiom": idiom, "size": size_str, "scale": f"{scale}x",
                       "filename": name})
        print(f"  {name} ({px}×{px})")

    with open(os.path.join(ICON_DIR, "Contents.json"), "w", encoding="utf-8") as f:
        json.dump({"images": images, "info": {"version": 1, "author": "xcode"}},
                  f, indent=2, ensure_ascii=False)
    print(f"已写出 {ICON_DIR}/Contents.json（{len(images)} 项）")

    # 预览图（方便人工确认）
    base.save(os.path.join(ROOT, "design", "assets", "images", "app_icon_1024.png"))
    print("预览：design/assets/images/app_icon_1024.png")


if __name__ == "__main__":
    main()
