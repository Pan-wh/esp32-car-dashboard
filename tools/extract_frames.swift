// 从视频抽帧存成 PNG，供 pgi.py 转 .pgi
// 用法: swift extract_frames.swift <video> <outdir> <帧数> <边长>
import AVFoundation
import AppKit
import Foundation

let args = CommandLine.arguments
guard args.count >= 5 else { print("用法: <video> <outdir> <帧数> <边长>"); exit(1) }
let videoPath = args[1], outDir = args[2]
let wantFrames = Int(args[3]) ?? 35
let size = Int(args[4]) ?? 466

let asset = AVURLAsset(url: URL(fileURLWithPath: videoPath))
let dur = CMTimeGetSeconds(asset.duration)
guard dur.isFinite, dur > 0 else { print("读数失败"); exit(1) }
print("  视频时长 \(String(format: "%.1f", dur)) 秒")

let gen = AVAssetImageGenerator(asset: asset)
gen.appliesPreferredTrackTransform = true
gen.requestedTimeToleranceBefore = .zero
gen.requestedTimeToleranceAfter = .zero
gen.maximumSize = CGSize(width: size * 2, height: size * 2)

try? FileManager.default.createDirectory(atPath: outDir, withIntermediateDirectories: true)

var saved = 0
for i in 0..<wantFrames {
    let t = dur * Double(i) / Double(max(wantFrames - 1, 1))
    let time = CMTime(seconds: min(t, dur - 0.01), preferredTimescale: 600)
    do {
        let cg = try gen.copyCGImage(at: time, actualTime: nil)
        let rep = NSBitmapImageRep(cgImage: cg)
        guard let png = rep.representation(using: .png, properties: [:]) else { continue }
        let path = String(format: "%@/f%03d.png", outDir, i)
        try png.write(to: URL(fileURLWithPath: path))
        saved += 1
    } catch { continue }
}
print("  已抽 \(saved) 帧 → \(outDir)")
