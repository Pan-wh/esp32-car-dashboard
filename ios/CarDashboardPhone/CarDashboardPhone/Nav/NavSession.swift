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
 *  1. **不自己起 Timer**：由 `DriveSessionManager` 在既有 tick 里调用 `onFix`
 *     与 `tick`，与遥测共用同一条驱动链。两个定时器各自读定位只会互相打架。
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

    /// 上次已发布的快照，用于"只有变化才赋值"
    private var lastPublished = NavSnapshot()

    // MARK: - 生命周期

    /// 绑定 BLE 客户端（DriveSessionManager 初始化时调用一次即可）
    func attach(ble: BLEClient) {
        self.ble = ble
    }

    /// 配置网关（UI 修改后调用）
    func configure(gateway: String, token: String) {
        gatewayURL = gateway.trimmingCharacters(in: .whitespacesAndNewlines)
        gatewayToken = token.trimmingCharacters(in: .whitespacesAndNewlines)
    }

    var isConfigured: Bool { !gatewayURL.isEmpty && !gatewayToken.isEmpty }

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
        /* 若此刻已有可用定位，下一次 onFix 会立刻触发取路线 */
    }

    /// 停止导航：清空核心、恢复 IDLE，并让仪表也回到"未开始导航"
    func stop() {
        pendingDestination = nil
        routeRequestInFlight = false
        rerouteInFlight = false
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
        handle(core.handle(fix: fix, nowMs: nowMs), nowMs: nowMs)
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

        /* 节流下发 */
        if nowMs - lastSentMs >= sendIntervalMs {
            lastSentMs = nowMs
            ble?.sendNav(snapshot: outcome.snapshot)
        }
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
