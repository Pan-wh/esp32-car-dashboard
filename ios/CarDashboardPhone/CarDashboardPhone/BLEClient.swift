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

    /**
     自动连接开关（由**用户意图**驱动，不是由连接状态驱动）。

     - `true`（默认）：蓝牙一就绪就自动扫描并连上仪表；掉线后自动重连。
       用户不必再手动点「连接仪表」。
     - `false`：用户点过「断开连接」→ 在用户重新点「连接仪表」之前，
       **不再自动扫描/重连**（否则刚断开就会被自动连回去，按钮形同虚设）。

     ⚠️ 不能用 `isConnected` 代替本标志：断连瞬间 `isConnected` 也是 false，
       但它表达的是"当前没连着"，而本标志表达的是"用户想不想连着"。
       两者混用会导致「断开连接」永远失效或掉线后不再自愈。
     */
    @Published private(set) var autoConnectEnabled = true

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

    /**
     演示"用户已手动断开"的界面状态（仅 Debug 构建，需启动参数 -demoDisconnected）。

     为什么需要：模拟器**不支持 CoreBluetooth**，蓝牙状态恒为 .unsupported，
     永远走不到"已连接 → 点断开"这条路径，因此"断开后界面长什么样"
     （是否显示「连接仪表」、是否提示已停止自动连接）无法在模拟器上验收。
     这个开关直接把状态置成"用户断开后的样子"，让该界面可截图验收。

     Release 构建里这段代码不存在（#if DEBUG），不会误发布。
     */
    static var demoDisconnectedEnabled: Bool {
        ProcessInfo.processInfo.arguments.contains("-demoDisconnected")
    }
    #endif
    /// 上报是否已就绪（界面据此区分"还没读到"与"设备确实没有表盘"）
    @Published var facesReady = false

    private var central: CBCentralManager!
    private var peripheral: CBPeripheral?
    private var telemetryChar: CBCharacteristic?
    /// 地图窗口的待发队列（见 sendMapMessages 的说明：必须等应答再发下一条）
    private var mapQueue: [Data] = []
    private var mapSending = false
    /**
     * 正在发送期间到来的**新窗口**（只保留最新一个）。
     *
     * ⚠️ 不能直接覆盖 `mapQueue`：一个窗口约 50 条消息，发送要 1~2 秒，
     *    期间导航时钟会再次取窗口。若直接覆盖，队列里**最前面那批**
     *    （也就是道路 `mr`）会被丢掉，而后面还没入队的建筑/路线反而能在下一次
     *    发送中存活 —— 实测症状正是"道路恒为 0 条，建筑和路线正常"。
     */
    private var pendingMap: [Data]?
    /// 队列最近一次推进的时刻（看门狗用，见 pumpMapQueue）
    private var mapLastProgress = Date.distantPast
    /// 看门狗定时器（见 watchMapQueue）
    private var mapWatchdog: Timer?
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

    /// 本次连接是否已下发过"纯时间包"（每次连接只发一次，见 sendTimeOnConnect）。
    /// 断连重连时会随连接生命周期重置，保证**每次新建连接**都能校一次时。
    private var didSendTimeOnConnect = false

    override init() {
        super.init()
        central = CBCentralManager(delegate: self, queue: .main)
    }

    /// 是否正在扫描（用于去重：CoreBluetooth 重复 scanForPeripherals 不会报错，
    /// 但会在状态回调里反复启动扫描，白耗电）
    private var isScanning = false

    func scanAndConnect() {
        startMapWatchdog()
        /*
         * 用户主动点「连接仪表」= 明确表达"我要连" → 重新打开自动连接。
         * 这样「断开连接」之后仍能靠这个按钮恢复自动连接行为。
         */
        autoConnectEnabled = true
        connectIfPossible()
    }

    /**
     尝试（重新）开始扫描并连接仪表。

     只在**蓝牙就绪 + 用户未要求断开 + 尚未连接 + 尚未在扫描**时真正启动扫描，
     其余情况只更新提示文案。

     这是自动连接与手动连接的**唯一入口**：`centralManagerDidUpdateState`
     （蓝牙刚就绪）、`didDisconnectPeripheral`（掉线重连）、用户点按钮，
     三条路径都汇聚到这里，避免"三处各自写一份判断"而漏改。
     */
    private func connectIfPossible() {
        /*
         * 决策全部交给纯函数 BLEConnectionPolicy.decide —— 那里穷举了
         * "用户断开优先于蓝牙就绪"等易错组合，并有 macOS 单测覆盖
         * （tools/test_ble_policy.swift）。此处只负责执行与提示。
         */
        let decision = BLEConnectionPolicy.decide(.init(
            bluetoothReady: state == .poweredOn,
            isConnected: isConnected || peripheral?.state == .connected,
            isScanning: isScanning,
            autoConnectEnabled: autoConnectEnabled))

        switch decision {
        case .startScan:
            lastNotice = "扫描中…请确认仪表已上电"
            isScanning = true
            central.scanForPeripherals(withServices: [serviceUUID], options: nil)

        case .alreadyScanning:
            // 已在扫描：保持原提示文案，不要覆盖（否则会反复闪成"扫描中…"）
            break

        case .alreadyConnected:
            /*
             * 已连接时**不要覆盖 lastNotice**。
             *
             * 真机踩坑：启动时 onAppear 自动连一次，用户随后点「开始驾驶」会再次
             * 调用本函数 → 文案被改成"扫描中…"；但此时连接已建立，不会再有
             * didConnect 回调来清空它 → 界面永久停在"扫描中…请确认仪表已上电"，
             * 而 BLE 状态却显示"已连接"（自相矛盾）。
             */
            lastNotice = "已连接 CarDashboard"

        case .userDisconnected:
            // 用户主动断开：不扫描、不重连，只说明现状与恢复方式
            lastNotice = "已断开连接（点「连接仪表」可重新连接）"

        case .bluetoothNotReady:
            switch state {
            case .unknown, .resetting:
                lastNotice = "蓝牙尚未就绪，请稍候再试"
            case .unsupported:
                lastNotice = "此设备不支持蓝牙 LE"
            case .unauthorized:
                lastNotice = "未授权蓝牙：请在 设置 → 隐私与安全性 → 蓝牙 中允许本 App"
            case .poweredOff:
                lastNotice = "蓝牙已关闭：请在控制中心或设置中打开蓝牙"
            case .poweredOn:
                break // 不可达：poweredOn 时决策不会是 .bluetoothNotReady
            @unknown default:
                lastNotice = "蓝牙状态异常"
            }
        }
    }

    /**
     手动断开连接（用户点「断开连接」）。

     语义要点：**断开的同时关掉自动连接**，否则下一秒就会被 `didDisconnectPeripheral`
     的自动重连接回去 —— 用户会看到"点了断开又自己连上了"。
     要恢复自动连接：点「连接仪表」（`scanAndConnect`）。
     */
    func disconnect() {
        autoConnectEnabled = false
        pendingPacket = nil
        resetMapQueue(reason: "用户主动断开")
        stopScanning()
        /*
         * ⚠️ 判断"是否真的连着"，而不是"peripheral 是否非空"。
         *
         * `peripheral` 在上一次连接后**不会被自动清空**，所以它非空并不代表
         * 当前处于连接态（例如：刚掉线正在重连时用户点了断开）。
         * 若只判断非空就去 cancelPeripheralConnection，会丢掉"清 UI 状态"
         * 这一步 —— 界面会停在"正在连接…"，而实际已经没有连接了。
         */
        let live = (peripheral?.state == .connected) || isConnected
        if let per = peripheral, live {
            central.cancelPeripheralConnection(per) // 触发 didDisconnectPeripheral → 清状态
        } else {
            clearConnectionState()
            lastNotice = "已断开连接（点「连接仪表」可重新连接）"
        }
    }

    /// 复位所有"当前连接"相关的状态（断连收尾的**唯一出口**，避免两处各清一半）
    private func clearConnectionState() {
        isConnected = false
        telemetryChar = nil
        statusChar = nil
        commandChar = nil
        facesChar = nil
        facesReady = false
    }

    /// 停止扫描并复位扫描标志（幂等）
    private func stopScanning() {
        guard isScanning else { return }
        isScanning = false
        central.stopScan()
    }

    /// 供 UI 显示的状态文案
    ///
    /// 蓝牙就绪与否的文案由 `BLEConnectionPolicy.stateText` 决定（有单测）；
    /// 这里只处理"蓝牙根本没就绪"的细分原因。
    var stateText: String {
        if state == .poweredOn {
            return BLEConnectionPolicy.stateText(bluetoothReady: true,
                                                isConnected: isConnected,
                                                autoConnectEnabled: autoConnectEnabled)
        }
        switch state {
        case .unknown: return "初始化中"
        case .resetting: return "重置中"
        case .unsupported: return "不支持 BLE"
        case .unauthorized: return "未授权"
        case .poweredOff: return "蓝牙已关闭"
        case .poweredOn: return "蓝牙就绪" // 不可达（上面已提前返回）
        @unknown default: return "未知"
        }
    }

    /// 发送设备控制命令（功能①/③）。返回是否已发出（未连接/特征未就绪时返回 false）。
    /// - Parameter json: 形如 {"cmd":"brightness","value":128}
    /**
     发送设备控制命令。返回是否已发出（未连接/特征未就绪时返回 false）。

     - Parameter quiet: **成功时不更新 `lastNotice`**。
       演示滑块会以 10Hz 连续发送，若每次都写 `lastNotice`，而它又被界面绑定，
       就会造成**每秒 10 次整页重绘**（用户实测的"卡顿"之一）。
       失败时**仍然**会写，因为那才是需要用户看到的信息。
     */
    @discardableResult
    func sendCommand(_ json: String, quiet: Bool = false) -> Bool {
        guard let per = peripheral, let ch = commandChar, isConnected else {
            lastNotice = "命令未发送：仪表未连接或命令特征未就绪"
            return false
        }
        guard let data = json.data(using: .utf8) else { return false }
        per.writeValue(data, for: ch, type: .withResponse)
        if !quiet { lastNotice = "已发送：\(json)" }
        return true
    }

    /// 便捷方法：亮度 0-255
    @discardableResult
    func setBrightness(_ value: Int) -> Bool {
        sendCommand("{\"cmd\":\"brightness\",\"value\":\(max(0, min(255, value)))}")
    }

    /**
     演示速度（App 的「模拟速度测试」滑块）。

     - Parameter kmh: 目标车速；**负数 = 退出演示**。

     ⚠️ 这只是**演示**用途：固件会把它显示为状态行 "DEMO"（琥珀色），
        并且**只要收到真实遥测 / BLE 断开就自动退出** —— 不让演示速度
        看起来像真实车速。App 在「开始驾驶」时也主动发一次退出，
        让切换立刻生效。
     */
    @discardableResult
    func sendDemoSpeed(_ kmh: Double) -> Bool {
        /* quiet：滑块 10Hz 连发，成功时不刷 lastNotice（否则整页 10Hz 重绘）*/
        sendCommand("{\"cmd\":\"demo_speed\",\"value\":\(Int(kmh.rounded()))}", quiet: true)
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
        // 含设备显示状态字段（bright/rot/screen）—— 固件 2026-10-15 起会上报。
        let json = """
        {"t":"faces","v":1,"count":5,"mask":31,"cur":0,"bright":70,"rot":0,"screen":1,"faces":[\
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

    /// 演示"用户已手动断开"的状态（仅 Debug + -demoDisconnected）。
    ///
    /// 直接走 `disconnect()` 的**同一套状态**（autoConnectEnabled=false + 清连接态），
    /// 不另写一份"看起来像断开"的假状态 —— 否则演示的就不是真实代码路径了。
    func enterDemoDisconnectedState() {
        autoConnectEnabled = false
        clearConnectionState()
        lastNotice = "已断开连接（点「连接仪表」可重新连接）"
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

    /// 连接成功后下发一次**纯时间包**，仅用于让仪表同步 RTC 时间。
    ///
    /// 背景：点「连接仪表」只建立 BLE 连接、**不启动定位**，因此设备不会收到
    /// 任何遥测包，RTC 时间也不会更新（要等点「开始驾驶」才发带 ts_utc_ms 的包）。
    /// 这里在特征发现完成后立刻发一次最小时间包，让"仅连接"也能校时。
    ///
    /// 语义：
    ///   - 每次 BLE 连接只发一次（didSendTimeOnConnect 标志，didConnect 时重置）；
    ///   - 不依赖定位/权限（纯时间包无定位字段，locationValid=false）；
    ///   - telemetryChar 未就绪时不发（等 didDiscoverCharacteristicsFor 里补发）。
    func sendTimeOnConnect() {
        guard isConnected, telemetryChar != nil, !didSendTimeOnConnect else { return }
        let nowUtcMs = Int64(Date().timeIntervalSince1970 * 1000)
        guard let data = try? TelemetryPacket.timeOnly(nowUtcMs: nowUtcMs).encode() else { return }
        peripheral?.writeValue(data, for: telemetryChar!, type: .withResponse)
        didSendTimeOnConnect = true
        NSLog("[BLE] 已下发纯时间包（连接校时）")
    }

    /// 下发导航消息（与遥测**共用同一个特征**，靠载荷里的 `"t":"nav"` 分流）。
    ///
    /// ⚠️ 与 `send(packet:)` 的一处不同：导航消息**不暂存补发**。
    /// 遥测是"当前状态"，晚一点发仍有意义；而导航指引是**时效性数据**
    /// （距下一转向 30 米，断开重连后这条早已过期），补发只会显示过期指引。
    /// 断连时直接丢弃，由 NavSession 在恢复后发新的一条。
    /// - Parameter vehicleOffset: 自车在地图窗口局部坐标中的偏移（米）；
    ///   没有地图窗口时传 nil（字段省略，固件不用旧偏移）。
    /**
     顺序下发一组地图窗口消息（`mw` … `me`）。

     ⚠️ **必须按顺序、且用 `.withResponse`**：固件靠"收到 `me` 才整体切换"来
     保证画面不会画到一半。若改成 `.withoutResponse`，写入可能乱序或丢失，
     窗口会一直等到 3 秒超时被丢弃。

     - Returns: 实际发出的条数；未连接时为 0。
     */
    @discardableResult
    func sendMapMessages(_ messages: [String]) -> Int {
        guard telemetryChar != nil, isConnected else { return 0 }
        /*
         * ⚠️ **不能一口气连发**。
         *
         * 一次地图窗口约 40 条消息。曾用 for 循环直接连发 —— CoreBluetooth 的
         * 内部发送队列会溢出，**静默丢弃**中间的写入。实测现象：
         * 约 70% 的窗口因收不到结束标记而被固件按超时丢弃，且丢的总是**前面**
         * 那批（道路/路线），后面的建筑反而存活 —— 因为队列从前往后溢出。
         *
         * 正确做法是"发一条、等 `didWriteValueFor`、再发下一条"，
         * 让 peripherals 的应答来限速（见 pumpMapQueue）。
         */
        watchMapQueue() /* 先兜底：若上一次发送卡死，这里就恢复 */

        let data = messages.compactMap { $0.data(using: .utf8) }
        if mapSending {
            /* 正在发上一个窗口：不要打断（打断会丢掉已排队的道路消息）*/
            pendingMap = data
            return 0
        }
        mapQueue = data
        pumpMapQueue()
        return data.count
    }

    /// 从队列取一条发出。每发的下一条都由 `didWriteValueFor` 驱动。
    /// 启动看门狗定时器（幂等）。每 2 秒检查一次队列是否卡死。
    private func startMapWatchdog() {
        guard mapWatchdog == nil else { return }
        mapWatchdog = Timer.scheduledTimer(withTimeInterval: 2.0, repeats: true) { [weak self] _ in
            self?.watchMapQueue()
        }
    }

    /// 重置地图发送队列（断连、写入失败、看门狗超时时调用）
    fileprivate func resetMapQueue(reason: String) {
        if mapSending || !mapQueue.isEmpty || pendingMap != nil {
            print("[BLE] 地图队列重置（\(reason)）：剩余 \(mapQueue.count)+\(pendingMap?.count ?? 0) 条")
        }
        mapQueue.removeAll()
        pendingMap = nil
        mapSending = false
    }

    /**
     看门狗：由**定时器**驱动，而不是由写入回调驱动。
     
     ⚠️ 这一点很关键：曾经把它写在 `pumpMapQueue()` 里 —— 而那个函数只由
        `didWriteValueFor` 调用。一旦某次应答丢失（或发送中途断连），
        `mapSending` 会永远停在 true：既不再发新数据，看门狗也永远不会跑，
        形成**死锁**。表现就是"网关日志显示 App 一直在取窗口、导航/遥测正常，
        但设备一条地图消息都收不到" —— 排查了很久。
     
     现在改为：断连时立即重置 + 定时器每 2 秒兜底检查一次。
     */
    fileprivate func watchMapQueue() {
        guard mapSending else { return }
        if Date().timeIntervalSince(mapLastProgress) > 4.0 {
            resetMapQueue(reason: "发送超时未推进")
        }
    }

    fileprivate func pumpMapQueue() {
        mapLastProgress = Date()

        /* 当前窗口发完 → 若有更新的窗口在等，接着发它 */
        if mapQueue.isEmpty {
            if let next = pendingMap {
                pendingMap = nil
                mapQueue = next
            } else {
                mapSending = false
                return
            }
        }
        guard let ch = telemetryChar, isConnected else {
            resetMapQueue(reason: "未连接")
            return
        }
        mapSending = true
        let data = mapQueue.removeFirst()
        peripheral?.writeValue(data, for: ch, type: .withResponse)
    }

    func sendNav(snapshot: NavSnapshot, vehicleOffset: (x: Int, y: Int)? = nil) {
        guard let data = try? NavPacket(snapshot: snapshot, vehicleOffset: vehicleOffset).encode()
        else { return }
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
            /*
             * 蓝牙被关掉时扫描也一并停止，并把标志复位 ——
             * 否则重新打开蓝牙后 connectIfPossible() 会因 isScanning 仍为 true
             * 而**拒绝启动扫描**，表现为"开了蓝牙也连不上"。
             */
            isScanning = false
        }
        /*
         * 蓝牙就绪即自动连接（本次优化的核心）。
         *
         * ⚠️ **无论什么状态都调用 connectIfPossible()**，不要只判断 `.poweredOn`
         *    再单独写一串 else if 设文案 —— 那样 `.unknown` / `.resetting` /
         *    `.unsupported` 三种状态会**漏设 lastNotice**，界面就停在上一轮的
         *    旧文案上（实测：模拟器不支持 BLE，却显示"蓝牙尚未就绪，请稍候再试"）。
         *    统一入口后，每个状态都有确定的文案。
         */
        connectIfPossible()
    }

    func centralManager(_ central: CBCentralManager,
                        didDiscover peripheral: CBPeripheral,
                        advertisementData: [String: Any], rssi RSSI: NSNumber) {
        // 用户在扫描途中点了「断开连接」→ 立刻放弃这次发现，不要再连
        guard autoConnectEnabled else {
            stopScanning()
            return
        }
        let name = (advertisementData[CBAdvertisementDataLocalNameKey] as? String) ?? peripheral.name
        // 优先匹配设备名，避免误连其他广播同一 service 的设备
        guard name == deviceName else { return }
        discoveredName = name
        self.peripheral = peripheral
        stopScanning()
        central.connect(peripheral, options: nil)
    }

    func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        NSLog("[BLE] 已连接，开始发现服务")
        isConnected = true
        isScanning = false
        lastNotice = "已连接 CarDashboard"
        didSendTimeOnConnect = false // 新连接：允许再校一次时
        peripheral.delegate = self
        peripheral.discoverServices([serviceUUID])
    }

    func centralManager(_ central: CBCentralManager, didFailToConnect peripheral: CBPeripheral, error: Error?) {
        isConnected = false
        /*
         * 连接失败也要继续重试（例如仪表刚上电、广播还没起来）。
         * 若用户已要求断开则不重试 —— 判断在 connectIfPossible 内统一做。
         */
        lastNotice = autoConnectEnabled
            ? "连接失败：\(error?.localizedDescription ?? "未知原因")，正在重试…"
            : "已断开连接（点「连接仪表」可重新连接）"
        connectIfPossible()
    }

    func centralManager(_ central: CBCentralManager, didDisconnectPeripheral peripheral: CBPeripheral, error: Error?) {
        NSLog("[BLE] 已断开: %@", error?.localizedDescription ?? "无错误")
        clearConnectionState()
        /*
         * ⚠️ 必须重置地图队列：若断连发生在发送途中，`mapSending` 会停在 true，
         *    之后的窗口只会写进 pendingMap 而永远发不出去（静默失效）。
         */
        resetMapQueue(reason: "连接断开")
        /*
         * 自动重连（固件断连后会重新广播）。
         *
         * 但**用户主动断开时不能重连**：`disconnect()` 已把 autoConnectEnabled
         * 置 false，connectIfPossible() 会识别并只更新提示文案。
         * 切勿在此处直接 scanForPeripherals —— 那会让「断开连接」按钮失效。
         */
        lastNotice = BLEConnectionPolicy.shouldReconnectAfterDrop(autoConnectEnabled: autoConnectEnabled)
            ? "已断开，正在尝试重连…"
            : "已断开连接（点「连接仪表」可重新连接）"
        connectIfPossible()
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
        // 特征就绪后补发一次纯时间包（连接校时，仅一次）
        sendTimeOnConnect()
    }

    func peripheral(_ peripheral: CBPeripheral, didWriteValueFor characteristic: CBCharacteristic, error: Error?) {
        /*
         * 地图窗口：写一条成功后再发下一条（限速，防止队列溢出）。
         *
         * ⚠️ **必须按 UUID 比较，不能比较对象**。
         * CoreBluetooth 在（重新）发现服务时会给特征创建**新的** CBCharacteristic
         * 实例，`characteristic == telemetryChar` 会恒为 false ——
         * 于是队列停在第一条再也不推进，表现为"导航/遥测正常，但地图一条都发不出去"。
         * 本文件其余地方（如 Status 特征）也是按 uuid 比较的，此处保持一致。
         */
        if characteristic.uuid == telemetryUUID {
            if let e = error {
                print("[BLE] 地图消息写入失败：\(e.localizedDescription)")
                resetMapQueue(reason: "写入失败")
            } else {
                pumpMapQueue()
            }
        }
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
