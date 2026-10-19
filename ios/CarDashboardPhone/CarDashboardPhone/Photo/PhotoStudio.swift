//
//  PhotoStudio.swift
//  CarDashboardPhone
//
//  照片表盘的素材准备：裁切、预览、动图/视频 → GIF。
//
//  ## 为什么全用系统框架
//
//  这一整套在 iOS 上都是内置能力，**不需要任何第三方库**：
//    - 选图/选视频：`PhotosPicker`（SwiftUI，无需权限弹窗）
//    - 裁切/预览：  `UIGraphicsImageRenderer`（Core Graphics）
//    - 读动图：     `CGImageSource`（GIF / APNG / WebP / Live Photo）
//    - 写 GIF：     `CGImageDestination`（ImageIO）
//    - 视频抽帧：   `AVAssetImageGenerator`（AVFoundation）
//
//  ## 与设备的关系
//
//  设备最终要的是 `.pgi`（见 docs/photo-gif-design.md），但**本阶段只导出 GIF**
//  —— 先让用户看到效果（导出到相册即可播放）。
//  `.pgi` 的打包与 Wi-Fi 上传在 P5 接入设备时再做。
//

import AVFoundation
import CoreGraphics
import Foundation
import ImageIO
import Photos
import UIKit
import UniformTypeIdentifiers

/// 圆屏裁切的几何（纯计算，可单测）
enum CropGeometry {

    /// 把 `imageSize` 按"铺满 + 居中"缩放到 `target` 见方，返回绘制矩形。
    ///
    /// 语义是 **aspect fill**（不留黑边）：短边铺满，长边裁掉。
    static func aspectFillRect(imageSize: CGSize, target: CGSize) -> CGRect {
        guard imageSize.width > 0, imageSize.height > 0,
              target.width > 0, target.height > 0 else {
            return .zero
        }
        let scale = max(target.width / imageSize.width, target.height / imageSize.height)
        let w = imageSize.width * scale
        let h = imageSize.height * scale
        // 居中：多出来的部分两边各裁一半（负 origin 即"超出画布"，由 CG 裁掉）
        return CGRect(x: (target.width - w) / 2, y: (target.height - h) / 2, width: w, height: h)
    }

    /// 目标边长（设备屏幕直径）
    static let deviceSize: CGFloat = 466
}

/// 一张待导出的帧（统一成 RGBA 位图，避免每帧各自解码）
struct StudioFrame {
    let image: CGImage
    /// 该帧显示多久（秒）；单帧图为 0
    let duration: TimeInterval
}

enum PhotoStudioError: LocalizedError {
    case noFrames
    case cannotReadSource
    case cannotEncodeGIF
    case videoTooLong(maxSeconds: Int)

    var errorDescription: String? {
        switch self {
        case .noFrames: return "没有可导出的画面"
        case .cannotReadSource: return "读不了这个文件（格式不支持？）"
        case .cannotEncodeGIF: return "GIF 编码失败"
        case .videoTooLong(let s): return "视频太长（上限 \(s) 秒），请先剪短"
        }
    }
}

enum PhotoStudio {

    /// 设备端建议的上限（见 docs/photo-gif-design.md §七：单文件 ≤ 8MB）
    static let maxVideoSeconds = 30
    static let maxFrames = 120

    // MARK: - 裁切

    /// 居中裁成正方形后缩放到 `size`
    static func squareCropped(_ image: UIImage, size: Int) -> UIImage {
        let target = CGSize(width: size, height: size)
        let format = UIGraphicsImageRendererFormat.default()
        format.scale = 1            // 1x：输出就是 size×size 像素，避免 Retina 放大
        format.opaque = true

        let renderer = UIGraphicsImageRenderer(size: target, format: format)
        return renderer.image { ctx in
            // 黑底：圆屏裁四角后露出的是黑（AMOLED 黑不发光）
            UIColor.black.setFill()
            ctx.fill(CGRect(origin: .zero, size: target))

            let rect = CropGeometry.aspectFillRect(imageSize: image.size, target: target)
            image.draw(in: rect)
        }
    }

    // MARK: - 读动图（GIF / APNG / WebP / Live Photo 的静态部分）

    /// 从图片数据逐帧读出（保持每帧的时长）
    static func frames(from data: Data, maxSize: Int) throws -> [StudioFrame] {
        guard let src = CGImageSourceCreateWithData(data as CFData, nil) else {
            throw PhotoStudioError.cannotReadSource
        }
        let count = CGImageSourceGetCount(src)
        guard count > 0 else { throw PhotoStudioError.noFrames }

        var out: [StudioFrame] = []
        out.reserveCapacity(min(count, maxFrames))
        for i in 0..<min(count, maxFrames) {
            guard let cg = CGImageSourceCreateImageAtIndex(src, i, nil) else { continue }
            let props = CGImageSourceCopyPropertiesAtIndex(src, i, nil) as? [CFString: Any]
            let gif = props?[kCGImagePropertyGIFDictionary] as? [CFString: Any]
            let delay = (gif?[kCGImagePropertyGIFUnclampedDelayTime] as? Double)
                ?? (gif?[kCGImagePropertyGIFDelayTime] as? Double)
                ?? 0.1
            let ui = UIImage(cgImage: cg)
            let scaled = squareCropped(ui, size: maxSize)
            guard let scaledCG = scaled.cgImage else { continue }
            out.append(StudioFrame(image: scaledCG, duration: max(delay, 0.02)))
        }
        guard !out.isEmpty else { throw PhotoStudioError.noFrames }
        return out
    }

    // MARK: - 视频抽帧

    /// 把视频按 `fps` 抽帧（最多 `maxFrames` 帧、`maxVideoSeconds` 秒）
    static func frames(fromVideo url: URL, size: Int, fps: Double) async throws -> [StudioFrame] {
        let asset = AVURLAsset(url: url)
        let durationS = CMTimeGetSeconds(asset.duration)
        guard durationS.isFinite, durationS > 0 else { throw PhotoStudioError.cannotReadSource }
        guard durationS <= Double(maxVideoSeconds) else {
            throw PhotoStudioError.videoTooLong(maxSeconds: maxVideoSeconds)
        }

        let gen = AVAssetImageGenerator(asset: asset)
        gen.appliesPreferredTrackTransform = true
        gen.requestedTimeToleranceBefore = .zero
        gen.requestedTimeToleranceAfter = .zero
        // 直接把目标尺寸告诉生成器：省一次缩放，也避免解出 4K 原图
        gen.maximumSize = CGSize(width: size, height: size)

        let step = 1.0 / max(fps, 1)
        var times: [NSValue] = []
        var t = 0.0
        while t < durationS && times.count < maxFrames {
            times.append(NSValue(time: CMTime(seconds: t, preferredTimescale: 600)))
            t += step
        }
        guard !times.isEmpty else { throw PhotoStudioError.noFrames }

        var out: [StudioFrame] = []
        out.reserveCapacity(times.count)
        for tv in times {
            do {
                let cg = try await gen.image(at: tv.timeValue).image
                let ui = UIImage(cgImage: cg)
                let scaled = squareCropped(ui, size: size)
                guard let scaledCG = scaled.cgImage else { continue }
                out.append(StudioFrame(image: scaledCG, duration: step))
            } catch {
                continue   // 单帧失败跳过，不整体失败
            }
        }
        guard !out.isEmpty else { throw PhotoStudioError.noFrames }
        return out
    }

    // MARK: - 写 GIF

    /// 把帧编码成 GIF 数据
    static func gifData(from frames: [StudioFrame], loop: Bool = true) throws -> Data {
        guard !frames.isEmpty else { throw PhotoStudioError.noFrames }

        let data = NSMutableData()
        guard let dest = CGImageDestinationCreateWithData(
            data as CFMutableData, UTType.gif.identifier as CFString, frames.count, nil
        ) else {
            throw PhotoStudioError.cannotEncodeGIF
        }

        let fileProps: [CFString: Any] = [
            kCGImagePropertyGIFDictionary: [
                kCGImagePropertyGIFLoopCount: loop ? 0 : 1
            ]
        ]
        CGImageDestinationSetProperties(dest, fileProps as CFDictionary)

        for f in frames {
            let frameProps: [CFString: Any] = [
                kCGImagePropertyGIFDictionary: [
                    kCGImagePropertyGIFDelayTime: max(f.duration, 0.02)
                ]
            ]
            CGImageDestinationAddImage(dest, f.image, frameProps as CFDictionary)
        }

        guard CGImageDestinationFinalize(dest) else {
            throw PhotoStudioError.cannotEncodeGIF
        }
        return data as Data
    }

    /// 存到系统相册（会触发一次权限弹窗，首次）
    static func saveToPhotoLibrary(_ data: Data) async throws {
        try await PHPhotoLibrarySaver.save(data)
    }
}

/// 把"保存到相册"单独包一层，方便以后替换实现
private enum PHPhotoLibrarySaver {
    static func save(_ data: Data) async throws {
        try await withCheckedThrowingContinuation { (cont: CheckedContinuation<Void, Error>) in
            PHPhotoLibrary.shared().performChanges {
                let req = PHAssetCreationRequest.forAsset()
                req.addResource(with: .photo, data: data, options: nil)
            } completionHandler: { ok, err in
                if ok { cont.resume() } else { cont.resume(throwing: err ?? PhotoStudioError.cannotEncodeGIF) }
            }
        }
    }
}
