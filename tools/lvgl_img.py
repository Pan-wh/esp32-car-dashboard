"""
lvgl_img.py —— PIL 图像 → LVGL C 数组的公共导出工具

为什么单独抽出来：表盘底图与驾驶员插画两套生成脚本要用同一套编码规则，
避免"两边各写一份、改一边漏一边"。

## 关于 8 位索引色（LV_IMG_CF_INDEXED_8BIT）

真机 Flash 紧张（应用分区 3 MB），而单张 466×466 的 RGB565 底图就要 434 KB。
这些图是"黑底 + 抗锯齿线条"，颜色分布集中，改用 8bpp 索引色可**减半**。

⚠️ 实测颜色数（**不是**最初以为的"都在 256 以内"）：
    dial_analog   131 色  → 无损
    dial_speed    979 色  → 需量化到 256
    driver_*      563~630 色 → 需量化到 256
因此本工具默认在超限时做**自适应量化**（256 色、不抖动），并报告实际色差。

**数据布局**（依据 LVGL 8.4 源码 `lv_img_decoder.c` / `lv_img_buf.c`）：

    [调色板 256 × 4 字节 = 1024 字节][像素索引 w×h 字节]

要点（都从源码核对过，弄错会偏色或花屏）：
  1. `src_type == LV_IMG_SRC_VARIABLE`（C 数组）时**没有**额外的 4 字节计数头；
     文件源才有（`lv_fs_seek(..., ofs + 4, ...)` 的注释 "skip the header"）。
  2. 调色板**恒为 256 项**，不足部分补零。
  3. 调色板每项字节序是 **B, G, R, A**（`lv_color32_t` 的字段顺序），
     LVGL 侧执行 `lv_color_make(ch.red, ch.green, ch.blue)`。

**为什么能做到逐像素无损**：调色板里存的是**原始 RGB888**，
LVGL 取用时按 `LV_COLOR_DEPTH 16` 截断成 RGB565——
与原先直接写 RGB565 的取值完全一致。因此只要某张图的
**不同 RGB565 值 ≤ 256**，转换后渲染结果与原来**逐像素相同**。
"""
from __future__ import annotations

import sys

from PIL import Image

MAX_COLORS = 256


def rgb565(r: int, g: int, b: int) -> int:
    """与 LVGL LV_COLOR_DEPTH=16 一致的 RGB888 → RGB565 取值。"""
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def _distinct_rgb565(img) -> int:
    """统计图像中不同的 RGB565 取值个数。"""
    px = img.load()
    w, h = img.size
    # 注意：这里**完整统计**，不要"超限就提前返回" ——
    # 提前返回会把日志写成"257 色"（其实有 979 色），误导判断。
    seen = set()
    for y in range(h):
        for x in range(w):
            seen.add(rgb565(*px[x, y]))
    return len(seen)


def quantize_if_needed(img, name: str):
    """颜色数超限时量化到 256 色，返回 (图像, 是否量化过)。

    量化策略：PIL 自适应调色板、**不抖动**。
    不抖动是因为这些图是线条/表盘类图形，抖动的颗粒感比轻微色带更显眼。
    """
    n = _distinct_rgb565(img)
    if n <= MAX_COLORS:
        return img, False

    q = img.convert("RGB").quantize(colors=MAX_COLORS, method=Image.MEDIANCUT,
                                    dither=Image.Dither.NONE).convert("RGB")
    print(f"  [量化] {name}: {n} 色 → ≤{MAX_COLORS} 色（自适应调色板，不抖动）")
    return q, True


def pixel_diff(before, after):
    """返回 (不同像素数, 最大通道偏差)，用于评估量化损失。"""
    a, b = before.convert("RGB"), after.convert("RGB")
    pa, pb = a.load(), b.load()
    w, h = a.size
    ndiff = 0
    maxd = 0
    for y in range(h):
        for x in range(w):
            ra, ga, ba = pa[x, y]
            rb, gb, bb = pb[x, y]
            d = max(abs(ra - rb), abs(ga - gb), abs(ba - bb))
            if d:
                ndiff += 1
                if d > maxd:
                    maxd = d
    return ndiff, maxd


def _encode_indexed8(img, name: str):
    """返回 (调色板字节列表 1024 项, 索引字节列表, 颜色数)。"""
    w, h = img.size
    px = img.load()

    order = []      # 按首次出现顺序记录 (r,g,b)
    index_of = {}   # rgb565 → 调色板下标
    indices = bytearray(w * h)

    i = 0
    for y in range(h):
        for x in range(w):
            r, g, b = px[x, y]
            v = rgb565(r, g, b)
            idx = index_of.get(v)
            if idx is None:
                if len(order) >= MAX_COLORS:
                    raise ValueError(
                        f"{name}: 不同 RGB565 颜色数超过 {MAX_COLORS}，"
                        f"无法用 8bpp 索引色无损表示（请改用 4bpp+量化或保留 RGB565）"
                    )
                idx = len(order)
                index_of[v] = idx
                order.append((r, g, b))
            indices[i] = idx
            i += 1

    # 调色板：B, G, R, A；未用项补 0（LVGL 固定读 256 项）
    palette = bytearray()
    for r, g, b in order:
        palette += bytes((b & 0xFF, g & 0xFF, r & 0xFF, 0xFF))
    palette += bytes((256 - len(order)) * 4)

    return list(palette), list(indices), len(order)


def _fmt_bytes(data, per_line: int = 24, indent: str = "    ") -> str:
    out = []
    for i in range(0, len(data), per_line):
        out.append(indent + ",".join(str(b) for b in data[i:i + per_line]) + ",")
    return "\n".join(out)


def img_to_lvgl_indexed8(img, name: str, note: str = "") -> str:
    """PIL 图像 → LVGL 8bpp 索引色 C 源码。

    :param note: 附加到注释里的一句说明（例如来源 PNG 名）。
    """
    w, h = img.size
    q, quantized = quantize_if_needed(img, name)
    if quantized:
        ndiff, maxd = pixel_diff(img, q)
        total = w * h
        print(f"  [色差] {name}: {ndiff}/{total} 像素变化 "
              f"（{ndiff * 100.0 / total:.2f}%），最大通道偏差 {maxd}/255")
    palette, indices, ncolors = _encode_indexed8(q, name)
    data_size = len(palette) + len(indices)

    extra = f"，{note}" if note else ""
    return f"""
/* {name}：{w}×{h} 8bpp 索引色（LV_IMG_CF_INDEXED_8BIT），{ncolors} 色{extra}
 *
 * 数据布局：[调色板 256×4 字节 (B,G,R,A)][像素索引 {w}×{h}]
 * 相比 RGB565 省下一半 Flash（{w*h*2} → {data_size} 字节）。
 * 由 tools/ 下的生成脚本产出，请勿手改。 */
static const uint8_t {name}_map[] = {{
{_fmt_bytes(palette)}
{_fmt_bytes(indices)}
}};

/* 非 const：真机启动时会把它"充气"成 PSRAM 中的 RGB565 直通图
 * （索引图每帧都要查表解码，滑页会慢约 45ms；见 simulator/ui/ui_img_inflate.c）*/
lv_img_dsc_t {name} = {{
    .header.cf = LV_IMG_CF_INDEXED_8BIT,
    .header.always_zero = 0,
    .header.reserved = 0,
    .header.w = {w},
    .header.h = {h},
    .data_size = {data_size},
    .data = {name}_map,
}};
"""


def report(name: str, w: int, h: int, ncolors: int) -> None:
    """打印一行体积对比，便于在生成时直观看收益。"""
    before = w * h * 2
    after = 1024 + w * h
    print(f"  {name}: {w}×{h}, {ncolors} 色, "
          f"{before/1024:.0f} KB → {after/1024:.0f} KB（省 {(before-after)/1024:.0f} KB）")


def write_atomic(path: str, text: str) -> None:
    """原子写出：先写临时文件再改名。

    为什么需要：生成脚本先前是直接以 "w" 打开输出文件，
    一旦中途抛异常（例如颜色数超限），**输出文件会被截断成半个**，
    仓库里的生成物就坏了（已实际踩到一次）。原子写可避免。
    """
    import os
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        f.write(text)
    os.replace(tmp, path)
