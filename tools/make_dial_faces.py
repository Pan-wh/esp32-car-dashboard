#!/usr/bin/env python3
"""
静态表盘烘焙：把「刻度 + 数字」这些**不变**的元素离线渲染成图片，
运行时只保留会动的指针。

为什么要烘焙（2026-10-13 实测，见 docs/troubleshooting.md）：
  lv_meter 是「整对象重绘」模型——任一脏区都会重新计算**全部**刻度
  （每个刻度都要三角函数 + 画线）。滑页时两页同时可见，两个表盘全量重算。
  实测把模拟时钟的 74 个刻度减到 17 个，滑页单帧从 228ms 降到 180ms
  → 刻度约值 50ms。

几何依据（与 simulator/deps/lvgl/src/extra/widgets/meter/lv_meter.c 一致）：
  刻度：从 r_out=220 向内延伸 tick_length；角度 angle = rotation + i*360/(cnt-1)
        屏幕坐标 x = cx + cos(a)*r, y = cy + sin(a)*r（a 为度，0°=3点钟，顺时针）
  数字：ui_analog.c 中按 NUM_RADIUS 摆放，中心对齐 + NUM_Y_NUDGE

用法：python3 tools/make_dial_faces.py
输出：simulator/ui/ui_dial_faces.c（lv_img_dsc_t: dial_analog）
"""
import argparse
import math
import os

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FONT_DIR = os.path.join(ROOT, "design", "assets", "fonts", "src")
OUT_C = os.path.join(ROOT, "simulator", "ui", "ui_dial_faces.c")

SIZE = 466          # 屏幕边长
CENTER = SIZE / 2.0
SS = 4              # 超采样倍数（先放大再缩小 → 平滑边缘）

# ---- 模拟时钟：Apple StandBy 小组件风格 ----
R_EDGE = 216.0
TICK_LEN_MIN = 7.0
TICK_LEN_MAJ = 12.0
TICK_W_MIN = 2.0
TICK_W_MAJ = 3.0
NUM_RADIUS = 160.0
NUM_Y_NUDGE = -2.0
NUM_FONT_SIZE_MAIN = 52.0
NUM_FONT_SIZE_MINOR = 40.0

C_BG = (0x00, 0x00, 0x00)
C_TICK_MINOR = (0x5E, 0x5E, 0x63)
C_TICK_MAJOR = (0xE5, 0xE5, 0xEA)
C_TICK_CARDINAL = (0xFF, 0x95, 0x00)
C_NUM = (0xF5, 0xF5, 0xF7)

# 指针使用改版前的几何；秒针改为连续扫秒（ui_analog.c 同值）。
HAND_HOUR_LEN = 106.0
HAND_MIN_LEN = 150.0
HAND_SEC_LEN = 166.0
HAND_HOUR_W = 9.0
HAND_MIN_W = 5.0
HAND_SEC_W = 3.0
C_HAND_WHITE = (0xF5, 0xF5, 0xF7)
C_HAND_SEC = (0xFF, 0x95, 0x00)


def _font(size_px):
    """Nunito Bold 子集（与运行时 font_num_dial_38 同源）"""
    path = os.path.join(FONT_DIR, "nunito-num-sub.ttf")
    if not os.path.exists(path):
        raise SystemExit(f"缺少字体 {path}（先运行 tools/make_fonts.sh 生成子集）")
    return ImageFont.truetype(path, int(size_px * SS))


def _tick(d, angle_deg, r_out, r_in, width):
    """按 LVGL 语义画一个刻度：从 r_in 到 r_out 的线段，端点为圆头"""
    a = math.radians(angle_deg)
    x1 = CENTER * SS + math.cos(a) * r_in * SS
    y1 = CENTER * SS + math.sin(a) * r_in * SS
    x2 = CENTER * SS + math.cos(a) * r_out * SS
    y2 = CENTER * SS + math.sin(a) * r_out * SS
    w = width * SS
    d.line([(x1, y1), (x2, y2)], fill=None, width=int(round(w)))
    # Pillow 的 line 不支持圆头，用两个圆点补出圆头端
    for (x, y) in ((x1, y1), (x2, y2)):
        rr = w / 2.0
        d.ellipse([x - rr, y - rr, x + rr, y + rr], fill=None)


def render_analog_face():
    """Apple StandBy 风格：短刻度 + 四个主数字 + 八个次数字。"""
    img = Image.new("RGB", (SIZE * SS, SIZE * SS), C_BG)
    d = ImageDraw.Draw(img)

    def line(p1, p2, fill, w):
        d.line([p1, p2], fill=fill, width=int(round(w * SS)))

    def dot(x, y, r, fill):
        d.ellipse([x - r, y - r, x + r, y + r], fill=fill)

    # 60 个短刻度：整点更亮、更长，其余分钟刻度弱化。
    for i in range(60):
        a = math.radians(270.0 + i * 6.0)
        major = i % 5 == 0
        tick_len = TICK_LEN_MAJ if major else TICK_LEN_MIN
        cardinal = i % 15 == 0
        tick_w = 4.0 if cardinal else (TICK_W_MAJ if major else TICK_W_MIN)
        color = C_TICK_CARDINAL if cardinal else (C_TICK_MAJOR if major else C_TICK_MINOR)
        r_in, r_out = R_EDGE - tick_len, R_EDGE
        x1, y1 = (CENTER + math.cos(a) * r_in) * SS, (CENTER + math.sin(a) * r_in) * SS
        x2, y2 = (CENTER + math.cos(a) * r_out) * SS, (CENTER + math.sin(a) * r_out) * SS
        line((x1, y1), (x2, y2), color, tick_w)
        rr = tick_w * SS / 2.0
        dot(x1, y1, rr, color)
        dot(x2, y2, rr, color)

    # Apple 小组件以 12/3/6/9 建立主层级，其余数字更小。
    font_main = _font(NUM_FONT_SIZE_MAIN)
    font_minor = _font(NUM_FONT_SIZE_MINOR)
    for h in range(1, 13):
        a = math.radians(h * 30.0)
        cx = (CENTER + NUM_RADIUS * math.sin(a)) * SS
        cy = (CENTER - NUM_RADIUS * math.cos(a) + NUM_Y_NUDGE) * SS
        font = font_main if h in (3, 6, 9, 12) else font_minor
        d.text((cx, cy), str(h), font=font, fill=C_NUM, anchor="mm")

    return img.resize((SIZE, SIZE), Image.LANCZOS)


def render_analog_preview(face, hour, minute, second):
    """用与 LVGL 相同的指针参数生成审批预览，不依赖模拟器编译。"""
    img = face.copy()
    d = ImageDraw.Draw(img)

    def hand(angle_deg, length, width, color):
        a = math.radians(angle_deg - 90.0)
        x2 = CENTER + math.cos(a) * length
        y2 = CENTER + math.sin(a) * length
        w = int(round(width))
        d.line([(CENTER, CENTER), (x2, y2)], fill=color, width=w)
        r = width / 2.0
        d.ellipse([CENTER - r, CENTER - r, CENTER + r, CENTER + r], fill=color)
        d.ellipse([x2 - r, y2 - r, x2 + r, y2 + r], fill=color)

    total_seconds = minute * 60.0 + second
    hand((hour % 12) * 30.0 + total_seconds / 120.0,
         HAND_HOUR_LEN, HAND_HOUR_W, C_HAND_WHITE)
    hand(minute * 6.0 + second / 10.0,
         HAND_MIN_LEN, HAND_MIN_W, C_HAND_WHITE)
    hand(second * 6.0, HAND_SEC_LEN, HAND_SEC_W, C_HAND_SEC)

    # 改版前的中心轴：18px 白底 + 8px 橙色轴心。
    d.ellipse([CENTER - 9, CENTER - 9, CENTER + 9, CENTER + 9], fill=C_HAND_WHITE)
    d.ellipse([CENTER - 4, CENTER - 4, CENTER + 4, CENTER + 4], fill=C_HAND_SEC)
    return img



# ---- 速度表盘（V5 设计常量，与 ui_speed.c 对应）----
SPEED_MAX = 200.0
SPEED_MINOR_CNT = 41
SPEED_MINOR_LEN = 10.0
SPEED_MINOR_W = 2.0
SPEED_MAJOR_CNT = 7
SPEED_MAJOR_LEN = 18.0
SPEED_MAJOR_W = 3.0
SPEED_LABEL_R = 178.0
SPEED_ROT = 135.0
SPEED_SWEEP = 270.0
C_SPEED_TICK_MIN = (0x9A, 0x9A, 0x9E)
C_SPEED_TICK_MAJ = (0x4A, 0x4A, 0x50)
SPEEDING_V = 120.0


def speed_color(v):
    """与 ui_speed.c 的 speed_color() 一致：<120 深灰→橙；>=120 橙→红"""
    if v < 0:
        v = 0.0
    if v > SPEED_MAX:
        v = SPEED_MAX
    if v <= SPEEDING_V:
        c0, c1, t = (0x4A, 0x4A, 0x50), (0xFF, 0x95, 0x00), v / SPEEDING_V
    else:
        c0, c1, t = (0xFF, 0x95, 0x00), (0xFF, 0x3B, 0x30), (v - SPEEDING_V) / (SPEED_MAX - SPEEDING_V)
    return tuple(int(a + (b - a) * t) for a, b in zip(c0, c1))


def render_speed_face():
    """速度表盘：41 小刻度（按速度渐变着色）+ 7 大刻度 + 数字 0/40/…/200（白底）"""
    img = Image.new("RGB", (SIZE * SS, SIZE * SS), (0xFF, 0xFF, 0xFF))
    d = ImageDraw.Draw(img)

    def seg(angle_deg, r_in, r_out, color, w):
        a = math.radians(angle_deg)
        x1, y1 = (CENTER + math.cos(a) * r_in) * SS, (CENTER + math.sin(a) * r_in) * SS
        x2, y2 = (CENTER + math.cos(a) * r_out) * SS, (CENTER + math.sin(a) * r_out) * SS
        d.line([(x1, y1), (x2, y2)], fill=color, width=int(round(w * SS)))
        rr = w * SS / 2.0
        for (x, y) in ((x1, y1), (x2, y2)):
            d.ellipse([x - rr, y - rr, x + rr, y + rr], fill=color)

    def val_to_angle(v):
        return SPEED_ROT + (v / SPEED_MAX) * SPEED_SWEEP

    # 大刻度（先画，被小刻度/渐变覆盖内段）
    for i in range(SPEED_MAJOR_CNT):
        v = SPEED_MAX * i / (SPEED_MAJOR_CNT - 1)
        seg(val_to_angle(v), R_EDGE - SPEED_MAJOR_LEN, R_EDGE, C_SPEED_TICK_MAJ, SPEED_MAJOR_W)

    # 小刻度：按该刻度对应的速度取渐变颜色（复刻 scale_lines 覆盖在刻度上的观感）
    for i in range(SPEED_MINOR_CNT):
        v = SPEED_MAX * i / (SPEED_MINOR_CNT - 1)
        w = SPEED_MINOR_W + (2.0 if v >= SPEEDING_V else 0.0)
        seg(val_to_angle(v), R_EDGE - SPEED_MINOR_LEN, R_EDGE, speed_color(v), w)

    # 刻度数字：0/40/80/120/160/200，半径 178
    font = _font(26)
    for i in range(6):
        v = 40.0 * i
        a = math.radians(val_to_angle(v))
        cx = (CENTER + SPEED_LABEL_R * math.cos(a)) * SS
        cy = (CENTER + SPEED_LABEL_R * math.sin(a)) * SS
        d.text((cx, cy), str(int(v)), font=font, fill=speed_color(v), anchor="mm")

    return img.resize((SIZE, SIZE), Image.LANCZOS)


def img_to_lvgl_c(img, name):
    """RGB888 → LVGL RGB565 (LV_IMG_CF_TRUE_COLOR) C 数组"""
    w, h = img.size
    px = img.load()
    data = []
    for y in range(h):
        for x in range(w):
            r, g, b = px[x, y]
            v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
            data.append(v & 0xFF)
            data.append((v >> 8) & 0xFF)  # 小端：LV_COLOR_16_SWAP=0
    body = ",".join(str(b) for b in data)
    return f"""
/* {name}：{w}×{h} RGB565（LV_IMG_CF_TRUE_COLOR，无 alpha） */
static const uint8_t {name}_map[] = {{{body}}};

const lv_img_dsc_t {name} = {{
    .header.cf = LV_IMG_CF_TRUE_COLOR,
    .header.always_zero = 0,
    .header.reserved = 0,
    .header.w = {w},
    .header.h = {h},
    .data_size = {len(data)},
    .data = {name}_map,
}};
"""


def main():
    parser = argparse.ArgumentParser(description="生成 LVGL 表盘资源或 UI 审批预览")
    parser.add_argument("--preview", help="只生成静态预览 PNG，不改仓库生成物")
    parser.add_argument("--preview-gif", help="只生成 1 秒扫秒预览 GIF，不改仓库生成物")
    parser.add_argument("--time", default="09:40:30", help="预览时间 H:M:S")
    args = parser.parse_args()

    if not os.path.exists(FONT_DIR):
        raise SystemExit(f"缺少字体目录 {FONT_DIR}")

    print("渲染模拟时钟表盘…")
    analog = render_analog_face()
    if args.preview or args.preview_gif:
        try:
            hour, minute, second = (int(v) for v in args.time.split(":"))
        except ValueError as exc:
            raise SystemExit("--time 格式应为 H:M:S") from exc
        if args.preview:
            render_analog_preview(analog, hour, minute, float(second)).save(args.preview)
            print(f"已写出静态预览 {args.preview}")
        if args.preview_gif:
            frames = [render_analog_preview(analog, hour, minute, second + i / 20.0)
                      for i in range(20)]
            frames[0].save(args.preview_gif, save_all=True, append_images=frames[1:],
                           duration=50, loop=0)
            print(f"已写出扫秒预览 {args.preview_gif}")
        return

    os.makedirs(os.path.join(ROOT, "design", "assets", "images"), exist_ok=True)
    analog.save(os.path.join(ROOT, "design", "assets", "images", "dial_analog_466.png"))

    with open(OUT_C, "w", encoding="utf-8") as f:
        f.write("""/**
 * ui_dial_faces.c —— 静态表盘烘焙图（由 tools/make_dial_faces.py 生成，请勿手改）
 *
 * 刻度与数字都是不变的，烘焙成图片后运行时只需重绘会动的指针，
 * 可显著降低整屏重绘（滑页）成本。几何与 ui_analog.c 的设计常量一致。
 */
#include "lvgl.h"

""")
        f.write(img_to_lvgl_c(analog, "dial_analog"))
    print("渲染速度表盘…")
    speed = render_speed_face()
    speed.save(os.path.join(ROOT, "design", "assets", "images", "dial_speed_466.png"))
    with open(OUT_C, "a", encoding="utf-8") as f:
        f.write(img_to_lvgl_c(speed, "dial_speed"))
    size_kb = os.path.getsize(OUT_C) // 1024
    print(f"已写出 {OUT_C}（{size_kb} KB 源码）")


if __name__ == "__main__":
    main()
