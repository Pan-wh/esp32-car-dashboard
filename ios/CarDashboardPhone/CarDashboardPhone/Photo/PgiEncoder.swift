//
//  PgiEncoder.swift
//  CarDashboardPhone
//
//  把帧编码成设备用的 `.pgi`（格式见设备端 `simulator/ui/pgi.h`）。
//
//  ## 为什么不自己写调色板量化
//
//  256 色量化的好坏直接决定画质，自己写 median-cut 既长又难调。
//  这里**借用现成的 GIF 编码器**：先把帧写成动量 GIF，再逐帧读回来 ——
//  读回来的 `CGImage` 是**索引色**的（`CGImageGetColorTable` 能拿到调色板），
//  于是"量化 + 调色板"两件事都由系统做完了，我们只负责重排字节。
//
//  如果某些帧读回来不是索引色（极少数解码路径），退化为**固定的 3-3-2 调色板**
//  —— 256 色、画质一般但永远可用，保证不会因为量化问题整体失败。
//

import CoreGraphics
import Foundation
import ImageIO
import UIKit
import UniformTypeIdentifiers

enum PgiEncoderError: LocalizedError {
    case noFrames
    case encodeFailed(String)
    case tooLarge(Int)

    var errorDescription: String? {
        switch self {
        case .noFrames: return "没有可编码的帧"
        case .encodeFailed(let s): return "编码失败：\(s)"
        case .tooLarge(let n): return "生成的 .pgi 太大（\(n / 1024 / 1024) MB，上限 8 MB）"
        }
    }
}

enum PgiEncoder {

    /// 设备端硬上限（见 docs/photo-gif-design.md §七）
    static let maxBytes = 8 * 1024 * 1024
    static let paletteBytes = 1024      // 256 × 4
    static let headerBytes = 32

    /// 一帧的原始字节（调色板 + 索引）
    private struct IndexedFrame {
        let w: Int
        let h: Int
        let paletteBGRA: [UInt8]   // 恰好 1024 字节
        let indices: [UInt8]       // w*h
    }

    /// 给定尺寸下，最多能放多少帧才不超过设备上限
    static func maxFramesFit(w: Int, h: Int) -> Int {
        let per = paletteBytes + w * h
        let overhead = headerBytes + 8
        let n = (maxBytes - overhead) / per
        return max(1, min(n, 512))
    }

    /// 把帧编码成 `.pgi`（**自动裁剪到设备上限以内**）
    /// - Parameter fps: 动图帧率；单帧传 0（静态）
    static func encode(frames allFrames: [CGImage], fps: Int) throws -> Data {
        guard !allFrames.isEmpty else { throw PgiEncoderError.noFrames }
        let w = allFrames[0].width
        let h = allFrames[0].height

        /*
         * ⚠️ **按体积上限自动裁帧**。
         *
         * 原来不裁：120 帧 × 466×466 ≈ 26 MB，远超设备 8 MB 上限 ——
         * 用户就会看到"动不动就超大"。现在按 `maxFramesFit` 截断，
         * 并**均匀抽帧**（而不是只留前 N 帧，否则等于只取了视频开头一段）。
         */
        let fit = maxFramesFit(w: w, h: h)
        let frames: [CGImage]
        if allFrames.count > fit {
            var picked: [CGImage] = []
            picked.reserveCapacity(fit)
            for i in 0..<fit {
                let src = Int(Double(i) * Double(allFrames.count - 1) / Double(max(fit - 1, 1)))
                picked.append(allFrames[min(src, allFrames.count - 1)])
            }
            frames = picked
        } else {
            frames = allFrames
        }
        guard w > 0, h > 0, w <= 1024, h <= 1024 else {
            throw PgiEncoderError.encodeFailed("尺寸非法：\(w)×\(h)")
        }
        // 尺寸必须一致
        for (i, f) in frames.enumerated() where f.width != w || f.height != h {
            throw PgiEncoderError.encodeFailed("第 \(i) 帧尺寸不一致")
        }

        // ① 借 GIF 做量化
        let quantized = try quantizeViaGIF(frames: frames)

        // ② 组装
        let n = quantized.count
        let frameBytes = paletteBytes + w * h
        let tableOff = headerBytes
        let dataOff = tableOff + n * 8
        let total = dataOff + frameBytes * n
        if total > maxBytes { throw PgiEncoderError.tooLarge(total) }

        var out = Data(capacity: total)
        // ── 头 32 B ──
        out.append(contentsOf: Array("PGI1".utf8))
        appendU16(&out, UInt16(w))
        appendU16(&out, UInt16(h))
        appendU16(&out, UInt16(n))
        out.append(UInt8(max(0, min(fps, 60))))
        out.append(0)                       // flags：无 RLE
        appendU16(&out, 0)
        appendU16(&out, 0)
        appendU32(&out, UInt32(tableOff))
        appendU32(&out, UInt32(dataOff))
        appendU32(&out, 0)
        appendU32(&out, 0)
        assert(out.count == headerBytes, "文件头必须是 32 字节")

        // ── 帧索引表（先占位）──
        let tableStart = out.count
        out.append(contentsOf: [UInt8](repeating: 0, count: n * 8))

        // ── 帧数据 ──
        var entries: [(Int, Int)] = []
        for f in quantized {
            let off = out.count
            out.append(contentsOf: f.paletteBGRA)
            out.append(contentsOf: f.indices)
            entries.append((off, frameBytes))
        }

        // ── 回填 ──
        for (i, e) in entries.enumerated() {
            let p = tableStart + i * 8
            writeU32(&out, at: p, UInt32(e.0))
            writeU32(&out, at: p + 4, UInt32(e.1))
        }
        return out
    }

    // MARK: - 量化（借 GIF）

    /// 先把帧写成 GIF，再读回索引色帧
    private static func quantizeViaGIF(frames: [CGImage]) throws -> [IndexedFrame] {
        // 写 GIF（用 ImageIO，不落盘）
        let gif = NSMutableData()
        guard let dest = CGImageDestinationCreateWithData(
            gif as CFMutableData, UTType.gif.identifier as CFString, frames.count, nil
        ) else {
            throw PgiEncoderError.encodeFailed("创建 GIF 目标失败")
        }
        CGImageDestinationSetProperties(dest, [
            kCGImagePropertyGIFDictionary: [kCGImagePropertyGIFLoopCount: 0]
        ] as CFDictionary)
        for f in frames {
            CGImageDestinationAddImage(dest, f, [
                kCGImagePropertyGIFDictionary: [kCGImagePropertyGIFDelayTime: 0.1]
            ] as CFDictionary)
        }
        guard CGImageDestinationFinalize(dest) else {
            throw PgiEncoderError.encodeFailed("GIF 编码失败")
        }

        // 读回（索引色）
        guard let src = CGImageSourceCreateWithData(gif as CFData, nil) else {
            throw PgiEncoderError.encodeFailed("读回 GIF 失败")
        }
        let count = CGImageSourceGetCount(src)
        guard count > 0 else { throw PgiEncoderError.noFrames }

        var out: [IndexedFrame] = []
        out.reserveCapacity(count)
        for i in 0..<count {
            guard let cg = CGImageSourceCreateImageAtIndex(src, i, nil) else { continue }
            out.append(indexed(from: cg))
        }
        guard !out.isEmpty else { throw PgiEncoderError.noFrames }
        return out
    }

    /// 把 CGImage 拆成 调色板 + 索引（**5-5-5 直方图选色**）
    ///
    /// 为什么不用 `CGImageGetColorTable`：那个 API 在 iOS 上**不可用**（编译不过）。
    /// 也不写完整的 median-cut（太长）。这里用更朴素但对照片够用的办法：
    ///   - RGB 各取高 5 位 → 32768 个桶，统计出现次数；
    ///   - 取出现最多的前 256 个桶作调色板；
    ///   - 每个像素映射到最近的调色板项（用桶中心代表）。
    ///
    /// 比固定 3-3-2（只有 8×8×4）好很多；代价是同帧内不保证最优。
    private static func indexed(from cg: CGImage) -> IndexedFrame {
        let w = cg.width, h = cg.height
        let px = w * h

        // ── 取 RGBA ──
        var rgba = [UInt8](repeating: 0, count: px * 4)
        guard let ctx = CGContext(
            data: &rgba, width: w, height: h, bitsPerComponent: 8, bytesPerRow: w * 4,
            space: CGColorSpaceCreateDeviceRGB(),
            bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue
        ) else {
            return fallback332(cg, w: w, h: h)
        }
        ctx.draw(cg, in: CGRect(x: 0, y: 0, width: w, height: h))

        // ── 直方图（5-5-5）──
        var hist = [UInt32](repeating: 0, count: 32768)
        for i in 0..<px {
            let r = Int(rgba[i * 4]) >> 3
            let g = Int(rgba[i * 4 + 1]) >> 3
            let b = Int(rgba[i * 4 + 2]) >> 3
            hist[(r << 10) | (g << 5) | b] &+= 1
        }

        // ── 取前 256 个桶 ──
        var order = Array(0..<32768)
        order.sort { hist[$0] > hist[$1] }      // 32768 个元素排序，几毫秒
        let used = min(256, order.reduce(0) { $0 + (hist[$1] > 0 ? 1 : 0) })
        let keep = max(1, min(256, used))

        var pal = [UInt8](repeating: 0, count: paletteBytes)
        /*
         * 桶 → 调色板下标。
         *
         * ⚠️ **没入选的桶必须回退到"最近的已选颜色"，不能用 0。**
         * 原来 `lut` 初值全 0，于是所有没入选的桶都被映射到调色板第 0 项
         * （出现最多的那个颜色）—— 大片像素被压成单调色，**看起来就像黑白/偏色**。
         *
         * 这里改成：先记下"已选桶 → 调色板下标"，再对每个未选桶线性搜一个最近的已选桶
         * （32768 个桶 × 最多 256 个已选，只在编码时算一次，代价可接受）。
         */
        var chosen: [(bin: Int, pal: Int)] = []
        chosen.reserveCapacity(keep)
        for i in 0..<keep {
            let bin = order[i]
            let r5 = (bin >> 10) & 0x1F
            let g5 = (bin >> 5) & 0x1F
            let b5 = bin & 0x1F
            // 桶中心：5 位扩到 8 位
            let r = UInt8((r5 << 3) | (r5 >> 2))
            let g = UInt8((g5 << 3) | (g5 >> 2))
            let b = UInt8((b5 << 3) | (b5 >> 2))
            // 设备端调色板字节序 B,G,R,A
            pal[i * 4 + 0] = b
            pal[i * 4 + 1] = g
            pal[i * 4 + 2] = r
            pal[i * 4 + 3] = 0xFF
            chosen.append((bin, i))
        }

        /* 未入选的桶 → 找最近的已选桶（按 5-5-5 三通道差值平方和）*/
        var lut = [UInt8](repeating: 0, count: 32768)
        for bin in 0..<32768 {
            let r5 = (bin >> 10) & 0x1F, g5 = (bin >> 5) & 0x1F, b5 = bin & 0x1F
            var best = 0, bestD = Int.max
            for c in chosen {
                let cr = (c.bin >> 10) & 0x1F, cg = (c.bin >> 5) & 0x1F, cb = c.bin & 0x1F
                let d = (r5 - cr) * (r5 - cr) + (g5 - cg) * (g5 - cg) + (b5 - cb) * (b5 - cb)
                if d < bestD { bestD = d; best = c.pal }
                if bestD == 0 { break }
            }
            lut[bin] = UInt8(best)
        }

        // ── 映射像素 + **Floyd–Steinberg 抖动** ──
        //
        // 只做"最近桶"映射的话，256 色会让渐变出现明显色带（用户实测"画质有点差"）。
        // 抖动把量化误差扩散到邻近像素，用空间混色换回观感 —— 对照片提升非常明显，
        // 代价只是几十行。
        var idx = [UInt8](repeating: 0, count: px)
        // 用 Int 保存误差（可正可负），按行滚动
        var cur = [Int](repeating: 0, count: (w + 2) * 3)
        var nxt = [Int](repeating: 0, count: (w + 2) * 3)
        for y in 0..<h {
            for x in 0..<w {
                let i = y * w + x
                let o = (x + 1) * 3
                let r = min(255, max(0, Int(rgba[i * 4]) + cur[o] / 16))
                let g = min(255, max(0, Int(rgba[i * 4 + 1]) + cur[o + 1] / 16))
                let b = min(255, max(0, Int(rgba[i * 4 + 2]) + cur[o + 2] / 16))

                let bin = ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3)
                let pi = Int(lut[bin])
                idx[i] = UInt8(pi)

                // 该调色板项的实际颜色（桶中心的 8 位值）
                let pr = Int(pal[pi * 4 + 2]), pg2 = Int(pal[pi * 4 + 1]), pb = Int(pal[pi * 4])
                let er = r - pr, eg = g - pg2, eb = b - pb

                // 误差扩散：右 7/16、左下 3/16、下 5/16、右下 1/16
                cur[o + 3]     += er * 7; cur[o + 4]     += eg * 7; cur[o + 5]     += eb * 7
                nxt[o - 3]     += er * 3; nxt[o - 2]     += eg * 3; nxt[o - 1]     += eb * 3
                nxt[o]         += er * 5; nxt[o + 1]     += eg * 5; nxt[o + 2]     += eb * 5
                nxt[o + 3]     += er;     nxt[o + 4]     += eg;     nxt[o + 5]     += eb
            }
            cur = nxt
            nxt = [Int](repeating: 0, count: (w + 2) * 3)
        }
        return IndexedFrame(w: w, h: h, paletteBGRA: pal, indices: idx)
    }

    /// 极端退路：固定 3-3-2（256 色）。只在 RGBA 转换失败时用。
    private static func fallback332(_ cg: CGImage, w: Int, h: Int) -> IndexedFrame {
        var pal = [UInt8](repeating: 0, count: paletteBytes)
        for i in 0..<256 {
            let r = (i >> 5) & 0x07
            let g = (i >> 2) & 0x07
            let b = i & 0x03
            pal[i * 4 + 0] = UInt8(b * 85)
            pal[i * 4 + 1] = UInt8(g * 36)
            pal[i * 4 + 2] = UInt8(r * 36)
            pal[i * 4 + 3] = 0xFF
        }
        let idx = [UInt8](repeating: 0, count: max(1, w * h))
        return IndexedFrame(w: w, h: h, paletteBGRA: pal, indices: idx)
    }

    // MARK: - 小工具

    private static func appendU16(_ d: inout Data, _ v: UInt16) {
        d.append(UInt8(v & 0xFF))
        d.append(UInt8(v >> 8))
    }

    private static func appendU32(_ d: inout Data, _ v: UInt32) {
        for i in 0..<4 { d.append(UInt8((v >> (8 * i)) & 0xFF)) }
    }

    private static func writeU32(_ d: inout Data, at offset: Int, _ v: UInt32) {
        for i in 0..<4 { d[offset + i] = UInt8((v >> (8 * i)) & 0xFF) }
    }
}
