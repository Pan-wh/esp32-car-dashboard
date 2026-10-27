//
//  PGJEncoder.swift
//  CarDashboardPhone
//
//  把一张 JPEG 包成设备要的 `.pgj` 文件（32 B 头 + JPEG 载荷）。
//
//  ## 为什么要落盘，而不是在内存里排队
//
//  上传可能要 2–5 秒，期间用户切后台、断线重连、甚至取消重试。
//  如果把 JPEG 一直放在内存里：
//    ① 断线续传时得重新编码一遍（编码几百毫秒 + 内存峰值）；
//    ② 队列里 10 张照片就是 10 份 Data，内存不可控。
//  协议里明确要求"编码产物先写 App 沙盒临时文件，续传时直接从文件流式读"
//  （docs/photo-protocol.md §10），所以这里负责把它写到
//  `tmp/photo-upload/<id>.pgj` 并返回字节数 / CRC / 尺寸等元数据。
//
//  ## 格式（docs/photo-protocol.md §7）
//
//      偏移  长度  字段
//        0     4   magic "PGJ1"
//        4     2   width  u16
//        6     2   height u16
//        8     1   version = 1
//        9     1   flags（bit0=1 表示 baseline JPEG）
//       10     2   quality
//       12     4   jpeg_bytes
//       16     4   crc32（**整个文件**的 CRC32，与 begin 命令里的 crc 一致）
//       20     4   created_unix
//       24     8   reserved
//
//  头的字节拼装由 `PhotoProtocol` 统一实现（两端共用一份，测试有跨语言夹具），
//  本文件只负责：校验输入 → 调 `PhotoProtocol.makePGJ` → 计算 CRC → 写盘。
//
//  本文件是 **Foundation-only**，方便 `swiftc` 直接编译单测。
//

import Foundation

enum PGJEncoderError: LocalizedError {
    case progressiveJPEG
    case invalidDimensions(width: Int, height: Int)
    case tooLarge(bytes: Int, limit: Int)
    case cannotWriteFile(String)

    var errorDescription: String? {
        switch self {
        case .progressiveJPEG:
            return "这张 JPEG 是渐进式（progressive），仪表只认普通 JPEG"
        case .invalidDimensions(let w, let h):
            return "图片尺寸不合法（\(w)×\(h)），请重新选择"
        case .tooLarge(let bytes, let limit):
            return "照片太大（\(bytes / 1024) KB），仪表单张上限 \(limit / 1024) KB"
        case .cannotWriteFile(let detail):
            return "临时文件写入失败：\(detail)"
        }
    }
}

/// 编码完成、已经落到沙盒里的一份上传素材。
///
/// UI 用它构造 `PhotoUploadRequest`（这些字段正好是协议 `begin` 命令需要的）。
struct EncodedPhotoFile {
    /// `tmp/photo-upload/` 下的临时文件（`.pgj` 或 `.pgi`）
    let fileURL: URL
    /// 文件总字节数（= 设备 begin 命令里的 bytes）
    let byteCount: Int
    /// 整个文件的 CRC32（= 设备 begin 命令里的 crc）
    let crc32: UInt32
    let width: Int
    let height: Int
    let frameCount: Int
    let fps: Int
    let quality: Int
    /// true = `.pgi` 动图；false = `.pgj` 静态照片
    let isAnimated: Bool
}

enum PGJEncoder {

    /// 设备单张静态上限（协议 §2 `max_kb` 默认 512 KiB）
    static let maxStaticBytes = 512 * 1024
    /// 动图（`.pgi`）单文件上限（协议 §2 `anim.max_kb` 默认 2048 KiB）
    static let maxAnimatedBytes = 2 * 1024 * 1024
    /// `.pgj` 头长度
    static let headerBytes = 32

    // MARK: - 内存路径（纯逻辑，便于测试）

    /// JPEG → `.pgj` 字节。返回的 `crc32` 覆盖**整个文件**。
    static func pgjData(jpeg: Data,
                        width: Int,
                        height: Int,
                        quality: Int,
                        createdUnix: UInt32 = 0) throws -> (data: Data, crc32: UInt32) {
        guard width >= 1, width <= 466, height >= 1, height <= 466 else {
            throw PGJEncoderError.invalidDimensions(width: width, height: height)
        }
        // 渐进式 JPEG 设备解不了 —— 在 App 侧就拦下来，别等传输到一半被拒。
        guard isBaselineJPEG(jpeg) else { throw PGJEncoderError.progressiveJPEG }

        let data = PhotoProtocol.makePGJ(
            jpeg: jpeg,
            width: width,
            height: height,
            quality: quality,
            createdUnix: createdUnix
        )
        let total = data.count
        guard total <= maxStaticBytes else {
            throw PGJEncoderError.tooLarge(bytes: total, limit: maxStaticBytes)
        }
        // begin 命令的 crc 与头里一致：整文件 CRC，crc 字段自身按 0 计算
        return (data, PhotoProtocol.fileCRC32(data))
    }

    // MARK: - 落盘路径

    /// 静态照片：JPEG → `.pgj` → 写临时文件。
    static func stageStatic(jpeg: Data,
                            width: Int,
                            height: Int,
                            quality: Int,
                            createdUnix: UInt32 = 0) throws -> EncodedPhotoFile {
        let (data, crc) = try pgjData(jpeg: jpeg, width: width, height: height,
                                      quality: quality, createdUnix: createdUnix)
        let url = try writeTemporary(data: data, ext: "pgj")
        return EncodedPhotoFile(fileURL: url,
                                byteCount: data.count,
                                crc32: crc,
                                width: width,
                                height: height,
                                frameCount: 1,
                                fps: 0,
                                quality: quality,
                                isAnimated: false)
    }

    /// 动图（FULL 档）：`.pgi` 字节 → 写临时文件。
    ///
    /// `.pgi` 的打包在 `PgiEncoder` 里（它原本就有完整的调色板量化实现），
    /// 这里只统一"校验体积 + 计算 CRC + 落盘 + 返回元数据"这一层。
    static func stageAnimated(pgi: Data,
                              width: Int,
                              height: Int,
                              frameCount: Int,
                              fps: Int) throws -> EncodedPhotoFile {
        guard width >= 1, height >= 1, frameCount >= 1 else {
            throw PGJEncoderError.invalidDimensions(width: width, height: height)
        }
        guard pgi.count <= maxAnimatedBytes else {
            throw PGJEncoderError.tooLarge(bytes: pgi.count, limit: maxAnimatedBytes)
        }
        let url = try writeTemporary(data: pgi, ext: "pgi")
        return EncodedPhotoFile(fileURL: url,
                                byteCount: pgi.count,
                                crc32: PhotoProtocol.crc32(pgi),
                                width: width,
                                height: height,
                                frameCount: frameCount,
                                fps: fps,
                                quality: 0,
                                isAnimated: true)
    }

    /// 删除某次上传的临时文件（上传完成 / 取消后调用，避免占盘）。
    static func removeStagedFile(at url: URL) {
        try? FileManager.default.removeItem(at: url)
    }

    /// 清理超过 `maxAge` 的残留临时文件（上次崩溃/被杀留下的）。
    /// App 启动或进入照片页时调用一次即可。
    static func cleanStaleFiles(olderThan maxAge: TimeInterval = 24 * 60 * 60) {
        let fm = FileManager.default
        guard let files = try? fm.contentsOfDirectory(at: uploadDirectory,
                                                      includingPropertiesForKeys: [.contentModificationDateKey])
        else { return }
        let deadline = Date().addingTimeInterval(-maxAge)
        for file in files {
            let modified = (try? file.resourceValues(forKeys: [.contentModificationDateKey])
                .contentModificationDate) ?? .distantPast
            if modified < deadline { try? fm.removeItem(at: file) }
        }
    }

    /// 上传临时目录：`tmp/photo-upload/`
    static var uploadDirectory: URL {
        let base = FileManager.default.temporaryDirectory
        let dir = base.appendingPathComponent("photo-upload", isDirectory: true)
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        return dir
    }

    // MARK: - JPEG 结构检查

    /// 是否是 **baseline** JPEG（SOF0）。progressive（SOF2）返回 false。
    ///
    /// 设备（ROM TJpgDec）只解 baseline；iOS 侧编码默认就是 baseline，
    /// 但用户素材/旧缓存可能是 progressive，提前检查能给出可读的失败原因。
    static func isBaselineJPEG(_ data: Data) -> Bool {
        var i = 0
        let bytes = [UInt8](data)
        guard bytes.count > 4, bytes[0] == 0xFF, bytes[1] == 0xD8 else { return false }
        i = 2
        while i + 3 < bytes.count {
            // 跳过填充的 0xFF
            guard bytes[i] == 0xFF else { i += 1; continue }
            var markerIndex = i + 1
            while markerIndex < bytes.count, bytes[markerIndex] == 0xFF { markerIndex += 1 }
            guard markerIndex < bytes.count else { return false }
            let marker = bytes[markerIndex]
            i = markerIndex + 1
            // 无长度字段的标记
            if marker == 0xD8 || marker == 0xD9 || (0xD0...0xD7).contains(marker) { continue }
            guard i + 1 < bytes.count else { return false }
            let length = Int(bytes[i]) << 8 | Int(bytes[i + 1])
            if marker == 0xC0 { return true }     // SOF0 = baseline
            if marker == 0xC2 { return false }    // SOF2 = progressive
            guard length >= 2 else { return false }
            i += length
        }
        return false
    }

    /// 从 JPEG 的 SOF 段读宽高（用作编码后的自检）。
    ///
    /// 读不到返回 nil —— 调用方应按"尺寸未知"处理，不要拿 0 去填协议字段。
    static func jpegDimensions(_ data: Data) -> (width: Int, height: Int)? {
        let bytes = [UInt8](data)
        guard bytes.count > 4, bytes[0] == 0xFF, bytes[1] == 0xD8 else { return nil }
        var i = 2
        while i + 8 < bytes.count {
            guard bytes[i] == 0xFF else { i += 1; continue }
            var markerIndex = i + 1
            while markerIndex < bytes.count, bytes[markerIndex] == 0xFF { markerIndex += 1 }
            guard markerIndex < bytes.count else { return nil }
            let marker = bytes[markerIndex]
            i = markerIndex + 1
            if marker == 0xD8 || marker == 0xD9 || (0xD0...0xD7).contains(marker) { continue }
            guard i + 1 < bytes.count else { return nil }
            let length = Int(bytes[i]) << 8 | Int(bytes[i + 1])
            // SOF0/1/2 段：长度 2B + 精度 1B + 高 2B + 宽 2B ...
            if (0xC0...0xC3).contains(marker) {
                guard i + 6 < bytes.count else { return nil }
                let h = Int(bytes[i + 3]) << 8 | Int(bytes[i + 4])
                let w = Int(bytes[i + 5]) << 8 | Int(bytes[i + 6])
                return (w, h)
            }
            guard length >= 2 else { return nil }
            i += length
        }
        return nil
    }

    // MARK: - 内部

    private static func writeTemporary(data: Data, ext: String) throws -> URL {
        let name = UUID().uuidString + "." + ext
        let url = uploadDirectory.appendingPathComponent(name)
        do {
            try data.write(to: url, options: .atomic)
        } catch {
            throw PGJEncoderError.cannotWriteFile(error.localizedDescription)
        }
        return url
    }
}
