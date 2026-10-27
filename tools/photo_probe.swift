/**
 * photo_probe.swift —— macOS BLE 照片协议探针（真机联调用，不依赖 iPhone App）
 *
 * 用途：
 *   1. 连上 CarDashboard，读 PhotoInfo / PhotoState / 清单（验证固件上报）；
 *   2. 生成一张 466×466 baseline JPEG → 包成 `.pgj` → 走真机 BLE 直传（验证 S1 吞吐 / S3 落盘）；
 *   3. 可选：断连续传输（验证 .part 续传）、播放/删除（验证清单与状态联动）。
 *
 * 用法：
 *   swiftc -O -o /tmp/photo_probe tools/photo_probe.swift
 *   /tmp/photo_probe                 # 传一张，保留在设备上，打印吞吐
 *   /tmp/photo_probe --delete        # 传完验证后删掉
 *   /tmp/photo_probe --resume        # 断连续传测试（传一半断开→重连→从 off 续传）
 *   /tmp/photo_probe --file x.pgj    # 用现成 .pgj
 *   /tmp/photo_probe --ack           # 逐片带确认写（复现 iPhone 路径，~2.6 KB/s）
 *   /tmp/photo_probe --format        # 格式化设备 SD 卡（危险：清空全部照片）后列清单
 *   /tmp/photo_probe --size 200      # 生成 200×200 小图（快速回归）
 *   /tmp/photo_probe --save /tmp/a.pgj  # 保存生成的 .pgj 便于离线核对 CRC
 *
 * 协议以 docs/photo-protocol.md 为准。
 */
import CoreBluetooth
import CoreGraphics
import Foundation
import ImageIO
import UniformTypeIdentifiers

// MARK: - 协议常量

let photoService = CBUUID(string: "7D2A0010-8F4A-4B7C-9D21-5E6A3B8C1000")
let ctlUUID = CBUUID(string: "7D2A0011-8F4A-4B7C-9D21-5E6A3B8C1000")
let dataUUID = CBUUID(string: "7D2A0012-8F4A-4B7C-9D21-5E6A3B8C1000")
let evtUUID = CBUUID(string: "7D2A0013-8F4A-4B7C-9D21-5E6A3B8C1000")
let infoUUID = CBUUID(string: "7D2A0014-8F4A-4B7C-9D21-5E6A3B8C1000")
let stateUUID = CBUUID(string: "7D2A0015-8F4A-4B7C-9D21-5E6A3B8C1000")
let telemetryService = CBUUID(string: "7D2A0001-8F4A-4B7C-9D21-5E6A3B8C1000")

// MARK: - CRC32（与固件/App 一致：反射 poly 0xEDB88320）

enum CRC32 {
    static let table: [UInt32] = (0..<256).map { i -> UInt32 in
        var c = UInt32(i)
        for _ in 0..<8 { c = (c & 1) != 0 ? (0xEDB8_8320 ^ (c >> 1)) : (c >> 1) }
        return c
    }
    static func checksum(_ data: Data) -> UInt32 {
        var crc: UInt32 = 0xFFFF_FFFF
        for b in data {
            crc = table[Int((crc ^ UInt32(b)) & 0xFF)] ^ (crc >> 8)
        }
        return crc ^ 0xFFFF_FFFF
    }
}

// MARK: - PGJ

struct PGJFile {
    let data: Data
    let width: Int
    let height: Int
    let crc: UInt32
}

enum PGJ {
    /// 构造 `.pgj`：32B 头 + JPEG；CRC 覆盖整个文件，但 crc 字段自身按 0 计。
    static func wrap(jpeg: Data, width: Int, height: Int, quality: Int = 80) -> PGJFile {
        var header = Data(count: 32)
        header.replaceSubrange(0..<4, with: Array("PGJ1".utf8))
        header.writeLE(UInt16(width), at: 4)
        header.writeLE(UInt16(height), at: 6)
        header[8] = 1 // version
        header[9] = 1 // flags: baseline
        header.writeLE(UInt16(quality), at: 10)
        header.writeLE(UInt32(jpeg.count), at: 12)
        header.writeLE(UInt32(0), at: 16) // crc 字段先置 0
        header.writeLE(UInt32(Date().timeIntervalSince1970), at: 20)
        let file = header + jpeg
        let crc = CRC32.checksum(file)
        var headerWithCrc = header
        headerWithCrc.writeLE(crc, at: 16)
        return PGJFile(data: headerWithCrc + jpeg, width: width, height: height, crc: crc)
    }
}

private extension Data {
    mutating func writeLE<T: FixedWidthInteger>(_ value: T, at offset: Int) {
        var v = value.littleEndian
        Swift.withUnsafeBytes(of: &v) { raw in
            replaceSubrange(offset..<(offset + raw.count), with: raw)
        }
    }
}

// MARK: - 测试图片（噪声 + 渐变，保证 JPEG 不是几百字节）

enum TestImage {
    static func makeJPEG(size: Int = 466, quality: Double = 0.8, seed: UInt32 = 0x1234_5678) -> Data? {
        let cs = CGColorSpaceCreateDeviceRGB()
        guard let ctx = CGContext(data: nil, width: size, height: size, bitsPerComponent: 8,
                                  bytesPerRow: 0, space: cs,
                                  bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)
        else { return nil }

        // 渐变背景（斜向）
        let colors = [
            CGColor(red: 0.10, green: 0.16, blue: 0.32, alpha: 1),
            CGColor(red: 0.75, green: 0.30, blue: 0.15, alpha: 1),
        ] as CFArray
        if let g = CGGradient(colorsSpace: cs, colors: colors, locations: [0, 1]) {
            ctx.drawLinearGradient(g, start: .zero, end: CGPoint(x: size, y: size), options: [])
        }

        // 伪随机色块/圆：制造高频细节，JPEG 体积接近真实照片
        var s = seed
        func rnd() -> UInt32 {
            s = s &* 1664525 &+ 1013904223
            return s >> 8
        }
        for _ in 0..<900 {
            let x = CGFloat(rnd() % UInt32(size))
            let y = CGFloat(rnd() % UInt32(size))
            let r = CGFloat(2 + rnd() % 22)
            ctx.setFillColor(CGColor(red: CGFloat(rnd() % 255) / 255,
                                     green: CGFloat(rnd() % 255) / 255,
                                     blue: CGFloat(rnd() % 255) / 255,
                                     alpha: 0.55))
            ctx.fillEllipse(in: CGRect(x: x, y: y, width: r * 2, height: r * 2))
        }
        // 中间一圈高对比度环（确认圆心/朝向正确）
        ctx.setStrokeColor(CGColor(red: 1, green: 1, blue: 1, alpha: 0.9))
        ctx.setLineWidth(6)
        ctx.strokeEllipse(in: CGRect(x: 40, y: 40, width: size - 80, height: size - 80))

        guard let img = ctx.makeImage() else { return nil }
        let out = NSMutableData()
        guard let dest = CGImageDestinationCreateWithData(out, UTType.jpeg.identifier as CFString, 1, nil)
        else { return nil }
        CGImageDestinationAddImage(dest, img, [kCGImageDestinationLossyCompressionQuality: quality] as CFDictionary)
        guard CGImageDestinationFinalize(dest) else { return nil }
        return out as Data
    }
}

// MARK: - 探针

final class Probe: NSObject, CBCentralManagerDelegate, CBPeripheralDelegate {
    private let queue = DispatchQueue(label: "photo-probe.ble")
    private var central: CBCentralManager!
    private var per: CBPeripheral?

    private var ctl: CBCharacteristic?
    private var dataCh: CBCharacteristic?
    private var evt: CBCharacteristic?
    private var infoCh: CBCharacteristic?
    private var stateCh: CBCharacteristic?

    private var poweredSem = DispatchSemaphore(value: 0)
    private var readySem = DispatchSemaphore(value: 0)
    private var readInfoSem = DispatchSemaphore(value: 0)
    private var readStateSem = DispatchSemaphore(value: 0)
    private var goSem = DispatchSemaphore(value: 0)
    private var doneSem = DispatchSemaphore(value: 0)
    private var listDoneSem = DispatchSemaphore(value: 0)
    private var formatSem = DispatchSemaphore(value: 0)
    private var formatResult: String?

    // 上传状态（BLE 队列访问）
    private var upload: PGJFile?
    private var sent = 0
    private var acked = 0
    private var total = 0
    private var window = 4096
    private var chunkPayload = 170
    private var startedAt: Date?
    private var finishedAt: Date?
    private var doneID: String?
    private var failReason: String?
    private var goOffset = 0
    private var endSent = false
    private var armed = false
    private var pumpCalls = 0
    private var pumpBlockedByCanSend = 0
    private var sentFirstLogged = false
    private var ackMode = false
    private var ackInFlight = false
    private var resumeLimit: Int? // --resume：主动断开前只发这么多字节

    private var pendingList: [[String: Any]] = []
    private var listCount = 0
    private var infoJSON: [String: Any] = [:]
    private var stateJSON: [String: Any] = [:]

    private var connectedSem = DispatchSemaphore(value: 0)
    private var pumpTimer: DispatchSourceTimer?

    func run(deleteAfter: Bool, resumeTest: Bool, file: String?) -> Int32 {
        central = CBCentralManager(delegate: self, queue: queue)
        guard poweredSem.wait(timeout: .now() + 10) == .success else {
            print("❌ 蓝牙未就绪（10s 超时）")
            return 1
        }
        guard central.state == .poweredOn else {
            print("❌ 蓝牙状态：\(central.state.rawValue)（需要系统蓝牙权限）")
            return 1
        }

        let pgj: PGJFile
        if let file {
            guard let d = FileManager.default.contents(atPath: file) else {
                print("❌ 读不到 \(file)"); return 1
            }
            guard let jpgLen = d.count >= 32 ? Int(d.leU32(12)) : nil, d.count == jpgLen + 32 else {
                print("❌ 不是合法 .pgj（长度与头不符）"); return 1
            }
            pgj = PGJFile(data: d, width: Int(d.leU16(4)), height: Int(d.leU16(6)), crc: CRC32.checksum(adjustedCRC(d)))
        }
        else {
            let side = args.firstIndex(of: "--size").flatMap { $0 + 1 < args.count ? Int(args[$0 + 1]) : nil } ?? 466
            guard let jpeg = TestImage.makeJPEG(size: side) else { print("❌ 生成测试 JPEG 失败"); return 1 }
            pgj = PGJ.wrap(jpeg: jpeg, width: side, height: side, quality: 80)
            print(String(format: "测试图：%d×%d JPEG %d B → .pgj %d B（crc=%08X）",
                         side, side, jpeg.count, pgj.data.count, pgj.crc))
        }
        ackMode = args.contains("--ack")
        upload = pgj
        total = pgj.data.count
        if let i = args.firstIndex(of: "--save"), i + 1 < args.count {
            let path = args[i + 1]
            try? pgj.data.write(to: URL(fileURLWithPath: path))
            print("已保存 \(path)（crc=\(String(format: "%08X", pgj.crc))，header.crc=\(String(format: "%08X", pgj.data.leU32(16)))）")
        }

        // 扫描（设备广告 telemetry 服务；用服务过滤避免误连）
        central.scanForPeripherals(withServices: [telemetryService], options: nil)
        guard connectedSem.wait(timeout: .now() + 20) == .success else {
            print("❌ 20s 内没连上 CarDashboard（设备是否已被 iPhone 连接？）")
            return 1
        }
        central.stopScan()

        guard readySem.wait(timeout: .now() + 10) == .success else {
            print("❌ 服务/特征发现超时")
            return 1
        }
        print("✅ 已连接，MTU 写长：withResponse=\(per?.maximumWriteValueLength(for: .withResponse) ?? 0) B, "
              + "withoutResponse=\(per?.maximumWriteValueLength(for: .withoutResponse) ?? 0) B")
        chunkPayload = min(244, max(64, (per?.maximumWriteValueLength(for: .withoutResponse) ?? 174) - 4))

        /*
         * 主动泵：macOS 的 `peripheralIsReady(toSendWriteWithoutResponse:)` 在部分
         * 场景不回调（实测上传会停在 canSend=true 但没人继续发）。这里 5 ms 自己泵一次，
         * 不依赖系统事件；流控仍由 window/credit 保证，不会超发。
         */
        let pt = DispatchSource.makeTimerSource(queue: queue)
        pt.schedule(deadline: .now(), repeating: .milliseconds(10))
        pt.setEventHandler { [weak self] in self?.pumpLocked() }
        pt.resume()
        pumpTimer = pt

        readInfo(); readState(); requestList()

        if args.contains("--info") {
            Thread.sleep(forTimeInterval: 2.0)
            print("设备在线：info/state/list 已打印")
            return 0
        }

        if args.contains("--format") {
            _ = sendCtl("{\"t\":\"p\",\"op\":\"format\"}")
            guard formatSem.wait(timeout: .now() + 30) == .success else {
                print("❌ 30s 内没有收到格式化结果")
                return 1
            }
            print("格式化结果：\(formatResult ?? "?")")
            refreshAll()
            print("格式化后清单：\(listCount) 项")
            return formatResult == "ok" ? 0 : 1
        }

        if resumeTest {
            return resumeFlow()
        }

        if !uploadOnce(resumeFrom: 0) { return 1 }
        verifyAndReport(deleteAfter: deleteAfter)
        return 0
    }

    // MARK: 上传

    private func uploadOnce(resumeFrom: Int) -> Bool {
        guard let pgj = upload else { return false }
        failReason = nil
        doneID = nil
        endSent = false
        armed = false
        sent = resumeFrom
        acked = resumeFrom
        startedAt = nil
        finishedAt = nil
        resumeLimit = nil

        goSem = DispatchSemaphore(value: 0)
        sentFirstLogged = false
        print("   [begin] 发送 begin（bytes=\(pgj.data.count)）")
        guard let cmd = beginCommand(), sendCtl(cmd) else { print("❌ 写 begin 失败"); return false }
        guard goSem.wait(timeout: .now() + 8) == .success else {
            print("❌ 等 go 超时（设备未响应 begin）"); return false
        }
        if let failReason {
            print("❌ begin 被拒：\(failReason)")
            return false
        }
        if goOffset > 0 {
            print("↻ 设备要求从 \(goOffset) B 续传")
            sent = goOffset
            acked = goOffset
        }
        armed = true
        pump()
        // 每 2 s 打印一次进度，便于定位卡点
        let timer = DispatchSource.makeTimerSource(queue: queue)
        timer.schedule(deadline: .now() + 2, repeating: 2)
        timer.setEventHandler { [weak self] in
            guard let self, let per = self.per else { return }
            let canSend = per.canSendWriteWithoutResponse
            print(String(format: "  … 进度 sent=%d acked=%d outstanding=%d window=%d canSend=%@ armed=%@ err=%@",
                         self.sent, self.acked, self.sent - self.acked, self.window,
                         canSend ? "true" : "false", self.armed ? "1" : "0",
                         self.failReason ?? "-"))
        }
        timer.resume()

        let done = doneSem.wait(timeout: .now() + 45) == .success
        timer.cancel()
        guard done else {
            print("❌ 45s 内没收到 done（已发 \(sent)/\(total) B，acked=\(acked)）")
            return false
        }
        if let failReason {
            print("❌ 上传失败：\(failReason)（已发 \(sent)/\(total)，acked=\(acked)）")
            return false
        }
        return true
    }

    /// 断连续传：发一半 → 主动断连 → 重连 → begin（同 bytes/crc）→ 期望 off>0 → 续完
    private func resumeFlow() -> Int32 {
        guard upload != nil else { return 1 }
        resumeLimit = max(4096, total / 2)
        guard uploadOncePart(resumeFrom: 0) else { return 1 }
        print("↻ 已发 \(sent) B，主动断开连接，模拟断线…")
        central.cancelPeripheralConnection(per!)

        // 等断连回调
        Thread.sleep(forTimeInterval: 1.5)
        // 重新扫描连接
        connectedSem = DispatchSemaphore(value: 0)
        readySem = DispatchSemaphore(value: 0)
        central.scanForPeripherals(withServices: [telemetryService], options: nil)
        guard connectedSem.wait(timeout: .now() + 20) == .success,
              readySem.wait(timeout: .now() + 10) == .success
        else {
            print("❌ 重连失败")
            return 1
        }
        print("✅ 重连成功，发起续传 begin…")
        guard uploadOnce(resumeFrom: 0) else { return 1 }
        verifyAndReport(deleteAfter: true)
        return 0
    }

    /// resumeFlow 的第一段：发到 resumeLimit 就返回
    private func uploadOncePart(resumeFrom: Int) -> Bool {
        guard let pgj = upload else { return false }
        sent = resumeFrom
        acked = resumeFrom
        failReason = nil
        endSent = false
        armed = false
        goSem = DispatchSemaphore(value: 0)
        guard let cmd = beginCommand(), sendCtl(cmd) else { return false }
        guard goSem.wait(timeout: .now() + 8) == .success else { return false }
        if goOffset > 0 { sent = goOffset; acked = goOffset }
        armed = true
        pump()
        // 等到发出 resumeLimit 或 15s
        let deadline = Date().addingTimeInterval(15)
        while sent < resumeLimit! && Date() < deadline && failReason == nil {
            Thread.sleep(forTimeInterval: 0.02)
            queue.sync { pumpLocked() }
        }
        print(String(format: "  第一段：已发 %d B（%.1f%%）", sent, Double(sent) * 100 / Double(total)))
        return true
    }

    /// 只能在 BLE 队列里调用
    private func pumpLocked() {
        guard armed, let per, let ch = dataCh, let pgj = upload else { return }
        guard doneID == nil, failReason == nil else { return }
        pumpCalls += 1
        if startedAt == nil { startedAt = Date() }
        /*
         * --ack：逐片带响应写（每片等 didWriteValueFor 再发下一片）。
         * 用来在没有 iPhone 的情况下复现 App 的真机路径。
         */
        if ackMode {
            if ackInFlight { return }
            if sent >= total {
                if !endSent {
                    endSent = true
                    _ = sendCtl("{\"t\":\"p\",\"op\":\"end\"}")
                }
                return
            }
            let n = min(chunkPayload, total - sent)
            var frame = Data(capacity: n + 4)
            var off = UInt32(sent).littleEndian
            withUnsafeBytes(of: &off) { frame.append(contentsOf: $0) }
            frame.append(pgj.data.subdata(in: sent..<(sent + n)))
            if sent == 0 && !sentFirstLogged {
                sentFirstLogged = true
                print("   [first-chunk] off=0 armed=\(armed)")
            }
            per.writeValue(frame, for: ch, type: .withResponse)
            sent += n
            ackInFlight = true
            return
        }
        let limit = resumeLimit ?? total
        var bursts = 0
        while sent < total && sent < limit && (sent - acked) < window && per.canSendWriteWithoutResponse {
            if bursts >= 1 { break }
            bursts += 1
            let n = min(chunkPayload, total - sent)
            var frame = Data(capacity: n + 4)
            var off = UInt32(sent).littleEndian
            withUnsafeBytes(of: &off) { frame.append(contentsOf: $0) }
            frame.append(pgj.data.subdata(in: sent..<(sent + n)))
            if sent == 0 && !sentFirstLogged {
                sentFirstLogged = true
                print("   [first-chunk] off=0 armed=\(armed)")
            }
            per.writeValue(frame, for: ch, type: .withoutResponse)
            sent += n
        }
        if sent < total && (sent - acked) < window && !per.canSendWriteWithoutResponse {
            pumpBlockedByCanSend += 1
        }
        if sent >= total && !endSent && failReason == nil {
            endSent = true
            _ = sendCtl("{\"t\":\"p\",\"op\":\"end\"}")
            if startedAt == nil { startedAt = Date() }
        }
    }

    private func pump() {
        queue.async { self.pumpLocked() }
    }

    // MARK: 命令/读取

    private func beginCommand() -> String? {
        guard let pgj = upload else { return nil }
        return "{\"t\":\"p\",\"op\":\"begin\",\"kind\":0,\"bytes\":\(pgj.data.count),"
            + "\"crc\":\(pgj.crc),\"w\":\(pgj.width),\"h\":\(pgj.height),\"frames\":1,\"fps\":0}"
    }

    @discardableResult
    private func sendCtl(_ json: String) -> Bool {
        guard let per, let ch = ctl, let d = json.data(using: .utf8) else { return false }
        per.writeValue(d, for: ch, type: .withResponse)
        return true
    }

    private func readInfo() {
        guard let per, let ch = infoCh else { return }
        per.readValue(for: ch)
    }

    private func readState() {
        guard let per, let ch = stateCh else { return }
        per.readValue(for: ch)
    }

    private func requestList() {
        pendingList = []
        listCount = 0
        _ = sendCtl("{\"t\":\"p\",\"op\":\"list\"}")
    }

    private func refreshAll() {
        readInfo(); readState(); requestList()
        Thread.sleep(forTimeInterval: 0.8)
    }

    private func verifyAndReport(deleteAfter: Bool) {
        guard let id = doneID else { print("❌ done 事件没有 id"); return }
        let elapsed = finishedAt.map { $0.timeIntervalSince(startedAt ?? $0) } ?? 0
        let kb = Double(total) / 1024.0
        print(String(format: "✅ 上传完成：%d B（%.1f KB）用时 %.2f s → %.1f KB/s，id=%@",
                     total, kb, elapsed, elapsed > 0 ? kb / elapsed : 0, id))
        refreshAll()
        print("设备清单（\(listCount) 项）：")
        for item in pendingList {
            let i = item["i"] as? Int ?? -1
            let name = item["id"] as? String ?? "?"
            let bytes = item["b"] as? Int ?? 0
            let mark = name == id ? "  ← 本次上传" : ""
            print(String(format: "  [%d] %@  %d B%@", i, name, bytes, mark))
        }
        let hasOurs = pendingList.contains { ($0["id"] as? String) == id }
        print(hasOurs ? "✅ 清单里能看到新照片" : "❌ 清单里没有新照片")

        // 播这张（让设备屏幕显示它，便于人眼确认颜色/方向）
        if let idx = pendingList.firstIndex(where: { ($0["id"] as? String) == id }) {
            _ = sendCtl("{\"t\":\"p\",\"op\":\"play\",\"i\":\(idx)}")
            Thread.sleep(forTimeInterval: 1.0)
            readState()
            Thread.sleep(forTimeInterval: 0.4)

            // 顺手验证设置下发（把轮播关掉，读回确认；再恢复）
            let before = (stateJSON["slide"] as? Int) ?? -1
            _ = sendCtl("{\"t\":\"p\",\"op\":\"set\",\"slide\":0}")
            Thread.sleep(forTimeInterval: 0.5)
            readState(); Thread.sleep(forTimeInterval: 0.4)
            let after = (stateJSON["slide"] as? Int) ?? -1
            print(before == after ? "⚠️ 设置下发后 slide 未变化（before=\(before)）" : "✅ 设置下发生效：slide \(before) → \(after)")
            _ = sendCtl("{\"t\":\"p\",\"op\":\"set\",\"slide\":\(before == 0 ? 0 : 1)}")
        }

        if deleteAfter {
            print("→ 删除测试：\(id)")
            _ = sendCtl("{\"t\":\"p\",\"op\":\"del\",\"id\":\"\(id)\"}")
            Thread.sleep(forTimeInterval: 0.8)
            refreshAll()
            let still = pendingList.contains { ($0["id"] as? String) == id }
            print(still ? "❌ 删除后清单里仍有该照片" : "✅ 删除后清单里已不存在")
        }
    }

    // MARK: CBCentralManagerDelegate

    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        if central.state == .poweredOn { poweredSem.signal() }
        else { print("[蓝牙] state=\(central.state.rawValue)") }
    }

    func centralManager(_ central: CBCentralManager,
                        didDiscover peripheral: CBPeripheral,
                        advertisementData: [String: Any],
                        rssi RSSI: NSNumber) {
        let name = advertisementData[CBAdvertisementDataLocalNameKey] as? String
            ?? peripheral.name ?? ""
        guard name.contains("CarDashboard") else { return }
        central.stopScan()
        per = peripheral
        peripheral.delegate = self
        central.connect(peripheral, options: nil)
    }

    func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        print("✅ 已连接 \(peripheral.name ?? "?")，开始发现服务…")
        connectedSem.signal()
        peripheral.discoverServices([photoService, telemetryService])
    }

    func centralManager(_ central: CBCentralManager, didFailToConnect peripheral: CBPeripheral, error: Error?) {
        print("❌ 连接失败：\(error?.localizedDescription ?? "?")")
        connectedSem.signal()
    }

    func centralManager(_ central: CBCentralManager, didDisconnectPeripheral peripheral: CBPeripheral, error: Error?) {
        print("[蓝牙] 已断开\(error.map { "：\($0.localizedDescription)" } ?? "")")
    }

    // MARK: CBPeripheralDelegate

    func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        if let error { print("❌ discoverServices: \(error.localizedDescription)") }
        print("服务：\(peripheral.services?.map { $0.uuid.uuidString } ?? [])")
        guard error == nil, let services = peripheral.services else { return }
        for s in services {
            if s.uuid == photoService {
                peripheral.discoverCharacteristics([ctlUUID, dataUUID, evtUUID, infoUUID, stateUUID], for: s)
            }
        }
    }

    func peripheral(_ peripheral: CBPeripheral,
                    didDiscoverCharacteristicsFor service: CBService,
                    error: Error?) {
        if let error { print("❌ discoverCharacteristics: \(error.localizedDescription)") }
        guard service.uuid == photoService, let chars = service.characteristics else { return }
        print("照片特征：\(chars.map { $0.uuid.uuidString.prefix(8) + ":" + String($0.properties.rawValue) })")
        for c in chars {
            switch c.uuid {
            case ctlUUID: ctl = c
            case dataUUID: dataCh = c
            case evtUUID:
                evt = c
                peripheral.setNotifyValue(true, for: c)
            case infoUUID: infoCh = c
            case stateUUID:
                stateCh = c
                peripheral.setNotifyValue(true, for: c)
            default: break
            }
        }
        if ctl != nil && dataCh != nil && evt != nil && infoCh != nil && stateCh != nil {
            print("✅ 照片服务 5 个特征齐全")
            readySem.signal()
        }
        else {
            print("❌ 照片服务特征不全（ctl=\(ctl != nil) data=\(dataCh != nil) evt=\(evt != nil) info=\(infoCh != nil) state=\(stateCh != nil)）")
            readySem.signal()
        }
    }

    func peripheral(_ peripheral: CBPeripheral,
                    didUpdateValueFor characteristic: CBCharacteristic,
                    error: Error?) {
        guard error == nil, let d = characteristic.value else { return }
        if characteristic.uuid == infoUUID {
            if let obj = try? JSONSerialization.jsonObject(with: d) as? [String: Any] {
                infoJSON = obj
                print("ℹ️ PhotoInfo：\(String(data: d, encoding: .utf8) ?? "")")
            }
            readInfoSem.signal()
            return
        }
        if characteristic.uuid == stateUUID {
            if let obj = try? JSONSerialization.jsonObject(with: d) as? [String: Any] {
                stateJSON = obj
                print("ℹ️ PhotoState：\(String(data: d, encoding: .utf8) ?? "")")
            }
            readStateSem.signal()
            return
        }
        guard characteristic.uuid == evtUUID else { return }
        guard let obj = try? JSONSerialization.jsonObject(with: d) as? [String: Any] else {
            print("[evt] 非 JSON：\(String(data: d, encoding: .utf8) ?? "")")
            return
        }
        let t = obj["t"] as? String ?? ""
        switch t {
        case "pe":
            let ev = obj["ev"] as? String ?? ""
            switch ev {
            case "go":
                goOffset = obj["off"] as? Int ?? 0
                print("   [go] off=\(goOffset) sent=\(sent) armed=\(armed) t=\(Int(Date().timeIntervalSince1970 * 1000) % 100000)")
                window = min(2048, obj["win"] as? Int ?? 4096)
                if let c = obj["chunk"] as? Int { chunkPayload = min(chunkPayload, max(64, c)) }
                if let id = obj["id"] as? String { doneID = nil; _ = id }
                if !armed, let _ = upload {
                    // 中途 err offset 触发的续传 begin：直接在回调里恢复发送
                    if goOffset > 0 { sent = goOffset; acked = goOffset }
                    endSent = false
                    armed = true
                    queue.async { self.pumpLocked() }
                }
                goSem.signal()
            case "credit":
                acked = obj["off"] as? Int ?? acked
                queue.async { self.pumpLocked() }
            case "done":
                doneID = obj["id"] as? String
                finishedAt = Date()
                doneSem.signal()
            case "err":
                let code = obj["code"] as? String ?? "?"
                let want = obj["want"] as? Int
                if code == "offset", let w = want, w >= 0, w <= sent {
                    /*
                     * 设备要求从 w 续发：这是"可恢复"的偏移纠正，**不能**当失败。
                     * 之前把 failReason 也置上，结果 pumpLocked 直接 return → 卡死。
                     */
                    print("↻ err offset → 从 \(w) B 续发（已发 \(sent)）")
                    sent = w
                    acked = w
                    armed = false
                    endSent = false
                    if let cmd = beginCommand() { _ = sendCtl(cmd) }
                }
                else {
                    failReason = "\(code)\(want.map { " want=\($0)" } ?? "")"
                    if args.contains("--format") {
                        formatResult = "err:\(code)"
                        formatSem.signal()
                    }
                    doneSem.signal()
                }
            case "changed":
                break
            case "fmt":
                formatResult = (obj["ok"] as? Int ?? 0) != 0 ? "ok" : "fail"
                formatSem.signal()
            default:
                print("[evt] \(ev)")
            }
        case "pq":
            listCount = obj["n"] as? Int ?? 0
            pendingList = []
        case "pf":
            pendingList.append(obj)
        case "pz":
            listDoneSem.signal()
            Thread.sleep(forTimeInterval: 0.3)
        default:
            break
        }
    }

    func peripheral(_ peripheral: CBPeripheral, didWriteValueFor characteristic: CBCharacteristic, error: Error?) {
        guard characteristic.uuid == dataUUID else { return }
        if ackMode {
            ackInFlight = false
            if let error {
                failReason = "write:\(error.localizedDescription)"
                doneSem.signal()
                return
            }
            queue.async { self.pumpLocked() }
        }
    }

    /// CBPeripheralDelegate 的真实方法名（写成 isReadyToSend...: 那个重载不会被调用！
    /// 之前上传卡在 73 KB 就是这个原因：canSendWriteWithoutResponse 变 false 后没人恢复。）
    func peripheralIsReady(toSendWriteWithoutResponse peripheral: CBPeripheral) {
        queue.async { self.pumpLocked() }
    }
}

// MARK: - Data 小端读取

private extension Data {
    func leU16(_ o: Int) -> UInt16 { UInt16(self[o]) | (UInt16(self[o + 1]) << 8) }
    func leU32(_ o: Int) -> UInt32 {
        UInt32(self[o]) | (UInt32(self[o + 1]) << 8) | (UInt32(self[o + 2]) << 16) | (UInt32(self[o + 3]) << 24)
    }
}

/// .pgj 的 crc 字段（16..19）按 0 计
private func adjustedCRC(_ d: Data) -> Data {
    var copy = d
    if copy.count >= 20 { copy.replaceSubrange(16..<20, with: [0, 0, 0, 0]) }
    return copy
}

// MARK: - main

var args = Array(CommandLine.arguments.dropFirst())
let deleteAfter = args.contains("--delete")
let resume = args.contains("--resume")
var file: String?
if let i = args.firstIndex(of: "--file"), i + 1 < args.count { file = args[i + 1] }

let probe = Probe()
exit(probe.run(deleteAfter: deleteAfter, resumeTest: resume, file: file))
