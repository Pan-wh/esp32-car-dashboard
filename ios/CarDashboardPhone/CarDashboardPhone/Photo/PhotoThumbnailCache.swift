//
//  PhotoThumbnailCache.swift
//  CarDashboardPhone
//
//  设备照片列表的缩略图缓存（96×96 JPEG）。
//
//  ## 为什么要在 App 本地存缩略图
//
//  设备只上报清单（文件名/大小/帧数/尺寸），**不传画面** —— 照片本体可能有
//  几 MB，为了列表每行 96 pt 的图去拉一次原图不值得（而且协议里也没有这个通道）。
//  所以缩略图由 App 在上传/编码时顺手生成，按**设备照片 id** 存沙盒：
//    ① 上传完成后 `store(...)`；
//    ② 列表按 id `thumbnail(for:)` 读取；
//    ③ 读不到（比如这张是拔卡前从电脑拷进去的、或重装过 App）→ 显示占位图标。
//
//  存 Application Support（不是 Caches）：缩略图丢了列表就变占位，
//  而它只有几 KB，值得跟着 App 一起备份/迁移。系统清理 Caches 也不会影响它。
//

import Foundation
import UIKit

final class PhotoThumbnailCache {

    static let shared = PhotoThumbnailCache()

    /// 缩略图边长（像素）。列表行高约 44–56 pt，@3x 也就 168 px，
    /// 96 px 已经够清楚，又能把单张压到 2–4 KB。
    static let side = 96

    private let memory = NSCache<NSString, UIImage>()
    private let directory: URL
    /// 串行队列：内存缓存 + 磁盘读写都走它，避免"写入后立刻读"读到旧文件。
    private let queue = DispatchQueue(label: "com.cardashboard.photo.thumbnails", qos: .utility)

    private init() {
        let base = FileManager.default.urls(for: .applicationSupportDirectory,
                                           in: .userDomainMask).first
            ?? FileManager.default.temporaryDirectory
        directory = base.appendingPathComponent("PhotoThumbnails", isDirectory: true)
        memory.countLimit = 200
        try? FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    }

    // MARK: - 读

    /// 取某个设备照片的缩略图；没有返回 nil（UI 用占位图标）。
    func thumbnail(for id: String) -> UIImage? {
        let key = id as NSString
        if let cached = memory.object(forKey: key) { return cached }

        var image: UIImage?
        queue.sync {
            guard let data = try? Data(contentsOf: fileURL(for: id)),
                  let decoded = UIImage(data: data) else { return }
            image = decoded
        }
        if let image { memory.setObject(image, forKey: key) }
        return image
    }

    /// 是否存在（调试/清理用）
    func hasThumbnail(for id: String) -> Bool {
        queue.sync { FileManager.default.fileExists(atPath: fileURL(for: id).path) }
    }

    // MARK: - 写

    /// 从裁切后的 CGImage 生成并保存缩略图。
    func store(_ cgImage: CGImage, for id: String) {
        guard let data = PhotoEncoder.squareJPEGData(from: cgImage,
                                                     side: Self.side,
                                                     quality: 0.7) else { return }
        store(jpegData: data, for: id)
    }

    /// 从 UIImage 保存（视频取帧等已经转成 UIImage 的路径）
    func store(_ image: UIImage, for id: String) {
        guard let cg = image.cgImage else { return }
        store(cg, for: id)
    }

    /// 已经有 JPEG 数据时直接落盘（避免二次编码）
    func store(jpegData data: Data, for id: String) {
        let key = id as NSString
        if let image = UIImage(data: data) {
            memory.setObject(image, forKey: key)
        }
        queue.sync {
            try? FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
            try? data.write(to: fileURL(for: id), options: .atomic)
        }
    }

    /// 删除某张照片的缩略图（设备上删掉照片后顺手清掉，避免陈旧缓存）
    func remove(for id: String) {
        memory.removeObject(forKey: id as NSString)
        queue.sync {
            try? FileManager.default.removeItem(at: fileURL(for: id))
        }
    }

    /// 按设备清单清理不再存在的缩略图（连接后刷新列表时调用）。
    ///
    /// 只删"有文件但设备上已经没有"的项；**不动本机刚生成、设备清单还没刷新的** ——
    /// 上传完成到清单刷新之间有个时间窗，如果这时清理会把刚生成的缩略图删掉。
    /// 因此调用方应只在清单已就绪（items 非空或明确为空态）时调用。
    func prune(keeping ids: Set<String>) {
        queue.sync {
            guard let files = try? FileManager.default.contentsOfDirectory(
                at: directory, includingPropertiesForKeys: nil
            ) else { return }
            let keep = Set(ids.map { fileName(for: $0) })
            for file in files where !keep.contains(file.lastPathComponent) {
                try? FileManager.default.removeItem(at: file)
                // 内存缓存里对应的 id 也清掉（文件名 = id + ".jpg"）
                let name = file.lastPathComponent
                let id = name.hasSuffix(".jpg") ? String(name.dropLast(4)) : name
                memory.removeObject(forKey: id as NSString)
            }
        }
    }

    // MARK: - 路径

    private func fileURL(for id: String) -> URL {
        directory.appendingPathComponent(fileName(for: id))
    }

    /// 设备照片 id 是白名单字符（`[A-Za-z0-9_.-]`），这里再兜一层，
    /// 防止异常 id 里带路径分隔符把文件写到目录外。
    private func fileName(for id: String) -> String {
        let sanitized = id
            .replacingOccurrences(of: "/", with: "_")
            .replacingOccurrences(of: ":", with: "_")
            .replacingOccurrences(of: "\\", with: "_")
        return sanitized.hasSuffix(".jpg") ? sanitized : sanitized + ".jpg"
    }
}
