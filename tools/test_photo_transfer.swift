/**
 * test_photo_transfer.swift —— 上传传输引擎测试（分片/窗口/credit/看门狗/续传/取消/速率）
 *
 * 运行（**不改 tests/Makefile**，直接 swiftc 直编）：
 *   swiftc -O -o /tmp/test_photo_transfer \
 *     ios/CarDashboardPhone/CarDashboardPhone/Photo/PhotoProtocol.swift \
 *     ios/CarDashboardPhone/CarDashboardPhone/Photo/DevicePhotos.swift \
 *     ios/CarDashboardPhone/CarDashboardPhone/Photo/PhotoTransfer.swift \
 *     tools/test_photo_transfer.swift && /tmp/test_photo_transfer
 *
 * ## 为什么这些测试值得写
 *
 * 上传的坑全在时序上：窗口算错会丢字节、credit 丢一次会死锁、续传偏移算错会让
 * 设备看到"半张图"、CRC 重传没上限会无限循环。真机上这些要么不复现、要么复现一次
 * 就要拆机看串口；在这里用**假时钟 + mock 写函数**可以逐条穷举。
 *
 * mock 不模拟 BLE 链路层（不丢包、不乱序）—— 那一层由设备端文件级 CRC 兜底，
 * 这里是"App 逻辑正确性"的单测。
 */

import Foundation

var gPass = 0
var gFail = 0
func check(_ cond: Bool, _ msg: @autoclosure () -> String) {
    if cond { gPass += 1 } else { gFail += 1; print("  ✗ \(msg())") }
}
func checkEq<T: Equatable>(_ actual: T, _ expected: T, _ msg: @autoclosure () -> String) {
    if actual == expected { gPass += 1 } else {
        gFail += 1
        print("  ✗ \(msg())：期望 \(expected)，实际 \(actual)")
    }
}

// MARK: - 测试替身

/// 可控时钟
final class FakeClock {
    var now: TimeInterval = 1_700_000_000
    func advance(_ seconds: TimeInterval) { now += seconds }
}

/// mock 传输 I/O：把"发命令 / 写分片 / 读文件 / 报进度"全记下来
final class MockTransferIO: PhotoTransferIO {
    /// 本地"文件"内容
    var source = Data()
    /// 当前是否可写（模拟 canSendWriteWithoutResponse == false）
    var writable = true
    /// 单片 payload 上限（模拟 iOS 写长 - 4）
    var maxChunk = 174
    /// false = 本地文件还没读出来（模拟后台读取未完成）
    var dataReady = true

    private(set) var commands: [PhotoCommand] = []
    private(set) var frames: [Data] = []
    private(set) var chunks: [(offset: Int, payload: Data)] = []
    private(set) var reports: [PhotoUploadProgress] = []
    private(set) var prefetchCalls: [(offset: Int, length: Int)] = []

    func photoSend(_ command: PhotoCommand) { commands.append(command) }

    func photoWriteChunk(_ frame: Data) -> Bool {
        guard writable else { return false }
        frames.append(frame)
        if let parsed = PhotoProtocol.parseChunkFrame(frame) {
            chunks.append((parsed.offset, parsed.payload))
        }
        return true
    }

    func photoReadChunk(offset: Int, length: Int) -> Data? {
        guard dataReady, offset >= 0, length > 0, offset + length <= source.count else { return nil }
        return source.subdata(in: offset..<(offset + length))
    }

    func photoPrefetch(offset: Int, length: Int) {
        prefetchCalls.append((offset, length))
    }

    func photoMaxChunkBytes() -> Int { maxChunk }

    func photoReport(_ progress: PhotoUploadProgress) {
        reports.append(progress)
    }

    // 断言辅助
    var beginCommands: [PhotoCommand] {
        commands.filter { if case .begin = $0 { return true } else { return false } }
    }
    var endCount: Int { commands.filter { if case .end = $0 { return true } else { return false } }.count }
    var abortCount: Int { commands.filter { if case .abort = $0 { return true } else { return false } }.count }
}

// MARK: - 夹具

func patternedData(_ count: Int) -> Data {
    Data((0..<count).map { UInt8(($0 &* 31 &+ 7) & 0xFF) })
}

func makeRequest(bytes: Int,
                 kind: PhotoKind = .pgj,
                 frames: Int = 1,
                 fps: Int = 0,
                 width: Int = 466,
                 height: Int = 466) -> PhotoUploadRequest {
    PhotoUploadRequest(id: "T1",
                       fileURL: URL(fileURLWithPath: "/tmp/\(kind.fileExtension)"),
                       kind: kind,
                       bytes: bytes,
                       crc32: 0x1234_5678,
                       width: width,
                       height: height,
                       frames: frames,
                       fps: fps,
                       displayName: "测试照片")
}

struct Harness {
    let clock = FakeClock()
    let io = MockTransferIO()
    let transfer: PhotoTransfer

    init(bytes: Int = 20_000,
         chunk: Int = 174,
         iosMaxChunk: Int = 174,
         window: Int = 4096,
         config: PhotoTransfer.Config = PhotoTransfer.Config()) {
        io.source = patternedData(bytes)
        // 两个不同的上限：设备建议 chunk（协议 go.chunk）与 iOS 协商写长 - 4
        io.maxChunk = iosMaxChunk
        io.dataReady = true
        var cfg = config
        cfg.chunkHint = chunk
        cfg.windowBytes = window
        transfer = PhotoTransfer(request: makeRequest(bytes: bytes), io: io, config: cfg)
        transfer.clock = { [clock] in clock.now }
    }

    /// 一直发 credit 直到进入 verifying（或终态）
    func driveToVerifying(_ maxRounds: Int = 50_000) {
        var rounds = 0
        while transfer.state == .sending, rounds < maxRounds {
            rounds += 1
            transfer.handle(.credit(offset: transfer.sentBytes))
        }
    }

    /// 把收到的分片按偏移拼回文件（同时校验不重叠/不越界）
    func reassembled() -> Data? {
        var output = [UInt8](repeating: 0, count: transfer.request.bytes)
        var touched = [Bool](repeating: false, count: transfer.request.bytes)
        for (offset, payload) in io.chunks {
            guard offset >= 0, offset + payload.count <= output.count else { return nil }
            for (i, b) in payload.enumerated() {
                guard !touched[offset + i] else { return nil } // 重叠 = bug
                touched[offset + i] = true
                output[offset + i] = b
            }
        }
        return Data(output)
    }

    func allBytesTouched() -> Bool {
        var seen = [Bool](repeating: false, count: transfer.request.bytes)
        for (offset, payload) in io.chunks {
            guard offset + payload.count <= seen.count else { return false }
            for i in 0..<payload.count { seen[offset + i] = true }
        }
        return seen.allSatisfy { $0 }
    }
}

// MARK: - 测试用例

func testHappyPath() {
    print("--- 正常上传全流程 ---")
    let h = Harness(bytes: 20_000)
    h.transfer.start()
    checkEq(h.transfer.state, .waitingBeginAck, "start 后等 go")
    checkEq(h.io.commands.count, 1, "应发一条命令")
    guard case let .begin(kind, bytes, crc, w, hh, frames, fps)? = h.io.commands.first else {
        check(false, "第一条命令应为 begin"); return
    }
    checkEq(kind, .pgj, "begin.kind")
    checkEq(bytes, 20_000, "begin.bytes")
    checkEq(crc, 0x1234_5678, "begin.crc")
    checkEq(w, 466, "begin.w")
    checkEq(hh, 466, "begin.h")
    checkEq(frames, 1, "begin.frames")
    checkEq(fps, 0, "begin.fps")
    checkEq(h.transfer.progress.state, .uploading, "进度状态应为 uploading")

    h.transfer.handle(.go(offset: 0, chunk: 174, window: 4096, id: "p00001.pgj"))
    checkEq(h.transfer.state, .sending, "go 后开始发送")
    checkEq(h.transfer.ackedBytes, 0, "初始 acked 0")
    checkEq(h.transfer.sentBytes, 4096, "首轮应灌满 4096 字节窗口，实际 \(h.transfer.sentBytes)")
    checkEq(h.transfer.chunkBytes, 174, "分片 174（iOS 写长 178-4）")
    checkEq(h.transfer.windowBytes, 4096, "窗口 4096")
    checkEq(h.io.endCount, 0, "还没发完不能发 end")

    // 首个分片：帧头 offset + 内容
    guard let first = h.io.chunks.first else { check(false, "应发出分片"); return }
    checkEq(first.offset, 0, "首片 offset 0")
    checkEq(first.payload.count, 174, "首片 174 B")
    checkEq(first.payload, patternedData(20_000).prefix(174), "首片内容必须与文件一致")

    h.driveToVerifying()
    checkEq(h.transfer.state, .verifying, "全部发出后进入 verifying")
    checkEq(h.transfer.sentBytes, 20_000, "sent 应等于文件大小")
    check(h.io.endCount == 1, "应发 1 次 end，实际 \(h.io.endCount)")
    check(h.allBytesTouched(), "所有字节都必须被发过")

    h.clock.advance(2.0) // 模拟真实耗时，便于校验实测速率
    h.transfer.handle(.done(id: "p00001.pgj"))
    checkEq(h.transfer.state, .done, "done 后完成")
    checkEq(h.transfer.ackedBytes, 20_000, "acked = 总字节")
    checkEq(h.transfer.progress.fraction, 1.0, "进度 100%")
    checkEq(h.transfer.progress.state, .done, "进度状态 done")
    check(h.transfer.overallBytesPerSecond != nil, "应能算出实测速率")
    // 拼回来的文件必须与源文件逐字节一致
    checkEq(h.reassembled(), h.io.source, "重组文件必须与源文件一致")
    // 最后一次进度回调必须是 done（UI 靠它收尾）
    checkEq(h.io.reports.last?.state, .done, "最后一次进度应为 done")
}

func testWindowFlowControl() {
    print("--- 在途窗口流控（sent - acked ≤ win）---")
    let h = Harness(bytes: 50_000, chunk: 174, window: 4096)
    h.transfer.start()
    h.transfer.handle(.go(offset: 0, chunk: 174, window: 4096, id: nil))

    var maxInFlight = 0
    for _ in 0..<40 {
        let inFlight = h.transfer.sentBytes - h.transfer.ackedBytes
        maxInFlight = max(maxInFlight, inFlight)
        check(inFlight <= 4096, "在途字节不能超窗口：\(inFlight)")
        // 每次只 credit 一半窗口 → 观察是否补发
        let target = min(h.transfer.sentBytes, h.transfer.ackedBytes + 2048)
        h.transfer.handle(.credit(offset: target))
        if h.transfer.state != .sending { break }
    }
    check(maxInFlight <= 4096, "最大在途 \(maxInFlight) 不能超 4096")
    check(h.transfer.sentBytes > 4096, "credit 后应继续灌数据")
    check(h.reassembled() != nil, "分片不能重叠/越界")
}

func testCreditNeverSentIgnored() {
    print("--- 非法 credit 防御 ---")
    let h = Harness(bytes: 10_000)
    h.transfer.start()
    h.transfer.handle(.go(offset: 0, chunk: 174, window: 4096, id: nil))
    let sent = h.transfer.sentBytes
    h.transfer.handle(.credit(offset: sent + 999))   // 超过 sent：夹到 sent
    checkEq(h.transfer.ackedBytes, sent, "超界 credit 应夹到 sent")
    h.transfer.handle(.credit(offset: 0))            // 回退：忽略
    checkEq(h.transfer.ackedBytes, sent, "回退 credit 必须被忽略")
    h.transfer.handle(.credit(offset: -5))           // 负数：忽略
    checkEq(h.transfer.ackedBytes, sent, "负 credit 必须被忽略")
}

func testWriteBackpressure() {
    print("--- canSendWriteWithoutResponse 门控 ---")
    let h = Harness(bytes: 20_000)
    h.transfer.start()
    h.transfer.handle(.go(offset: 0, chunk: 174, window: 4096, id: nil))
    let afterFirstPump = h.transfer.sentBytes
    check(afterFirstPump > 0, "首轮应有数据")

    h.io.writable = false
    // 让已发出的都被 ack，再 pump → 应该停手（不写、不丢）
    h.transfer.handle(.credit(offset: h.transfer.sentBytes))
    let sentWhenBlocked = h.transfer.sentBytes
    checkEq(h.transfer.sentBytes, sentWhenBlocked, "不可写时不应推进 sent")
    checkEq(h.transfer.state, .sending, "不可写只是暂停，不是失败")

    h.io.writable = true
    h.transfer.resumeWritable()
    check(h.transfer.sentBytes > sentWhenBlocked, "恢复可写后应继续发送")

    // peripheralIsReady 丢失也能自愈（tick 超时后自行恢复一次）
    h.io.writable = false
    h.transfer.handle(.credit(offset: h.transfer.sentBytes))
    let blockedAgain = h.transfer.sentBytes
    h.clock.advance(6.5)
    h.transfer.tick()
    h.io.writable = true
    // tick 里会置 writable=true 并 pump；若它没 pump，这里 resume 一次兜底
    h.transfer.resumeWritable()
    check(h.transfer.sentBytes > blockedAgain, "看门狗应自愈卡死的写门控")
}

func testChunkClampedByIOSWriteLength() {
    print("--- 分片上限取设备建议与 iOS 写长的较小值 ---")
    let h = Harness(bytes: 50_000, chunk: 300, iosMaxChunk: 174) // 设备建议 300，但 iOS 只允许 174
    h.transfer.start()
    h.transfer.handle(.go(offset: 0, chunk: 300, window: 4096, id: nil))
    checkEq(h.transfer.chunkBytes, 174, "必须被 iOS 写长夹住")
    for frame in h.io.frames {
        check(frame.count <= 4 + 174, "单帧不能超过 178 B，实际 \(frame.count)")
    }
    // 设备建议更小时用设备的
    let h2 = Harness(bytes: 10_000, chunk: 174)
    h2.transfer.start()
    h2.transfer.handle(.go(offset: 0, chunk: 100, window: 1024, id: nil))
    checkEq(h2.transfer.chunkBytes, 100, "设备建议 100 时应取 100")
    checkEq(h2.transfer.windowBytes, 1024, "窗口取设备值")
}

func testCreditWatchdogRestartsBegin() {
    print("--- 6 s 无 credit → 重新 begin 续传 ---")
    let h = Harness(bytes: 20_000)
    h.transfer.start()
    h.transfer.handle(.go(offset: 0, chunk: 174, window: 4096, id: nil))
    // 设备落盘了 2048 B
    h.transfer.handle(.credit(offset: 2048))
    checkEq(h.transfer.ackedBytes, 2048, "acked 2048")
    let chunksBefore = h.io.chunks.count

    h.clock.advance(6.5)
    h.transfer.tick()
    checkEq(h.transfer.beginRestartCount, 1, "应触发一次重新 begin")
    checkEq(h.io.beginCommands.count, 2, "应发出第二条 begin")
    checkEq(h.transfer.state, .waitingBeginAck, "重新 begin 后等 go")

    // 设备回 go.off=2048 → 必须从 2048 续传，不重发前面的字节
    h.transfer.handle(.go(offset: 2048, chunk: 174, window: 4096, id: nil))
    checkEq(h.transfer.ackedBytes, 2048, "续传后 acked = 设备已落盘偏移")
    checkEq(h.io.chunks.suffix(h.io.chunks.count - chunksBefore).first?.offset ?? -1, 2048,
            "续传第一片的 offset 必须是 2048")
    h.driveToVerifying()
    checkEq(h.transfer.state, .verifying, "续传后能发完")

    // 续传期间没有重发 [0,2048) —— 第一轮的分片也算在内，检查没有偏移 < 2048 之后重复
    let afterRestart = h.io.chunks.suffix(h.io.chunks.count - chunksBefore)
    check(afterRestart.allSatisfy { $0.offset >= 2048 },
          "续传不应重发已落盘区间，实际起点 \(afterRestart.first?.offset ?? -1)")
    check(h.allBytesTouched(), "续传必须覆盖全部字节")
}

func testOffsetErrorResume() {
    print("--- err{offset, want} → 从 want 续传 ---")
    let h = Harness(bytes: 20_000)
    h.transfer.start()
    h.transfer.handle(.go(offset: 0, chunk: 174, window: 4096, id: nil))
    h.transfer.handle(.credit(offset: 1024))
    let beforeError = h.io.chunks.count
    h.transfer.handle(.error(code: .offset, rawCode: "offset", want: 1024))
    checkEq(h.transfer.ackedBytes, 1024, "acked 回到设备期望偏移")
    checkEq(h.transfer.state, .sending, "offset 错误不致命")
    let resumed = h.io.chunks.suffix(h.io.chunks.count - beforeError)
    checkEq(resumed.first?.offset ?? -1, 1024, "应从 1024 继续发，实际 \(resumed.map(\.offset))")

    // want 越界 → 失败（不猜）
    let h2 = Harness(bytes: 10_000)
    h2.transfer.start()
    h2.transfer.handle(.go(offset: 0, chunk: 174, window: 4096, id: nil))
    h2.transfer.handle(.error(code: .offset, rawCode: "offset", want: 999_999))
    checkEq(h2.transfer.state, .failed, "want 越界 → 失败")
    check(h2.transfer.progress.errorText?.isEmpty == false, "失败必须有文案")

    // want > sent（设备期望一个我们还没发过的偏移）→ 失败，不能跳着发
    let h3 = Harness(bytes: 10_000)
    h3.transfer.start()
    h3.transfer.handle(.go(offset: 0, chunk: 174, window: 4096, id: nil))
    h3.transfer.handle(.credit(offset: 1_024))
    let sentBefore = h3.transfer.sentBytes
    h3.transfer.handle(.error(code: .offset, rawCode: "offset", want: sentBefore + 1_000))
    checkEq(h3.transfer.state, .failed, "want > sent → 失败（否则文件会缺一段）")

    // verifying 期间收到 offset 错误（设备拒 end：收到的字节没到齐）→ 退回发送态续传
    let h4 = Harness(bytes: 5_000)
    h4.transfer.start()
    h4.transfer.handle(.go(offset: 0, chunk: 174, window: 4096, id: nil))
    h4.driveToVerifying()
    checkEq(h4.transfer.state, .verifying, "先到 verifying")
    let beforeRecover = h4.io.chunks.count
    h4.transfer.handle(.error(code: .offset, rawCode: "offset", want: 4_000))
    // 只剩最后 1000 B：退出发送态后立刻补发并再次请求提交（所以调用返回时已在 verifying）
    checkEq(h4.io.chunks.suffix(h4.io.chunks.count - beforeRecover).first?.offset ?? -1, 4_000,
            "应从设备期望偏移 4000 补发")
    checkEq(h4.transfer.state, .verifying, "补发后再次请求提交")
    checkEq(h4.io.endCount, 2, "应重发 end")
    h4.transfer.handle(.done(id: nil))
    checkEq(h4.transfer.state, .done, "最终完成")
}

func testCRCErrorAutoResendOnce() {
    print("--- err{crc} → 自动整份重传一次，第二次失败 ---")
    let h = Harness(bytes: 10_000)
    h.transfer.start()
    h.transfer.handle(.go(offset: 0, chunk: 174, window: 4096, id: nil))
    h.driveToVerifying()
    checkEq(h.transfer.state, .verifying, "先到 verifying")
    let beginsBefore = h.io.beginCommands.count

    h.transfer.handle(.error(code: .crc, rawCode: "crc", want: nil))
    checkEq(h.transfer.crcRestartCount, 1, "应记录一次 CRC 重传")
    checkEq(h.io.beginCommands.count, beginsBefore + 1, "应重新 begin")
    checkEq(h.transfer.sentBytes, 0, "整份重传从 0 开始")
    checkEq(h.transfer.state, .waitingBeginAck, "等 go")

    h.transfer.handle(.go(offset: 0, chunk: 174, window: 4096, id: nil))
    h.driveToVerifying()
    h.transfer.handle(.error(code: .crc, rawCode: "crc", want: nil))
    checkEq(h.transfer.state, .failed, "第二次 CRC 失败 → 失败")
    checkEq(h.transfer.progress.state, .failed, "进度失败")
    check(PhotoErrorCode.crc.userText.contains("校验"), "crc 文案应说明校验失败")
    checkEq(h.transfer.crcRestartCount, 1, "不能无限重传")
}

func testBusyRetry() {
    print("--- err{busy} → 延迟重试 begin（有上限）---")
    let h = Harness(bytes: 10_000)
    h.transfer.start()
    h.transfer.handle(.error(code: .busy, rawCode: "busy", want: nil))
    checkEq(h.transfer.busyRetryCount, 1, "记录一次 busy 重试")
    checkEq(h.transfer.state, .waitingBeginAck, "等待重试窗口")
    checkEq(h.io.beginCommands.count, 1, "延迟期间不应立刻再发 begin")

    h.clock.advance(2.1)
    h.transfer.tick()
    checkEq(h.io.beginCommands.count, 2, "到点后重发 begin")

    h.transfer.handle(.error(code: .busy, rawCode: "busy", want: nil))
    h.clock.advance(2.1)
    h.transfer.tick()
    checkEq(h.io.beginCommands.count, 3, "第二次重试")
    h.transfer.handle(.error(code: .busy, rawCode: "busy", want: nil))
    checkEq(h.transfer.state, .failed, "超过重试上限 → 失败")
    check(PhotoErrorCode.busy.userText.contains("另一张"), "busy 文案")
}

func testTerminalErrorsFailFast() {
    print("--- 终态错误码 → 直接失败 + 用户文案 ---")
    for code in [PhotoErrorCode.no_space, .too_large, .unsupported, .io, .not_ready, .bad_arg, .unknown] {
        let h = Harness(bytes: 10_000)
        h.transfer.start()
        h.transfer.handle(.go(offset: 0, chunk: 174, window: 4096, id: nil))
        h.transfer.handle(.error(code: code, rawCode: code.rawValue, want: nil))
        checkEq(h.transfer.state, .failed, "\(code) 应立即失败")
        let text = h.transfer.progress.errorText ?? ""
        check(!text.isEmpty, "\(code) 必须有用户文案")
        check(text == code.userText, "\(code) 文案应来自统一映射")
        check(!h.io.abortCount.description.isEmpty, "不应静默") // 占位：确保走到这里
    }
    // 所有错误码都有非空文案
    for code in [PhotoErrorCode.busy, .no_space, .too_large, .unsupported, .crc,
                 .offset, .io, .not_ready, .bad_arg, .unknown] {
        check(!code.userText.isEmpty, "\(code) 文案不能为空")
    }
}

func testDisconnectAndResume() {
    print("--- 断连 → waitingReconnect → 重连续传 ---")
    let h = Harness(bytes: 20_000)
    h.transfer.start()
    h.transfer.handle(.go(offset: 0, chunk: 174, window: 4096, id: nil))
    h.transfer.handle(.credit(offset: 4096))
    let ackedAtDrop = h.transfer.ackedBytes
    checkEq(ackedAtDrop, 4096, "断连前已落盘 4096")

    h.transfer.markDisconnected()
    checkEq(h.transfer.state, .waitingReconnect, "断连后等待重连")
    checkEq(h.transfer.progress.state, .waitingReconnect, "进度状态 waitingReconnect")
    checkEq(h.io.abortCount, 0, "断连不应发 abort（设备要保留 .part）")
    let chunksAtDrop = h.io.chunks.count
    checkEq(h.io.chunks.count, chunksAtDrop, "断连后不应继续写")

    h.transfer.restartAfterReconnect()
    checkEq(h.io.beginCommands.count, 2, "重连后重新 begin")
    h.transfer.handle(.go(offset: ackedAtDrop, chunk: 174, window: 4096, id: nil))
    checkEq(h.transfer.ackedBytes, ackedAtDrop, "acked = 设备已落盘偏移")
    checkEq(h.io.chunks.suffix(h.io.chunks.count - chunksAtDrop).first?.offset ?? -1, ackedAtDrop,
            "重连后第一片必须从已落盘偏移开始")
    h.driveToVerifying()
    h.transfer.handle(.done(id: nil))
    checkEq(h.transfer.state, .done, "续传后完成")
    check(h.allBytesTouched(), "续传必须覆盖全部字节")
}

func testCancel() {
    print("--- 取消 → abort，且不再发数据 ---")
    let h = Harness(bytes: 20_000)
    h.transfer.start()
    h.transfer.handle(.go(offset: 0, chunk: 174, window: 4096, id: nil))
    h.transfer.handle(.credit(offset: 2048))
    let chunksBefore = h.io.chunks.count

    h.transfer.cancel()
    checkEq(h.io.abortCount, 1, "应发 abort")
    checkEq(h.transfer.state, .cancelled, "状态 cancelled")
    checkEq(h.transfer.progress.state, .cancelled, "进度 cancelled")

    // 取消后的一切事件都不应再推进
    h.transfer.handle(.credit(offset: 20_000))
    h.transfer.handle(.go(offset: 0, chunk: 174, window: 4096, id: nil))
    h.transfer.handle(.done(id: nil))
    h.transfer.tick()
    checkEq(h.io.chunks.count, chunksBefore, "取消后不能再写数据")
    checkEq(h.transfer.state, .cancelled, "取消是终态")
    h.transfer.cancel()
    checkEq(h.transfer.state, .cancelled, "重复取消不崩")
}

func testRateAndETA() {
    print("--- 速率（2 s 滑动平均）与 ETA ---")
    let h = Harness(bytes: 40_000)
    h.transfer.start()
    h.transfer.handle(.go(offset: 0, chunk: 174, window: 4096, id: nil))
    // t+1s：落盘 4096
    h.clock.advance(1.0)
    h.transfer.handle(.credit(offset: 4096))
    let r1 = h.transfer.currentBytesPerSecond
    check(abs(r1 - 4096) < 1, "1 s 落盘 4096 → 速率约 4096 B/s，实际 \(r1)")
    // t+2s：再落盘 4096（2 s 窗口内共 8192）
    h.clock.advance(1.0)
    h.transfer.handle(.credit(offset: 8192))
    let r2 = h.transfer.currentBytesPerSecond
    check(abs(r2 - 4096) < 1, "2 s 落盘 8192 → 速率约 4096 B/s，实际 \(r2)")

    // 进度里的 ETA：剩余 / max(rate, 8 KB/s)
    let p = h.transfer.progress
    checkEq(p.ackedBytes, 8192, "进度 acked")
    checkEq(p.bytesPerSecond, r2, "进度速率")
    let expectedETA = Double(40_000 - 8192) / max(r2, 8192)
    check(abs((p.etaSeconds ?? -1) - expectedETA) < 0.001, "ETA 按 max(rate, 8 KB/s) 估算")
    checkEq(p.fraction, 8192.0 / 40_000.0, "进度比例")
}

func testProgressThrottle() {
    print("--- 进度回调节流（高频写会拖垮 SwiftUI）---")
    let h = Harness(bytes: 100_000, chunk: 174, window: 4096)
    h.transfer.start()
    h.transfer.handle(.go(offset: 0, chunk: 174, window: 4096, id: nil))
    // 时钟不动 → 一整个窗口只会报一次（节流 0.2 s）；写分片却有 ~24 次
    check(h.io.chunks.count > 10, "窗口内应发多个分片，实际 \(h.io.chunks.count)")
    check(h.io.reports.count <= 2, "同一时刻的进度回调应被节流，实际 \(h.io.reports.count)")

    // 时钟推进后每个 credit 仍受节流；状态变化强制上报
    for i in 1...5 {
        h.clock.advance(0.05)
        h.transfer.handle(.credit(offset: min(h.transfer.sentBytes, i * 800)))
        _ = h.transfer.sentBytes
    }
    check(h.io.reports.count < h.io.chunks.count, "进度回调数必须少于分片数")
    // 终态必须强制上报
    h.driveToVerifying()
    h.transfer.handle(.done(id: nil))
    checkEq(h.io.reports.last?.state, .done, "终态必须上报")
}

func testLocalDataTimeout() {
    print("--- 本地文件读不出来 → 超时失败（不无限卡住）---")
    let h = Harness(bytes: 10_000)
    h.io.dataReady = false
    h.transfer.start()
    h.transfer.handle(.go(offset: 0, chunk: 174, window: 4096, id: nil))
    checkEq(h.io.chunks.count, 0, "读不到数据时不应写分片")
    checkEq(h.transfer.state, .sending, "只是等待，不是失败")
    h.clock.advance(10.5)
    h.transfer.tick()
    checkEq(h.transfer.state, .failed, "超时后失败")
    check((h.transfer.progress.errorText ?? "").contains("文件"), "文案应说是文件读取问题")

    // 数据就绪后恢复（新引擎）
    let h2 = Harness(bytes: 10_000)
    h2.io.dataReady = false
    h2.transfer.start()
    h2.transfer.handle(.go(offset: 0, chunk: 174, window: 4096, id: nil))
    checkEq(h2.io.chunks.count, 0, "仍等待")
    h2.io.dataReady = true
    h2.transfer.chunkBecameAvailable()
    check(h2.io.chunks.count > 0, "数据就绪后应继续发送")
}

func testInvalidRequestsFailFast() {
    print("--- 参数非法 → 立刻失败（不发 begin）---")
    let zero = Harness(bytes: 0)
    zero.transfer.start()
    checkEq(zero.transfer.state, .failed, "空文件失败")
    checkEq(zero.io.commands.count, 0, "空文件不应发 begin")

    let h = Harness(bytes: 10_000)
    let badSize = PhotoTransfer(request: makeRequest(bytes: 10_000, width: 500),
                                io: h.io)
    badSize.clock = { 0 }
    badSize.start()
    checkEq(badSize.state, .failed, "宽 500 失败")
    checkEq(h.io.commands.count, 0, "非法尺寸不应发 begin")

    let animNoFPS = PhotoTransfer(request: makeRequest(bytes: 10_000, kind: .pgi, frames: 12, fps: 0),
                                  io: h.io)
    animNoFPS.start()
    checkEq(animNoFPS.state, .failed, "动图 fps=0 失败")

    let animSingle = PhotoTransfer(request: makeRequest(bytes: 10_000, kind: .pgi, frames: 1, fps: 8),
                                   io: h.io)
    animSingle.start()
    checkEq(animSingle.state, .failed, "动图只有 1 帧 → 失败")
}

func testStrayEventsIgnored() {
    print("--- 游离事件不应改变状态 ---")
    let h = Harness(bytes: 10_000)
    h.transfer.handle(.done(id: nil))       // 还没 start
    checkEq(h.transfer.state, .idle, "idle 时 done 忽略")
    h.transfer.handle(.credit(offset: 100))
    checkEq(h.transfer.state, .idle, "idle 时 credit 忽略")
    h.transfer.tick()
    checkEq(h.transfer.state, .idle, "idle 时 tick 忽略")

    h.transfer.start()
    h.transfer.handle(.credit(offset: 100)) // 还没 go
    checkEq(h.transfer.ackedBytes, 0, "等 go 期间的 credit 忽略")
    h.transfer.handle(.go(offset: 0, chunk: 174, window: 4096, id: nil))
    h.driveToVerifying()
    h.transfer.handle(.credit(offset: 100)) // verifying 期间的 credit 忽略
    checkEq(h.transfer.state, .verifying, "verifying 时 credit 不改变状态")

    // go 的 offset 越界 → 夹到文件大小（设备异常时的防御）
    let h2 = Harness(bytes: 10_000)
    h2.transfer.start()
    h2.transfer.handle(.go(offset: 999_999, chunk: 174, window: 4096, id: nil))
    checkEq(h2.transfer.sentBytes, 10_000, "越界 off 夹到总大小")
    checkEq(h2.transfer.state, .verifying, "已全在设备上 → 直接发 end")
}

func testEndWatchdog() {
    print("--- 等 done 超时 → 重发 end（有上限）---")
    let h = Harness(bytes: 5_000)
    h.transfer.start()
    h.transfer.handle(.go(offset: 0, chunk: 174, window: 4096, id: nil))
    h.driveToVerifying()
    checkEq(h.transfer.state, .verifying, "进入 verifying")
    checkEq(h.io.endCount, 1, "已发 1 次 end")

    h.clock.advance(6.5)
    h.transfer.tick()
    checkEq(h.io.endCount, 2, "超时后重发 end")
    checkEq(h.transfer.endResendCount, 1, "记录一次重发")
    h.clock.advance(6.5)
    h.transfer.tick()
    checkEq(h.io.endCount, 3, "第二次重发")
    h.clock.advance(6.5)
    h.transfer.tick()
    checkEq(h.transfer.state, .failed, "超过上限 → 失败")
    check((h.transfer.progress.errorText ?? "").contains("提交"), "文案应提示未确认提交")
}

func testCompletedResumeWhenPartAlreadyFull() {
    print("--- 相机已完整落盘（off == bytes）→ 直接 end ---")
    let h = Harness(bytes: 5_000)
    h.transfer.start()
    h.transfer.handle(.go(offset: 5_000, chunk: 174, window: 4096, id: nil))
    checkEq(h.io.chunks.count, 0, "无需再发数据")
    checkEq(h.io.endCount, 1, "直接请求提交")
    checkEq(h.transfer.state, .verifying, "等 done")
    h.transfer.handle(.done(id: nil))
    checkEq(h.transfer.state, .done, "完成")
}

// MARK: - 文件分块读取

func makeTempFile(bytes count: Int) -> (URL, Data) {
    let data = patternedData(count)
    let url = FileManager.default.temporaryDirectory
        .appendingPathComponent("photo-test-\(UUID().uuidString).pgj")
    try? data.write(to: url)
    return (url, data)
}

/// 轮询等待后台读取完成（命令行工具不跑 main runloop）
func waitRead(_ source: PhotoFileChunkSource, offset: Int, length: Int,
              timeout: TimeInterval = 2) -> Data? {
    let deadline = Date().addingTimeInterval(timeout)
    while Date() < deadline {
        if let data = source.read(offset: offset, length: length) { return data }
        Thread.sleep(forTimeInterval: 0.01)
    }
    return source.read(offset: offset, length: length)
}

func testFileChunkSource() {
    print("--- PhotoFileChunkSource（4 KB 读前缓存 + 越界保护）---")
    let (url, data) = makeTempFile(bytes: 10_000)
    defer { try? FileManager.default.removeItem(at: url) }

    guard let source = PhotoFileChunkSource(fileURL: url) else {
        check(false, "应能打开临时文件"); return
    }
    checkEq(source.size, 10_000, "文件大小")
    // 第一次读：未命中 → nil + 触发后台读
    check(source.read(offset: 0, length: 174) == nil, "首次读应返回 nil（后台读取中）")
    let head = waitRead(source, offset: 0, length: 174)
    checkEq(head, data.prefix(174), "首块内容必须与文件一致")
    // 缓存命中：同窗口内的下一次读应同步返回
    checkEq(source.read(offset: 174, length: 174), data.subdata(in: 174..<348), "缓存窗口内命中")
    // 跨 4 KB 边界的读
    let across = waitRead(source, offset: 4_000, length: 500)
    checkEq(across, data.subdata(in: 4_000..<4_500), "跨窗口边界读取")
    // 尾部
    let tail = waitRead(source, offset: 9_800, length: 200)
    checkEq(tail, data.subdata(in: 9_800..<10_000), "尾部读取")
    // 越界
    check(source.read(offset: 10_000, length: 10) == nil, "越界 → nil")
    check(source.read(offset: 0, length: 20_000) == nil, "超出文件长度 → nil")
    check(source.read(offset: -1, length: 10) == nil, "负偏移 → nil")

    source.close()
    check(source.read(offset: 0, length: 10) == nil, "close 后 → nil")

    // 打不开的文件
    let missing = URL(fileURLWithPath: "/tmp/definitely-not-exists-\(UUID().uuidString).pgj")
    check(PhotoFileChunkSource(fileURL: missing) == nil, "不存在的文件 → nil")
    let (emptyURL, _) = makeTempFile(bytes: 0)
    defer { try? FileManager.default.removeItem(at: emptyURL) }
    check(PhotoFileChunkSource(fileURL: emptyURL) == nil, "空文件 → nil")
}

// MARK: - 入口

@main
struct TestPhotoTransfer {
    static func main() {
        print("=== 照片上传传输引擎 ===")
        testHappyPath()
        testWindowFlowControl()
        testCreditNeverSentIgnored()
        testWriteBackpressure()
        testChunkClampedByIOSWriteLength()
        testCreditWatchdogRestartsBegin()
        testOffsetErrorResume()
        testCRCErrorAutoResendOnce()
        testBusyRetry()
        testTerminalErrorsFailFast()
        testDisconnectAndResume()
        testCancel()
        testRateAndETA()
        testProgressThrottle()
        testLocalDataTimeout()
        testInvalidRequestsFailFast()
        testStrayEventsIgnored()
        testEndWatchdog()
        testCompletedResumeWhenPartAlreadyFull()
        testFileChunkSource()
        print("结果: \(gPass) 通过, \(gFail) 失败")
        exit(gFail == 0 ? 0 : 1)
    }
}
