/**
 * PhotoTransfer.swift —— 单张照片上传的**纯逻辑状态机** + 文件分块读取
 *
 * 协议：`docs/photo-protocol.md` §6/§10（分片 / 流控 / 续传 / 错误码）。
 * 设计：`docs/photo-wireless-design.md` §8.3/§8.4/§10.3。
 *
 * ## 为什么把引擎抽成"纯逻辑 + 注入 I/O"
 *
 * 上传的坑几乎全在**时序**上（窗口、credit、看门狗、断连续传、CRC 重传），
 * 而它们只有在真机 + 慢连接下才会暴露。把"发命令 / 写分片 / 读文件 / 报进度"
 * 四件事注入进来之后，上面每条路径都能在 macOS 上用假时钟穷举
 * （见 `tools/test_photo_transfer.swift`），真机只剩"BLE 是否按预期回包"这一层。
 *
 * ## 状态机（与协议 §10 一一对应）
 *
 * ```
 * idle ─start─▶ waitingBeginAck ─go─▶ sending ─end─▶ verifying ─done─▶ done
 *                    │                  │                │
 *                    │                  ├─ credit ────────┘
 *                    │                  ├─ err{offset,want} → 从 want 续发
 *                    │                  ├─ err{crc} → 整份重传一次（begin, off=0）
 *                    │                  ├─ 断连 → waitingReconnect（重连 begin，off>0 续传）
 *                    │                  └─ 取消 → abort
 *                    └─ 6 s 无 go → 重发 begin
 * ```
 *
 * ## 三条纪律
 *
 * 1. **进度只认设备回执**（credit 的 `off`），`sentBytes` 只用来算在途窗口 ——
 *    靠"发送完成"报 100% 是自欺（BLE 链路层还没送达）。
 * 2. **在途字节 = sent - acked ≤ win**；`canSendWriteWithoutResponse == false` 时停手，
 *    由 `peripheralIsReady` 回调恢复（不要忙等）。
 * 3. **看门狗由定时器驱动**（`tick()`），不能只由写回调驱动 ——
 *    回执一丢就死锁，这是地图队列踩过的坑。
 */

import Foundation

// MARK: - 注入点

/**
 传输引擎的 I/O 接口。
 
 生产实现是 `BLEClient`（CoreBluetooth）；测试实现是内存 mock。
 所有方法都在**主线程**同步调用 —— 实现方自己负责把阻塞工作挪到后台队列。
 */
protocol PhotoTransferIO: AnyObject {
    /// 发送一条 `PhotoCtl` 命令（WRITE 带响应）
    func photoSend(_ command: PhotoCommand)

    /// 写一个数据分片（`PhotoData`，WRITE_NR；`frame` 含 4 B 偏移头）。
    /// - Returns: false = **当前不可写**（`canSendWriteWithoutResponse == false`），
    ///   引擎会停手，等 `resumeWritable()`。
    func photoWriteChunk(_ frame: Data) -> Bool

    /// 从本地文件同步读一段。
    /// - Returns: nil = 数据尚未就绪（实现方已在后台读取，完成后调 `chunkBecameAvailable()`）。
    func photoReadChunk(offset: Int, length: Int) -> Data?

    /// 预取提示（实现方可忽略）
    func photoPrefetch(offset: Int, length: Int)

    /// 单片 payload 上限 = `min(设备建议 chunk, iOS 写长 - 4)`
    func photoMaxChunkBytes() -> Int

    /// 进度回调（引擎内部已节流到 ~5 Hz，但仍建议实现方只更新必要的状态）
    func photoReport(_ progress: PhotoUploadProgress)
}

extension PhotoTransferIO {
    /// 预取默认空实现
    func photoPrefetch(offset: Int, length: Int) {}
}

// MARK: - 错误文案

extension PhotoErrorCode {
    /// 协议 §8.5 的"错误码 → 用户文案"一一对应（UI 直接显示，不要自己再拼）
    var userText: String {
        switch self {
        case .busy:
            return "仪表正在接收另一张照片，请稍后再试"
        case .no_space:
            return "仪表存储空间不足，请先删除一些照片"
        case .too_large:
            return "文件超过仪表的单张上限，请降低分辨率或质量后重试"
        case .unsupported:
            return "仪表不支持这种格式（需要 baseline JPEG；动图需设备支持）"
        case .crc:
            return "传输校验失败，已自动重传一次仍失败，请重试"
        case .offset:
            return "传输偏移异常，已尝试续传"
        case .io:
            return "仪表写入存储失败，请检查存储卡"
        case .not_ready:
            return "仪表存储未就绪（未检测到存储卡）"
        case .bad_arg:
            return "协议参数错误（内部错误，请反馈）"
        case .unknown:
            return "传输失败（设备未给出原因）"
        }
    }
}

// MARK: - 上传引擎

/// 单张照片的上传状态机。**不持有 CBPeripheral**，只通过 `PhotoTransferIO` 说话。
final class PhotoTransfer {

    /// 可调参数（测试里逐项改，生产用默认值）
    struct Config: Equatable {
        /// 设备没给 `chunk` 时的兜底值（协议建议 178）
        var chunkHint: Int = PhotoLimits.defaultChunkHint
        /// 设备没给 `win` 时的兜底值（协议 4096）
        var windowBytes: Int = PhotoLimits.defaultWindowBytes
        /// 6 s 无 credit / 无 go / 无 done → 自救（重发 begin 或 end）
        var creditTimeout: TimeInterval = PhotoLimits.creditTimeout
        /// 等本地文件数据超过这个时间 → 失败（不无限卡住）
        var dataTimeout: TimeInterval = 10
        /// 连续 begin 自救次数上限
        var maxBeginRestarts: Int = 8
        /// 重发 end 次数上限（等 done 时）
        var maxEndResends: Int = 2
        /// `err{crc}` 自动整份重传次数（协议 §10：一次）
        var maxCRCRestarts: Int = 1
        /// `err{busy}` 自动重试次数
        var maxBusyRetries: Int = 2
        /// `err{busy}` 后等多久再 begin
        var busyRetryDelay: TimeInterval = 2
        /// 进度回调最小间隔（高频回调会拖垮 SwiftUI）
        var progressInterval: TimeInterval = 0.2
        /// 速率滑动窗口（协议 §10：最近 2 s）
        var rateWindow: TimeInterval = 2

        init() {}
    }

    /// 引擎内部状态（比 UI 的 `PhotoUploadState` 多几个中间态）
    enum State: Equatable {
        case idle
        case waitingBeginAck
        case sending
        case verifying
        case done
        case failed
        case cancelled
        case waitingReconnect

        var isTerminal: Bool {
            switch self {
            case .done, .failed, .cancelled: return true
            case .idle, .waitingBeginAck, .sending, .verifying, .waitingReconnect: return false
            }
        }
    }

    // MARK: 公开只读状态（测试/调试用）

    let request: PhotoUploadRequest
    private(set) var state: State = .idle
    /// 已交给 BLE 的字节数（含在途）
    private(set) var sentBytes: Int = 0
    /// 设备已落盘的字节数（进度以它为准）
    private(set) var ackedBytes: Int = 0
    /// 当前分片 payload 上限
    private(set) var chunkBytes: Int = PhotoLimits.defaultChunkHint
    /// 当前在途窗口
    private(set) var windowBytes: Int = PhotoLimits.defaultWindowBytes
    /// 最近一次进度快照
    private(set) var progress: PhotoUploadProgress
    /// 自救计数（测试断言用：不重复 begin / 不无限重传）
    private(set) var beginRestartCount = 0
    private(set) var crcRestartCount = 0
    private(set) var busyRetryCount = 0
    private(set) var endResendCount = 0
    /// 是否**真的发过** `begin`（失败收尾时据此决定要不要发 `abort`：没开始的没必要打扰设备）
    private(set) var hasStartedTransfer = false

    // MARK: 内部

    private let io: PhotoTransferIO
    private let config: Config
    /// 时钟注入（测试用假时钟）
    var clock: () -> TimeInterval = { Date().timeIntervalSince1970 }

    private var writable = true
    private var waitingForDataSince: TimeInterval?
    private var lastEventAt: TimeInterval = 0
    private var lastPublishAt: TimeInterval = -Double.infinity
    private var retryBeginAt: TimeInterval?
    private var startedAt: TimeInterval = 0
    private var finishedAt: TimeInterval?
    private var errorText: String?
    private var rateSamples: [(t: TimeInterval, bytes: Int)] = []

    init(request: PhotoUploadRequest,
         io: PhotoTransferIO,
         config: Config = Config()) {
        self.request = request
        self.io = io
        self.config = config
        self.progress = PhotoUploadProgress(request: request)
    }

    // MARK: 生命周期

    /// 开始上传：校验参数 → 发 `begin` → 等 `go`
    func start() {
        guard state == .idle else { return }
        guard request.bytes > 0 else {
            fail(message: "上传文件为空")
            return
        }
        guard request.width >= 1, request.width <= PhotoLimits.pgjMaxDim,
              request.height >= 1, request.height <= PhotoLimits.pgjMaxDim else {
            fail(message: "照片尺寸非法（\(request.width)×\(request.height)）")
            return
        }
        if request.kind == .pgi {
            guard request.fps >= 1, request.frames > 1 else {
                fail(message: "动图参数非法（帧数/帧率）")
                return
            }
        }
        let now = clock()
        startedAt = now
        lastEventAt = now
        chunkBytes = max(1, min(config.chunkHint, io.photoMaxChunkBytes()))
        windowBytes = max(config.windowBytes, chunkBytes)
        sendBegin()
    }

    /// 是否占着上传通道
    var isActive: Bool { !state.isTerminal }

    /// 成功上传的实测速率（bytes/s；未完成返回 nil）。`BLEClient` 用它持久化分档门控。
    var overallBytesPerSecond: Double? {
        guard let finishedAt, finishedAt > startedAt else { return nil }
        return Double(request.bytes) / (finishedAt - startedAt)
    }

    /// 当前滑动窗口速率（bytes/s）
    var currentBytesPerSecond: Double {
        rate(at: clock())
    }

    // MARK: 事件入口

    /// 处理一条 `PhotoEvt`
    func handle(_ event: PhotoEvent) {
        let now = clock()
        switch event {
        case let .go(offset, chunk, window, _):
            handleGo(offset: offset, chunk: chunk, window: window, now: now)

        case let .credit(offset):
            handleCredit(offset: offset, now: now)

        case .done:
            handleDone(now: now)

        case let .error(code, _, want):
            handleError(code: code, want: want, now: now)

        case .changed, .info, .formatResult:
            // 清单/能力变化由 BLEClient 处理；引擎只关心传输本身
            break
        }
    }

    /// 定时器驱动（建议 0.5 s 一次）：看门狗 / 延迟重试
    func tick() {
        let now = clock()
        switch state {
        case .waitingBeginAck:
            if let at = retryBeginAt {
                if now >= at {
                    retryBeginAt = nil
                    sendBegin()
                }
                return
            }
            if now - lastEventAt > config.creditTimeout {
                beginRestartCount += 1
                guard beginRestartCount <= config.maxBeginRestarts else {
                    fail(message: "仪表没有响应上传请求，请检查连接后重试")
                    return
                }
                sendBegin()
            }

        case .sending:
            if let since = waitingForDataSince {
                if now - since > config.dataTimeout {
                    fail(message: "读取本地文件超时，请重新添加照片")
                }
                return
            }
            if !writable, now - lastEventAt > config.creditTimeout {
                // `peripheralIsReady` 万一丢了：自己恢复一次（写没发生，重试安全）
                writable = true
                pump()
                return
            }
            if sentBytes > ackedBytes, now - lastEventAt > config.creditTimeout {
                beginRestartCount += 1
                guard beginRestartCount <= config.maxBeginRestarts else {
                    fail(message: "仪表长时间没有确认接收进度，请重试")
                    return
                }
                // 重新 begin：设备回 `go.off` = 已落盘偏移（续传）
                sendBegin()
            }

        case .verifying:
            if now - lastEventAt > config.creditTimeout {
                endResendCount += 1
                guard endResendCount <= config.maxEndResends else {
                    fail(message: "仪表未确认提交结果，请重试")
                    return
                }
                io.photoSend(.end)
                lastEventAt = now
            }

        case .idle, .done, .failed, .cancelled, .waitingReconnect:
            break
        }
    }

    /// BLE 恢复可写（`peripheralIsReady(toSendWriteWithoutResponse:)`）
    func resumeWritable() {
        guard !state.isTerminal else { return }
        writable = true
        pump()
    }

    /// 周期校验"现在能不能写"。
    ///
    /// 为什么不能只靠 `peripheralIsReady`：真机实测上传会停在 0 B（go 已收到、
    /// canSend 曾为 false），而该回调在部分时序下没有到达 → 引擎永远不 pump。
    /// BLEClient 的 100 ms 定时器用 `peripheral.canSendWriteWithoutResponse`
    /// 主动喂一次，持续恢复发送；窗口/credit 仍由引擎自己把关。
    func refreshWritable(_ canWrite: Bool) {
        guard !state.isTerminal, state == .sending else { return }
        if canWrite {
            writable = true
            pump()
        }
    }

    /// 本地文件数据就绪（后台读取完成）
    func chunkBecameAvailable() {
        guard !state.isTerminal else { return }
        waitingForDataSince = nil
        pump()
    }

    /// 断连：保留已落盘偏移，等重连后续传
    func markDisconnected() {
        guard !state.isTerminal, state != .idle else { return }
        state = .waitingReconnect
        retryBeginAt = nil
        waitingForDataSince = nil
        publish(force: true)
    }

    /// 重连后恢复：重新 begin（设备回 `go.off>0` → 自动续传）
    func restartAfterReconnect() {
        guard state == .waitingReconnect else { return }
        writable = true
        waitingForDataSince = nil
        retryBeginAt = nil
        beginRestartCount = 0
        sendBegin()
    }

    /// 用户取消：发 `abort`（设备删 `.part`）
    func cancel() {
        guard !state.isTerminal, state != .idle else { return }
        io.photoSend(.abort)
        state = .cancelled
        errorText = nil
        publish(force: true)
    }

    // MARK: 事件处理

    private func handleGo(offset: Int, chunk: Int, window: Int, now: TimeInterval) {
        guard state == .waitingBeginAck || state == .sending || state == .waitingReconnect else {
            return
        }
        if chunk > 0 { chunkBytes = max(1, min(chunk, io.photoMaxChunkBytes())) }
        // 窗口至少要能放下一片，否则永远发不出去
        if window > 0 { windowBytes = max(window, chunkBytes) }
        else { windowBytes = max(windowBytes, chunkBytes) }

        // 设备给的 off 是**权威**的已落盘偏移（续传时 > 0）
        let off = min(max(0, offset), request.bytes)
        sentBytes = off
        ackedBytes = off
        beginRestartCount = 0
        waitingForDataSince = nil
        retryBeginAt = nil
        lastEventAt = now
        state = .sending
        resetRateSamples(at: now)
        publish(force: true)
        pump()
    }

    private func handleCredit(offset: Int, now: TimeInterval) {
        guard state == .sending else { return }
        // 防御：越界/回退的 credit 一律忽略（设备实现异常时不要把进度搞乱）
        let newAcked = min(max(offset, 0), sentBytes)
        guard newAcked > ackedBytes else { return }
        ackedBytes = newAcked
        lastEventAt = now
        recordRateSample(at: now)
        publish()
        pump()
    }

    private func handleDone(now: TimeInterval) {
        guard state == .verifying || state == .sending else { return }
        sentBytes = request.bytes
        ackedBytes = request.bytes
        finishedAt = now
        recordRateSample(at: now)
        state = .done
        errorText = nil
        publish(force: true)
    }

    private func handleError(code: PhotoErrorCode, want: Int?, now: TimeInterval) {
        switch code {
        case .offset:
            // 设备说"你发的偏移不对，我等你从 want 发" —— 从 want 续传。
            // `want > sent` 是设备实现异常（期望一个我们没发过的偏移）：直接失败，
            // 否则会跳着发、拼出一张损坏的文件。
            // `verifying` 期间也可能收到（设备拒绝 end 因为收到的字节没到齐）→ 退回发送态续传。
            guard let want, want >= 0, want <= sentBytes,
                  state == .sending || state == .verifying else {
                fail(code: code)
                return
            }
            state = .sending
            sentBytes = want
            ackedBytes = min(ackedBytes, want)
            lastEventAt = now
            resetRateSamples(at: now)
            publish(force: true)
            pump()

        case .crc:
            // 协议 §10：自动整份重传一次
            guard crcRestartCount < config.maxCRCRestarts else {
                fail(code: code)
                return
            }
            crcRestartCount += 1
            sentBytes = 0
            ackedBytes = 0
            beginRestartCount = 0
            resetRateSamples(at: now)
            sendBegin()

        case .busy:
            guard busyRetryCount < config.maxBusyRetries else {
                fail(code: code)
                return
            }
            busyRetryCount += 1
            state = .waitingBeginAck
            retryBeginAt = now + config.busyRetryDelay
            lastEventAt = now
            publish(force: true)

        default:
            fail(code: code)
        }
    }

    // MARK: 发送 / 泵

    private func sendBegin() {
        let now = clock()
        hasStartedTransfer = true
        io.photoSend(.begin(kind: request.kind,
                            bytes: request.bytes,
                            crc: request.crc32,
                            width: request.width,
                            height: request.height,
                            frames: request.frames,
                            fps: request.fps))
        state = .waitingBeginAck
        lastEventAt = now
        waitingForDataSince = nil
        publish(force: true)
    }

    /// 往窗口里灌数据；每次 credit / 可写恢复 / 数据就绪都会重新进入
    private func pump() {
        let now = clock()
        guard state == .sending, writable else { return }

        while sentBytes < request.bytes {
            let inFlight = sentBytes - ackedBytes
            let available = windowBytes - inFlight
            guard available > 0 else { return } // 等 credit
            let want = min(chunkBytes, min(available, request.bytes - sentBytes))
            guard want > 0 else { return }

            guard let payload = io.photoReadChunk(offset: sentBytes, length: want),
                  payload.count == want else {
                // 本地文件还没就绪：等 chunkBecameAvailable()（有 dataTimeout 兜底）
                if waitingForDataSince == nil {
                    waitingForDataSince = now
                }
                return
            }
            waitingForDataSince = nil

            let frame = PhotoProtocol.makeChunkFrame(offset: sentBytes, payload: payload)
            guard io.photoWriteChunk(frame) else {
                // BLE 写缓冲满：停手，等 peripheralIsReady
                writable = false
                return
            }
            sentBytes += payload.count
            lastEventAt = now
            io.photoPrefetch(offset: sentBytes, length: max(windowBytes, 4096))
            publish()
        }

        // 全部数据已交给 BLE → 请求校验提交（等 done）
        if sentBytes >= request.bytes {
            io.photoSend(.end)
            state = .verifying
            lastEventAt = now
            publish(force: true)
        }
    }

    // MARK: 失败

    private func fail(code: PhotoErrorCode) {
        fail(message: code.userText)
    }

    private func fail(message: String) {
        state = .failed
        errorText = message
        publish(force: true)
    }

    // MARK: 速率

    private func resetRateSamples(at now: TimeInterval) {
        rateSamples = [(now, ackedBytes)]
    }

    private func recordRateSample(at now: TimeInterval) {
        if rateSamples.isEmpty { rateSamples = [(now, ackedBytes)] }
        rateSamples.append((now, ackedBytes))
        while rateSamples.count > 2, now - rateSamples[0].t > config.rateWindow {
            rateSamples.removeFirst()
        }
    }

    private func rate(at now: TimeInterval) -> Double {
        guard let first = rateSamples.first, let last = rateSamples.last else { return 0 }
        let dt = last.t - first.t
        guard dt > 0.05 else { return progress.bytesPerSecond }
        let db = last.bytes - first.bytes
        guard db > 0 else { return 0 }
        return Double(db) / dt
    }

    // MARK: 进度

    /// 把引擎状态映射成 UI 的 `PhotoUploadState`
    private func uiState() -> PhotoUploadState {
        switch state {
        case .idle, .waitingBeginAck, .sending: return .uploading
        case .verifying: return .verifying
        case .done: return .done
        case .failed: return .failed
        case .cancelled: return .cancelled
        case .waitingReconnect: return .waitingReconnect
        }
    }

    private func publish(force: Bool = false) {
        let now = clock()
        var p = progress
        p.state = uiState()
        p.sentBytes = sentBytes
        p.ackedBytes = ackedBytes
        p.totalBytes = request.bytes
        p.bytesPerSecond = rate(at: now)
        if p.state == .failed { p.errorText = errorText }
        else if p.state == .done || p.state == .cancelled { p.errorText = nil }
        progress = p

        let changed = force || now - lastPublishAt >= config.progressInterval
        guard changed else { return }
        lastPublishAt = now
        io.photoReport(p)
    }
}

// MARK: - 本地文件分块读取

/**
 从已编码的文件里按需读块（**后台串行队列** + 4 KB 读前缓存）。
 
 为什么要读前缓存：协议分片只有 ~174 B，如果每个分片都做一次 `seek+read`，
 上传期间会做上千次 syscall，全部落在 BLE 回调线程上。
 这里一次读 4 KB（≈23 个分片），把 IO 次数降到 1/23。
 
 线程模型：
   - `read(at:)` 在调用线程（主线程）同步返回缓存命中；
   - 未命中时返回 nil，并在内部串行队列读文件，完成后在主队列调 `onReady`；
   - 所有 `FileHandle` 访问只发生在内部队列上。
 */
final class PhotoFileChunkSource {

    let fileURL: URL
    /// 文件大小（打开时快照）
    let size: Int
    /// 一次读多少字节进缓存（默认 4 KB）
    var windowBytes: Int = 4096
    /// 后台读完后在主队列回调（用于 `PhotoTransfer.chunkBecameAvailable()`）
    var onReady: (() -> Void)?

    private let queue = DispatchQueue(label: "car-dashboard.photo-file-source", qos: .userInitiated)
    private let lock = NSLock()
    private var handle: FileHandle?
    private var cacheStart = -1
    private var cache = Data()
    private var loading = false
    private var closed = false

    /// 打开文件；文件不存在/为空/无权限 → nil
    init?(fileURL: URL) {
        let path = fileURL.path
        guard let attrs = try? FileManager.default.attributesOfItem(atPath: path),
              let size = (attrs[.size] as? NSNumber)?.intValue,
              size > 0,
              let handle = try? FileHandle(forReadingFrom: fileURL) else {
            return nil
        }
        self.fileURL = fileURL
        self.size = size
        self.handle = handle
    }

    deinit {
        lock.lock()
        closed = true
        handle?.closeFile()
        handle = nil
        lock.unlock()
    }

    /// 同步读：命中缓存返回数据；未命中返回 nil 并触发后台预取
    func read(offset: Int, length: Int) -> Data? {
        guard length > 0, offset >= 0, offset < size else { return nil }
        lock.lock()
        defer { lock.unlock() }
        if closed { return nil }
        if cacheStart >= 0,
           offset >= cacheStart,
           offset + length <= cacheStart + cache.count {
            let start = offset - cacheStart
            return cache.subdata(in: start..<(start + length))
        }
        scheduleLoadLocked(offset: offset)
        return nil
    }

    /// 预取（可在命中也调用：只在不命中时触发）
    func prefetch(offset: Int, length: Int) {
        guard offset >= 0, offset < size else { return }
        lock.lock()
        defer { lock.unlock() }
        if closed { return }
        if cacheStart >= 0,
           offset >= cacheStart,
           offset < cacheStart + cache.count {
            return
        }
        scheduleLoadLocked(offset: offset)
    }

    /// 关闭文件（上传结束/取消时调）
    func close() {
        lock.lock()
        closed = true
        cache = Data()
        cacheStart = -1
        let h = handle
        handle = nil
        lock.unlock()
        h?.closeFile()
    }

    /// 调用方需持有 lock
    private func scheduleLoadLocked(offset: Int) {
        guard !loading, !closed else { return }
        loading = true
        let start = max(0, offset)
        queue.async { [weak self] in
            guard let self else { return }
            self.lock.lock()
            if self.closed || self.handle == nil {
                self.loading = false
                self.lock.unlock()
                return
            }
            let handle = self.handle
            let size = self.size
            let window = max(1, self.windowBytes)
            self.lock.unlock()

            let len = max(1, min(window, size - start))
            var data = Data()
            if let handle {
                do {
                    try handle.seek(toOffset: UInt64(start))
                    data = handle.readData(ofLength: len)
                } catch {
                    data = Data()
                }
            }

            self.lock.lock()
            if !self.closed, !data.isEmpty {
                self.cache = data
                self.cacheStart = start
            }
            self.loading = false
            let callback = self.onReady
            self.lock.unlock()
            if let callback {
                DispatchQueue.main.async { callback() }
            }
        }
    }
}
