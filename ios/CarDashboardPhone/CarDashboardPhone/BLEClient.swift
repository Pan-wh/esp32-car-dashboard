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

    /// 设备上报表盘列表（连接后自动读取；设备侧变更时会通知并触发重读）
    @Published var faces = DeviceFaceReport()

    #if DEBUG
    /*
     * 演示模式（仅 Debug 构建，且需启动参数 -demoFaces）。
     *
     * 为什么需要：**iOS 模拟器不支持 CoreBluetooth**（状态恒为 .unsupported），
     * 因此"表盘分组 + 启用/隐藏开关"这套界面在模拟器上**无法连真机验证**。
     * 这个模式注入一份与设备真实上报同形状的数据，让 UI 能脱离硬件开发与验收。
     *
     * Release 构建里这段代码不存在（#if DEBUG），不会误发布。
     */
    static var demoModeEnabled: Bool {
        ProcessInfo.processInfo.arguments.contains("-demoFaces")
    }
    #endif
    /// 上报是否已就绪（界面据此区分"还没读到"与"设备确实没有表盘"）
    @Published var facesReady = false

    private var central: CBCentralManager!
    private var peripheral: CBPeripheral?
    private var telemetryChar: CBCharacteristic?
    private var statusChar: CBCharacteristic?
    private var facesChar: CBCharacteristic?
    private var commandChar: CBCharacteristic?

    private let serviceUUID = CBUUID(string: "7D2A0001-8F4A-4B7C-9D21-5E6A3B8C1000")
    private let telemetryUUID = CBUUID(string: "7D2A0002-8F4A-4B7C-9D21-5E6A3B8C1000")
    private let statusUUID = CBUUID(string: "7D2A0003-8F4A-4B7C-9D21-5E6A3B8C1000")
    private let commandUUID = CBUUID(string: "7D2A0004-8F4A-4B7C-9D21-5E6A3B8C1000")
    /// 表盘列表（Read + Notify）。**Read 才是取数据的正途**：
    /// 列表 JSON 约 360 字节，超过 BLE 单次通知的 MTU（iOS 约 185），
    /// 通知会截断；而 Read 由系统自动用 Read Blob 取回完整值。
    /// 因此收到通知后应**重新 Read**，不要直接用通知里的字节。
    private let facesUUID = CBUUID(string: "7D2A0005-8F4A-4B7C-9D21-5E6A3B8C1000")
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
        #if DEBUG
        if BLEClient.demoModeEnabled {
            // 演示模式：无设备可写，就地更新"当前表盘"，让界面有反馈
            let ok = faces.faces.contains { $0.id == id && $0.enabled }
            if ok {
                faces.currentId = id
                lastNotice = "演示模式：当前表盘 → \(id)"
            } else {
                lastNotice = "演示模式：表盘 \(id) 不可用或已隐藏"
            }
            return ok
        }
        #endif
        return sendCommand("{\"cmd\":\"face\",\"value\":\(id)}")
    }

    /// 启用 / 隐藏某个表盘。设备侧会拒绝"隐藏最后一个"，
    /// 并会在隐藏当前表盘时自动切到另一个启用的表盘。
    @discardableResult
    func setFaceEnabled(_ id: Int, _ on: Bool) -> Bool {
        #if DEBUG
        if BLEClient.demoModeEnabled {
            // 演示模式：无设备可写，就地更新本地状态，保证开关"看起来是活的"
            if let i = faces.faces.firstIndex(where: { $0.id == id }) {
                // 与设备侧同一条安全规则：不允许隐藏最后一个
                if !on && faces.enabledCount <= 1 {
                    lastNotice = "至少保留一个表盘（设备侧也会拒绝）"
                    return false
                }
                faces.faces[i].enabled = on
                lastNotice = "演示模式：表盘 \(id) → \(on ? "启用" : "隐藏")"
            }
            return true
        }
        #endif
        return sendCommand("{\"cmd\":\"face_enabled\",\"value\":\(id),\"on\":\(on ? 1 : 0)}")
    }

    #if DEBUG
    /// 载入演示数据（与固件 face_report_build 的输出同形状）
    func loadDemoFaces() {
        // 与真机实际上报**完全一致**（3 个分组、5 个表盘），
        // 这样截图能代表用户实际会看到的样子。
        let json = """
        {"t":"faces","v":1,"count":5,"mask":31,"cur":0,"faces":[\
        {"id":0,"name":"analog","label":"Analog","group":"clock","on":1},\
        {"id":1,"name":"digital","label":"Digital","group":"clock","on":1},\
        {"id":2,"name":"speed","label":"Speed","group":"speed","on":1},\
        {"id":3,"name":"color","label":"Color","group":"clock","on":1},\
        {"id":4,"name":"nav","label":"Nav","group":"nav","on":1}]}
        """
        if let r = DeviceFaceParser.parse(Data(json.utf8)) {
            faces = r
            facesReady = true
            lastNotice = "演示模式：已载入样本表盘列表（未连接真实设备）"
        }
    }
    #endif

    /// 主动读取表盘列表（连接后与收到通知后调用）
    func refreshFaces() {
        guard let per = peripheral, let ch = facesChar else { return }
        per.readValue(for: ch)
    }

    /// 设置屏幕方向（度）。仅接受 0/90/180/270 —— 固件侧会严格校验，
    /// 非 90 的整数倍一律拒绝（避免误配方向）。
    /// 方向会持久化到仪表 NVS，重启后保持。
    func setRotation(_ degrees: Int) -> Bool {
        sendCommand("{\"cmd\":\"rotate\",\"value\":\(degrees)}")
    }

    func send(packet: TelemetryPacket) {
        guard let data = try? packet.encode() else { return }
        if let ch = telemetryChar, isConnected {
            peripheral?.writeValue(data, for: ch, type: .withResponse)
        } else {
            pendingPacket = data // 暂存，连接后补发
        }
    }

    /// 下发导航消息（与遥测**共用同一个特征**，靠载荷里的 `"t":"nav"` 分流）。
    ///
    /// ⚠️ 与 `send(packet:)` 的一处不同：导航消息**不暂存补发**。
    /// 遥测是"当前状态"，晚一点发仍有意义；而导航指引是**时效性数据**
    /// （距下一转向 30 米，断开重连后这条早已过期），补发只会显示过期指引。
    /// 断连时直接丢弃，由 NavSession 在恢复后发新的一条。
    func sendNav(snapshot: NavSnapshot) {
        guard let data = try? NavPacket(snapshot: snapshot).encode() else { return }
        guard let ch = telemetryChar, isConnected else { return }
        peripheral?.writeValue(data, for: ch, type: .withResponse)
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
            peripheral.discoverCharacteristics([telemetryUUID, statusUUID, commandUUID, facesUUID],
                                               for: svc)
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
            } else if ch.uuid == facesUUID {
                facesChar = ch
                // 订阅变更提示；真正的数据靠 Read 取回（见 facesUUID 注释）
                peripheral.setNotifyValue(true, for: ch)
                peripheral.readValue(for: ch)
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
            return
        }

        // 表盘列表：通知的载荷可能被 MTU 截断，**解析失败就重新 Read**。
        // 用"能否解析成完整 JSON"来区分"通知截断"与"Read 取回"，
        // 无需额外状态机；完整值解析成功后即结束，不会反复重读。
        if characteristic.uuid == facesUUID {
            guard let data = characteristic.value else { return }
            if let report = DeviceFaceParser.parse(data) {
                faces = report
                facesReady = true
                if report.truncated {
                    lastNotice = "表盘列表不完整（设备端已截断），部分开关可能显示不准"
                }
            } else {
                // 截断或格式不对 → 走 Read 取完整值
                // 复用 refreshFaces() 而不是重复一遍 readValue，
                // 避免"两处各自读取"将来只改一处而漏改（此前它就是个没人用的死方法）
                refreshFaces()
            }
        }
    }
}
