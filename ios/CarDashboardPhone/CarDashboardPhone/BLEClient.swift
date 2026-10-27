// BLEClient.swift —— Core Bluetooth Central（连接 ESP32-S3 Peripheral）
//
// 与固件 ble_telemetry.cpp 对应：
//   设备名 CarDashboard；Service 7D2A0001-8F4A-4B7C-9D21-5E6A3B8C1000
//   Telemetry 特征（Write）7D2A0002-…；Status（Read/Notify）7D2A0003-…
//   Command（Write）7D2A0004-…：设备控制（亮度/息屏/表盘切换）
import CoreBluetooth
import Combine
import Foundation

/// 「格式化 SD 卡」的 UI 状态机（唯一真值来自设备 `fmt` / `err` 事件）
enum PhotoFormatState: Equatable {
    case idle
    case formatting
    case success
    case failed(String)
}

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

    // MARK: - 照片服务（协议见 Photo/PhotoProtocol.swift、docs/photo-protocol.md）
    //
    // 设计沿用表盘那套已经验证过的模式：
    //   - 能力/状态/清单都是 **Read 为主**（载荷可能被 Notify 截断）；
    //   - `PhotoState` 的 Notify **只当"变了"的提示**，收到就重新 Read，不用通知载荷；
    //   - 清单走 `pq → pf×N → pz` 分页组装，不依赖单条大包。

    /// 设备是否暴露照片服务（`7D2A0010`）。**没有 = 旧固件** → UI 退回"SD 卡导入"说明。
    @Published private(set) var photoSupported = false
    /// 照片服务是否已探测过（区分「还没连上」与「固件不支持」）
    @Published private(set) var photoServiceProbed = false
    /// 照片库快照（能力 + 状态 + 清单）。nil = 还没读到过
    @Published private(set) var photoLibrary: DevicePhotoLibrary?
    /// 上传队列进度（串行执行；UI 直接渲染）
    @Published private(set) var photoUploads: [PhotoUploadProgress] = []
    /// 最近一次**成功**上传的实测速率（bytes/s）。持久化到 UserDefaults，
    /// 用于动图入口第③层门控（"本次连接速率 ≥30 KB/s"）。
    @Published private(set) var photoMeasuredUploadRate: Double? = BLEClient.loadPhotoMeasuredRate()
    /// 格式化 SD 卡：idle / formatting / success / failed
    @Published private(set) var photoFormatState: PhotoFormatState = .idle
    private var photoFormatTimeout: DispatchWorkItem?

    /// 照片服务的 5 个特征（发现后按 UUID 字符串比较，**不能比较对象**）
    private var photoCtlChar: CBCharacteristic?
    private var photoDataChar: CBCharacteristic?
    private var photoEvtChar: CBCharacteristic?
    private var photoInfoChar: CBCharacteristic?
    private var photoStateChar: CBCharacteristic?

    private let photoServiceUUID = CBUUID(string: PhotoGATT.service)

    /// 正在等 `PhotoState` 的 Read 回包（用来区分 Read 结果与 Notify 提示）
    private var photoStateReadPending = false
    /// 已排过一次"因 Notify 而重读"（限流：设备侧 notify ≥200 ms，不能反过来放大成 Read 风暴）
    private var photoStateReadScheduled = false
    /// 清单分页组装缓存（`pq` 清空 → `pf` 追加 → `pz` 发布）
    private var photoListBuffer: [DevicePhoto] = []
    /// 是否已有一次 list 在途（避免重复 list 导致两套分页交错）
    private var photoListRequested = false
    /// list 请求发出的时刻（超时兜底，防止设备不回导致永远不再请求）
    private var photoListRequestedAt: TimeInterval = 0
    /// 清单/状态刷新防抖（`changed` 事件可能连发）
    private var photoRefreshScheduled = false

    /// 上传队列（**串行**：一次只传一张，避免 4096 B 窗口被切碎）
    private var photoUploadQueue: [PhotoUploadRequest] = []
    /// 所有上传请求（`retryPhotoUpload` 要拿回 fileURL/crc 等元数据）
    private var photoRequestStore: [String: PhotoUploadRequest] = [:]
    /// 正在上传的那一张
    private var photoActiveTransfer: PhotoTransfer?
    private var photoActiveRequest: PhotoUploadRequest?
    /// 当前上传的本地文件读取器（后台串行队列 + 4 KB 读前缓存）
    private var photoChunkSource: PhotoFileChunkSource?
    /// 首包放行：iOS 17 实测新连接上 `canSendWriteWithoutResponse` 初始为 false，
    /// 若严格等它是 true 就永远不写 → 永远等不到 peripheralIsReady（死锁）。
    private var photoFirstWriteBootstrap = true
    /// 上传看门狗：**必须定时器驱动**（写在写回调里会因丢回执死锁，见地图队列的教训）
    private var photoWatchdog: Timer?

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
        /*
         * 照片服务：特征全部失效（重连后要重新发现）。
         *
         * `photoLibrary` 故意**不清空** —— 界面上"上次看到的照片列表"比"突然空白"
         * 更有用，未连接状态由 `isConnected` 单独表达。
         * 正在上传的那一张转成 `waitingReconnect`：设备保留 `.part`，
         * 重连后 begin 会拿到 `off > 0` 自动续传（协议 §8.4）。
         */
        photoCtlChar = nil
        photoDataChar = nil
        photoEvtChar = nil
        photoInfoChar = nil
        photoStateChar = nil
        photoSupported = false
        photoServiceProbed = false
        photoStateReadPending = false
        photoStateReadScheduled = false
        photoListRequested = false
        photoListBuffer = []
        photoRefreshScheduled = false
        stopPhotoWatchdog()
        photoActiveTransfer?.markDisconnected()
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

    /// 便捷方法：亮度 0-255（单次发送，不等确认）
    @discardableResult
    func setBrightness(_ value: Int) -> Bool {
        sendCommand("{\"cmd\":\"brightness\",\"value\":\(max(0, min(255, value)))}")
    }

    /**
     拖动亮度滑块时**持续**下发（做到"所见即所得"）。

     ## 为什么需要它

     早先只在滑块**松手**时发一次 —— 拖动过程中屏幕完全不动，
     松手瞬间才"啪"地跳到目标值。用户反馈很准确："像发个时间包重置的感觉"，
     因为确实是**一次跳变**而不是连续跟随。

     ## 限流策略

     不能每拖一格都发（1 秒可能几十次，会堵住 BLE 链路，连遥测都发不出去）。
     这里用 **50ms 最小间隔**（20Hz）—— 人手拖动大约 60-120Hz 采样，
     20Hz 已经足够顺滑，人眼看不出一格一格的。
     用 `.withoutResponse` 是因为亮度**不要确认**，延迟越低越好；
     丢一两个中间值无所谓（后面的会覆盖），最终值由松手时补发保证。

     - Parameter value: 目标亮度 0-255
     */
    func setBrightnessLive(_ value: Int) {
        let v = max(0, min(255, value))
        /* 限流：50ms 内只发一次 */
        let now = Date().timeIntervalSince1970
        if now - lastLiveBrightnessAt < 0.05 { return }
        lastLiveBrightnessAt = now
        guard let per = peripheral, let ch = commandChar, isConnected else { return }
        let json = "{\"cmd\":\"brightness\",\"value\":\(v)}"
        guard let data = json.data(using: .utf8) else { return }
        per.writeValue(data, for: ch, type: .withoutResponse)
    }

    /// 上次"实时亮度"下发的时间戳（限流用）
    private var lastLiveBrightnessAt: TimeInterval = 0

    /**
     拖动「白天/夜间亮度」滑块时**实时**下发曲线（跳过 0.25s 防抖）。

     ## 为什么不能沿用防抖

     防抖的本意是"拖动停止后再发最终值"，但那样拖动过程中屏幕**完全不动**，
     松手才跳变 —— 用户形容"像发个时间包重置"。见 setBrightnessLive 的说明。

     ## 限流

     50ms（20Hz），与手动亮度一致。用 `.withResponse` 而不是
     `.withoutResponse` —— 曲线是**配置**，乱序会让设备停在错误的值上
     （实测过 9 个包乱序到达）。这里牺牲一点延迟换正确性。

     ⚠️ 只在**白天/夜间对应的时段**才影响屏幕亮度：白天拖「夜间亮度」
        屏幕本来就不该变，这是正确行为，不是 bug。
     */
    func setBrightnessCurveLive(_ curve: BrightnessCurve) {
        brightnessCurve = curve
        Self.savedCurve = curve
        let now = Date().timeIntervalSince1970
        if now - lastLiveCurveAt < 0.05 { return }
        lastLiveCurveAt = now
        guard let per = peripheral, let ch = telemetryChar, isConnected else { return }
        guard let data = try? curve.encode() else { return }
        per.writeValue(data, for: ch, type: .withResponse)
    }

    /// 上次"实时曲线"下发的时间戳（限流用）
    private var lastLiveCurveAt: TimeInterval = 0

    // MARK: - 亮度曲线（按时段自动调亮度）

    /**
     当前亮度曲线（供 UI 绑定）。

     初值从 UserDefaults 读（用户偏好），改了就立刻下发设备。
     设备**不持久化**该曲线，所以每次连接都要重发（见 didDiscoverCharacteristicsFor）。
     */
    @Published var brightnessCurve: BrightnessCurve = BLEClient.savedCurve

    /**
     设置并下发亮度曲线。

     - Parameter curve: 新曲线；会保存到 UserDefaults 并立即发给设备。
     */
    func setBrightnessCurve(_ curve: BrightnessCurve) {
        brightnessCurve = curve
        Self.savedCurve = curve
        /*
         * 防抖：拖动滑块时 setter 会被高频调用（每次 1 个刻度）。
         * 若每次都发 BLE，几十个包挤在一起 —— 即使 `.withResponse` 保证了
         * 顺序，也会把链路堵住、让拖动明显卡顿。
         *
         * 这里只在"停止操作 0.25 秒后"发**最终值**。曲线本身是
         * "最终一致"语义（设备只关心最后是什么），中间值没有意义。
         *
         * ⚠️ 关闭/打开这种**离散操作**不该等 0.25 秒 —— 单独用
         *    `sendBrightnessCurveNow()` 立即发（见下面的 enabled 处理）。
         */
        curveSendDebounce?.invalidate()
        curveSendDebounce = Timer.scheduledTimer(withTimeInterval: 0.25, repeats: false) {
            [weak self] _ in
            guard let self else { return }
            self.sendBrightnessCurve(self.brightnessCurve)
        }
    }

    /**
     立即下发当前曲线（不走防抖）。

     用于**离散**操作（如"手动/自动"模式切换）—— 这类操作分次数少、
     用户期望立刻见效，等 0.25 秒反而像"没反应"。
     */
    func sendBrightnessCurveNow() {
        curveSendDebounce?.invalidate()
        curveSendDebounce = nil
        sendBrightnessCurve(brightnessCurve)
    }

    /// 曲线下发防抖定时器（见 setBrightnessCurve）
    private var curveSendDebounce: Timer?

    /**
     演示速度（App 的「模拟速度测试」滑块）。

     - Parameter kmh: 目标车速；**负数 = 退出演示**。

     ⚠️ 这只是**演示**用途：固件会把它显示为状态行 "DEMO"（琥珀色），
        并且**只要收到真实遥测 / BLE 断开就自动退出** —— 不让演示速度
        看起来像真实车速。App 在「开始驾驶」时也主动发一次退出，
        让切换立刻生效。
     */
    @discardableResult
    func sendDemoSpeed(_ kmh: Double, announce: Bool = false) -> Bool {
        /*
         * 默认 quiet：滑块 10Hz 连发，成功时不刷 lastNotice，
         * 否则每秒 10 次整页重绘（本身就是卡顿源）。
         * announce=true 只用在**松手**那一次 —— 让"到底发出去没有"看得见，
         * 否则用户没法区分"没发"和"发了但仪表没动"。
         */
        sendCommand("{\"cmd\":\"demo_speed\",\"value\":\(Int(kmh.rounded()))}",
                    quiet: !announce)
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

    /**
     下发亮度曲线（**每次连接一次**，由 didDiscoverCharacteristicsFor 补发）。

     ## 为什么由 App 决定曲线

     曲线是**用户偏好**，而 App 才是用户改设置的地方；设备只负责按时段应用。
     设备也**不持久化**它 —— 每次连接 App 都重发，避免两边各存一份后不一致
     （那会导致"App 显示 07:00 但设备按 06:00 跑"这类极难排查的问题）。

     - Parameter curve: 要下发的曲线；传 nil 则用本地保存的
     */
    func sendBrightnessCurve(_ curve: BrightnessCurve? = nil) {
        guard isConnected, let ch = telemetryChar else {
            /* 未连接：不排队 —— 连接后 didDiscoverCharacteristicsFor 会补发 */
            return
        }
        let c = curve ?? Self.savedCurve
        guard let data = try? c.encode() else { return }
        /*
         * 同样用 `.withoutResponse` 降低初始化延迟。
         *
         * 安全性：设备端对"曲线内容是否变化"做了判断（见 bright_poll），
         * 且 App 每次连接都会重发，偶发丢失会被下一次连接补上。
         * 另外这里**不参与**"地图窗口"那套按序发送机制 —— 那个机制是为了
         * 保证 mw/mr/mb/mp/me 不被 BLE 栈合并，与单包曲线无关。
         */
        guard let per = peripheral else { return }
        /*
         * ⚠️ **必须用 `.withResponse`**，不能用 `.withoutResponse`。
         *
         * 实测（2026-10-19）：短时间内连发 9 个曲线包，
         * 设备收到的顺序是 130,160,170,140,120,110,180,100,150 —— **完全乱序**，
         * 最终亮度停在 150 而不是最后发的 180。
         *
         * 原因：`.withoutResponse` 是"即发即忘"，BLE 栈按自己的调度发送，
         * 不保证到达顺序。而曲线是**配置**，必须先来后到 ——
         * App 里拖动滑块会连发多个值，乱序就会让设备停在某个中间值，
         * 用户看到的就是"拖到最后没反应/亮度乱跳"。
         *
         * 用 `.withResponse` 后每包都有确认，交给 didWriteValueFor 统一按序处理；
         * 曲线不是高频包（拖滑块也只有几到几十次），这点延时可接受。
         */
        per.writeValue(data, for: ch, type: .withResponse)
        NSLog("[BLE] 已下发亮度曲线：%@", c.summary)
    }



    /**
     当前生效的亮度曲线（**本地保存**，连接时下发）。

     用 UserDefaults 存：这是 App 自己的用户偏好，不是设备状态
     （设备状态一律走 BLE 上报，见 DeviceFaces）。因此不需要 Keychain ——
     里面没有凭据。
     */
    static var savedCurve: BrightnessCurve {
        get {
            let d = UserDefaults.standard
            var c = BrightnessCurve()
            if d.object(forKey: "bright.enabled") != nil {
                c.enabled = d.bool(forKey: "bright.enabled")
            }
            if d.object(forKey: "bright.ds") != nil { c.dayStartMin = d.integer(forKey: "bright.ds") }
            if d.object(forKey: "bright.de") != nil { c.dayEndMin = d.integer(forKey: "bright.de") }
            if d.object(forKey: "bright.day") != nil { c.dayLevel = d.integer(forKey: "bright.day") }
            if d.object(forKey: "bright.night") != nil { c.nightLevel = d.integer(forKey: "bright.night") }
            if d.object(forKey: "bright.ramp") != nil { c.rampMin = d.integer(forKey: "bright.ramp") }
            return c
        }
        set {
            let d = UserDefaults.standard
            d.set(newValue.enabled, forKey: "bright.enabled")
            d.set(newValue.dayStartMin, forKey: "bright.ds")
            d.set(newValue.dayEndMin, forKey: "bright.de")
            d.set(newValue.dayLevel, forKey: "bright.day")
            d.set(newValue.nightLevel, forKey: "bright.night")
            d.set(newValue.rampMin, forKey: "bright.ramp")
        }
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
        peripheral.delegate = self
        /*
         * 一次把两个服务都发现出来（少一次往返）：
         *   - 主服务：遥测/状态/命令/表盘列表（既有行为，不动）；
         *   - 照片服务：**有没有它就是"是不是旧固件"的判据**（协议 §8.1）。
         * 旧固件没这个服务时 `didDiscoverServices` 只会给出主服务，
         * 我们据此把 `photoSupported` 置 false，界面优雅降级（不报错）。
         */
        peripheral.discoverServices([serviceUUID, photoServiceUUID])
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
        guard let services = peripheral.services else {
            // 一个服务都没发现：照片服务也确定不存在（例如设备刚重启、GATT 还没起来）
            photoServiceProbed = true
            photoSupported = false
            return
        }
        /*
         * 照片服务存在性 = 是否支持"无线传照片"。
         * 这里按 **UUID 字符串**比较（见 didWriteValueFor 里"不能比对象"的教训）。
         */
        let hasPhotoService = services.contains {
            $0.uuid.uuidString.uppercased() == PhotoGATT.service
        }
        photoServiceProbed = true
        photoSupported = hasPhotoService
        if !hasPhotoService {
            NSLog("[BLE] 未发现照片服务 → 旧固件，照片页退回 SD 卡导入说明")
        }
        for svc in services {
            if svc.uuid.uuidString.uppercased() == PhotoGATT.service {
                peripheral.discoverCharacteristics(PhotoGATT.allCharacteristics.map { CBUUID(string: $0) },
                                                   for: svc)
            } else if svc.uuid == serviceUUID {
                peripheral.discoverCharacteristics([telemetryUUID, statusUUID, commandUUID, facesUUID],
                                                   for: svc)
            }
        }
    }

    func peripheral(_ peripheral: CBPeripheral, didDiscoverCharacteristicsFor service: CBService, error: Error?) {
        guard let chars = service.characteristics else { return }
        NSLog("[BLE] 发现 %d 个特征", chars.count)
        // 照片服务：单独一套特征，处理完就返回（不碰下面表盘/遥测那套逻辑）
        if service.uuid.uuidString.uppercased() == PhotoGATT.service {
            discoverPhotoCharacteristics(peripheral, chars)
            return
        }
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
        /*
         * 同时下发亮度曲线 —— 与时间包同一时机（每次连接一次）。
         *
         * 放在这里而不是 didConnect：此时 telemetryChar 才就绪。
         * 设备端不持久化该曲线，所以**每次连接都要发**，
         * 否则设备会退回内置默认值（表现为"重连后亮度时段不对"）。
         */
        sendBrightnessCurve()
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
        /*
         * 照片命令（WRITE 带响应）：失败只记日志，不改 `lastNotice`。
         * 上传过程中命令写得比较频繁，写 @Published 会拖垮 SwiftUI（见 sendCommand 的 quiet 说明）；
         * 而且命令失败会由 PhotoTransfer 的看门狗兜住（重发 begin / end）。
         */
        if characteristic.uuid.uuidString.uppercased() == PhotoGATT.ctl, let error {
            NSLog("[BLE] 照片命令写入失败：%@", error.localizedDescription)
        }
        if characteristic.uuid.uuidString.uppercased() == PhotoGATT.data {
            // 分片写确认：一片一个，推进窗口
            photoActiveTransfer?.resumeWritable()
        }
        // 写入确认（阶段3前台可打点/日志；断连重试由 didDisconnect 处理）
    }

    /**
     写缓冲腾空（`canSendWriteWithoutResponse` 由 false 变 true）。

     ⚠️ 上传大文件时**必须**靠这个回调恢复发送：`withoutResponse` 写满后
        iOS 会暂停，继续硬发会被静默丢弃（地图队列踩过"队列溢出丢前面的"）。
     */
    func peripheralIsReady(toSendWriteWithoutResponse peripheral: CBPeripheral) {
        photoActiveTransfer?.resumeWritable()
    }

    func peripheral(_ peripheral: CBPeripheral, didUpdateValueFor characteristic: CBCharacteristic, error: Error?) {
        // ── 照片服务（3 个特征，全部按 UUID 字符串分流）────────────────
        let photoUUID = characteristic.uuid.uuidString.uppercased()
        if photoUUID == PhotoGATT.info {
            if let data = characteristic.value, let info = DevicePhotoParser.parseInfo(data) {
                applyPhotoInfo(info)
            }
            return
        }
        if photoUUID == PhotoGATT.state {
            /*
             * 与表盘列表同一套模式：**Read 才是取数据的正途**。
             *
             * - `photoStateReadPending` 为 true → 这是我们刚发起的 Read 回包，解析它；
             * - 否则 → 是设备的 Notify"变了"提示，**不信任载荷**，重新 Read。
             *   （协议 §1 明确要求这样做：通知可能被 MTU 截断，且设备限流 ≥200 ms）
             */
            if photoStateReadPending {
                photoStateReadPending = false
                if let data = characteristic.value,
                   let state = DevicePhotoParser.parseState(data) {
                    applyPhotoState(state)
                }
            } else {
                schedulePhotoStateRead()
            }
            return
        }
        if photoUUID == PhotoGATT.evt {
            if let data = characteristic.value { handlePhotoEventData(data) }
            return
        }

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

// MARK: - 照片服务：发现 / 读取 / 清单 / 上传队列
//
// 这里的所有"读取"都遵守协议 §1 的顺序与方式：
//   read PhotoInfo → read PhotoState → {"op":"list"}
// 清单用 pq/pf/pz 分页组装；Notify 只当"变了"的提示（不信任载荷）。
// 上传队列**串行**（一次一张），每张由 `PhotoTransfer` 纯逻辑引擎驱动。

extension BLEClient {

    // MARK: 发现

    /// 发现照片服务的 5 个特征并按 UUID **字符串**匹配（不能比对象）
    private func discoverPhotoCharacteristics(_ peripheral: CBPeripheral, _ chars: [CBCharacteristic]) {
        for ch in chars {
            switch ch.uuid.uuidString.uppercased() {
            case PhotoGATT.ctl:
                photoCtlChar = ch
            case PhotoGATT.data:
                photoDataChar = ch
            case PhotoGATT.evt:
                photoEvtChar = ch
                peripheral.setNotifyValue(true, for: ch) // 窗口/结果/清单分页全靠它
            case PhotoGATT.info:
                photoInfoChar = ch
            case PhotoGATT.state:
                photoStateChar = ch
                // Notify 只用来触发重读（见 didUpdateValueFor 的说明）
                peripheral.setNotifyValue(true, for: ch)
            default:
                continue
            }
        }
        photoFirstWriteBootstrap = true
        // 上传至少要 ctl + data + evt 三个特征齐全，否则视为不可用（比"半可用"好排查）
        photoSupported = photoCtlChar != nil && photoDataChar != nil && photoEvtChar != nil
        NSLog("[BLE] 照片服务：ctl=%d data=%d evt=%d info=%d state=%d supported=%d",
              photoCtlChar != nil ? 1 : 0,
              photoDataChar != nil ? 1 : 0,
              photoEvtChar != nil ? 1 : 0,
              photoInfoChar != nil ? 1 : 0,
              photoStateChar != nil ? 1 : 0,
              photoSupported ? 1 : 0)

        // 协议 §1：连接后一次 read Info / read State / list
        refreshPhotoLibrary()
        // 断连期间挂起的上传：重连后从设备已落盘偏移续传
        resumePhotoUploadsAfterReconnect()
    }

    // MARK: 读取（Read 主）

    /// 刷新照片库：读能力 + 读状态 + 请求清单。UI 下拉刷新也调它。
    func refreshPhotoLibrary() {
        guard isConnected, photoSupported else { return }
        readPhotoInfo()
        readPhotoState()
        requestPhotoList()
    }

    private func readPhotoInfo() {
        guard let per = peripheral, let ch = photoInfoChar else { return }
        per.readValue(for: ch)
    }

    private func readPhotoState() {
        guard let per = peripheral, let ch = photoStateChar else { return }
        photoStateReadPending = true
        per.readValue(for: ch)
    }

    /// 因收到 Notify 而重读 `PhotoState`（限流 250 ms：设备侧 notify ≥200 ms）
    private func schedulePhotoStateRead() {
        guard !photoStateReadScheduled else { return }
        photoStateReadScheduled = true
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.25) { [weak self] in
            guard let self else { return }
            self.photoStateReadScheduled = false
            guard self.isConnected, self.photoStateChar != nil else { return }
            self.readPhotoState()
        }
    }

    /// 清单/状态变化后的防抖刷新（`changed` 事件可能连发：上传提交时会连发 done + changed）
    private func schedulePhotoLibraryRefresh() {
        guard !photoRefreshScheduled else { return }
        photoRefreshScheduled = true
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.3) { [weak self] in
            guard let self else { return }
            self.photoRefreshScheduled = false
            guard self.isConnected, self.photoSupported else { return }
            self.readPhotoState()
            self.requestPhotoList()
        }
    }

    private func requestPhotoList() {
        guard isConnected, let per = peripheral, let ch = photoCtlChar else { return }
        // 已有一次 list 在途：不重复发（两套分页交错会把清单拼错）
        guard !photoListRequested else { return }
        guard let data = PhotoCommand.list.encode() else { return }
        photoListRequested = true
        photoListRequestedAt = Date().timeIntervalSince1970
        per.writeValue(data, for: ch, type: .withResponse)
        /*
         * 兜底：设备若因忙碌/异常没回 pq/pz，不能永远不再请求。
         * 3 s 后自动解锁（并结束 loading 态），下次刷新可以重新取。
         */
        DispatchQueue.main.asyncAfter(deadline: .now() + 3) { [weak self] in
            guard let self else { return }
            guard self.photoListRequested,
                  Date().timeIntervalSince1970 - self.photoListRequestedAt >= 3 else { return }
            self.photoListRequested = false
            if self.photoLibrary?.isLoading ?? false {
                var lib = self.photoLibrary ?? DevicePhotoLibrary()
                lib.isLoading = false
                lib.lastError = "照片列表请求超时，请下拉刷新"
                self.photoLibrary = lib
            }
        }
    }

    // MARK: 应用上报

    private func applyPhotoInfo(_ info: DevicePhotoInfo) {
        var lib = photoLibrary ?? DevicePhotoLibrary()
        lib.info = info
        photoLibrary = lib
    }

    private func applyPhotoState(_ state: DevicePhotoState) {
        var lib = photoLibrary ?? DevicePhotoLibrary()
        lib.state = state
        photoLibrary = lib
        /*
         * 设备说"有 N 张"但我们手上的清单数量对不上（外部插卡/设备端删了/上传提交完成）
         * → 重新取清单。清单在途时不动，避免重复请求。
         */
        if state.reported(.count), state.count != lib.items.count, !photoListRequested {
            requestPhotoList()
        }
    }

    // MARK: 事件

    private func handlePhotoEventData(_ data: Data) {
        guard let incoming = PhotoProtocol.parseIncoming(data) else {
            // 载荷被截断/未知：按"状态可能变了"处理（与 Faces 的"解析失败就重读"同思路）
            schedulePhotoStateRead()
            return
        }
        switch incoming {
        case let .event(event):
            handlePhotoEvent(event)

        case let .listBegin(count):
            photoListBuffer = []
            photoListRequested = true
            var lib = photoLibrary ?? DevicePhotoLibrary()
            lib.isLoading = count > 0
            lib.lastError = nil
            photoLibrary = lib

        case let .listItem(item):
            photoListBuffer.append(item)

        case .listEnd:
            var lib = photoLibrary ?? DevicePhotoLibrary()
            // 设备按 `i` 升序发；这里再排一次，保证 items 顺序与 `cur` 下标语义一致
            lib.items = photoListBuffer.sorted { $0.index < $1.index }
            lib.isLoading = false
            photoLibrary = lib
            photoListBuffer = []
            photoListRequested = false
        }
    }

    private func handlePhotoEvent(_ event: PhotoEvent) {
        switch event {
        case .go, .credit, .done, .error:
            // 传输事件交给当前引擎；没有引擎 = 上一次上传的迟到回包，忽略
            photoActiveTransfer?.handle(event)
            if case .done = event { schedulePhotoLibraryRefresh() }
            // 格式化期间收到 err：UI 必须结束等待并给出原因
            if photoFormatState == .formatting, case let .error(code, _, _) = event {
                photoFormatTimeout?.cancel()
                photoFormatTimeout = nil
                photoFormatState = .failed(Self.formatFailureText(code))
            }

        case .changed:
            // 清单/当前项/设置变了 → 重读状态 + 重新取清单（协议 §5）
            schedulePhotoLibraryRefresh()

        case .info:
            // 能力/自检结果变了 → 重读 PhotoInfo（bench 的结果走这条）
            readPhotoInfo()

        case let .formatResult(ok):
            photoFormatTimeout?.cancel()
            photoFormatTimeout = nil
            if ok {
                photoFormatState = .success
                // 空卡后清单/空间/能力都变了：主动刷新
                schedulePhotoLibraryRefresh()
                readPhotoInfo()
            } else {
                photoFormatState = .failed("仪表拒绝了格式化，请检查存储卡后重试。")
            }
        }
    }

    // MARK: 看门狗

    /// 0.1 s 定时器：超时自救 + **主动泵**。
    ///
    /// 0.1 s 而不是 0.5 s：窗口只有 4 KB，若 `peripheralIsReady` 没回调，
    /// 0.5 s 才补一轮会把吞吐压到 ~8 KB/s；100 ms 补一轮足够跑满 BLE。
    private func startPhotoWatchdog() {
        guard photoWatchdog == nil else { return }
        photoWatchdog = Timer.scheduledTimer(withTimeInterval: 0.1, repeats: true) { [weak self] _ in
            guard let self else { return }
            self.photoActiveTransfer?.refreshWritable(
                self.peripheral?.canSendWriteWithoutResponse ?? false)
            self.photoActiveTransfer?.tick()
        }
    }

    private func stopPhotoWatchdog() {
        photoWatchdog?.invalidate()
        photoWatchdog = nil
    }

    // MARK: 上传队列

    /**
     入队一批上传（**串行执行**：一次只传一张）。

     - 未连接 / 旧固件：不会静默失败，每条都会在 `photoUploads` 里出现 `.failed` + 原因；
     - 已连接：排在当前上传后面，逐张执行。
     */
    func enqueuePhotoUploads(_ requests: [PhotoUploadRequest]) {
        guard !requests.isEmpty else { return }
        let ready = isConnected && photoSupported && photoCtlChar != nil && photoDataChar != nil
        let rejectReason: String? = {
            if ready { return nil }
            if !isConnected { return "请先连接仪表" }
            if photoServiceProbed && !photoSupported { return "当前固件不支持无线传照片（可更新固件，或走 SD 卡导入）" }
            return "照片服务尚未就绪，请稍后重试"
        }()

        for request in requests {
            photoRequestStore[request.id] = request
            // 同一 id 的旧终态记录先清掉（重试/重复添加时不出现两条）
            photoUploads.removeAll { $0.id == request.id && $0.isFinished }
            if let reason = rejectReason {
                var p = PhotoUploadProgress(request: request)
                p.state = .failed
                p.errorText = reason
                photoUploads.append(p)
                continue
            }
            // 已在排队/上传中的同一 id：不重复入队
            if photoUploads.contains(where: { $0.id == request.id && !$0.isFinished }) { continue }
            photoUploads.append(PhotoUploadProgress(request: request))
            photoUploadQueue.append(request)
        }
        guard rejectReason == nil else { return }
        pumpPhotoUploads()
    }

    /// 取消一张：正在传的发 `abort`；还在排队的直接移出队列
    func cancelPhotoUpload(id: String) {
        if photoActiveRequest?.id == id, let transfer = photoActiveTransfer {
            // 引擎会发 abort 并把状态置 cancelled（photoReport → finishActivePhotoUpload）
            transfer.cancel()
            return
        }
        if let idx = photoUploadQueue.firstIndex(where: { $0.id == id }) {
            photoUploadQueue.remove(at: idx)
            updatePhotoUpload(id: id, state: .cancelled, errorText: nil)
        }
    }

    /// 重试一张失败/取消的上传（从设备已落盘偏移续传，不重新编码）
    func retryPhotoUpload(id: String) {
        guard let request = photoRequestStore[id] else { return }
        guard let idx = photoUploads.firstIndex(where: { $0.id == id }) else { return }
        guard photoUploads[idx].isFinished else { return } // 正在传的不重试

        // 未连接/旧固件时不能"排队等一个永远不来的连接"——直接给出原因
        let ready = isConnected && photoSupported && photoCtlChar != nil && photoDataChar != nil
        guard ready else {
            photoUploads[idx] = PhotoUploadProgress(request: request)
            updatePhotoUpload(id: id, state: .failed,
                              errorText: isConnected
                                ? "当前固件不支持无线传照片（可更新固件，或走 SD 卡导入）"
                                : "请先连接仪表")
            return
        }
        guard photoActiveTransfer == nil else {
            // 通道被占用：排队，等当前这张结束
            photoUploads[idx] = PhotoUploadProgress(request: request)
            photoUploadQueue.insert(request, at: 0)
            return
        }
        photoUploads[idx] = PhotoUploadProgress(request: request)
        photoUploadQueue.insert(request, at: 0)
        pumpPhotoUploads()
    }

    /// 清掉已完成/失败/取消的记录（不动正在进行的）
    func clearFinishedPhotoUploads() {
        let finishedIDs = photoUploads.filter { $0.isFinished }.map(\.id)
        photoUploads.removeAll { $0.isFinished }
        for id in finishedIDs where photoActiveRequest?.id != id {
            photoRequestStore.removeValue(forKey: id)
        }
    }

    /// 从队列取下一张开始传
    private func pumpPhotoUploads() {
        guard photoActiveTransfer == nil else { return }
        guard isConnected, photoSupported, photoCtlChar != nil, photoDataChar != nil else { return }
        guard !photoUploadQueue.isEmpty else { return }

        let request = photoUploadQueue.removeFirst()
        guard let source = PhotoFileChunkSource(fileURL: request.fileURL) else {
            updatePhotoUpload(id: request.id, state: .failed,
                              errorText: "找不到已编码的上传文件，请重新添加")
            scheduleNextPhotoUpload()
            return
        }
        guard source.size == request.bytes else {
            source.close()
            updatePhotoUpload(id: request.id, state: .failed,
                              errorText: "上传文件已变化（大小与编码时不一致），请重新添加")
            scheduleNextPhotoUpload()
            return
        }

        photoChunkSource = source
        // 后台读完一块 → 通知引擎继续灌数据（ISO：读文件不在 BLE 回调线程上阻塞）
        source.onReady = { [weak self] in
            self?.photoActiveTransfer?.chunkBecameAvailable()
        }
        let transfer = PhotoTransfer(request: request, io: self)
        photoActiveTransfer = transfer
        photoActiveRequest = request
        startPhotoWatchdog()
        NSLog("[BLE] 开始上传照片 %@（%d B，kind=%d）",
              request.id, request.bytes, request.kind.rawValue)
        transfer.start()
    }

    /// 当前这张结束后，稍等一点再传下一张（给设备做 CRC + rename 的时间）
    private func scheduleNextPhotoUpload() {
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.35) { [weak self] in
            self?.pumpPhotoUploads()
        }
    }

    private func finishActivePhotoUpload() {
        photoActiveTransfer = nil
        photoActiveRequest = nil
        photoChunkSource?.close()
        photoChunkSource = nil
        stopPhotoWatchdog()
    }

    /// 重连后恢复：正在传的那张从设备已落盘偏移续传；否则接着传队列
    private func resumePhotoUploadsAfterReconnect() {
        guard photoSupported else { return }
        if let transfer = photoActiveTransfer {
            if transfer.state == .waitingReconnect {
                startPhotoWatchdog()
                transfer.restartAfterReconnect()
            }
            return
        }
        pumpPhotoUploads()
    }

    // MARK: 进度发布

    private func updatePhotoUpload(id: String, state: PhotoUploadState, errorText: String?) {
        guard let idx = photoUploads.firstIndex(where: { $0.id == id }) else { return }
        var p = photoUploads[idx]
        p.state = state
        p.errorText = errorText
        photoUploads[idx] = p
    }

    // MARK: 删除 / 播放 / 设置 / 自检

    /// 删除设备上的一张照片（结果以 `changed` → 重新取清单为准）
    @discardableResult
    func deletePhoto(id: String) -> Bool {
        guard isConnected, photoSupported,
              let per = peripheral, let ch = photoCtlChar else { return false }
        guard let data = PhotoCommand.del(id: id).encode() else {
            NSLog("[BLE] 删除照片：文件名非法 %@", id)
            return false
        }
        per.writeValue(data, for: ch, type: .withResponse)
        return true
    }

    /// 切到第 i 张并钉住（设备回 `changed`，`cur` 变化）
    @discardableResult
    func playPhoto(index: Int) -> Bool {
        guard isConnected, photoSupported,
              let per = peripheral, let ch = photoCtlChar else { return false }
        guard let data = PhotoCommand.play(index: index).encode() else { return false }
        per.writeValue(data, for: ch, type: .withResponse)
        return true
    }

    /**
     下发播放设置（**任意子集**，nil = 不下发该字段）。

     ⚠️ 不做本地乐观更新：界面以 `PhotoState` 回读为准（协议 §7.3：
        播放设置从此以设备为准，修掉 App 本地乐观值漂移）。
     */
    @discardableResult
    func setPhotoSettings(slideshow: Bool? = nil,
                          interval: Int? = nil,
                          shift: Bool? = nil,
                          screenOff: Int? = nil) -> Bool {
        guard isConnected, photoSupported,
              let per = peripheral, let ch = photoCtlChar else { return false }
        guard let data = PhotoCommand.set(slideshow: slideshow, interval: interval,
                                          shift: shift, screenOff: screenOff).encode() else {
            return false
        }
        per.writeValue(data, for: ch, type: .withResponse)
        return true
    }

    /// 设备自检（解码耗时 / 实测动图 fps），结果通过 `info` 事件 → 重读 PhotoInfo
    func runPhotoBench() {
        guard isConnected, photoSupported,
              let per = peripheral, let ch = photoCtlChar else { return }
        guard let data = PhotoCommand.bench.encode() else { return }
        per.writeValue(data, for: ch, type: .withResponse)
    }

    // MARK: 格式化 SD 卡

    /// 格式化仪表 SD 卡（会删除全部照片）。UI 必须先二次确认。
    ///
    /// 成功/失败以设备 `fmt` / `err` 事件为准，UI 观察 `photoFormatState`；
    /// 30 s 没有回包按超时失败处理（避免用户对着永远转圈的按钮）。
    @discardableResult
    func formatPhotoCard() -> Bool {
        guard isConnected, photoSupported,
              let per = peripheral, let ch = photoCtlChar else {
            photoFormatState = .failed("请先连接仪表，再格式化存储卡。")
            return false
        }
        guard photoActiveTransfer == nil,
              !photoUploads.contains(where: { !$0.isFinished }) else {
            photoFormatState = .failed("还有照片在上传，等队列完成后再格式化。")
            return false
        }
        guard let data = PhotoCommand.format.encode() else { return false }

        photoFormatState = .formatting
        per.writeValue(data, for: ch, type: .withResponse)

        photoFormatTimeout?.cancel()
        let work = DispatchWorkItem { [weak self] in
            guard let self, self.photoFormatState == .formatting else { return }
            self.photoFormatState = .failed("仪表没有响应格式化请求，请重试。")
        }
        photoFormatTimeout = work
        DispatchQueue.main.asyncAfter(deadline: .now() + 30, execute: work)
        return true
    }

    /// UI 已消费完结果（toast/alert 展示后）复位，避免重复提示
    func clearPhotoFormatState() {
        photoFormatTimeout?.cancel()
        photoFormatTimeout = nil
        photoFormatState = .idle
    }

    private static func formatFailureText(_ code: PhotoErrorCode) -> String {
        switch code {
        case .busy: return "仪表正在接收照片，等完成后再格式化。"
        case .not_ready: return "仪表没有检测到存储卡，请插好卡再试。"
        case .io: return "存储卡格式化失败，可能是卡损坏或接触不良。"
        case .unsupported: return "当前固件不支持格式化存储卡，请更新固件。"
        default: return "格式化失败，请重试。"
        }
    }

    // MARK: 速率持久化

    private static let photoMeasuredRateKey = "photo.measuredRate"

    /// 读取上次成功上传的实测速率（nil = 还没传成功过）
    static func loadPhotoMeasuredRate() -> Double? {
        let defaults = UserDefaults.standard
        guard defaults.object(forKey: photoMeasuredRateKey) != nil else { return nil }
        let value = defaults.double(forKey: photoMeasuredRateKey)
        return value > 0 ? value : nil
    }

    private func storePhotoMeasuredRate(_ rate: Double) {
        guard rate > 0 else { return }
        photoMeasuredUploadRate = rate
        UserDefaults.standard.set(rate, forKey: BLEClient.photoMeasuredRateKey)
    }
}

// MARK: - 照片传输 I/O（PhotoTransfer 的唯一宿主）
//
// `PhotoTransfer` 是纯逻辑；这里把四个 I/O 动作接到 CoreBluetooth 上。
// 写分片用 `writeValue(_, for:, type: .withoutResponse)` —— 无响应写入靠
// `canSendWriteWithoutResponse` + `peripheralIsReady` 做流控（协议 §6）。

extension BLEClient: PhotoTransferIO {

    func photoSend(_ command: PhotoCommand) {
        guard isConnected, let per = peripheral, let ch = photoCtlChar else { return }
        // 每次 begin（含看门狗重发）都重新给一次"首包放行"机会：
        // 否则一次失败/取消后 bootstrap 已被消耗，重试会永远卡在 0%。
        if command.op == "begin" { photoFirstWriteBootstrap = true }
        guard let data = command.encode() else {
            NSLog("[BLE] 照片命令编码失败：%@", command.op)
            return
        }
        // 命令走 WRITE（带响应）：包短、且设备必须按序处理
        per.writeValue(data, for: ch, type: .withResponse)
    }

    func photoWriteChunk(_ frame: Data) -> Bool {
        guard isConnected, let per = peripheral, let ch = photoDataChar else { return false }
        /*
         * ⚠️ **必须检查** `canSendWriteWithoutResponse`。
         * 上传时一个窗口 4096 B ≈ 23 个分片连着发；写缓冲满后继续硬发会被
         * iOS 静默丢弃（不是报错），而设备只会在 6 s 后靠 credit 超时自救 ——
         * 表现为"上传很慢、偶尔卡住"。这里返回 false 让引擎停手，
         * 等 `peripheralIsReady(toSendWriteWithoutResponse:)` 再继续。
         */
        /*
         * 真机定案（2026-10-27）：iOS 17 上 `canSendWriteWithoutResponse` 会永久卡在
         * false，且 `peripheralIsReady` 不再回调 —— WRITE_NR 路径无法保证走完
         * （实测随机卡在 0/11/25 KB）。改为**逐片带响应写**：
         * 每片都有 didWriteValueFor 确认，流控确定；代价是上限约 6 KB/s
         * （178 B / 30 ms），对 ≤512 KB 的静态照片可接受。
         * 写确认回调里调 `resumeWritable()` 继续灌下一片。
         */
        if !ch.properties.contains(.write) {
            // 旧固件只声明 WRITE_NR：退回无响应写，只做首包放行避免死锁
            if !per.canSendWriteWithoutResponse {
                guard photoFirstWriteBootstrap else { return false }
                photoFirstWriteBootstrap = false
            }
            per.writeValue(frame, for: ch, type: .withoutResponse)
            return true
        }
        per.writeValue(frame, for: ch, type: .withResponse)
        return true
    }

    func photoReadChunk(offset: Int, length: Int) -> Data? {
        photoChunkSource?.read(offset: offset, length: length)
    }

    func photoPrefetch(offset: Int, length: Int) {
        // 预先读下一块（4 KB），避免每 174 B 都做一次 seek+read
        photoChunkSource?.prefetch(offset: offset, length: length)
    }

    func photoMaxChunkBytes() -> Int {
        guard let per = peripheral, photoDataChar != nil else {
            return PhotoLimits.defaultChunkHint
        }
        // iOS 协商写长（通常 178）− 4 B 的 offset 头（协议 §6）
        let maxLength = per.maximumWriteValueLength(for: .withoutResponse)
        return max(1, maxLength - 4)
    }

    func photoReport(_ progress: PhotoUploadProgress) {
        // 更新 UI 数组（引擎内部已把回调节流到 ~5 Hz，避免拖垮 SwiftUI）
        if let idx = photoUploads.firstIndex(where: { $0.id == progress.id }) {
            photoUploads[idx] = progress
        } else {
            photoUploads.append(progress)
        }

        switch progress.state {
        case .done:
            // 记录本次连接实测速率（动图门控第③层），并刷新清单
            let measured = photoActiveTransfer?.overallBytesPerSecond
                ?? (progress.bytesPerSecond > 0 ? progress.bytesPerSecond : 0)
            storePhotoMeasuredRate(measured)
            NSLog("[BLE] 照片上传完成 %@（%.1f KB/s）", progress.id, measured / 1024)
            finishActivePhotoUpload()
            schedulePhotoLibraryRefresh()
            scheduleNextPhotoUpload()

        case .failed:
            /*
             * 失败收尾：发一次 abort 让设备删掉 `.part`（避免残留半截文件占空间）。
             * 设备若已自行清理/没有在传，会回 `err`，此时我们已没有活动引擎 → 忽略。
             * 参数校验就失败的（还没发过 begin）不必打扰设备。
             */
            if photoActiveTransfer?.hasStartedTransfer == true {
                photoSend(.abort)
            }
            NSLog("[BLE] 照片上传失败 %@：%@", progress.id, progress.errorText ?? "未知")
            finishActivePhotoUpload()
            scheduleNextPhotoUpload()

        case .cancelled:
            finishActivePhotoUpload()
            scheduleNextPhotoUpload()

        case .waitingReconnect:
            // 断连：保留引擎与已落盘偏移，等重连后 restartAfterReconnect()
            stopPhotoWatchdog()

        case .queued, .uploading, .verifying:
            break
        }
    }
}
