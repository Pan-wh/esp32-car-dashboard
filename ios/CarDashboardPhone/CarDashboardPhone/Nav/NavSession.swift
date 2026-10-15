/**
 * NavSession.swift —— App 侧的导航会话（把各部分接起来的**唯一中枢**）
 *
 * 数据流：
 *
 *     CLLocation（WGS84）
 *        │  Coords.wgs84ToGcj02  ← NavCore 内部一律 GCJ-02，转换必须在入口做
 *        ▼
 *     NavFix ──▶ NavCore.handle(fix:nowMs:) ──▶ NavOutcome
 *                                                  ├─ snapshot ──▶ BLE 下发 + UI 展示
 *                                                  └─ commands ──▶ 偏航重算（取新路线）
 *
 * 设计要点（都有理由，改动前请先读）：
 *
 *  1. **自己起时钟，但不自己读定位**：导航必须能独立于「开始驾驶」工作
 *     （否则用户只点「开始导航」永远收不到定位 —— 这是最初的实现缺陷）。
 *     时钟只负责"按节奏推进"，定位仍取 `LocationService.lastLocation` 这个
 *     已发布的缓存值，因此**不会与驾驶定时器争抢**（两边读的是同一份数据）。
 *
 *  2. **`NavCore` 绝不离开主线程**：它不是 `Sendable`。
 *     网络请求（`GatewayClient.fetchRoute` 是同步实现，会阻塞）放到后台，
 *     拿到纯数据（`RouteBundle`）后回主线程再 `core.setRoute`。
 *     重算同理 —— 后台用 `RerouteDispatcher.fetchNewRoute`（**只取路线**），
 *     回主线程才碰核心。这样"坐标转换 + 折线校验"仍只有一处实现。
 *
 *  3. **先记目标、拿到可用定位再取路线**：用户点"开始导航"时不一定有可用定位
 *     （室内/刚启动）。此刻强行请求必然失败，故先存目标点，等第一个定位到来
 *     再请求，UI 显示"等待定位…"。
 *
 *  4. **下发节流**：按 `sendIntervalMs`（500ms，与手机 2Hz 遥测同量级）下发。
 *     距离是连续变化的，不节流会把 BLE 打满。
 *
 *  5. **断连不补发**：导航指引时效性极强，补发过期数据毫无意义
 *     （`BLEClient.sendNav` 同样的取舍）。
 *
 *  6. **只在快照真的变化时赋值 `@Published`**：`@Published` 在**赋值时**就发通知，
 *     所以"抑制抖动"必须靠**不赋值**来实现，而不是补发一次 `objectWillChange`。
 */

import Foundation
import CoreLocation

@MainActor
final class NavSession: ObservableObject {

    // MARK: - 对外状态（UI 只读）

    /// 最新导航快照（UI 展示 + 下发的来源）
    @Published private(set) var snapshot = NavSnapshot()
    /// 是否处于导航中（用户点了开始且路线已就绪）
    @Published private(set) var isNavigating = false
    /// 一句话状态（UI 顶部显示）
    @Published private(set) var statusText = "未开始导航"
    /// 路线概要，如 "12.3 km · 25 分钟"；无路线时为 nil
    @Published private(set) var routeSummary: String?
    /// 最近一次错误（取路线失败 / 重算失败 / 网关未配置）
    @Published private(set) var lastError: String?
    /// 本次导航的重算次数（用于观察偏航处理是否生效）
    @Published private(set) var rerouteCount = 0

    // MARK: - 内部状态

    private let core = NavCore()
    private weak var ble: BLEClient?
    private weak var location: LocationService?

    /// 导航自己的推进时钟（见文件头说明 1）
    private var clock: Timer?
    private let clockInterval: TimeInterval = 0.5

    /// 网关访问参数（由 UI 设置）
    private var gatewayURL = ""
    private var gatewayToken = ""

    /// 用户设定的终点（**GCJ-02**）。非 nil 表示"等待定位以请求路线"
    private var pendingDestination: GeoPoint?
    /// 是否正在请求路线（避免重复请求）
    private var routeRequestInFlight = false
    /// 是否有重算在飞行中（避免叠加）
    private var rerouteInFlight = false

    /// 下发节流
    private var lastSentMs: Int64 = 0
    private let sendIntervalMs: Int64 = 500

    // MARK: - 地图窗口（阶段 4）

    /// 当前已下发的地图窗口（nil = 没有）。仅用于诊断展示与算偏移。
    private var mapBundle: MapWindowBundle?
    /// **局部坐标原点**（GCJ-02）。自车偏移相对它计算。
    private var mapOriginGcj: GeoPoint?
    /// 上次请求窗口时的 WGS84 位置（用于判断"移动超过 100 米"）
    private var lastMapQueryPos: GeoPoint?
    /**
     * 上次**成功取到并下发**窗口时的位置。
     * 与 `lastMapQueryPos`（请求时即写）不同，它只有成功后才更新 ——
     * 周期刷新要靠它判断"相对上次的窗口是否真的移动过"。
     */
    private var lastMapWindowPos: GeoPoint?
    /// 上次请求窗口的时刻（毫秒）
    private var lastMapQueryMs: Int64 = 0
    /// 是否有窗口请求在飞行中（避免叠加）
    private var mapQueryInFlight = false
    /// 已下发的窗口次数（诊断用，UI 可见）
    @Published private(set) var mapWindowCount = 0

    // MARK: - 多方案选路（网关 /routes）

    /// 已取得的路线候选（空 = 没有候选，UI 不显示选择区）
    @Published private(set) var routeCandidates: [RouteCandidate] = []
    /// 是否正在取候选（禁用按钮、显示进度）
    @Published private(set) var candidatesLoading = false
    /// 是否正在导航某条候选（用于高亮）
    @Published private(set) var activeCandidateId: Int?

    /// 当前所在道路名（逆地理编码，可能为空）
    @Published private(set) var currentRoadName: String?
    /// 当前路况（如 "畅通"）
    @Published private(set) var trafficText: String?

    /// 本次导航所选路线的**总里程/总耗时**（用于按比例估算剩余 ETA）
    @Published private(set) var activeRouteDistanceM: Double?
    @Published private(set) var activeRouteDurationS: Double?

    /// 导航中用户点了「换一条路线」（P2-4）：此时界面应显示候选列表
    @Published private(set) var isChoosingRoute = false

    /**
     导航中的**剩余 ETA**（如 "14:32 到"）。

     ⚠️ `NavSnapshot` 里**没有剩余耗时**字段（只有剩余里程），所以这里按
     `剩余里程 / 总里程` 的比例去缩放总耗时 —— 是个估算，但比"不显示"有用得多：
     用户开车时想知道的是"我几点到"，不是"还剩 12.3 公里"。
     */
    var remainingEtaText: String? {
        guard isNavigating,
              let totalD = activeRouteDistanceM, totalD > 0,
              let totalS = activeRouteDurationS else { return nil }
        let frac = max(0.0, min(1.0, snapshot.remainingDistanceM / totalD))
        return NavCandidateFormat.etaText(durationS: totalS * frac)
    }

    /// 进入「换一条路线」（P2-4）
    func beginChoosingRoute() { isChoosingRoute = true }

    /// 放弃「换一条路线」，回到导航
    func cancelChoosingRoute() { isChoosingRoute = false }

    /// 最近一次定位（WGS84）。取候选与逆地理编码都要用它做起点。
    private var lastVehicleWGS84: GeoPoint?
    /// 上次查逆地理/路况的时刻（毫秒）
    private var lastEnvQueryMs: Int64 = 0
    /**
     * 逆地理/路况的查询间隔（毫秒）。
     *
     * ⚠️ 刻意取 60 秒：这两项都是**纯展示**信息，却各自消耗一次高德额度
     *    （逆地理编码 + 交通态势）。跟着 2Hz 的导航时钟去查会瞬间吃光配额，
     *    所以只在"导航中 + 距上次超过 60 秒"时才查。
     */
    private let envQueryIntervalMs: Int64 = 60000

    /// 供 UI 使用：最近一次 WGS84 定位（周边搜索要以"当前位置"为中心）
    var currentPositionWGS84: GeoPoint? { lastVehicleWGS84 }

    /// 本次导航的目的地（WGS84）。UI 用它把目的地记进「最近」。
    @Published private(set) var currentDestinationGeo: GeoPoint?

    /**
     直接从 `LocationService` 取当前位置（不经过导航时钟）。

     ⚠️ **为什么不能只看 `lastVehicleWGS84`**：
     那个字段只在 `onFix` 里赋值，而 `onFix` 由**导航时钟**驱动 ——
     时钟又只在"开始驾驶/开始导航"之后才起。于是"取路线前要等定位"这件事
     就变成了：定位服务明明已经在跑、`LocationService.lastLocation` 也有值，
     但没人把它抄出来 → 等 10 秒也等不到 → 误报"拿不到定位"。

     这是本项目第二次踩"定位只在时钟里流动"的坑（第一次是"还没有定位"），
     所以这里直接读源头。
     */
    private func readLocationServicePosition() -> GeoPoint? {
        guard let loc = location?.lastLocation else { return nil }
        return GeoPoint(lon: loc.coordinate.longitude, lat: loc.coordinate.latitude)
    }

    /**
     确保定位在跑并尽量拿到一个位置（给"周边搜索"等非导航场景用）。

     - Returns: 拿到（或已有）的位置；10 秒内仍拿不到则返回 nil。
     */
    func ensurePositionWGS84() async -> GeoPoint? {
        if let p = lastVehicleWGS84 { return p }
        location?.requestWhenInUse()
        location?.start()
        statusText = "正在获取定位…"
        for _ in 0..<50 {
            try? await Task.sleep(nanoseconds: 200_000_000)
            /* 直接读源头，而不是等导航时钟喂 lastVehicleWGS84 */
            if let p = readLocationServicePosition() {
                lastVehicleWGS84 = p /* 顺手同步，后续逻辑不必再等一次 */
                return p
            }
        }
        return nil
    }
    /**
     * 最近一次采纳的**自车 GCJ-02 位置**。
     *
     * ⚠️ 必须自己记：`NavSnapshot` 里**没有**位置字段（它只描述"指引状态"），
     * 而算自车偏移需要位置。也不能用 `NavFix`，那是每次采样新建的临时值。
     */
    private var lastVehicleGCJ: GeoPoint?

    /// 移动超过这么多米就重新取窗口（对齐 docs/ble.md 的约定）
    private let mapMoveThresholdM = 100.0
    /**
     * 周期刷新间隔（毫秒）。
     *
     * ⚠️ 必须**明显小于**固件的窗口过期时间（`NAV_MAP_STALE_MS` = 60 秒），
     *    否则停车等红灯时地图会突然消失。取 30 秒留一倍余量。
     *
     * ⚠️ 也**不能加"没移动就不刷新"的条件**：曾经加过（省带宽），结果
     *    设备重启后 App 因为一直没动而永不重发，地图再也回不来；
     *    停车超过 60 秒同样会让地图过期消失。
     *    "重发会打断上一个窗口"的问题已由 BLEClient 的 pendingMap 解决，
     *    不再需要靠"不重发"来回避。
     */
    private let mapRefreshIntervalMs: Int64 = 30000
    /**
     * 路线只发自车前方这么远的部分。
     *
     * ⚠️ 必须**明显大于屏幕可见范围**（`NAV_MAP_VIEW_RADIUS_M` = 300m）：
     * 否则白线会在屏幕中间就断掉，让人误以为"路就修到这儿"（实测反馈）。
     * 取 2000m 可保证任何缩放/航向下都超出屏幕边缘，由设备自行裁掉多余部分。
     */
    private let routeAheadM = 2000.0

    /// 上次已发布的快照，用于"只有变化才赋值"
    private var lastPublished = NavSnapshot()

    // MARK: - 生命周期

    /// 绑定 BLE 客户端与定位服务（DriveSessionManager 初始化时调用一次即可）
    func attach(ble: BLEClient, location: LocationService) {
        self.ble = ble
        self.location = location
    }

    /// 导航时钟：只做"按节奏取一次最新定位并推进"，不自己调 CLLocationManager
    private func startClock() {
        guard clock == nil else { return }
        clock = Timer.scheduledTimer(withTimeInterval: clockInterval, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.onClock() }
        }
    }

    private func stopClock() {
        clock?.invalidate()
        clock = nil
    }

    private func onClock() {
        let nowMs = Int64(Date().timeIntervalSince1970 * 1000)
        if let loc = location?.lastLocation {
            onFix(location: loc, nowMs: nowMs)
        } else {
            tick(nowMs: nowMs) /* 没有定位也要推进：核心据此判定"数据过期" */
        }
    }

    /// 配置网关（UI 修改后调用）
    func configure(gateway: String, token: String) {
        gatewayURL = gateway.trimmingCharacters(in: .whitespacesAndNewlines)
        gatewayToken = token.trimmingCharacters(in: .whitespacesAndNewlines)
    }

    var isConfigured: Bool { !gatewayURL.isEmpty && !gatewayToken.isEmpty }

    /// 地名 → 坐标（支持中文）。UI 在用户填的是地名而非经纬度时调用。
    ///
    /// 这是 `async` 的（用 URLSession 的 async API），可直接从 SwiftUI 的 Task 调用，
    /// **不会阻塞主线程** —— 与路线请求不同，那个是同步实现必须放后台。
    func lookup(address: String) async throws -> [GeocodeResult] {
        guard isConfigured else { throw GatewayError.badURL("网关未配置") }
        return try await GatewayClient.geocode(gateway: gatewayURL,
                                               token: gatewayToken,
                                               address: address)
    }

    /// 开始导航到指定终点（**WGS84**，来自地图选点或定位）。
    /// 这里转成 GCJ-02 后再存 —— NavCore 与网关路线都是 GCJ-02。
    func start(destinationWGS84: GeoPoint) {
        guard isConfigured else {
            lastError = "请先填写网关地址与令牌"
            statusText = "未配置网关"
            return
        }
        lastError = nil
        rerouteCount = 0
        rerouteInFlight = false
        pendingDestination = Coords.wgs84ToGcj02(destinationWGS84)
        statusText = "等待定位…"

        /*
         * 导航**独立于「开始驾驶」**：自己拉起定位、自己起时钟。
         * 否则用户只点「开始导航」会永远停在"等待定位"（最初的实现缺陷）。
         * 若驾驶已经在跑，这里只是重复调用 start()，LocationService 内部幂等。
         */
        location?.start()
        startClock()
        /* 若此刻已有定位，下一个时钟节拍就会触发取路线 */
    }

    /// 停止导航：清空核心、恢复 IDLE，并让仪表也回到"未开始导航"
    func stop() {
        pendingDestination = nil
        routeRequestInFlight = false
        rerouteInFlight = false
        stopClock()

        /* 清掉地图窗口，并**立即**通知设备清空（否则要等 60 秒过期）*/
        mapBundle = nil
        mapOriginGcj = nil
        lastVehicleGCJ = nil
        lastVehicleWGS84 = nil
        routeCandidates = []
        activeCandidateId = nil
        currentDestinationGeo = nil
        activeRouteDistanceM = nil
        activeRouteDurationS = nil
        isChoosingRoute = false
        currentRoadName = nil
        trafficText = nil
        lastEnvQueryMs = 0
        lastMapQueryPos = nil
        lastMapWindowPos = nil
        lastMapQueryMs = 0
        mapQueryInFlight = false
        ble?.sendMapMessages(["{\"t\":\"mw\"}", "{\"t\":\"me\"}"])
        /*
         * ⚠️ 这里**不关定位**：定位可能正被「开始驾驶」用着（遥测仍要发速度）。
         * 谁开的谁关 —— 定位只由 DriveSessionManager / 用户显式停止。
         */
        core.clear()
        /* 主动下发一条 idle，避免仪表上残留上一次的指引 */
        ble?.sendNav(snapshot: NavSnapshot())
        update(snapshot: core.snapshot, force: true)
        isNavigating = false
        routeSummary = nil
        statusText = "未开始导航"
        lastError = nil
    }

    // MARK: - 每帧驱动（由 DriveSessionManager 调用）

    /// 新定位到达。`location` 是 WGS84（CoreLocation 原生坐标系）。
    func onFix(location: CLLocation, nowMs: Int64) {
        let wgs = GeoPoint(lon: location.coordinate.longitude,
                           lat: location.coordinate.latitude)

        /* 1) 等待路线：拿到定位就去请求（见文件头说明 3） */
        if let dest = pendingDestination, !routeRequestInFlight, core.route == nil {
            requestRoute(originWGS84: wgs, destinationGCJ: dest)
        }

        /* 2) 喂给核心。坐标必须转成 GCJ-02 */
        /*
         * ⚠️ 定位的时间戳用 **location.timestamp**（采样时刻），不是 nowMs。
         * 核心靠 `nowMs - fix.timestampMs` 判断"这条定位有多旧"；
         * 若这里填当前时间，陈旧定位会被当成新鲜数据，偏航/过期判定全部失效。
         */
        let fixMs = Int64(location.timestamp.timeIntervalSince1970 * 1000)
        let fix = NavFix(position: Coords.wgs84ToGcj02(wgs),
                         horizontalAccuracyM: location.horizontalAccuracy,
                         courseDeg: location.course >= 0 ? location.course : nil,
                         timestampMs: fixMs)
        lastVehicleGCJ = fix.position
        lastVehicleWGS84 = wgs
        handle(core.handle(fix: fix, nowMs: nowMs), nowMs: nowMs)

        /*
         * 地图窗口：与导航指引分开的一条数据流。
         * 只有"在导航中且有路线"时才取 —— 没路线时圆屏画底图也没有意义。
         */
        maybeRefreshMapWindow(vehicleWGS84: wgs, vehicleGCJ: fix.position, nowMs: nowMs)
        maybeRefreshEnvironment(vehicleWGS84: wgs, nowMs: nowMs)
    }

    /// 没有新定位时推进（数据过期判定 / 到达确认等）
    func tick(nowMs: Int64) {
        handle(core.tick(nowMs: nowMs), nowMs: nowMs)
    }

    // MARK: - 内部：应用核心输出

    private func handle(_ outcome: NavOutcome, nowMs: Int64) {
        if !outcome.commands.isEmpty {
            dispatchReroute(commands: outcome.commands)
        }
        update(snapshot: outcome.snapshot, force: false)

        /* 节流下发（带上自车在窗口内的偏移，让圆屏把地图平移到固定箭头下） */
        if nowMs - lastSentMs >= sendIntervalMs {
            lastSentMs = nowMs
            ble?.sendNav(snapshot: outcome.snapshot, vehicleOffset: currentVehicleOffset())
        }
    }

    // MARK: - 地图窗口

    // MARK: - 当前道路 / 路况（纯展示，低频）

    /// 按间隔查询逆地理编码与路况。见 `envQueryIntervalMs` 的说明。
    private func maybeRefreshEnvironment(vehicleWGS84: GeoPoint, nowMs: Int64) {
        guard isNavigating, isConfigured else { return }
        guard nowMs - lastEnvQueryMs >= envQueryIntervalMs else { return }
        lastEnvQueryMs = nowMs

        let gateway = gatewayURL, token = gatewayToken
        Task { [weak self] in
            /* 逆地理：坐标 → 道路名（失败只是不显示，不影响导航）*/
            if let r = try? await GatewayClient.fetchRegeo(
                gateway: gateway, token: token, positionWGS84: vehicleWGS84) {
                await MainActor.run {
                    let t = r.displayText
                    self?.currentRoadName = t.isEmpty ? nil : String(t.prefix(20))
                }
            }
            /* 路况 */
            if let t = try? await GatewayClient.fetchTraffic(
                gateway: gateway, token: token, positionWGS84: vehicleWGS84) {
                await MainActor.run {
                    if let sp = t.averageSpeedKmh {
                        self?.trafficText = String(format: "%@（均速 %.0f km/h）", t.status, sp)
                    } else {
                        self?.trafficText = t.status
                    }
                }
            }
        }
    }

    // MARK: - 多方案选路

    /**
     取多方案路线候选（供用户挑选）。

     与 `start` 的区别：`start` 直接取一条就开跑；这里先取**若干条真正不同的路**
     让用户选。用户选定后再调 `start(with:)`，**不会再请求网关**（候选里已带
     完整几何），因此选路不额外消耗高德额度。

     - Returns: 候选条数（0 表示失败或没有定位）。
     */
    @discardableResult
    func loadRouteCandidates(destinationWGS84: GeoPoint) async -> Int {
        guard isConfigured else {
            lastError = "网关未配置（请填写地址与令牌）"
            return 0
        }

        /*
         * ⚠️ **必须先确保定位在跑，并等第一个定位**。
         *
         * 这里踩过一个循环依赖的坑：新流程是"先选路线方案、再开始导航"，
         * 而取路线需要起点坐标 —— 但定位只有「开始驾驶/开始导航」之后才跑，
         * 于是点「开始导航」永远得到"还没有定位"。
         *
         * 修法就是本函数自己把定位拉起来（这正是"开始导航等同于开始驾驶"
         * 那条规则的延伸），并给用户明确的过程反馈。
         */
        candidatesLoading = true
        defer { candidatesLoading = false }

        location?.requestWhenInUse()   /* 权限未定时会弹系统框；已授权时是空操作 */
        location?.start()
        if lastVehicleWGS84 == nil, let p = readLocationServicePosition() {
            lastVehicleWGS84 = p /* 已经有定位了，不必等 */
        }
        if lastVehicleWGS84 == nil {
            statusText = "正在获取定位…"
            /* 最多等 10 秒（定位冷启动可能要几秒）*/
            for _ in 0..<50 {
                try? await Task.sleep(nanoseconds: 200_000_000)
                if let p = readLocationServicePosition() {
                    lastVehicleWGS84 = p
                    break
                }
            }
        }
        guard let origin = lastVehicleWGS84 else {
            /*
             * 失败时把"可能的原因"直接说清楚 —— 只报"还没有定位"用户无从下手。
             */
            switch location?.authorizationStatus {
            case .denied, .restricted:
                lastError = "定位权限被拒绝：请到「设置 → 隐私与安全性 → 定位服务」允许本 App"
            case .notDetermined:
                lastError = "尚未授权定位：请在弹出的系统提示里选择「使用 App 时允许」"
            default:
                /*
                 * 走到这里说明权限没问题、但 10 秒内没有定位回调。
                 * 室内其实**通常也能定位**（iOS 会融合 Wi-Fi/基站），
                 * 所以不要把责任推给环境 —— 提示要覆盖多种可能。
                 */
                lastError = "定位服务已启动但 10 秒内没收到位置：可先到窗边试试；若仍不行请检查系统定位服务是否开启"
            }
            statusText = "等待定位…"
            return 0
        }
        do {
            let list = try await GatewayClient.fetchRouteCandidates(
                gateway: gatewayURL, token: gatewayToken,
                originWGS84: origin, destinationWGS84: destinationWGS84)
            routeCandidates = list
            currentDestinationGeo = destinationWGS84
            lastError = nil
            statusText = list.isEmpty ? "没有可用路线" : "请选择路线（\(list.count) 条）"
            return list.count
        } catch {
            lastError = "取路线方案失败：\(error)"
            routeCandidates = []
            return 0
        }
    }

    /**
     用**已取到的候选**开始导航（不再请求网关）。

     - Parameter candidate: `loadRouteCandidates` 返回列表中的一条。
     */
    func start(with candidate: RouteCandidate) {
        core.setRoute(candidate.toBundle().toNavRoute())
        pendingDestination = nil          /* 已经有路线了，不必再等定位去取 */
        routeRequestInFlight = false
        activeCandidateId = candidate.strategy
        isNavigating = true
        rerouteCount = 0
        lastError = nil
        routeSummary = Self.summary(bundle: candidate.toBundle())
        activeRouteDistanceM = candidate.distanceM
        activeRouteDurationS = candidate.durationS
        isChoosingRoute = false
        updateStatusText(core.snapshot)
        statusText = "导航中（\(candidate.label)）"
        update(snapshot: core.snapshot, force: true)

        /* 与 start() 一致：自己拉起定位与时钟，并重置地图窗口 */
        location?.start()
        startClock()
        lastMapQueryPos = nil
        lastMapWindowPos = nil
        lastMapQueryMs = 0
    }

    /// 清空候选（换目的地 / 停止导航时调用）
    func clearCandidates() {
        routeCandidates = []
        activeCandidateId = nil
    }

    /// 自车相对窗口原点的偏移（米）。没有窗口时返回 nil（此时不下发 ox/oy）。
    ///
    /// 用**核心已转好的 GCJ-02 位置**：窗口原点也是 GCJ-02，
    /// 两边同系才能直接相减（WGS84 与 GCJ-02 在北京差约 556 米）。
    private func currentVehicleOffset() -> (x: Int, y: Int)? {
        guard let origin = mapOriginGcj, let veh = lastVehicleGCJ else { return nil }
        return LocalProjection.toMeters(veh, origin: origin)
    }

    /// 是否需要重新取窗口：移动超过阈值、或超时、或首次。
    private func maybeRefreshMapWindow(vehicleWGS84: GeoPoint, vehicleGCJ: GeoPoint,
                                      nowMs: Int64) {
        guard isNavigating, core.route != nil else { return }
        guard isConfigured, !mapQueryInFlight else { return }

        let movedFar: Bool
        if let last = lastMapQueryPos {
            movedFar = distanceM(last, vehicleWGS84) >= mapMoveThresholdM
        } else {
            movedFar = true /* 首次必取 */
        }
        /*
         * 周期刷新：无条件按间隔重发（**不能**加位移门槛，见 mapRefreshIntervalMs 的说明）。
         * 停车时重发的数据与上次相同，但这是保证"地图不会过期消失"的必要代价。
         */
        let timedOut = (nowMs - lastMapQueryMs) >= mapRefreshIntervalMs
        guard movedFar || timedOut else { return }

        mapQueryInFlight = true
        lastMapQueryPos = vehicleWGS84
        lastMapQueryMs = nowMs

        /*
         * 路线只发自车**前方**一段：设备只有 500m 的窗口，发整条 10km 路线
         * 既浪费带宽，又会在按 64 点降采样时把眼前那段抽稀得看不清。
         */
        let routeAhead = routeAheadSlice(from: vehicleGCJ)

        let gateway = gatewayURL
        let token = gatewayToken
        Task { [weak self] in
            guard let self else { return }
            do {
                let bundle = try await GatewayClient.fetchMapWindow(
                    gateway: gateway, token: token, positionWGS84: vehicleWGS84)
                await MainActor.run {
                    self.mapBundle = bundle
                    self.mapOriginGcj = bundle.centerGcj
                    self.lastMapWindowPos = vehicleWGS84
                    self.mapQueryInFlight = false
                    let msgs = MapWindowCodec.messages(bundle: bundle, routeGCJ: routeAhead)
                    let sent = self.ble?.sendMapMessages(msgs) ?? 0
                    self.mapWindowCount += 1
                    self.lastError = nil
                    print(String(format: "[地图] 窗口 #%d：道路 %d 建筑 %d 路线 %d 点 → 下发 %d 条消息",
                                 self.mapWindowCount, bundle.roads.count,
                                 bundle.buildings.count, routeAhead.count, sent))
                }
            } catch {
                await MainActor.run {
                    self.mapQueryInFlight = false
                    /*
                     * ⚠️ **不把窗口失败写进 lastError**：地图是辅助层，
                     *    取不到底图不应让用户以为导航坏了（导航指引仍正常）。
                     *    只打日志，界面上的导航状态不受影响。
                     */
                    print(String(format: "[地图] 取窗口失败：%@", String(describing: error)))
                }
            }
        }
    }

    /// 取自车前方 `routeAheadM` 米内的路线点（含自车最近点）
    private func routeAheadSlice(from vehicleGCJ: GeoPoint) -> [GeoPoint] {
        guard let route = core.route, route.polyline.count >= 2 else { return [] }
        let pts = route.polyline

        /* 找离自车最近的折线点 */
        var bestIdx = 0
        var bestD = Double.greatestFiniteMagnitude
        for (i, p) in pts.enumerated() {
            let d = distanceM(p, vehicleGCJ)
            if d < bestD { bestD = d; bestIdx = i }
        }

        /* 从该点向后累积到超过 routeAheadM 为止（并多取一点保证连线不断）*/
        var out: [GeoPoint] = [pts[bestIdx]]
        var acc = 0.0
        for i in (bestIdx + 1)..<pts.count {
            acc += distanceM(pts[i - 1], pts[i])
            out.append(pts[i])
            if acc >= routeAheadM { break }
        }
        return out
    }

    /// 两点间近似距离（米）。导航中只需几十米量级的判断，等距圆柱足够。
    private func distanceM(_ a: GeoPoint, _ b: GeoPoint) -> Double {
        let mPerDegLat = LocalProjection.metersPerDegLat
        let mPerDegLon = mPerDegLat * cos(a.lat * .pi / 180)
        let dx = (b.lon - a.lon) * mPerDegLon
        let dy = (b.lat - a.lat) * mPerDegLat
        return (dx * dx + dy * dy).squareRoot()
    }

    /// 发布快照 + 刷新文案。**只有变化才赋值**（见文件头说明 6）
    private func update(snapshot s: NavSnapshot, force: Bool) {
        if force || s != lastPublished {
            lastPublished = s
            snapshot = s
        }
        updateStatusText(s)
    }

    private func updateStatusText(_ s: NavSnapshot) {
        switch s.state {
        case .noData:   statusText = isNavigating ? "等待定位…" : "无数据（等待定位）"
        case .idle:     statusText = "未开始导航"
        case .stale:    statusText = "定位信号弱，指引已暂停"
        case .rerouting: statusText = "已偏航，正在重新规划…"
        case .arrived:  statusText = "已到达目的地"
        case .active:
            if s.offRoute {
                statusText = "已偏航，确认中…"
            } else if let d = s.distanceToManeuverM {
                statusText = "距下一动作 \(d) 米"
            } else {
                statusText = "导航中"
            }
        }
    }

    // MARK: - 内部：取路线（后台 HTTP，回主线程落核心）

    private func requestRoute(originWGS84: GeoPoint, destinationGCJ: GeoPoint) {
        routeRequestInFlight = true
        statusText = "正在获取路线…"
        let gateway = gatewayURL
        let token = gatewayToken
        /* 网关收 WGS84（由它转 GCJ-02），故终点要转回 WGS84 */
        let destWGS = Coords.gcj02ToWgs84(destinationGCJ)

        Task { [weak self] in
            let result: Result<(NavRoute, String?), Error> = await withCheckedContinuation { cont in
                DispatchQueue.global(qos: .userInitiated).async {
                    do {
                        let bundle = try GatewayClient.fetchRoute(gateway: gateway, token: token,
                                                                  origin: originWGS84,
                                                                  destination: destWGS)
                        let route = bundle.toNavRoute()
                        guard route.polyline.count >= 2 else {
                            throw GatewayError.decode("路线折线点不足（\(route.polyline.count)）")
                        }
                        cont.resume(returning: .success((route, Self.summary(bundle: bundle))))
                    } catch {
                        cont.resume(returning: .failure(error))
                    }
                }
            }

            guard let self else { return }
            self.routeRequestInFlight = false
            switch result {
            case .success(let (route, summary)):
                self.core.setRoute(route)
                self.pendingDestination = nil
                self.isNavigating = true
                self.routeSummary = summary
                self.lastError = nil
                self.update(snapshot: self.core.snapshot, force: true)
            case .failure(let e):
                self.lastError = "获取路线失败：\(e)"
                self.statusText = "获取路线失败"
                self.isNavigating = false
                self.update(snapshot: self.core.snapshot, force: true)
            }
        }
    }

    private func dispatchReroute(commands: [NavCommand]) {
        guard !rerouteInFlight else { return }
        guard isConfigured else {
            lastError = "重新规划需要网关，但网关未配置"
            return
        }
        rerouteInFlight = true
        rerouteCount += 1
        statusText = "已偏航，正在重新规划…"

        let provider = GatewayRouteProvider(gateway: gatewayURL, token: gatewayToken)

        Task { [weak self] in
            /* 后台只"取路线"（不含核心）；折线校验与坐标转换复用同一实现 */
            let out: RerouteDispatcher.Outcome = await withCheckedContinuation { cont in
                DispatchQueue.global(qos: .userInitiated).async {
                    cont.resume(returning: RerouteDispatcher.fetchNewRoute(commands: commands,
                                                                           provider: provider))
                }
            }
            guard let self else { return }
            self.rerouteInFlight = false
            if let r = out.newRoute {
                self.core.setRoute(r) /* 回主线程才碰核心 */
                self.routeSummary = nil /* 新路线拿不到 bundle 概要，UI 用剩余里程即可 */
                self.lastError = nil
            } else if let e = out.errorText {
                self.lastError = "重新规划失败：\(e)"
            }
            self.update(snapshot: self.core.snapshot, force: true)
        }
    }

    private static func summary(bundle: RouteBundle) -> String? {
        guard let d = bundle.distanceM else { return nil }
        let km = d / 1000
        if let s = bundle.durationS {
            return String(format: "%.1f km · %d 分钟", km, Int((s / 60).rounded()))
        }
        return String(format: "%.1f km", km)
    }
}
