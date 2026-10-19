#!/usr/bin/env python3
"""
pgi.py —— 把 PNG / JPG / GIF 转成设备用的 `.pgi`（Photo/Graphic Indexed）

格式定义见 `simulator/ui/pgi.h`（**以那份为准**，这里只是生产者）。

## 用法

    # 单张静态图
    python3 tools/pgi.py photo.jpg -o 01_sunset.pgi

    # 动图（保留帧与帧延时）
    python3 tools/pgi.py cat.gif -o 03_cat.pgi

    # 指定尺寸 / 限制帧数 / 强制 fps
    python3 tools/pgi.py cat.gif -o cat.pgi --size 466 --max-frames 30 --fps 12

    # 只看不写（自检：打印头信息与调色板前几项）
    python3 tools/pgi.py cat.gif --describe

## 设计要点

- **每帧自带调色板**（256 × 4 B，字节序 **B,G,R,A**）—— 与 `img_indexed_decode8`
  期望的布局完全一致，所以设备端零转换逻辑。
- **图像先转 RGBA 再量化**：GIF 的帧可能带透明/局部调色板，直接取原调色板容易出
  色块；统一走 RGBA → `quantize(256)` 更稳。
- 输入不是目标尺寸时，**先居中裁成正方形再缩放**（圆屏最终还会裁掉四角）。
- **不做 RLE**：格式里留了 `flags` bit0，但 v1 不产生也不接受（见 pgi.h）。
"""

import argparse
import struct
import sys

from PIL import Image, ImageSequence

HEADER_BYTES = 32
PALETTE_BYTES = 1024          # 256 * 4
MAGIC = b"PGI1"
FLAG_RLE = 0x01


def build_palette_bytes(pal: Image.Image) -> bytes:
    """
    Pillow 的调色板是 **RGB** 三元组；设备要 **B,G,R,A**（`lv_color32_t` 字段序）。

    不足 256 项的部分补 0；alpha 统一给 0xFF（量化后的 P 模式不带 alpha）。
    """
    raw = pal.getpalette() or []
    out = bytearray(PALETTE_BYTES)
    n = min(256, len(raw) // 3)
    for i in range(n):
        r, g, b = raw[i * 3], raw[i * 3 + 1], raw[i * 3 + 2]
        off = i * 4
        out[off + 0] = b
        out[off + 1] = g
        out[off + 2] = r
        out[off + 3] = 0xFF
    return bytes(out)


def fit_square(im: Image.Image, size: int) -> Image.Image:
    """居中裁成正方形再缩放到 size×size（圆屏最终还会裁四角）。"""
    w, h = im.size
    side = min(w, h)
    left = (w - side) // 2
    top = (h - side) // 2
    im = im.crop((left, top, left + side, top + side))
    if side != size:
        # 缩小时用 LANCZOS；放大时用 BICUBIC（避免过锐）
        resample = Image.LANCZOS if side > size else Image.BICUBIC
        im = im.resize((size, size), resample)
    return im


def collect_frames(path: str, size: int, max_frames: int, force_fps: int | None):
    """
    读出 (RGBA 帧列表, fps)。

    - 多帧（GIF/APNG/WebP 动图）：逐帧读，取各帧 duration 的**中位数**换算 fps；
    - 单帧：fps = 0（静态）。
    """
    im = Image.open(path)
    frames = []
    durations = []

    for i, frame in enumerate(ImageSequence.Iterator(im)):
        if i >= max_frames:
            break
        rgba = frame.convert("RGBA")
        frames.append(fit_square(rgba, size))
        d = frame.info.get("duration", 0)
        if d and d > 0:
            durations.append(d)

    if not frames:
        raise ValueError("没有读到任何帧")

    if len(frames) <= 1:
        fps = 0
    else:
        if force_fps:
            fps = int(force_fps)
        elif durations:
            durations.sort()
            med = durations[len(durations) // 2]
            fps = max(1, min(60, int(round(1000.0 / med)))) if med else 12
        else:
            fps = 12
    return frames, fps


def encode(frames, fps: int) -> bytes:
    """把 RGBA 帧列表编码成 `.pgi` 字节。"""
    n = len(frames)
    w, h = frames[0].size
    frame_bytes = PALETTE_BYTES + w * h
    table_off = HEADER_BYTES
    data_off = table_off + n * 8

    out = bytearray()

    # ── 文件头 32 B ──
    out += MAGIC
    out += struct.pack("<HHHBB", w, h, n, fps, 0)      # w,h,count,fps,flags
    out += struct.pack("<HH", 0, 0)                    # reserved
    out += struct.pack("<II", table_off, data_off)     # 表偏移 / 数据偏移
    out += struct.pack("<II", 0, 0)                    # reserved
    assert len(out) == HEADER_BYTES, f"文件头应为 {HEADER_BYTES} 字节，实际 {len(out)}"

    # ── 帧索引表（先占位，写完数据再回填）──
    table_pos = len(out)
    out += b"\x00" * (n * 8)

    # ── 帧数据 ──
    entries = []
    for idx, rgba in enumerate(frames):
        # 量化到 256 色；GIF 的透明像素量化后会变成某个不透明色，这里先垫白底
        bg = Image.new("RGBA", rgba.size, (0, 0, 0, 255))
        merged = Image.alpha_composite(bg, rgba).convert("RGB")
        p = merged.quantize(colors=256, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)

        off = len(out)
        out += build_palette_bytes(p)
        idx_bytes = p.tobytes()          # P 模式：每像素 1 字节
        assert len(idx_bytes) == w * h, f"帧{idx} 索引数不符：{len(idx_bytes)} != {w*h}"
        out += idx_bytes
        entries.append((off, PALETTE_BYTES + w * h))

    # ── 回填帧索引表 ──
    for i, (off, size) in enumerate(entries):
        struct.pack_into("<II", out, table_pos + i * 8, off, size)

    return bytes(out)


def describe(path: str, blob: bytes) -> None:
    """自检输出：头信息 + 每帧偏移 + 前几个像素的 RGB565（便于和 C 端对照）。"""
    magic = blob[0:4]
    w, h, n, fps, flags = struct.unpack_from("<HHHBB", blob, 4)
    table_off, data_off = struct.unpack_from("<II", blob, 16)
    print(f"文件      : {path}")
    print(f"magic     : {magic!r}（应为 b'PGI1'）")
    print(f"尺寸      : {w}×{h}")
    print(f"帧数      : {n}（{'静态' if n == 1 else '动图'}）")
    print(f"fps       : {fps}")
    print(f"flags     : {flags}（0 = 未压缩）")
    print(f"表偏移    : {table_off}")
    print(f"数据偏移  : {data_off}")
    print(f"总字节    : {len(blob)}")

    for i in range(min(n, 3)):
        off, size = struct.unpack_from("<II", blob, table_off + i * 8)
        print(f"  帧{i}: off={off} size={size}")

    # 前 4 个像素的 RGB565（B,G,R,A 调色板 → v = (r>>3)<<11 | (g>>2)<<5 | (b>>3)）
    off, _ = struct.unpack_from("<II", blob, table_off)
    idx = blob[off + PALETTE_BYTES:off + PALETTE_BYTES + 4]
    px = []
    for k in idx:
        b, g, r = blob[off + k * 4 + 0], blob[off + k * 4 + 1], blob[off + k * 4 + 2]
        v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
        px.append(f"0x{v:04X}")
    print(f"  前4像素(帧0): {' '.join(px)}")


def main() -> int:
    ap = argparse.ArgumentParser(description="PNG/JPG/GIF → .pgi（设备照片表盘格式）")
    ap.add_argument("input", help="输入图片或动图")
    ap.add_argument("-o", "--output", help="输出 .pgi 路径")
    ap.add_argument("--size", type=int, default=466, help="边长（默认 466，即屏幕直径）")
    ap.add_argument("--max-frames", type=int, default=120, help="最多取多少帧（默认 120）")
    ap.add_argument("--fps", type=int, default=None, help="强制按此帧率播放")
    ap.add_argument("--describe", action="store_true", help="只打印头信息，不写文件")
    args = ap.parse_args()

    try:
        frames, fps = collect_frames(args.input, args.size, args.max_frames, args.fps)
        blob = encode(frames, fps)
    except Exception as e:  # noqa: BLE001 —— CLI 工具，打印原因即可
        print(f"失败: {e}", file=sys.stderr)
        return 1

    if args.describe or not args.output:
        describe(args.input, blob)
        if not args.output:
            print("\n（未指定 -o，故未写文件）")
        return 0

    with open(args.output, "wb") as f:
        f.write(blob)
    print(f"已写入 {args.output}：{len(blob)} 字节，{len(frames)} 帧，fps={fps}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
