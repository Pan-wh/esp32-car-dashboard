// BLEClient.swift —— Core Bluetooth Central（连接 ESP32-S3 Peripheral）
//
// 与固件 ble_telemetry.cpp 对应：
//   设备名 CarDashboard；Service 7D2A0001-8F4A-4B7C-9D21-5E6A3B8C1000
//   Telemetry 特征（Write）7D2A0002-…；Status（Read/Notify）7D2A0003-…
//   Command（Write）7D2A0004-…：设备控制（亮度/息屏/表盘切换）
import CoreBluetooth
import Combine
import Foundation

final class BLEClient: NSObject, ObservableObject {
    @Published var state: CBManagerState = .unknown
    @Published var isConnected = false
    @Published var discoveredName: String?
    /// 最近一次失败/等待原因（供 UI 显示，避免"点了没反应"）
    @Published var lastNotice: String?

    private var central: CBCentralManager!
    private var peripheral: CBPeripheral?
    private var telemetryChar: CBCharacteristic?
    private var statusChar: CBCharacteristic?
    private var commandChar: CBCharacteristic?

    private let serviceUUID = CBUUID(string: "7D2A0001-8F4A-4B7C-9D21-5E6A3B8C1000")
    private let telemetryUUID = CBUUID(string: "7D2A0002-8F4A-4B7C-9D21-5E6A3B8C1000")
    private let statusUUID = CBUUID(string: "7D2A0003-8F4A-4B7C-9D21-5E6A3B8C1000")
    private let commandUUID = CBUUID(string: "7D2A0004-8F4A-4B7C-9D21-5E6A3B8C1000")
    private let deviceName = "CarDashboard"

    // 待发队列：BLE 尚未就绪时缓存最新一条，连接/发现特征后立即补发
    private var pendingPacket: Data?

    override init() {
        super.init()
        central = CBCentralManager(delegate: self, queue: .main)
    }

    func scanAndConnect() {
        /*
         * 已连接时直接返回，**不要覆盖 lastNotice**。
         *
         * 真机踩坑：启动时 onAppear 自动连一次，用户随后点「开始驾驶」会再次
         * 调用本函数 → 文案被改成"扫描中…"；但此时连接已建立，不会再有
         * didConnect 回调来清空它 → 界面永久停在"扫描中…请确认仪表已上电"，
         * 而 BLE 状态却显示"已连接"（自相矛盾）。
         */
        if isConnected {
            lastNotice = "已连接 CarDashboard"
            return
        }
        switch state {
        case .poweredOn:
            lastNotice = "扫描中…请确认仪表已上电"
            central.scanForPeripherals(withServices: [serviceUUID], options: nil)
        case .unknown, .resetting:
            lastNotice = "蓝牙尚未就绪，请稍候再试"
        case .unsupported:
            lastNotice = "此设备不支持蓝牙 LE"
        case .unauthorized:
            lastNotice = "未授权蓝牙：请在 设置 → 隐私与安全性 → 蓝牙 中允许本 App"
        case .poweredOff:
            lastNotice = "蓝牙已关闭：请在控制中心或设置中打开蓝牙"
        @unknown default:
            lastNotice = "蓝牙状态异常"
        }
    }

    /// 供 UI 显示的状态文案
    var stateText: String {
        switch state {
        case .poweredOn: return isConnected ? "已连接" : "蓝牙就绪"
        case .unknown: return "初始化中"
        case .resetting: return "重置中"
        case .unsupported: return "不支持 BLE"
        case .unauthorized: return "未授权"
        case .poweredOff: return "蓝牙已关闭"
        @unknown default: return "未知"
        }
    }

    /// 发送设备控制命令（功能①/③）。返回是否已发出（未连接/特征未就绪时返回 false）。
    /// - Parameter json: 形如 {"cmd":"brightness","value":128}
    @discardableResult
    func sendCommand(_ json: String) -> Bool {
        guard let per = peripheral, let ch = commandChar, isConnected else {
            lastNotice = "命令未发送：仪表未连接或命令特征未就绪"
            return false
        }
        guard let data = json.data(using: .utf8) else { return false }
        per.writeValue(data, for: ch, type: .withResponse)
        lastNotice = "已发送：\(json)"
        return true
    }

    /// 便捷方法：亮度 0-255
    @discardableResult
    func setBrightness(_ value: Int) -> Bool {
        sendCommand("{\"cmd\":\"brightness\",\"value\":\(max(0, min(255, value)))}")
    }

    /// 便捷方法：息屏 / 亮屏
    @discardableResult
    func setScreen(on: Bool) -> Bool {
        sendCommand("{\"cmd\":\"screen\",\"value\":\"\(on ? "on" : "off")\"}")
    }

    /// 便捷方法：切换到指定表盘（功能③）
    @discardableResult
    func setFace(_ id: Int) -> Bool {
        sendCommand("{\"cmd\":\"face\",\"value\":\(id)}")
    }

    func send(packet: TelemetryPacket) {
        guard let data = try? packet.encode() else { return }
        if let ch = telemetryChar, isConnected {
            peripheral?.writeValue(data, for: ch, type: .withResponse)
        } else {
            pendingPacket = data // 暂存，连接后补发
        }
    }
}

extension BLEClient: CBCentralManagerDelegate {
    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        NSLog("[BLE] 状态回调: %d", central.state.rawValue)
        state = central.state
        if central.state != .poweredOn {
            isConnected = false
        }
        if central.state == .poweredOn, discoveredName == nil {
            lastNotice = "蓝牙就绪，点「连接仪表」开始扫描"
        } else if central.state == .unauthorized {
            lastNotice = "未授权蓝牙：请在 设置 → 隐私与安全性 → 蓝牙 中允许本 App"
        } else if central.state == .poweredOff {
            lastNotice = "蓝牙已关闭：请在控制中心或设置中打开蓝牙"
        }
    }

    func centralManager(_ central: CBCentralManager,
                        didDiscover peripheral: CBPeripheral,
                        advertisementData: [String: Any], rssi RSSI: NSNumber) {
        let name = (advertisementData[CBAdvertisementDataLocalNameKey] as? String) ?? peripheral.name
        // 优先匹配设备名，避免误连其他广播同一 service 的设备
        guard name == deviceName else { return }
        discoveredName = name
        self.peripheral = peripheral
        central.stopScan()
        central.connect(peripheral, options: nil)
    }

    func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        NSLog("[BLE] 已连接，开始发现服务")
        isConnected = true
        lastNotice = "已连接 CarDashboard"
        peripheral.delegate = self
        peripheral.discoverServices([serviceUUID])
    }

    func centralManager(_ central: CBCentralManager, didFailToConnect peripheral: CBPeripheral, error: Error?) {
        isConnected = false
        lastNotice = "连接失败：\(error?.localizedDescription ?? "未知原因")"
    }

    func centralManager(_ central: CBCentralManager, didDisconnectPeripheral peripheral: CBPeripheral, error: Error?) {
        NSLog("[BLE] 已断开: %@", error?.localizedDescription ?? "无错误")
        isConnected = false
        lastNotice = "已断开，正在尝试重连…"
        telemetryChar = nil
        statusChar = nil
        // 自动重连（固件断连后会重新广播）
        central.scanForPeripherals(withServices: [serviceUUID], options: nil)
    }
}

extension BLEClient: CBPeripheralDelegate {
    func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        guard let services = peripheral.services else { return }
        for svc in services where svc.uuid == serviceUUID {
            peripheral.discoverCharacteristics([telemetryUUID, statusUUID, commandUUID], for: svc)
        }
    }

    func peripheral(_ peripheral: CBPeripheral, didDiscoverCharacteristicsFor service: CBService, error: Error?) {
        guard let chars = service.characteristics else { return }
        NSLog("[BLE] 发现 %d 个特征", chars.count)
        for ch in chars {
            if ch.uuid == telemetryUUID {
                telemetryChar = ch
            } else if ch.uuid == statusUUID {
                statusChar = ch
                peripheral.setNotifyValue(true, for: ch)
            } else if ch.uuid == commandUUID {
                commandChar = ch
            }
        }
        // 若连接前有待发包，立即补发
        if let pending = pendingPacket, let ch = telemetryChar {
            peripheral.writeValue(pending, for: ch, type: .withResponse)
            pendingPacket = nil
        }
    }

    func peripheral(_ peripheral: CBPeripheral, didWriteValueFor characteristic: CBCharacteristic, error: Error?) {
        // 写入确认（阶段3前台可打点/日志；断连重试由 didDisconnect 处理）
    }

    func peripheral(_ peripheral: CBPeripheral, didUpdateValueFor characteristic: CBCharacteristic, error: Error?) {
        // Status 特征（Notify）：可读固件版本/同步状态，前台展示用
        if characteristic.uuid == statusUUID, let data = characteristic.value {
            _ = String(data: data, encoding: .utf8)
        }
    }
}
