/**
 * NavSimBLE.swift —— 常驻 BLE 连接（扮演 iPhone 的 GATT 角色）
 *
 * 与 tools/ble_command_test.swift 的区别：那个工具"连上→写一次→退出"，
 * 本类是**常驻连接**，供模拟行车时按秒持续下发。
 *
 * 协议：写 Telemetry 特征（7D2A0002），消息用 "t":"nav" 分流 ——
 * 详见 docs/ble.md。写到 Command 特征不会进入导航通路（曾踩过这个坑）。
 */

import CoreBluetooth
import Foundation

public final class NavSimBLE: NSObject, CBCentralManagerDelegate, CBPeripheralDelegate {
    public static let serviceUUID = CBUUID(string: "7D2A0001-8F4A-4B7C-9D21-5E6A3B8C1000")
    /// 遥测/导航特征（导航消息必须走这个）
    public static let telemetryUUID = CBUUID(string: "7D2A0002-8F4A-4B7C-9D21-5E6A3B8C1000")

    /// ⚠️ CoreBluetooth 回调必须走**独立队列**，不能用主队列。
    ///
    /// 原因：模拟器是"顺序循环"（推进位置 → 发送 → 休眠 1s），主线程会被阻塞；
    /// 若回调也排在主队列上，连接建立/写入完成的回调永远得不到执行 → 死锁。
    /// 主线程只通过信号量等待"就绪"这一次，其余时间可以自由休眠。
    private let bleQueue = DispatchQueue(label: "navsim.ble")

    private var central: CBCentralManager!
    private var peripheral: CBPeripheral?
    private var telemetry: CBCharacteristic?

    private let deviceName: String
    private var readySem: DispatchSemaphore?
    private var readyResult: Bool = false

    /// 已成功发送的条数；发送失败（无特征/未连接）也会返回 false
    public private(set) var sentCount = 0
    public private(set) var failCount = 0

    public init(deviceName: String = "CarDashboard") {
        self.deviceName = deviceName
        super.init()
    }

    /// 扫描并连接，等待"发现可写特征"或超时。
    /// - Returns: 是否就绪
    @discardableResult
    public func connect(timeoutSec: Double = 20) -> Bool {
        let sem = DispatchSemaphore(value: 0)
        readySem = sem
        readyResult = false

        // CoreBluetooth 的回调需要一个活跃的 run loop
        central = CBCentralManager(delegate: self, queue: bleQueue)

        if sem.wait(timeout: .now() + timeoutSec) == .timedOut {
            print("[BLE] 连接超时（\(Int(timeoutSec))s）")
            return false
        }
        return readyResult
    }

    /// 发送一条导航消息。
    /// - Parameter json: 完整 JSON（会自动补换行无关；长度需 ≤ 重组缓冲 512B）
    @discardableResult
    public func send(_ json: String) -> Bool {
        guard let per = peripheral, let ch = telemetry, let data = json.data(using: .utf8) else {
            failCount += 1
            return false
        }
        guard data.count <= 480 else {
            print("[BLE] 消息过长（\(data.count)B），固件重组缓冲上限 512B —— 已丢弃")
            failCount += 1
            return false
        }
        per.writeValue(data, for: ch, type: .withResponse)
        sentCount += 1
        return true
    }

    public func disconnect() {
        if let per = peripheral { central?.cancelPeripheralConnection(per) }
        peripheral = nil
        telemetry = nil
    }

    // MARK: CBCentralManagerDelegate

    public func centralManagerDidUpdateState(_ c: CBCentralManager) {
        switch c.state {
        case .poweredOn:
            c.scanForPeripherals(withServices: [NavSimBLE.serviceUUID], options: nil)
        case .unauthorized:
            print("[BLE] 未授权蓝牙（系统设置 → 隐私与安全性 → 蓝牙）")
            readySem?.signal()
        case .poweredOff:
            print("[BLE] 蓝牙已关闭")
            readySem?.signal()
        default:
            break
        }
    }

    public func centralManager(_ c: CBCentralManager, didDiscover per: CBPeripheral,
                               advertisementData: [String: Any], rssi RSSI: NSNumber) {
        // 有广播名时按名字匹配；否则取第一个（服务 UUID 已过滤）
        let advName = (advertisementData[CBAdvertisementDataLocalNameKey] as? String) ?? per.name
        if let n = advName, n != deviceName { return }
        c.stopScan()
        peripheral = per
        per.delegate = self
        c.connect(per, options: nil)
    }

    public func centralManager(_ c: CBCentralManager, didConnect per: CBPeripheral) {
        per.discoverServices([NavSimBLE.serviceUUID])
    }

    public func centralManager(_ c: CBCentralManager, didFailToConnect per: CBPeripheral,
                               error: Error?) {
        print("[BLE] 连接失败：\(error?.localizedDescription ?? "未知")")
        readySem?.signal()
    }

    public func centralManager(_ c: CBCentralManager, didDisconnectPeripheral per: CBPeripheral,
                               error: Error?) {
        // 设备侧会在断连时把导航置为"无数据"（固件行为），这里只提示
        print("[BLE] 已断开\(error.map { "（\($0.localizedDescription)）" } ?? "")")
    }

    // MARK: CBPeripheralDelegate

    public func peripheral(_ per: CBPeripheral, didDiscoverServices error: Error?) {
        guard let svc = per.services?.first(where: { $0.uuid == NavSimBLE.serviceUUID }) else {
            print("[BLE] 未找到目标服务")
            readySem?.signal()
            return
        }
        per.discoverCharacteristics([NavSimBLE.telemetryUUID], for: svc)
    }

    public func peripheral(_ per: CBPeripheral, didDiscoverCharacteristicsFor svc: CBService,
                           error: Error?) {
        guard let ch = svc.characteristics?.first(where: { $0.uuid == NavSimBLE.telemetryUUID }) else {
            print("[BLE] 未找到 Telemetry 特征 —— 固件是否已烧录？")
            readySem?.signal()
            return
        }
        telemetry = ch
        readyResult = true
        print("[BLE] 已连接 \(per.name ?? "?" )，Telemetry 特征就绪")
        readySem?.signal()
    }

    public func peripheral(_ per: CBPeripheral, didWriteValueFor ch: CBCharacteristic,
                           error: Error?) {
        if let e = error {
            failCount += 1
            print("[BLE] 写入失败：\(e.localizedDescription)")
        }
    }
}
