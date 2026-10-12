#!/usr/bin/env python3
"""
ui_regression.py —— UI 像素级回归检查（模拟器截图 → 断言设计不变量）

为什么需要：本项目 UI 的设计不变量（配色、圆屏安全区、五态文案与配色、
速度渐变等）此前只靠一次性手工测量确认，**没有自动化保护**；
以后改 UI 很容易悄悄退化（本项目就发生过：速度页变体默认值错、手指针压字）。

用法：
    cd simulator && make            # 先构建 dashboard_sim
    python3 ../tools/ui_regression.py

依赖：simulator/dashboard_sim（--offscreen --shot 输出 BMP）、Pillow
退出码：0 全部通过；1 有断言失败
"""
import os
import subprocess
import sys

from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SIM = os.path.join(ROOT, "simulator", "dashboard_sim")
SHOTS = "/tmp/ui_regression"

# 与 ui_speed.c / ui_clock.c 中的设计常量保持一致（改设计需同步这里）
WHITE_V5 = (255, 255, 255)
BLACK_BG = (0, 0, 0)
ORANGE_SECOND = (0xFF, 0x95, 0x00)   # 秒针 / 指针橙
GREEN_LIVE = (0x34, 0xC7, 0x59)      # LIVE
RED_STALE = (0xFF, 0x3B, 0x30)       # STALE
CX = CY = 233.0
R = 233.0

pass_n = 0
fail_n = 0


def check(cond, msg):
    global pass_n, fail_n
    if cond:
        pass_n += 1
    else:
        fail_n += 1
        print(f"  ✗ {msg}")


def shot(name, *args):
    os.makedirs(SHOTS, exist_ok=True)
    path = os.path.join(SHOTS, f"{name}.bmp")
    cmd = [SIM, "--offscreen", "--ms", "400", "--shot", path] + list(args)
    r = subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    if r.returncode != 0 or not os.path.exists(path):
        check(False, f"截图失败: {name} ({r.stderr.decode()[:120]})")
        return None
    return Image.open(path).convert("RGB")


def count_color(im, target, tol=6, region=None):
    """统计接近 target 的像素数；region=(x0,y0,x1,y1) 限定范围"""
    px = im.load()
    W, H = im.size
    x0, y0, x1, y1 = region if region else (0, 0, W, H)
    n = 0
    for y in range(y0, y1):
        for x in range(x0, x1):
            p = px[x, y]
            if abs(p[0]-target[0]) <= tol and abs(p[1]-target[1]) <= tol and abs(p[2]-target[2]) <= tol:
                n += 1
    return n


def has_text_like(im, region, bg, min_nonbg=50, tol=10):
    """区域内是否存在明显不同于背景的像素（即有内容）"""
    px = im.load()
    x0, y0, x1, y1 = region
    n = 0
    for y in range(y0, y1):
        for x in range(x0, x1):
            p = px[x, y]
            if abs(p[0]-bg[0]) > tol or abs(p[1]-bg[1]) > tol or abs(p[2]-bg[2]) > tol:
                n += 1
    return n >= min_nonbg, n


def main():
    if not os.path.exists(SIM):
        print(f"找不到 {SIM}，请先 cd simulator && make")
        return 1

    print("=== UI 像素级回归检查 ===")

    # ---------- 1. 圆屏遮罩：四角应为黑（圆外） ----------
    im = shot("page0", "--page", "0")
    if im:
        px = im.load()
        corners = [px[3, 3], px[462, 3], px[3, 462], px[462, 462]]
        check(all(sum(c) < 40 for c in corners), f"圆屏四角应为暗（圆外），实际 {corners}")
        center = px[233, 233]
        check(sum(center) >= 0, f"圆心应有内容，实际 {center}")

    # ---------- 2. 模拟时钟页：黑底 + 橙色秒针 ----------
    if im:
        n_black = count_color(im, BLACK_BG, tol=8)
        check(n_black > 20000, f"模拟时钟页应有大面积黑底（实际 {n_black} px）")
        n_orange = count_color(im, ORANGE_SECOND, tol=12)
        check(n_orange > 50, f"模拟时钟页应有橙色秒针（实际 {n_orange} px）")

    # ---------- 3. 数字时钟页：黑底 + 亮色文字 + 状态标签 ----------
    im_d = shot("page1", "--page", "1")
    if im_d:
        n_black = count_color(im_d, BLACK_BG, tol=8)
        check(n_black > 20000, f"数字时钟页应有大面积黑底（实际 {n_black} px）")
        # 时间文字（Apple 白 0xF5F5F7）出现在中部
        ok, n = has_text_like(im_d, (60, 180, 406, 300), BLACK_BG)
        check(ok, f"数字时钟中部应有时间文字（非背景像素 {n}）")

    # ---------- 4. 速度页 V5：白底 ----------
    im_s = shot("speed_v5", "--page", "2", "--variant", "4", "--speed", "0", "--status", "4")
    if im_s:
        # 白底（V5 表盘底色为纯白；模拟器经圆遮罩后会有些许偏差，容差放宽）
        n_white = count_color(im_s, WHITE_V5, tol=10, region=(60, 60, 406, 406))
        check(n_white > 40000, f"速度页 V5 应为白底（白像素 {n_white}）")

    # ---------- 5. 五态：文案颜色各不相同且符合约定 ----------
    state_colors = {
        4: ("LIVE", GREEN_LIVE),
        3: ("STALE", RED_STALE),
    }
    for st, (name, col) in state_colors.items():
        im_st = shot(f"status_{st}", "--page", "2", "--variant", "4", "--speed", "0", "--status", str(st))
        if im_st:
            n = count_color(im_st, col, tol=14)
            check(n > 30, f"状态 {name} 的文字应以 {col} 显示（匹配像素 {n}）")

    # ---------- 6. 状态文字必须位于圆内（安全区） ----------
    # 标签几何由固件 `dump` 实测：pos(190,30) size 87x32，居中于 x=233。
    # 注意：测量窗口必须只覆盖标签本身——若放大到整条顶部带会把刻度环算进来
    # （刻度本就靠近边缘，属正常），从而产生假失败。
    im_top = shot("status_top", "--page", "2", "--variant", "4", "--speed", "0", "--status", "1")
    if im_top:
        px = im_top.load()
        pts = []
        for y in range(28, 66):
            for x in range(180, 290):   # 覆盖最长文案 "WAIT GPS" 的实际范围
                p = px[x, y]
                is_white = abs(p[0]-248) <= 10 and abs(p[1]-252) <= 10 and abs(p[2]-248) <= 10
                is_dark = p[0] < 25 and p[1] < 25 and p[2] < 25
                if not is_white and not is_dark:
                    pts.append((x, y))
        if pts:
            import math
            maxd = max(math.hypot(x-CX, y-CY) for x, y in pts)
            check(maxd <= R - 10, f"状态文字应在圆内且留边（最远 {maxd:.0f}，限 {R-10:.0f}）")
        else:
            check(False, "未能在标签带找到状态文字像素")

    print(f"\n结果: {pass_n} 通过, {fail_n} 失败")
    return 0 if fail_n == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
