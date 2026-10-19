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

    /// 手势变换（缩放 + 平移），与预览上的手势一一对应。
    ///
    /// `panFraction` 是**相对预览边长的比例**（不是像素、也不是点）——
    /// 这样预览尺寸变了、导出尺寸变了，同一个值都能用，**所见即所得**。
    struct CropTransform: Equatable {
        var zoom: CGFloat = 1          // 1...maxZoom
        var panX: CGFloat = 0          // -0.5...0.5（相对边长）
        var panY: CGFloat = 0
        /// 旋转角度（度）。任意角度都支持；UI 上按 90° 步进（捏合旋转也吸附到 90°）
        var rotation: CGFloat = 0

        static let minZoom: CGFloat = 1
        static let maxZoom: CGFloat = 4
        static let identity = CropTransform()
    }

    /// 把图片按 `degrees` 旋转（逆时针为正，与 SwiftUI 的 `.rotationEffect` 同向）。
    ///
    /// ⚠️ 90°/270° 会**交换宽高**，所以旋转后必须用新的 size 去算 aspect-fill 矩形，
    /// 否则会算出错误的裁剪比例。
    static func rotated(_ image: UIImage, degrees: CGFloat) -> UIImage {
        let norm = degrees.truncatingRemainder(dividingBy: 360)
        if abs(norm) < 0.01 { return image }

        let radians = norm * .pi / 180
        let w = image.size.width, h = image.size.height
        let cosA = abs(cos(radians)), sinA = abs(sin(radians))
        let newW = w * cosA + h * sinA
        let newH = w * sinA + h * cosA

        let format = UIGraphicsImageRendererFormat.default()
        format.scale = image.scale
        format.opaque = false

        let renderer = UIGraphicsImageRenderer(size: CGSize(width: newW, height: newH),
                                               format: format)
        return renderer.image { ctx in
            let c = ctx.cgContext
            c.translateBy(x: newW / 2, y: newH / 2)
            c.rotate(by: radians)
            // UIImage.draw 用的是"左上原点、y 向下"，先转回中心再画
            image.draw(in: CGRect(x: -w / 2, y: -h / 2, width: w, height: h))
        }
    }

    /// 居中裁成正方形后缩放到 `size`（无手势）
    static func squareCropped(_ image: UIImage, size: Int) -> UIImage {
        squareCropped(image, size: size, transform: .identity)
    }

    /// 居中裁成正方形、缩放到 `size`，并应用手势变换
    static func squareCropped(_ image: UIImage, size: Int,
                              transform: CropTransform) -> UIImage {
        let target = CGSize(width: size, height: size)
        let format = UIGraphicsImageRendererFormat.default()
        format.scale = 1            // 1x：输出就是 size×size 像素，避免 Retina 放大
        format.opaque = true

        let renderer = UIGraphicsImageRenderer(size: target, format: format)
        return renderer.image { ctx in
            // 黑底：圆屏裁四角后露出的是黑（AMOLED 黑不发光）
            UIColor.black.setFill()
            ctx.fill(CGRect(origin: .zero, size: target))

            // 先旋转（会改宽高），再算 aspect-fill —— 顺序反了会算错比例
            let base = rotated(image, degrees: transform.rotation)
            var rect = CropGeometry.aspectFillRect(imageSize: base.size, target: target)

            /*
             * 手势变换：
             *   - 缩放围绕**画布中心** → 同时放大 rect 并保持中心不动；
             *   - 平移用**相对边长的比例**，乘上目标边长换算成像素。
             * 与预览上的 `.scaleEffect(zoom) + .offset(...)` 是同一套语义。
             */
            let zoom = max(CropTransform.minZoom, min(transform.zoom, CropTransform.maxZoom))
            if zoom != 1 {
                let cx = rect.midX, cy = rect.midY
                rect = CGRect(x: cx - rect.width * zoom / 2,
                              y: cy - rect.height * zoom / 2,
                              width: rect.width * zoom,
                              height: rect.height * zoom)
            }
            rect.origin.x -= transform.panX * target.width
            rect.origin.y -= transform.panY * target.height

            base.draw(in: rect)
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
