/**
 * DevicePhotos.swift —— 设备照片能力 / 状态 / 清单 / 上传队列的模型与解析
 *
 * 契约：`docs/photo-protocol.md`（冻结）。本文件 **Foundation-only**，
 * 不依赖 CoreBluetooth / UIKit，因此可在 macOS 上 `swiftc` 直接单测
 * （见 `tools/test_photo_models.swift`）。
 *
 * ## 沿用 `DeviceFaces.swift` 的三条约定（都是踩过坑的）
 *
 * 1. **Read 是取数据的正途**：这些 JSON 都 ≤180 B，但 Notify 仍可能被截断；
 *    BLEClient 收到 Notify 只当"变了"的提示，数据一律重新 Read。
 * 2. **坏 JSON / 错 `t` → 返回 nil，不猜**：宁可保持上一次的状态，
 *    也不要把半截数据当成真值显示（否则用户会看到"照片全没了"）。
 * 3. **区分「设备没上报」与「真的是 0」**：属性用非可选值（UI 直接画），
 *    但每个模型都带 `missing: Set<String>`，通过 `reported(_:)` 判断该字段
 *    设备是否真的说了。把两者混为一谈，正是历史上"亮度被凭空改回默认值"的根源。
 *
 * ## 为什么 UI 用非可选值而不是 Optional
 *
 * 照片页展示的是"设备现在是什么样"，缺字段时最合理的行为是显示一个安全的默认
 * （0 / 关 / LITE），同时用 `missing` 标注"这是默认值不是设备说的"，
 * 而不是让每个 UI 站点都写一遍 `if let`。需要精确语义的调用点用 `reported(_:)`。
 */

import Foundation

// MARK: - 照片类型

/// 照片文件类型（与协议 `kind` / `pf.k` 一致）
enum PhotoKind: Int, Equatable, CaseIterable {
    /// 静态照片：32 B `.pgj` 头 + 1 张 baseline JPEG
    case pgj = 0
    /// 动图：`.pgi`（一帧调色板 + 索引）
    case pgi = 1

    /// 设备文件名后缀（含点）
    var fileExtension: String {
        switch self {
        case .pgj: return ".pgj"
        case .pgi: return ".pgi"
        }
    }

    /// 给用户看的类型名
    var displayName: String {
        switch self {
        case .pgj: return "静态照片"
        case .pgi: return "动图"
        }
    }
}

// MARK: - 能力（PhotoInfo）

/// `PhotoInfo`（READ）：设备能力/档位。字段见协议 §2。
struct DevicePhotoInfo: Equatable {
    /// 协议版本（当前 1）
    var version: Int = 0
    /// 1 = LITE（仅静态），2 = FULL（可传/播动图）
    var tier: Int = 0
    /// 能力位掩码：bit0 静态 JPEG、bit1 动图、bit2 删除、bit3 内置存储
    var capability: Int = 0
    /// 静态照片边长上限（正常 466）
    var maxPixel: Int = 0
    /// 单张静态文件上限（**KiB**）
    var maxBytes: Int = 0
    /// 动图边长（300）
    var animPixel: Int = 0
    /// 动图目标帧率（8）
    var animFPS: Int = 0
    /// 动图最大帧数（12）
    var animFrames: Int = 0
    /// 动图文件上限（**KiB**）
    var animMaxBytes: Int = 0
    /// **上次实测动图帧率**（0 = 没播过）
    var animFPSMeasured: Int = 0
    /// 最近一次 JPEG 解码耗时（ms，0 = 未测）
    var benchDecodeMs: Int = 0
    /// 设备**没上报**的字段（`PhotoInfoKey.rawValue` 点分路径）。空集 = 全部上报。
    var missing: Set<String> = Set(PhotoInfoKey.allCases.map(\.rawValue))

    init() {}

    // ── 派生：能力位 ────────────────────────────────────────────
    // 位定义见协议 §2：bit0 静态、bit1 动图、bit2 删除、bit3 内置存储

    /// 固件实现了静态 JPEG（bit0）
    var supportsStaticPhoto: Bool { (capability & 1) != 0 }
    /// 固件实现了动图 `.pgi`（bit1）—— **只是"固件支持"，不代表这台机器够快**
    var supportsAnimated: Bool { (capability & 2) != 0 }
    /// 支持删除（bit2）
    var supportsDelete: Bool { (capability & 4) != 0 }
    /// 支持内置存储 FFat（bit3）
    var supportsInternalStorage: Bool { (capability & 8) != 0 }
    /// 设备自己认为能跑 FULL 档（tier >= 2）
    var isFullTier: Bool { tier >= 2 }

    /**
     动图入口门控的 **①固件能力 + ②设备实测** 两层（协议 §7.1）。

     第三层"本次连接实测上传速率 ≥30 KB/s"由 UI 用 `BLEClient.photoMeasuredUploadRate` 判。

     - 坑：`animFPSMeasured == 0` 表示**从没播过**，不能当成"实测 0 fps 很慢"来报错，
       要给不同文案（见 `animationUnavailableReason`）。
     */
    var supportsAnimation: Bool {
        (capability & 2) != 0 && tier >= 2 && animFPSMeasured >= 8
    }

    /// 动图不可用的具体原因（UI 必须显示，不能只把入口藏起来）
    var animationUnavailableReason: String? {
        if supportsAnimation { return nil }
        if !supportsAnimated { return "此版固件不支持动图（只能传静态照片）" }
        if !isFullTier { return "设备未启用完整档位（只能传静态照片）" }
        if animFPSMeasured == 0 { return "设备还没播过动图，实测帧率未知" }
        return "设备实测动图只有 \(animFPSMeasured) fps（低于 8 fps），为流畅起见只提供静态照片"
    }

    /// 单张静态文件上限（**字节**，方便和 `Data.count` 比）
    var maxFileBytes: Int { max(0, maxBytes) * 1024 }
    /// 动图文件上限（**字节**）
    var animMaxFileBytes: Int { max(0, animMaxBytes) * 1024 }

    /// 该字段设备是否真的上报了（false = 当前值是默认值，不是设备说的）
    func reported(_ key: PhotoInfoKey) -> Bool { !missing.contains(key.rawValue) }
}

/// `PhotoInfo` 的字段 key（`rawValue` 与协议 JSON 的点分路径一致，便于 `missing` 记录）
enum PhotoInfoKey: String, CaseIterable {
    case version = "v"
    case tier = "tier"
    case capability = "cap"
    case maxPixel = "px"
    case maxBytes = "max_kb"
    case animPixel = "anim.px"
    case animFPS = "anim.fps"
    case animFrames = "anim.frames"
    case animMaxBytes = "anim.max_kb"
    case animFPSMeasured = "anim.fps_meas"
    case benchDecodeMs = "bench.dec_ms"
}

// MARK: - 状态（PhotoState）

/// 存储介质（协议 `stor`：0 未就绪 / 1 SD / 2 内置 FFat）
enum PhotoStorage: Equatable {
    case unknown
    case sd
    case internalFlash

    init(raw: Int) {
        switch raw {
        case 1: self = .sd
        case 2: self = .internalFlash
        default: self = .unknown
        }
    }

    /// 给用户看的存储名
    var displayName: String {
        switch self {
        case .sd: return "SD 卡"
        case .internalFlash: return "内置存储"
        case .unknown: return "存储未就绪"
        }
    }

    var isReady: Bool { self != .unknown }
}

/// `PhotoState`（READ + NOTIFY）：状态/设置。字段见协议 §3。
struct DevicePhotoState: Equatable {
    /// 当前播放的照片下标（0 起；`count == 0` 时设备给 -1）
    var currentIndex: Int = 0
    /// 照片数量
    var count: Int = 0
    /// `stor`：0 未就绪 / 1 SD / 2 内置 FFat
    var storage: Int = 0
    /// 可用空间（**KiB**）
    var freeKiB: Int = 0
    /// 自动轮播
    var slideshow: Bool = false
    /// 轮播间隔（秒）
    var intervalSeconds: Int = 0
    /// 像素位移（防烧屏）
    var shift: Bool = false
    /// 自动息屏（秒，0 = 关）
    var screenOffSeconds: Int = 0
    /// 0 空闲 / 1 正在接收上传
    var receiving: Bool = false
    /// 设备**没上报**的字段（`PhotoStateKey.rawValue`）
    var missing: Set<String> = Set(PhotoStateKey.allCases.map(\.rawValue))

    init() {}

    /// 存储介质枚举
    var storageKind: PhotoStorage { PhotoStorage(raw: storage) }
    /// 存储是否可用（`stor != 0`）
    var isStorageReady: Bool { storage != 0 }
    /// 可用空间（字节）
    var freeBytes: Int { max(0, freeKiB) * 1024 }
    /// 当前是否正在接收上传（设备侧）
    var isReceiving: Bool { receiving }

    /// 该字段设备是否真的上报了
    func reported(_ key: PhotoStateKey) -> Bool { !missing.contains(key.rawValue) }
}

/// `PhotoState` 的字段 key
enum PhotoStateKey: String, CaseIterable {
    case currentIndex = "cur"
    case count = "n"
    case storage = "stor"
    case freeKiB = "free_kb"
    case slideshow = "slide"
    case intervalSeconds = "intv"
    case shift = "shift"
    case screenOffSeconds = "soff_s"
    case receiving = "rx"
}

// MARK: - 清单项（PhotoEvt 的 `pf`）

/// 设备照片清单里的一项。字段见协议 §5 的 `pf`。
struct DevicePhoto: Identifiable, Equatable {
    /// 清单下标（`i`，0 起；播放顺序）
    var index: Int = 0
    /// 文件名（= id，如 `p00012.pgj`）
    var id: String = ""
    /// 文件总字节数（`b`）
    var bytes: Int = 0
    /// 帧数（`f`）：1 = 静态
    var frames: Int = 1
    /// 类型（`k`）：0 PGJ / 1 PGI
    var kind: Int = 0
    /// 宽（`w`）
    var width: Int = 0
    /// 高（`h`）
    var height: Int = 0
    /// 帧率（`fps`，静态为 0）
    var fps: Int = 0

    /// 默认值构造（测试/预览用；设备上报走 `DevicePhotoParser.parseItem`）
    init(index: Int = 0,
         id: String = "",
         bytes: Int = 0,
         frames: Int = 1,
         kind: Int = 0,
         width: Int = 0,
         height: Int = 0,
         fps: Int = 0) {
        self.index = index
        self.id = id
        self.bytes = bytes
        self.frames = frames
        self.kind = kind
        self.width = width
        self.height = height
        self.fps = fps
    }

    /// 是否动图（帧数 > 1）
    var isAnimated: Bool { frames > 1 }
    /// 类型枚举（未知值回退 `.pgj`）
    var kindValue: PhotoKind { PhotoKind(rawValue: kind) ?? .pgj }
    /// 文件大小（KB，界面显示用；1024 进制）
    var kilobytes: Int { max(0, bytes + 1023) / 1024 }
    /// 给用户看的一行描述（如 "静态照片 · 45 KB" / "动图 · 12 帧 · 8 fps · 210 KB"）
    var summaryText: String {
        if isAnimated {
            return "动图 · \(frames) 帧 · \(fps) fps · \(kilobytes) KB"
        }
        return "静态照片 · \(kilobytes) KB"
    }
}

// MARK: - 照片库快照

/// 照片页需要的一切（`@Published` 的整体快照，避免多个独立状态互相漂移）
struct DevicePhotoLibrary: Equatable {
    /// 设备能力/档位
    var info: DevicePhotoInfo = DevicePhotoInfo()
    /// 设备状态/设置
    var state: DevicePhotoState = DevicePhotoState()
    /// 清单项（**已按 `index` 升序**）
    var items: [DevicePhoto] = []
    /// 清单分页是否还没收完（`pq` 已到、`pz` 未到）
    var isLoading: Bool = false
    /// 最近一次错误文案（仅用于提示，不清空已有数据）
    var lastError: String?

    init() {}

    init(info: DevicePhotoInfo,
         state: DevicePhotoState,
         items: [DevicePhoto],
         isLoading: Bool = false,
         lastError: String? = nil) {
        self.info = info
        self.state = state
        self.items = items
        self.isLoading = isLoading
        self.lastError = lastError
    }

    /// 照片数量（**以清单为准**：`state.count` 可能滞后一个事件）
    var count: Int { items.count }
    /// 是否没有照片
    var isEmpty: Bool { items.isEmpty }
    /// 能否删除（能力位 + 有照片）
    var canDelete: Bool { info.supportsDelete && !items.isEmpty }
    /// 动图入口（①固件 + ②设备实测；第三层速率门控在 UI）
    var canAnimate: Bool { info.supportsAnimation }
    /// 是否已连上并读到过设备数据（`info.version > 0` 或有过上报）
    var hasDeviceReport: Bool { !info.missing.contains(PhotoInfoKey.version.rawValue) }
    /// 当前正在播放的那一项（下标对不上时返回 nil，不猜）
    var currentPhoto: DevicePhoto? {
        items.first { $0.index == state.currentIndex }
    }
}

// MARK: - 上传队列模型

/// 一次上传的状态（协议 §10；UI 直接 switch 这 7 个 case）
enum PhotoUploadState: Equatable {
    /// 排队中（还没开始 begin）
    case queued
    /// 正在传数据（含等 credit）
    case uploading
    /// 数据发完、已发 `end`，等设备校验提交（`done` / `err`）
    case verifying
    /// 设备已确认提交
    case done
    /// 失败（可重试；`errorText` 说原因）
    case failed
    /// 用户取消（已发 `abort`）
    case cancelled
    /// 断连等待重连（重连后从设备已落盘偏移续传）
    case waitingReconnect

    /// 终态（不会再变化）
    var isFinished: Bool {
        switch self {
        case .done, .failed, .cancelled: return true
        case .queued, .uploading, .verifying, .waitingReconnect: return false
        }
    }

    /// 正在进行（占着上传通道）
    var isActive: Bool {
        switch self {
        case .uploading, .verifying, .waitingReconnect: return true
        case .queued, .done, .failed, .cancelled: return false
        }
    }

    /// 是否可重试
    var canRetry: Bool {
        switch self {
        case .failed, .cancelled: return true
        default: return false
        }
    }

    /// 给用户看的一行状态
    var stateText: String {
        switch self {
        case .queued: return "排队中"
        case .uploading: return "正在上传"
        case .verifying: return "正在校验"
        case .done: return "已完成"
        case .failed: return "失败"
        case .cancelled: return "已取消"
        case .waitingReconnect: return "等待重连后续传"
        }
    }
}

/**
 一次上传请求（UI 在编码完成后构造）。

 - Note: `id` 由 UI 生成（建议 UUID 字符串）；`fileURL` 是**已落盘的最终文件**
   （`.pgj` / `.pgi`），续传时直接从文件里读，**不要重新编码**。
 */
struct PhotoUploadRequest: Equatable {
    /// 本地唯一 id
    var id: String
    /// 已编码好的文件（`tmp/photo-upload/<id>.pgj`）
    var fileURL: URL
    /// 类型
    var kind: PhotoKind
    /// 文件总字节数（必须 == 文件实际大小）
    var bytes: Int
    /// 整文件 CRC32（口径见 `PhotoProtocol.fileCRC32`）
    var crc32: UInt32
    /// JPEG/动图宽
    var width: Int
    /// JPEG/动图高
    var height: Int
    /// 帧数（静态 1）
    var frames: Int
    /// 帧率（静态 0）
    var fps: Int
    /// 展示名（如 "IMG_1234"），仅 UI 用
    var displayName: String? = nil

    /// 界面显示名（没有就回退文件名）
    var displayTitle: String {
        if let displayName, !displayName.isEmpty { return displayName }
        return fileURL.lastPathComponent
    }
}

/// 一次上传的进度快照（`@Published` 数组元素；每项独立，UI 可 diff）
struct PhotoUploadProgress: Identifiable, Equatable {
    /// 对应 `PhotoUploadRequest.id`
    var id: String
    /// 状态
    var state: PhotoUploadState = .queued
    /// 已交给 BLE 的字节数（含在途）
    var sentBytes: Int = 0
    /// 设备已**落盘**的字节数（进度以它为准，不能靠"发送完成"自欺）
    var ackedBytes: Int = 0
    /// 文件总字节数
    var totalBytes: Int = 0
    /// 实测速率（bytes/s，最近 2 s 滑动平均）
    var bytesPerSecond: Double = 0
    /// 失败原因（`state == .failed` 时有值）
    var errorText: String?
    /// 展示名
    var displayName: String?

    init(id: String,
         state: PhotoUploadState = .queued,
         sentBytes: Int = 0,
         ackedBytes: Int = 0,
         totalBytes: Int = 0,
         bytesPerSecond: Double = 0,
         errorText: String? = nil,
         displayName: String? = nil) {
        self.id = id
        self.state = state
        self.sentBytes = sentBytes
        self.ackedBytes = ackedBytes
        self.totalBytes = totalBytes
        self.bytesPerSecond = bytesPerSecond
        self.errorText = errorText
        self.displayName = displayName
    }

    /// 由请求构造一条"排队中"的初始进度
    init(request: PhotoUploadRequest) {
        self.id = request.id
        self.state = .queued
        self.sentBytes = 0
        self.ackedBytes = 0
        self.totalBytes = request.bytes
        self.bytesPerSecond = 0
        self.errorText = nil
        self.displayName = request.displayName ?? request.fileURL.lastPathComponent
    }

    /// 进度 0...1（按设备已落盘字节算）
    var fraction: Double {
        guard totalBytes > 0 else { return state == .done ? 1 : 0 }
        return min(1, max(0, Double(ackedBytes) / Double(totalBytes)))
    }

    /// 剩余字节
    var remainingBytes: Int { max(0, totalBytes - ackedBytes) }

    /// 剩余时间（秒）。速率未知或过低时按 8 KB/s 估（协议 §11.3），避免显示 ∞
    var etaSeconds: Double? {
        guard state == .uploading || state == .verifying else { return nil }
        guard remainingBytes > 0 else { return 0 }
        let rate = max(bytesPerSecond, 8 * 1024)
        return Double(remainingBytes) / rate
    }

    /// 是否终态
    var isFinished: Bool { state.isFinished }

    /// 是否正在进行
    var isActive: Bool { state.isActive }

    /// 给用户看的一行状态（含百分比）
    var stateText: String {
        switch state {
        case .uploading, .verifying:
            return "\(state.stateText) \(Int(fraction * 100))%"
        case .failed:
            return errorText ?? "失败"
        default:
            return state.stateText
        }
    }
}

// MARK: - 解析

/// 设备上报的 JSON → 模型。**坏 JSON / 错 `t` 一律返回 nil**（不猜，保持上一次状态）。
enum DevicePhotoParser {

    /**
     解析 `PhotoInfo`（协议 §2）。

     - 缺字段：用 0 默认值并记入 `missing`（**不是**把缺失当 0 上报）。
     - 错误：`t != "pi"`、不是 JSON 对象 → nil。
     */
    static func parseInfo(_ data: Data) -> DevicePhotoInfo? {
        guard let root = jsonObject(data), (root["t"] as? String) == "pi" else { return nil }
        var info = DevicePhotoInfo()
        var missing = Set<String>()

        func take(_ key: PhotoInfoKey, _ any: Any?) -> Int? {
            guard let v = PhotoProtocol.intValue(any) else {
                missing.insert(key.rawValue)
                return nil
            }
            return v
        }

        if let v = take(.version, root["v"]) { info.version = v }
        if let v = take(.tier, root["tier"]) { info.tier = v }
        if let v = take(.capability, root["cap"]) { info.capability = v }
        if let v = take(.maxPixel, root["px"]) { info.maxPixel = v }
        if let v = take(.maxBytes, root["max_kb"]) { info.maxBytes = v }

        // anim 子对象：整个缺失 → 6 个字段全记 missing；存在但不是对象同理
        let anim = root["anim"] as? [String: Any]
        if let v = take(.animPixel, anim?["px"]) { info.animPixel = v }
        if let v = take(.animFPS, anim?["fps"]) { info.animFPS = v }
        if let v = take(.animFrames, anim?["frames"]) { info.animFrames = v }
        if let v = take(.animMaxBytes, anim?["max_kb"]) { info.animMaxBytes = v }
        if let v = take(.animFPSMeasured, anim?["fps_meas"]) { info.animFPSMeasured = v }

        let bench = root["bench"] as? [String: Any]
        if let v = take(.benchDecodeMs, bench?["dec_ms"]) { info.benchDecodeMs = v }

        // 负值没有意义（协议里全是无符号语义）：保留原值但夹到 0，避免 UI 出现负数
        info.version = max(0, info.version)
        info.tier = max(0, info.tier)
        info.capability = max(0, info.capability)
        info.maxPixel = max(0, info.maxPixel)
        info.maxBytes = max(0, info.maxBytes)
        info.animPixel = max(0, info.animPixel)
        info.animFPS = max(0, info.animFPS)
        info.animFrames = max(0, info.animFrames)
        info.animMaxBytes = max(0, info.animMaxBytes)
        info.animFPSMeasured = max(0, info.animFPSMeasured)
        info.benchDecodeMs = max(0, info.benchDecodeMs)

        info.missing = missing
        return info
    }

    /**
     解析 `PhotoState`（协议 §3）。

     - 缺字段：0/false + 记入 `missing`。
     - `slide`/`shift`/`rx` 容错接受 0/1 与 true/false。
     */
    static func parseState(_ data: Data) -> DevicePhotoState? {
        guard let root = jsonObject(data), (root["t"] as? String) == "ps" else { return nil }
        var state = DevicePhotoState()
        var missing = Set<String>()

        func takeInt(_ key: PhotoStateKey, _ any: Any?) -> Int? {
            guard let v = PhotoProtocol.intValue(any) else {
                missing.insert(key.rawValue)
                return nil
            }
            return v
        }

        if let v = takeInt(.currentIndex, root["cur"]) { state.currentIndex = v }
        if let v = takeInt(.count, root["n"]) { state.count = max(0, v) }
        if let v = takeInt(.storage, root["stor"]) { state.storage = v }
        if let v = takeInt(.freeKiB, root["free_kb"]) { state.freeKiB = max(0, v) }
        if let v = takeInt(.slideshow, root["slide"]) { state.slideshow = v != 0 }
        if let v = takeInt(.intervalSeconds, root["intv"]) { state.intervalSeconds = max(0, v) }
        if let v = takeInt(.shift, root["shift"]) { state.shift = v != 0 }
        if let v = takeInt(.screenOffSeconds, root["soff_s"]) { state.screenOffSeconds = max(0, v) }
        if let v = takeInt(.receiving, root["rx"]) { state.receiving = v != 0 }

        state.missing = missing
        return state
    }

    /**
     解析一条清单项（`{"t":"pf",...}`，协议 §5）。

     - Returns: nil = 这不是 `pf` / 坏 JSON / `i` 非法 / 文件名非法 / `k` 非法。
       **文件名必须过白名单**：设备不可能给非法名，出现即说明协议或解析有问题，
       宁可不显示也不要在 App 侧拿它去拼路径。
     */
    static func parseItem(_ data: Data) -> DevicePhoto? {
        guard let root = jsonObject(data), (root["t"] as? String) == "pf" else { return nil }
        guard let index = PhotoProtocol.intValue(root["i"]), index >= 0 else { return nil }
        guard let id = root["id"] as? String, PhotoProtocol.isValidPhotoName(id) else { return nil }
        // `k` 缺省按 PGJ（0）；但**给了却不是数字**说明设备发坏了 → 整项丢弃，不猜
        let kindRaw: Int
        if let raw = root["k"] {
            guard let parsedKind = PhotoProtocol.intValue(raw) else { return nil }
            kindRaw = parsedKind
        } else {
            kindRaw = PhotoKind.pgj.rawValue
        }
        guard kindRaw == PhotoKind.pgj.rawValue || kindRaw == PhotoKind.pgi.rawValue else {
            return nil
        }

        var item = DevicePhoto()
        item.index = index
        item.id = id
        item.bytes = max(0, PhotoProtocol.intValue(root["b"]) ?? 0)
        item.frames = max(1, PhotoProtocol.intValue(root["f"]) ?? 1)
        item.kind = kindRaw
        item.width = max(0, PhotoProtocol.intValue(root["w"]) ?? 0)
        item.height = max(0, PhotoProtocol.intValue(root["h"]) ?? 0)
        item.fps = max(0, PhotoProtocol.intValue(root["fps"]) ?? 0)
        return item
    }

    /// JSON → 顶层对象；失败/非对象返回 nil
    private static func jsonObject(_ data: Data) -> [String: Any]? {
        (try? JSONSerialization.jsonObject(with: data)) as? [String: Any]
    }
}
