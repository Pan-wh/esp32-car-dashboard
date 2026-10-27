//
//  PhotoEncoder.swift
//  CarDashboardPhone
//
//  素材 → 设备能收的像素数据。
//
//  ## 为什么单独一层
//
//  照片页要做三件不同的事，但都需要"从 CGImage 出 JPEG / 从视频取帧"：
//    ① 单张静态照片：裁切后的 466×466 → **baseline JPEG**（设备用 ROM TJpgDec 解，
//       只认 baseline；progressive 会被拒绝，见 docs/photo-protocol.md §7）；
//    ② 视频取一帧：LITE 档只允许"取一帧作静态照片"，需要按时间点抽帧；
//    ③ FULL 档动图：300×300 / 8fps / ≤12 帧的帧序列，交给 `PgiEncoder` 打包。
//
//  放在独立文件里，UI 只关心"得到 Data / 得到帧"，不掺编码细节。
//

import AVFoundation
import CoreGraphics
import Foundation
import ImageIO
import UniformTypeIdentifiers
#if canImport(UIKit)
import UIKit
#endif

enum PhotoEncoderError: LocalizedError {
    case cannotRenderImage
    case cannotEncodeJPEG
    case cannotReadVideo(String)
    case cannotReadSourceForCrop
    case cannotDecodeImage(String)
    case noFrames

    var errorDescription: String? {
        switch self {
        case .cannotRenderImage: return "这张图片读不出可用的画面"
        case .cannotEncodeJPEG: return "JPEG 编码失败"
        case .cannotReadVideo(let detail): return "这个视频读不了：\(detail)"
        case .cannotReadSourceForCrop: return "这张图片读不出来（可能是 iCloud 还没下载完）"
        case .cannotDecodeImage(let detail): return "图片解码失败：\(detail)"
        case .noFrames: return "没有取到画面"
        }
    }
}

/// 动图素材（视频 / GIF）抽完帧后的结果。
///
/// `fps` 是**实际**使用的帧率：视频按固定间隔抽；GIF 按原始帧间隔推算。
struct PhotoAnimationClip {
    let frames: [CGImage]
    let fps: Int
    /// 来源是视频还是动图（UI 上文案不同：视频说"取前 1.5 秒"，GIF 说"取前 12 帧"）
    let isVideo: Bool
}

enum PhotoEncoder {

    // MARK: - 设备侧的硬参数（与固件 / docs/photo-wireless-design.md 对齐）

    /// 静态照片边长：设备圆屏直径 466
    static let photoSide = 466
    /// 静态照片质量：q80 约 40–60 KB（设备单张上限 512 KB，留足余量）
    static let photoQuality: CGFloat = 0.80
    /// 裁切编辑的源图上边。
    ///
    /// 为什么不直接用原图：手机照片常见 4000×3000（48 MB RGBA），一次选 10 张
    /// 就是几百 MB 内存。裁切只输出 466×466，源图 1200 px 已足够（放大 2.5 倍
    /// 仍清晰），既省内存又不影响成品。
    static let cropSourceMaxPixel = 1200
    /// 动图边长（FULL 档）
    static let animSide = 300
    /// 动图目标帧率（FULL 档，设备播放索引帧）
    static let animFPS = 8
    /// 动图最多帧数（FULL 档；1.5 秒 @8fps）
    static let animMaxFrames = 12

    // MARK: - JPEG

    /// 把任意 CGImage 编成 **baseline** JPEG。
    ///
    /// ImageIO 默认写 baseline（progressive 需要显式打开，而 iOS 没有公开的
    /// "写 progressive" 开关），所以这里只要用 CGImageDestination 就是设备要的格式。
    /// 编码前先铺黑底并去掉 alpha：JPEG 不支持透明通道，直接编码 RGBA 在不同
    /// 图片上行为不一致（有的发白、有的变暗），统一合成到黑底最稳。
    static func jpegData(from cgImage: CGImage, quality: CGFloat = photoQuality) -> Data? {
        let opaque = opaqueImage(from: cgImage) ?? cgImage
        let data = NSMutableData()
        guard let dest = CGImageDestinationCreateWithData(
            data as CFMutableData, UTType.jpeg.identifier as CFString, 1, nil
        ) else {
            return nil
        }
        let options: [CFString: Any] = [
            kCGImageDestinationLossyCompressionQuality: max(0.1, min(quality, 1.0)),
        ]
        CGImageDestinationAddImage(dest, opaque, options as CFDictionary)
        guard CGImageDestinationFinalize(dest) else { return nil }
        return data as Data
    }

    /// 居中方形裁切并缩放到 `side` 后编码 JPEG。
    ///
    /// 相机/GIF 的 CGImage **不带 EXIF 方向**（方向在 UIImage 上），所以这里
    /// 只接受"已经摆正"的 CGImage —— 调用方（`PhotoStudio.frames(from:)`、
    /// `AVAssetImageGenerator.appliesPreferredTrackTransform`）都已经处理过方向。
    static func squareJPEGData(from cgImage: CGImage,
                               side: Int = photoSide,
                               quality: CGFloat = photoQuality) -> Data? {
        guard let square = squareCGImage(cgImage, side: side) else { return nil }
        return jpegData(from: square, quality: quality)
    }

    /// 居中方形裁切（aspect fill）并缩放到 `side`×`side`。
    ///
    /// 设备是圆屏，四角最终会被裁掉，所以默认取**最大内接正方形**而不是拉伸：
    /// 拉伸会让人脸变形，比裁掉左右两边更难接受。
    static func squareCGImage(_ cgImage: CGImage, side: Int) -> CGImage? {
        guard side > 0, cgImage.width > 0, cgImage.height > 0 else { return nil }
        /*
         * ⚠️ 位图必须是 **4 字节/像素**：`noneSkipLast` 表示 RGBX（32 bpp），
         * bytesPerRow 必须 ≥ width*4。曾按 3 字节/像素分配，CGContext 创建直接失败
         * （真机表现：所有照片都报"读不出来"，2026-10-27 用户反馈实锤）。
         */
        var pixels = [UInt8](repeating: 0, count: side * side * 4)
        guard let ctx = CGContext(
            data: &pixels,
            width: side,
            height: side,
            bitsPerComponent: 8,
            bytesPerRow: side * 4,
            space: CGColorSpaceCreateDeviceRGB(),
            bitmapInfo: CGImageAlphaInfo.noneSkipLast.rawValue
        ) else {
            return nil
        }
        ctx.interpolationQuality = .high
        // 黑底：圆屏裁四角后露出的就是黑（AMOLED 黑不发光）
        ctx.setFillColor(CGColor(gray: 0, alpha: 1))
        ctx.fill(CGRect(x: 0, y: 0, width: side, height: side))

        let rect = CropGeometry.aspectFillRect(
            imageSize: CGSize(width: cgImage.width, height: cgImage.height),
            target: CGSize(width: side, height: side)
        )
        ctx.draw(cgImage, in: rect)
        return ctx.makeImage()
    }

    /// 把 CGImage 合成到不透明 RGB 位图（JPEG 不支持 alpha）
    static func opaqueImage(from cgImage: CGImage) -> CGImage? {
        let w = cgImage.width, h = cgImage.height
        guard w > 0, h > 0 else { return nil }
        guard let ctx = CGContext(
            data: nil,
            width: w,
            height: h,
            bitsPerComponent: 8,
            bytesPerRow: 0,
            space: CGColorSpaceCreateDeviceRGB(),
            bitmapInfo: CGImageAlphaInfo.noneSkipLast.rawValue
        ) else {
            return nil
        }
        ctx.setFillColor(CGColor(gray: 0, alpha: 1))
        ctx.fill(CGRect(x: 0, y: 0, width: w, height: h))
        ctx.draw(cgImage, in: CGRect(x: 0, y: 0, width: w, height: h))
        return ctx.makeImage()
    }

    // MARK: - 图片：整帧（供裁切编辑）

    /// 读图片的**完整画面**（按 EXIF 方向摆正，等比缩到 ≤`maxPixelSize`）。
    ///
    /// 裁切编辑必须拿到整图：如果先用 `squareCGImage` 裁成方形再交给裁切页，
    /// 用户就永远看不到被裁掉的左右两侧，无法重新构图。
    /// `kCGImageSourceCreateThumbnailWithTransform` 负责应用方向，
    /// 否则竖拍照片会横过来（历史踩过）。
    static func uprightImage(from data: Data,
                             maxPixelSize: Int = cropSourceMaxPixel) -> CGImage? {
        guard let source = CGImageSourceCreateWithData(data as CFData, nil) else { return nil }
        let options: [CFString: Any] = [
            kCGImageSourceCreateThumbnailFromImageAlways: true,
            kCGImageSourceThumbnailMaxPixelSize: maxPixelSize,
            kCGImageSourceCreateThumbnailWithTransform: true,
        ]
        if let cg = CGImageSourceCreateThumbnailAtIndex(source, 0, options as CFDictionary) {
            return cg
        }
        /*
         * 兜底：个别截图/广色域图（Display P3、HDR）在 ImageIO 缩略图路径上会失败，
         * UIImage 解码同一份数据往往还能成功。这里不缩放，交给 squareCGImage 统一处理。
         */
        #if canImport(UIKit)
        if let image = UIImage(data: data), let cg = image.cgImage { return cg }
        #endif
        return nil
    }

    /// 图片数据里有多少帧（GIF/APNG 等多帧图 >1）。
    ///
    /// 只查数量、不解码：用来判断"这是动图素材" —— 真正抽帧留到 FULL 档生成动图时再做。
    static func imageFrameCount(from data: Data) -> Int {
        guard let source = CGImageSourceCreateWithData(data as CFData, nil) else { return 0 }
        return CGImageSourceGetCount(source)
    }

    // MARK: - 视频：时长 / 取帧

    /// 视频时长（秒）。读不到返回 0，调用方按"未知"处理。
    static func videoDuration(at url: URL) async -> Double {
        let asset = AVURLAsset(url: url)
        guard let duration = try? await asset.load(.duration) else { return 0 }
        let seconds = CMTimeGetSeconds(duration)
        return seconds.isFinite && seconds > 0 ? seconds : 0
    }

    /// 视频在 `seconds` 处取一帧，并居中方形裁切到 `side`。
    ///
    /// LITE 档视频只给"取一帧作静态照片"，默认取**中间帧**（比第一帧更有代表性）。
    static func videoFrame(at url: URL,
                           seconds: TimeInterval,
                           side: Int = photoSide) async throws -> CGImage {
        let asset = AVURLAsset(url: url)
        let duration = await videoDuration(at: url)
        let clamped = duration > 0 ? min(max(seconds, 0), max(0, duration - 0.05)) : 0
        let generator = AVAssetImageGenerator(asset: asset)
        // 竖拍视频必须靠这个把方向摆正，否则上传后横过来（历史踩过）
        generator.appliesPreferredTrackTransform = true
        generator.maximumSize = CGSize(width: side, height: side)
        generator.requestedTimeToleranceBefore = CMTime(seconds: 0.05, preferredTimescale: 600)
        generator.requestedTimeToleranceAfter = CMTime(seconds: 0.05, preferredTimescale: 600)
        do {
            let (cg, _) = try await generator.image(
                at: CMTime(seconds: clamped, preferredTimescale: 600)
            )
            guard let square = squareCGImage(cg, side: side) else {
                throw PhotoEncoderError.cannotRenderImage
            }
            return square
        } catch let error as PhotoEncoderError {
            throw error
        } catch {
            throw PhotoEncoderError.cannotReadVideo(error.localizedDescription)
        }
    }

    /// 视频在 `seconds` 处取**完整一帧**（等比 ≤`maxPixelSize`），供裁切编辑。
    ///
    /// 与 `videoFrame(at:seconds:side:)` 的区别：那个直接给 466 正方形（默认裁切用），
    /// 这个保留整帧，让用户可以在裁切页里重新构图。
    static func rawVideoFrame(at url: URL,
                              seconds: TimeInterval,
                              maxPixelSize: Int = cropSourceMaxPixel) async throws -> CGImage {
        let asset = AVURLAsset(url: url)
        let duration = await videoDuration(at: url)
        let clamped = duration > 0 ? min(max(seconds, 0), max(0, duration - 0.05)) : 0
        let generator = AVAssetImageGenerator(asset: asset)
        generator.appliesPreferredTrackTransform = true
        generator.maximumSize = CGSize(width: maxPixelSize, height: maxPixelSize)
        generator.requestedTimeToleranceBefore = CMTime(seconds: 0.05, preferredTimescale: 600)
        generator.requestedTimeToleranceAfter = CMTime(seconds: 0.05, preferredTimescale: 600)
        do {
            let (cg, _) = try await generator.image(
                at: CMTime(seconds: clamped, preferredTimescale: 600)
            )
            return cg
        } catch {
            throw PhotoEncoderError.cannotReadVideo(error.localizedDescription)
        }
    }

    // MARK: - 动图（FULL 档）

    /// 从视频抽 ≤`maxFrames` 帧，固定 `fps`。
    ///
    /// 只取**开头一段**（12 帧 @8fps = 1.5 秒），不做全片均匀抽帧：
    /// 设备单文件上限 2 MB，全片抽帧会导致帧数/体积不可控；而"选个短视频片段
    /// 当动图"本来就是用户预期。UI 上会明确写"取前 1.5 秒"。
    static func animationClip(fromVideo url: URL,
                              side: Int = animSide,
                              fps: Int = animFPS,
                              maxFrames: Int = animMaxFrames) async throws -> PhotoAnimationClip {
        let safeFPS = max(1, min(fps, 30))
        let count = max(1, min(maxFrames, 120))
        let duration = await videoDuration(at: url)
        guard duration > 0 else { throw PhotoEncoderError.cannotReadVideo("时长为 0") }

        let asset = AVURLAsset(url: url)
        let generator = AVAssetImageGenerator(asset: asset)
        generator.appliesPreferredTrackTransform = true
        generator.maximumSize = CGSize(width: side, height: side)
        generator.requestedTimeToleranceBefore = CMTime(seconds: 0.05, preferredTimescale: 600)
        generator.requestedTimeToleranceAfter = CMTime(seconds: 0.05, preferredTimescale: 600)

        var frames: [CGImage] = []
        frames.reserveCapacity(count)
        for i in 0..<count {
            let t = min(Double(i) / Double(safeFPS), max(0, duration - 0.05))
            // 单帧失败跳过，不整体失败（与 PhotoStudio 视频抽帧同一策略）
            guard let (cg, _) = try? await generator.image(
                at: CMTime(seconds: t, preferredTimescale: 600)
            ) else { continue }
            if let square = squareCGImage(cg, side: side) {
                frames.append(square)
            }
        }
        guard !frames.isEmpty else { throw PhotoEncoderError.noFrames }
        return PhotoAnimationClip(frames: frames, fps: safeFPS, isVideo: true)
    }

    /// 从动图数据（GIF/APNG/WebP）抽 ≤`maxFrames` 帧。
    ///
    /// 复用 `PhotoStudio.frames(from:maxSize:)` 的解码 + 方向处理；
    /// 帧率由原始帧间隔的平均值推算（GIF 的 delay 可能不均匀）。
    static func animationClip(fromImageData data: Data,
                              side: Int = animSide,
                              maxFrames: Int = animMaxFrames) throws -> PhotoAnimationClip {
        let studioFrames = try PhotoStudio.frames(from: data, maxSize: side)
        let picked = Array(studioFrames.prefix(max(1, maxFrames)))
        guard !picked.isEmpty else { throw PhotoEncoderError.noFrames }

        let totalDuration = picked.reduce(0.0) { $0 + max($1.duration, 0.02) }
        let avg = totalDuration / Double(picked.count)
        let fps = max(1, min(Int((1.0 / max(avg, 0.02)).rounded()), 30))
        return PhotoAnimationClip(frames: picked.map(\.image), fps: fps, isVideo: false)
    }

    /// 动图 `.pgi` 的体积估算（UI 在编码前显示"预计 1.1 MB"用）。
    ///
    /// 公式来自 `pgi.h`：32 B 头 + 8 B/帧表 + 每帧（1024 B 调色板 + w×h 索引）。
    static func estimatedAnimatedBytes(side: Int, frames: Int) -> Int {
        let perFrame = 1024 + side * side
        return 32 + frames * 8 + frames * perFrame
    }
}
