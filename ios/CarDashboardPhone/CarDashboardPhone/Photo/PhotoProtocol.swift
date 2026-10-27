/**
 * PhotoProtocol.swift —— 照片服务 BLE 协议（**两端契约的 Swift 侧唯一实现**）
 *
 * 协议原文：`docs/photo-protocol.md`（冻结版，字段名必须逐字一致）。
 * 设计背景：`docs/photo-wireless-design.md` §7/§8/§10。
 *
 * ## 本文件的边界
 *
 * 只做**纯编解码**：UUID 常量、PGJ 头构造/解析、CRC32、命令 JSON、事件 JSON、分片帧。
 * 不依赖 CoreBluetooth、不依赖 UIKit —— 因此可以在 macOS 上用 `swiftc` 直接单测
 * （见 `tools/test_photo_models.swift`、`tools/test_photo_transfer.swift`）。
 *
 * ## 三条硬约束（违反必出线上事故）
 *
 * 1. **单条 JSON ≤ 180 B**：iOS 单次 Notify/Read 有效载荷约 184 B，超过会被截断。
 *    本文件所有命令/事件编码都远小于该值，测试里对每条都做了断言。
 * 2. **UUID 一律按字符串比较**：CoreBluetooth 重新发现服务后会给特征创建**新实例**，
 *    比较对象会恒为 false（历史事故：地图队列停在第一条，见 BLEClient 注释）。
 * 3. **CRC32 口径**：标准 IEEE 反射 CRC（poly 0xEDB88320，init/xor 0xFFFFFFFF，
 *    与 zlib `crc32()` 一致）。`.pgj` 的 `crc32` 字段覆盖**整个文件**，
 *    但**该字段自身参与计算时视为 0**（否则是循环定义）。`begin` 里的 `crc` 用同一口径。
 *    ⚠️ 固件 `simulator/ui/pgj.c` 必须用同一口径，否则每次提交都会 `err{crc}`。
 */

import Foundation

// MARK: - GATT 常量

/// 照片服务的 5 个特征（UUID **字符串**：比较一律用字符串，不要比对象）
enum PhotoGATT {
    /// Photo Service —— **没有它 = 旧固件**（App 优雅降级）
    static let service = "7D2A0010-8F4A-4B7C-9D21-5E6A3B8C1000"
    /// PhotoCtl：App→设备，JSON 命令（WRITE 带响应）
    static let ctl = "7D2A0011-8F4A-4B7C-9D21-5E6A3B8C1000"
    /// PhotoData：App→设备，二进制分片（WRITE_NR）
    static let data = "7D2A0012-8F4A-4B7C-9D21-5E6A3B8C1000"
    /// PhotoEvt：设备→App，JSON 事件（NOTIFY）
    static let evt = "7D2A0013-8F4A-4B7C-9D21-5E6A3B8C1000"
    /// PhotoInfo：能力/档位（READ）
    static let info = "7D2A0014-8F4A-4B7C-9D21-5E6A3B8C1000"
    /// PhotoState：状态/设置（READ + NOTIFY）
    static let state = "7D2A0015-8F4A-4B7C-9D21-5E6A3B8C1000"

    /// 全部特征 UUID（发现用）
    static let allCharacteristics: [String] = [ctl, data, evt, info, state]
}

// MARK: - 协议常量

enum PhotoLimits {
    /// 单条 JSON 上限（iOS Notify/Read 有效载荷约 184 B）
    static let maxMessageBytes = 180
    /// 设备建议的默认分片 data 上限（协议 `go.chunk`）
    static let defaultChunkHint = 178
    /// 设备默认在途窗口（协议 `go.win`）
    static let defaultWindowBytes = 4096
    /// 6 s 无 credit → 重新 begin 续传
    static let creditTimeout: TimeInterval = 6

    /// `.pgj` 头长度
    static let pgjHeaderBytes = 32
    /// `.pgj` magic
    static let pgjMagic = "PGJ1"
    /// `.pgj` 版本
    static let pgjVersion = 1
    /// flags bit0：baseline JPEG（v1 必须为 1）
    static let pgjFlagBaseline = 0x01
    /// 支持的最大边长（与屏幕一致）
    static let pgjMaxDim = 466
    /// 单张 JPEG 载荷上限（512 KiB）
    static let pgjMaxJPEGBytes = 512 * 1024
    /// 设备文件名上限（字节，UTF-8）
    static let photoNameMaxBytes = 63
}

// MARK: - CRC32

extension PhotoProtocol {
    /// 标准 CRC-32（IEEE 802.3，反射；与 zlib `crc32()` / 固件 `pgj_crc32()` 一致）
    static func crc32(_ data: Data) -> UInt32 {
        var c: UInt32 = 0xFFFF_FFFF
        let table = crc32Table
        data.withUnsafeBytes { (buf: UnsafeRawBufferPointer) in
            for byte in buf {
                c = table[Int((c ^ UInt32(byte)) & 0xFF)] ^ (c >> 8)
            }
        }
        return c ^ 0xFFFF_FFFF
    }

    /// 同上（`[UInt8]` 重载，测试/小包方便）
    static func crc32(_ bytes: [UInt8]) -> UInt32 {
        var c: UInt32 = 0xFFFF_FFFF
        let table = crc32Table
        for byte in bytes {
            c = table[Int((c ^ UInt32(byte)) & 0xFF)] ^ (c >> 8)
        }
        return c ^ 0xFFFF_FFFF
    }

    /// 查表（256 项，静态懒初始化）
    private static let crc32Table: [UInt32] = {
        var table = [UInt32](repeating: 0, count: 256)
        for i in 0..<256 {
            var c = UInt32(i)
            for _ in 0..<8 {
                c = (c & 1) != 0 ? (0xEDB8_8320 ^ (c >> 1)) : (c >> 1)
            }
            table[i] = c
        }
        return table
    }()
}

// MARK: - PGJ 头

/// `.pgj` 文件头（32 B）解析结果
struct PGJHeader: Equatable {
    /// 宽（1..466）
    var width: Int
    /// 高（1..466）
    var height: Int
    /// 版本（必须 == 1）
    var version: Int
    /// flags（bit0 必须 == 1：baseline JPEG）
    var flags: Int
    /// JPEG 质量（10..100，仅记录）
    var quality: Int
    /// JPEG 载荷字节数（= 文件大小 - 32）
    var jpegBytes: Int
    /// 文件里**存储**的 CRC32（整文件，crc 字段自身按 0 计算）
    var storedCRC: UInt32
    /// 创建时间（Unix 秒，0 = 未知）
    var createdUnix: UInt32

    /// 是否为 baseline JPEG（v1 必须为 true）
    var isBaseline: Bool { (flags & PhotoLimits.pgjFlagBaseline) != 0 }
    /// 文件总字节数
    var totalBytes: Int { PhotoLimits.pgjHeaderBytes + jpegBytes }
}

/// PGJ 头/整文件校验失败的具体原因（给日志与用户文案用；测试逐项断言）
enum PGJHeaderError: Equatable {
    /// 不足 32 B
    case tooShort(actual: Int)
    /// magic 不是 "PGJ1"
    case badMagic
    /// 版本不是 1
    case badVersion(Int)
    /// flags bit0 不是 1（progressive / 未知）
    case notBaseline(flags: Int)
    /// 宽或高越界
    case badDimension(width: Int, height: Int)
    /// 质量越界
    case badQuality(Int)
    /// jpeg_bytes 越界
    case badJPEGBytes(Int)
    /// 文件长度与 `32 + jpeg_bytes` 不符
    case badLength(expected: Int, actual: Int)
    /// 整文件 CRC32 不符
    case badCRC(expected: UInt32, actual: UInt32)

    /// 给用户看的一句话（不暴露内部细节）
    var userText: String {
        switch self {
        case .tooShort, .badMagic, .badVersion, .notBaseline, .badDimension,
             .badQuality, .badJPEGBytes, .badLength:
            return "文件格式不正确（不是有效的 PGJ 照片）"
        case .badCRC:
            return "文件校验失败（可能已损坏，请重新添加）"
        }
    }
}

extension PhotoProtocol {
    /**
     构造 `.pgj` 文件（32 B 头 + JPEG 载荷）。

     - Parameters:
       - jpeg: baseline JPEG 载荷（调用方保证；本函数只打包，不做 SOF 校验）
       - width/height: 头里的尺寸（越界会被夹到 1...466 —— 正常情况下不该发生）
       - quality: 10...100（仅记录，越界夹取）
       - createdUnix: 0 = 未知
     - Returns: 完整文件（头 + 载荷）。CRC32 覆盖整文件，crc 字段自身按 0 计算。

     - Note: 返回值**必定**是 32 + jpeg.count 字节；如果 jpeg 为空，得到一个
       `jpeg_bytes == 0` 的非法文件（`parsePGJHeader` 会拒），由调用方兜住。
     */
    static func makePGJ(jpeg: Data,
                        width: Int,
                        height: Int,
                        quality: Int = 80,
                        createdUnix: UInt32 = 0) -> Data {
        let w = min(max(width, 1), PhotoLimits.pgjMaxDim)
        let h = min(max(height, 1), PhotoLimits.pgjMaxDim)
        let q = min(max(quality, 10), 100)

        var header = Data(capacity: PhotoLimits.pgjHeaderBytes)
        header.append(contentsOf: Array(PhotoLimits.pgjMagic.utf8)) // 0..3 magic
        appendU16(&header, UInt16(w))                               // 4..5 width
        appendU16(&header, UInt16(h))                               // 6..7 height
        header.append(UInt8(PhotoLimits.pgjVersion))                // 8 version
        header.append(UInt8(PhotoLimits.pgjFlagBaseline))           // 9 flags
        appendU16(&header, UInt16(q))                               // 10..11 quality
        appendU32(&header, UInt32(jpeg.count))                      // 12..15 jpeg_bytes
        appendU32(&header, 0)                                       // 16..19 crc32（先占位）
        appendU32(&header, createdUnix)                             // 20..23 created_unix
        header.append(contentsOf: [UInt8](repeating: 0, count: 8))  // 24..31 reserved
        assert(header.count == PhotoLimits.pgjHeaderBytes, "PGJ 头必须 32 字节")

        var file = header
        file.append(jpeg)
        // 整文件 CRC32（此时 crc 字段为 0，即"字段自身不参与"的口径）
        let crc = crc32(file)
        writeU32(&file, at: 16, crc)
        return file
    }

    /**
     只解析 32 B 头并做严格校验（对应固件 `pgj_parse_header`）。

     失败返回 nil（**不猜**：坏文件绝不能进清单/上屏）。需要失败原因用
     `diagnosePGJHeader(_:)`。
     */
    static func parsePGJHeader(_ data: Data) -> PGJHeader? {
        guard diagnosePGJHeader(data) == nil, let h = readHeaderFields(data) else { return nil }
        return h
    }

    /// 解析头并返回失败原因；nil = 合法
    static func diagnosePGJHeader(_ data: Data) -> PGJHeaderError? {
        guard data.count >= PhotoLimits.pgjHeaderBytes else {
            return .tooShort(actual: data.count)
        }
        guard let h = readHeaderFields(data) else { return .tooShort(actual: data.count) }
        let magic = String(decoding: data[0..<4], as: UTF8.self)
        guard magic == PhotoLimits.pgjMagic else { return .badMagic }
        guard h.version == PhotoLimits.pgjVersion else { return .badVersion(h.version) }
        guard h.isBaseline else { return .notBaseline(flags: h.flags) }
        guard h.width >= 1, h.width <= PhotoLimits.pgjMaxDim,
              h.height >= 1, h.height <= PhotoLimits.pgjMaxDim else {
            return .badDimension(width: h.width, height: h.height)
        }
        guard h.quality >= 1, h.quality <= 100 else { return .badQuality(h.quality) }
        guard h.jpegBytes >= 1, h.jpegBytes <= PhotoLimits.pgjMaxJPEGBytes else {
            return .badJPEGBytes(h.jpegBytes)
        }
        return nil
    }

    /**
     校验**完整文件**：头合法 + 长度自洽 + 整文件 CRC32 一致。

     - Returns: nil = 合法；否则是失败原因。
     */
    static func validatePGJ(_ file: Data) -> PGJHeaderError? {
        if let e = diagnosePGJHeader(file) { return e }
        guard let h = readHeaderFields(file) else {
            return .tooShort(actual: file.count)
        }
        let expected = PhotoLimits.pgjHeaderBytes + h.jpegBytes
        guard file.count == expected else {
            return .badLength(expected: expected, actual: file.count)
        }
        let actual = fileCRC32(file)
        guard actual == h.storedCRC else {
            return .badCRC(expected: h.storedCRC, actual: actual)
        }
        return nil
    }

    /**
     整文件 CRC32（与 `begin` 的 `crc` 同一口径）。

     crc 字段自身按 0 计算。文件不足 32 B 时按原样计算（调用方应先 parse 头）。
     */
    static func fileCRC32(_ file: Data) -> UInt32 {
        guard file.count >= PhotoLimits.pgjHeaderBytes else { return crc32(file) }
        var copy = file
        copy[16] = 0
        copy[17] = 0
        copy[18] = 0
        copy[19] = 0
        return crc32(copy)
    }

    /// 读头字段（不做合法性判断；`data` 至少 32 B）
    private static func readHeaderFields(_ data: Data) -> PGJHeader? {
        guard data.count >= PhotoLimits.pgjHeaderBytes else { return nil }
        let bytes = [UInt8](data.prefix(PhotoLimits.pgjHeaderBytes))
        return PGJHeader(
            width: Int(bytes[4]) | (Int(bytes[5]) << 8),
            height: Int(bytes[6]) | (Int(bytes[7]) << 8),
            version: Int(bytes[8]),
            flags: Int(bytes[9]),
            quality: Int(bytes[10]) | (Int(bytes[11]) << 8),
            jpegBytes: Int(readU32(bytes, 12)),
            storedCRC: readU32(bytes, 16),
            createdUnix: readU32(bytes, 20))
    }
}

// MARK: - 数据分片帧

extension PhotoProtocol {
    /// 分片帧：`[u32 offset LE][payload]`（`PhotoData` 特征上的字节布局）
    static func makeChunkFrame(offset: Int, payload: Data) -> Data {
        var frame = Data(capacity: 4 + payload.count)
        appendU32(&frame, UInt32(truncatingIfNeeded: max(0, offset)))
        frame.append(payload)
        return frame
    }

    /// 解析分片帧；不足 4 B 返回 nil
    static func parseChunkFrame(_ frame: Data) -> (offset: Int, payload: Data)? {
        guard frame.count >= 4 else { return nil }
        let off = Int(readU32([UInt8](frame.prefix(4)), 0))
        return (off, Data(frame.dropFirst(4)))
    }
}

// MARK: - 文件名白名单

extension PhotoProtocol {
    /**
     设备文件名白名单（与 `photo_list.h: photo_name_valid()` 一致）：
     长度 1..63；只允许 `A-Za-z0-9_.-`；不以 `.` 开头、不含 `..`；
     扩展名（大小写不敏感）必须是 `.pgj` / `.pgi`。
     */
    static func isValidPhotoName(_ name: String) -> Bool {
        guard !name.isEmpty, name.utf8.count <= PhotoLimits.photoNameMaxBytes else { return false }
        guard !name.hasPrefix("."), !name.contains("..") else { return false }
        for scalar in name.unicodeScalars {
            switch scalar {
            case "A"..."Z", "a"..."z", "0"..."9", "_", ".", "-":
                continue
            default:
                return false
            }
        }
        let lower = name.lowercased()
        return lower.hasSuffix(".pgj") || lower.hasSuffix(".pgi")
    }
}

// MARK: - 命令（App → 设备，PhotoCtl）

/// 错误码（协议 §5；`unknown` 兜未知 code，`rawCode` 保留原文便于日志）
enum PhotoErrorCode: String, Equatable {
    case busy
    case no_space
    case too_large
    case unsupported
    case crc
    case offset
    case io
    case not_ready
    case bad_arg
    case unknown

    init(raw: String) {
        self = PhotoErrorCode(rawValue: raw) ?? .unknown
    }
}

/// 一条 `PhotoCtl` 命令。用枚举保证"只有合法组合能被构造出来"。
enum PhotoCommand: Equatable {
    /// 开始上传。`crc` = 整文件 CRC32（口径见 `PhotoProtocol.fileCRC32`）
    case begin(kind: PhotoKind, bytes: Int, crc: UInt32,
               width: Int, height: Int, frames: Int, fps: Int)
    /// 数据发完，请求校验提交
    case end
    /// 放弃本次（设备删 `.part`）
    case abort
    /// 删除照片
    case del(id: String)
    /// 切到第 i 张并钉住
    case play(index: Int)
    /// 设置任一子集（nil = 不下发该字段）
    case set(slideshow: Bool?, interval: Int?, shift: Bool?, screenOff: Int?)
    /// 请求清单分页
    case list
    /// 自检
    case bench
    /// 格式化仪表 SD 卡（危险操作；UI 必须二次确认）
    case format

    /// 协议里的 `op` 字符串
    var op: String {
        switch self {
        case .begin: return "begin"
        case .end: return "end"
        case .abort: return "abort"
        case .del: return "del"
        case .play: return "play"
        case .set: return "set"
        case .list: return "list"
        case .bench: return "bench"
        case .format: return "format"
        }
    }

    /**
     编码成 JSON（字段名/顺序与 `docs/photo-protocol.md` §4 一致）。

     - Returns: 非法参数返回 nil（**不静默夹取**：App 侧参数错应在测试里就暴露）。
     */
    func encode() -> Data? {
        guard let json = jsonString else { return nil }
        let data = Data(json.utf8)
        guard data.count <= PhotoLimits.maxMessageBytes else { return nil }
        return data
    }

    /// JSON 字符串（非法参数返回 nil）
    var jsonString: String? {
        switch self {
        case let .begin(kind, bytes, crc, width, height, frames, fps):
            guard bytes >= 1, bytes <= 512 * 1024 * 1024 else { return nil }
            guard width >= 1, width <= PhotoLimits.pgjMaxDim,
                  height >= 1, height <= PhotoLimits.pgjMaxDim else { return nil }
            guard frames >= 1 else { return nil }
            guard fps >= 0, fps <= 120 else { return nil }
            if kind == .pgi {
                guard fps >= 1 else { return nil }
            }
            return "{\"t\":\"p\",\"op\":\"begin\",\"kind\":\(kind.rawValue),\"bytes\":\(bytes),"
                + "\"crc\":\(crc),\"w\":\(width),\"h\":\(height),"
                + "\"frames\":\(frames),\"fps\":\(fps)}"

        case .end:
            return "{\"t\":\"p\",\"op\":\"end\"}"

        case .abort:
            return "{\"t\":\"p\",\"op\":\"abort\"}"

        case let .del(id):
            guard PhotoProtocol.isValidPhotoName(id) else { return nil }
            return "{\"t\":\"p\",\"op\":\"del\",\"id\":\"\(id)\"}"

        case let .play(index):
            guard index >= 0 else { return nil }
            return "{\"t\":\"p\",\"op\":\"play\",\"i\":\(index)}"

        case let .set(slideshow, interval, shift, screenOff):
            var parts: [String] = ["\"t\":\"p\"", "\"op\":\"set\""]
            var any = false
            if let slideshow {
                parts.append("\"slide\":\(slideshow ? 1 : 0)")
                any = true
            }
            if let interval {
                guard interval >= 1, interval <= 3600 else { return nil }
                parts.append("\"intv\":\(interval)")
                any = true
            }
            if let shift {
                parts.append("\"shift\":\(shift ? 1 : 0)")
                any = true
            }
            if let screenOff {
                guard screenOff >= 0, screenOff <= 86400 else { return nil }
                parts.append("\"soff_s\":\(screenOff)")
                any = true
            }
            guard any else { return nil }
            return "{" + parts.joined(separator: ",") + "}"

        case .list:
            return "{\"t\":\"p\",\"op\":\"list\"}"

        case .bench:
            return "{\"t\":\"p\",\"op\":\"bench\"}"

        case .format:
            return "{\"t\":\"p\",\"op\":\"format\"}"
        }
    }
}

// MARK: - 事件（设备 → App，PhotoEvt）

/// `PhotoEvt` 上的短事件（协议 §5）
enum PhotoEvent: Equatable {
    /// `{"t":"pe","ev":"go","off":0,"chunk":178,"win":4096,"id":"p00012.pgj"}`
    /// `offset` = 期望起始偏移（续传时 > 0）
    case go(offset: Int, chunk: Int, window: Int, id: String?)
    /// `{"t":"pe","ev":"credit","off":4096}` 已落盘到 off
    case credit(offset: Int)
    /// `{"t":"pe","ev":"done","id":"p00012.pgj"}`
    case done(id: String?)
    /// `{"t":"pe","ev":"err","code":"crc","want":0}`；`want` 可选
    case error(code: PhotoErrorCode, rawCode: String, want: Int?)
    /// `{"t":"pe","ev":"changed"}` 清单/当前项/设置变了
    case changed
    /// `{"t":"pe","ev":"info"}` 能力/自检结果变了
    case info
    /// `{"t":"pe","ev":"fmt","ok":1}` 格式化 SD 卡完成（失败走 err）
    case formatResult(ok: Bool)
}

/// `PhotoEvt` 上可能收到的一切（短事件 + 清单分页）
enum PhotoIncoming: Equatable {
    /// `pq`
    case listBegin(count: Int)
    /// `pf`（一条清单项）
    case listItem(DevicePhoto)
    /// `pz`
    case listEnd
    /// `pe`
    case event(PhotoEvent)
}

extension PhotoProtocol {
    /**
     解析一条 `PhotoEvt` 载荷（≤180 B）。

     - Returns: 解析失败/未知类型/坏 JSON → nil（**不猜**，宁可不处理也不误触发状态机）。
     */
    static func parseIncoming(_ data: Data) -> PhotoIncoming? {
        guard let root = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any],
              let tag = root["t"] as? String else { return nil }

        switch tag {
        case "pe":
            return parseEvent(root)
        case "pq":
            guard let n = intValue(root["n"]), n >= 0 else { return nil }
            return .listBegin(count: n)
        case "pf":
            guard let item = DevicePhotoParser.parseItem(data) else { return nil }
            return .listItem(item)
        case "pz":
            return .listEnd
        default:
            return nil
        }
    }

    /// `pe` 事件解析
    private static func parseEvent(_ root: [String: Any]) -> PhotoIncoming? {
        guard let ev = root["ev"] as? String else { return nil }
        switch ev {
        case "go":
            return .event(.go(offset: max(0, intValue(root["off"]) ?? 0),
                              chunk: max(0, intValue(root["chunk"]) ?? 0),
                              window: max(0, intValue(root["win"]) ?? 0),
                              id: root["id"] as? String))
        case "credit":
            guard let off = intValue(root["off"]) else { return nil }
            return .event(.credit(offset: max(0, off)))
        case "done":
            return .event(.done(id: root["id"] as? String))
        case "err":
            let raw = (root["code"] as? String) ?? ""
            return .event(.error(code: PhotoErrorCode(raw: raw),
                                 rawCode: raw,
                                 want: intValue(root["want"])))
        case "changed":
            return .event(.changed)
        case "info":
            return .event(.info)
        case "fmt":
            return .event(.formatResult(ok: (intValue(root["ok"]) ?? 0) != 0))
        default:
            return nil
        }
    }

    /// JSON 数字 → Int（Bool 也可容错映射成 0/1；供本文件与 DevicePhotos 共用）
    static func intValue(_ any: Any?) -> Int? {
        switch any {
        case let n as Int: return n
        case let n as Bool: return n ? 1 : 0
        case let n as Double: return Int(n)
        case let n as NSNumber: return n.intValue
        default: return nil
        }
    }
}

// MARK: - 小工具（小端写入）

extension PhotoProtocol {
    /// 追加 u16（小端）
    static func appendU16(_ data: inout Data, _ value: UInt16) {
        data.append(UInt8(value & 0xFF))
        data.append(UInt8((value >> 8) & 0xFF))
    }

    /// 追加 u32（小端）
    static func appendU32(_ data: inout Data, _ value: UInt32) {
        for i in 0..<4 { data.append(UInt8((value >> (8 * i)) & 0xFF)) }
    }

    /// 就地写 u32（小端）
    static func writeU32(_ data: inout Data, at offset: Int, _ value: UInt32) {
        guard offset >= 0, offset + 4 <= data.count else { return }
        for i in 0..<4 { data[offset + i] = UInt8((value >> (8 * i)) & 0xFF) }
    }

    /// 读 u32（小端）
    static func readU32(_ bytes: [UInt8], _ offset: Int) -> UInt32 {
        guard offset >= 0, offset + 4 <= bytes.count else { return 0 }
        return UInt32(bytes[offset])
            | (UInt32(bytes[offset + 1]) << 8)
            | (UInt32(bytes[offset + 2]) << 16)
            | (UInt32(bytes[offset + 3]) << 24)
    }
}

// MARK: - 协议命名空间

/// 协议常量的命名空间（所有编解码入口都挂在这里，方便测试与查找）
enum PhotoProtocol {}
