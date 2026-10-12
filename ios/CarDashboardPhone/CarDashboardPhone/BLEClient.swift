// BLEClient.swift —— Core Bluetooth Central（连接 ESP32-S3 Peripheral）
//
// 与固件 ble_telemetry.cpp 对应：
//   设备名 CarDashboard；Service 7D2A0001-8F4A-4B7C-9D21-5E6A3B8C1000
//   Telemetry 特征（Write）7D2A0002-…；Status（Read/Notify）7D2A0003-…
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

    private let serviceUUID = CBUUID(string: "7D2A0001-8F4A-4B7C-9D21-5E6A3B8C1000")
    private let telemetryUUID = CBUUID(string: "7D2A0002-8F4A-4B7C-9D21-5E6A3B8C1000")
    private let statusUUID = CBUUID(string: "7D2A0003-8F4A-4B7C-9D21-5E6A3B8C1000")
    private let deviceName = "CarDashboard"

    // 待发队列：BLE 尚未就绪时缓存最新一条，连接/发现特征后立即补发
    private var pendingPacket: Data?

    override init() {
        super.init()
        central = CBCentralManager(delegate: self, queue: .main)
    }

    func scanAndConnect() {
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
        isConnected = true
        lastNotice = nil
        peripheral.delegate = self
        peripheral.discoverServices([serviceUUID])
    }

    func centralManager(_ central: CBCentralManager, didFailToConnect peripheral: CBPeripheral, error: Error?) {
        isConnected = false
        lastNotice = "连接失败：\(error?.localizedDescription ?? "未知原因")"
    }

    func centralManager(_ central: CBCentralManager, didDisconnectPeripheral peripheral: CBPeripheral, error: Error?) {
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
            peripheral.discoverCharacteristics([telemetryUUID, statusUUID], for: svc)
        }
    }

    func peripheral(_ peripheral: CBPeripheral, didDiscoverCharacteristicsFor service: CBService, error: Error?) {
        guard let chars = service.characteristics else { return }
        for ch in chars {
            if ch.uuid == telemetryUUID {
                telemetryChar = ch
            } else if ch.uuid == statusUUID {
                statusChar = ch
                peripheral.setNotifyValue(true, for: ch)
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
