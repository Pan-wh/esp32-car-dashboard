/**
 * NavCore.swift —— 导航核心（路线进度 / 偏航 / 到达判定）
 *
 * ## 为什么放在手机端
 *
 * 路由与位置匹配需要实时位置，若放到 ESP32 或网关，前者算力/内存有限、
 * 后者要每次定位都走网络往返（2Hz 不现实）。故本模块跑在 iPhone 上，
 * ESP32 只接收"已算好的显示快照"（见 docs/nav-backend.md §2.2）。
 *
 * ## 设计约束
 *
 * 1. **不依赖 CoreLocation / UIKit**：只用 Foundation，因此可在 macOS 上
 *    直接编译单测（见 tools/test_nav_core.swift，测试与本文件**编译同一份源码**，
 *    不存在"测试副本与实现漂移"的问题）。
 *    调用方负责把 CLLocation 适配成 `NavFix`。
 * 2. **坐标一律为 GCJ-02**：与网关返回、高德一致。
 *    WGS84 → GCJ-02 的转换由调用方在**定位回调里**完成（见 Coords.swift）。
 * 3. **安全优先**：定位精度差、数据过期、偏航时**不得**给出误导性指引 ——
 *    宁可显示"未知/过期"，也不能显示"方向相反"的转向。
 */

import Foundation

// MARK: - 基础类型

/// 经纬度点（GCJ-02）
public struct GeoPoint: Equatable, Sendable {
    public var lon: Double
    public var lat: Double

    public init(lon: Double, lat: Double) {
        self.lon = lon
        self.lat = lat
    }
}

/// 一次定位（由调用方从 CLLocation 适配而来；坐标必须已是 GCJ-02）
public struct NavFix: Equatable, Sendable {
    public var position: GeoPoint
    /// 水平精度（米）。越大越不可信；超过 `maximumUsableAccuracyM` 时**不用于更新进度**
    public var horizontalAccuracyM: Double
    /// 地面行进方向（度，0..360）。无有效值时给 nil
    public var courseDeg: Double?
    /// 定位时间戳（毫秒，单调时钟更佳）
    public var timestampMs: Int64

    public init(position: GeoPoint,
                horizontalAccuracyM: Double,
                courseDeg: Double? = nil,
                timestampMs: Int64) {
        self.position = position
        self.horizontalAccuracyM = horizontalAccuracyM
        self.courseDeg = courseDeg
        self.timestampMs = timestampMs
    }
}

/// 转向动作。**数值必须与固件 `nav_maneuver_t`、网关 maneuver.js 逐值一致**
public enum Maneuver: Int, Sendable {
    case unknown = 0
    case straight = 1
    case left = 2
    case right = 3
    case uTurn = 4
    case keepLeft = 5
    case keepRight = 6
    case arrive = 7
}

/// 导航状态。**数值必须与固件 `nav_state_t`、网关/BLE 协议一致**
public enum NavState: Int, Sendable {
    case idle = 0
    case active = 1
    case rerouting = 2
    case arrived = 3
    case stale = 4
    case noData = 5
}

// MARK: - 路线模型

/// 路线中的一个步骤
public struct NavStep: Equatable, Sendable {
    public var maneuver: Maneuver
    /// 该步骤长度（米）。高德返回的 `step_distance`；为 nil 时用几何长度兜底
    public var distanceM: Double?
    public var roadName: String?
    public var polyline: [GeoPoint]

    public init(maneuver: Maneuver,
                distanceM: Double?,
                roadName: String? = nil,
                polyline: [GeoPoint] = []) {
        self.maneuver = maneuver
        self.distanceM = distanceM
        self.roadName = roadName
        self.polyline = polyline
    }
}

/// 一条可导航的路线。
///
/// 构造时会把各 step 的折线拼接成整条路线，并预先算出：
///   - 每个折线点的累计距离 `cumulativeM`
///   - 每个 step 在整条路线上的距离区间 `stepRanges`
///
/// ⚠️ **几何长度与 step 上报长度不一致时的处理**：高德给的 `step_distance`
/// 是它自己的测算，未必等于折线几何长度。若二者总长差异超过 `scaleTolerance`，
/// 构造时会**用几何长度按比例缩放 step 区间**，使"距下一转向的距离"与
/// 折线进度自洽（否则会出现"已过转向点但还显示还有 200 米"）。
public struct NavRoute: Sendable {
    /// 拼接后的完整折线
    public private(set) var polyline: [GeoPoint]
    /// 每个折线点的累计距离（米），与 polyline 等长
    public private(set) var cumulativeM: [Double]
    /// 每个 step 的距离区间 [(起点距离, 终点距离)]
    public private(set) var stepRanges: [(start: Double, end: Double)]
    public let steps: [NavStep]
    /// 路线总长（米）——以**几何长度**为准
    public var totalDistanceM: Double { cumulativeM.last ?? 0 }

    /// 几何长度与 step 上报长度的允许相对差异
    public static let scaleTolerance = 0.05

    public init(steps: [NavStep]) {
        self.steps = steps

        var pts: [GeoPoint] = []
        var stepRanges: [(start: Double, end: Double)] = []
        var geometricLengths: [Double] = []

        for st in steps {
            let startIdx = pts.count
            // 拼接时去掉与上一个 step 重复的首点，避免零长段
            var seg = st.polyline
            if let last = pts.last, let first = seg.first, last == first {
                seg.removeFirst()
            }
            pts.append(contentsOf: seg)

            let len = NavRoute.polylineLength(st.polyline)
            geometricLengths.append(len)

            // 临时区间（后面按比例修正）
            _ = startIdx
            stepRanges.append((start: 0, end: len))
        }

        self.polyline = pts
        self.cumulativeM = NavRoute.cumulative(pts)

        // 用几何总长修正 step 区间：
        // 优先按高德上报长度分配；若与几何长度差异过大，则改用几何长度比例分配。
        let geomTotal = cumulativeM.last ?? 0
        let reportedTotal = steps.reduce(0.0) { $0 + ($1.distanceM ?? 0) }
        let reportedUsable = steps.contains { $0.distanceM != nil }

        var weights: [Double]
        if reportedUsable && reportedTotal > 0
            && abs(reportedTotal - geomTotal) <= max(NavRoute.scaleTolerance * geomTotal, 1.0) {
            // 二者一致：直接用上报长度（更贴近手机导航显示）
            weights = steps.map { $0.distanceM ?? 0 }
        } else {
            // 差异过大或没有上报长度：用几何长度
            weights = geometricLengths
        }

        // 归一化到几何总长，保证区间与 cumulativeM 同一尺度
        let wSum = weights.reduce(0, +)
        if wSum > 0 && geomTotal > 0 {
            let k = geomTotal / wSum
            weights = weights.map { $0 * k }
        }

        var acc = 0.0
        for i in weights.indices {
            stepRanges[i] = (start: acc, end: acc + weights[i])
            acc += weights[i]
        }
        // 浮点误差兜底：最后一段对齐到总长
        if !stepRanges.isEmpty {
            let last = stepRanges.count - 1
            stepRanges[last].end = geomTotal
        }
        self.stepRanges = stepRanges
    }

    /// 折线总长
    static func polylineLength(_ pts: [GeoPoint]) -> Double {
        guard pts.count >= 2 else { return 0 }
        var sum = 0.0
        for i in 0..<(pts.count - 1) { sum += NavMath.distanceM(pts[i], pts[i + 1]) }
        return sum
    }

    /// 逐点累计距离
    static func cumulative(_ pts: [GeoPoint]) -> [Double] {
        guard !pts.isEmpty else { return [] }
        var out = [0.0]
        out.reserveCapacity(pts.count)
        for i in 1..<pts.count {
            out.append(out[i - 1] + NavMath.distanceM(pts[i - 1], pts[i]))
        }
        return out
    }

    /// 由"沿路线已行驶距离"取所在 step 下标（越界时返回最后一个）
    public func stepIndex(atProgressM d: Double) -> Int {
        guard !stepRanges.isEmpty else { return -1 }
        for (i, r) in stepRanges.enumerated() where d < r.end { return i }
        return stepRanges.count - 1
    }

    /// 由 step 下标取该 step 起点的沿路线距离
    public func stepStartM(_ index: Int) -> Double {
        guard index >= 0 && index < stepRanges.count else { return 0 }
        return stepRanges[index].start
    }

    /// 取"沿路线行驶 `distanceM` 米处"的位置（线性插值到所在折线段内）。
    ///
    /// 用途：模拟行车（tools/nav_sim）、以及将来画"已走过/未走过"的路线着色。
    /// 越界会被钳制到路线首/尾，不返回 nil（调用方通常是"按进度取点"）。
    public func pointAt(distanceM d: Double) -> GeoPoint? {
        guard polyline.count >= 1 else { return nil }
        if polyline.count == 1 { return polyline[0] }

        let clamped = min(max(0, d), totalDistanceM)

        // 找到 clamped 落在哪一段（cumulativeM 单调递增）
        var i = 0
        while i < cumulativeM.count - 2 && cumulativeM[i + 1] < clamped { i += 1 }

        let segStart = cumulativeM[i]
        let segEnd = cumulativeM[i + 1]
        let segLen = segEnd - segStart
        guard segLen > 1e-9 else { return polyline[i] }

        let t = min(1, max(0, (clamped - segStart) / segLen))
        let a = polyline[i]
        let b = polyline[i + 1]
        return GeoPoint(lon: a.lon + (b.lon - a.lon) * t,
                        lat: a.lat + (b.lat - a.lat) * t)
    }
}

// MARK: - 数学工具

enum NavMath {
    static let earthRadiusM = 6371008.8

    static func toRad(_ deg: Double) -> Double { deg * .pi / 180 }

    /// 两点球面距离（Haversine，米）
    static func distanceM(_ a: GeoPoint, _ b: GeoPoint) -> Double {
        let dLat = toRad(b.lat - a.lat)
        let dLon = toRad(b.lon - a.lon)
        let la1 = toRad(a.lat)
        let la2 = toRad(b.lat)
        let h = pow(sin(dLat / 2), 2) + cos(la1) * cos(la2) * pow(sin(dLon / 2), 2)
        return 2 * earthRadiusM * asin(min(1, sqrt(h)))
    }

    /// 把经纬度映射到以 `origin` 为原点的局部平面（米）。
    ///
    /// 用等距圆柱近似：在几公里范围内误差远小于 GPS 自身误差，
    /// 且避免了每次投影都做球面三角运算的开销。
    static func toLocalMeters(_ p: GeoPoint, origin: GeoPoint) -> (x: Double, y: Double) {
        let x = toRad(p.lon - origin.lon) * cos(toRad(origin.lat)) * earthRadiusM
        let y = toRad(p.lat - origin.lat) * earthRadiusM
        return (x, y)
    }

    /// 点到线段的投影：返回 (沿线段比例 t∈[0,1], 垂距米)
    static func projectToSegment(point: (x: Double, y: Double),
                                 a: (x: Double, y: Double),
                                 b: (x: Double, y: Double)) -> (t: Double, crossM: Double) {
        let dx = b.x - a.x
        let dy = b.y - a.y
        let len2 = dx * dx + dy * dy
        guard len2 > 1e-12 else {
            let ex = point.x - a.x, ey = point.y - a.y
            return (0, sqrt(ex * ex + ey * ey))
        }
        var t = ((point.x - a.x) * dx + (point.y - a.y) * dy) / len2
        t = min(1, max(0, t))
        let px = a.x + t * dx
        let py = a.y + t * dy
        let ex = point.x - px, ey = point.y - py
        return (t, sqrt(ex * ex + ey * ey))
    }
}

// MARK: - 配置 / 快照

public struct NavConfig: Sendable {
    /// 水平精度差于此值（米）时，本次定位**不用于更新进度**（避免被差定位带偏）
    public var maximumUsableAccuracyM: Double = 50
    /// 偏航判定阈值（米）：垂距超过它视为偏航候选
    public var offRouteThresholdM: Double = 45
    /// 回归判定阈值（米）：垂距小于它立即恢复
    public var onRouteThresholdM: Double = 25
    /// 偏航需连续确认次数（抑制 GPS 抖动误报）
    public var offRouteConfirmations: Int = 3
    /// 到达判定半径（米）
    public var arrivalRadiusM: Double = 20
    /// 到达需连续确认次数
    public var arrivalConfirmations: Int = 2
    /// 无定位超过此时长（毫秒）→ 数据过期
    public var staleAfterMs: Int64 = 5000
    /// 允许进度回退的最大距离（米）：GPS 抖动导致的轻微倒退不视为"掉头"
    public var backtrackAllowanceM: Double = 30

    public init() {}
}

/// 交给 UI / BLE 的显示快照
public struct NavSnapshot: Equatable, Sendable {
    public var state: NavState = .noData
    public var maneuver: Maneuver = .unknown
    /// 距下一转向的距离（米）。未知时为 nil（UI 应显示 "--"）
    public var distanceToManeuverM: Int?
    public var routeProgressM: Double = 0
    public var remainingDistanceM: Double = 0
    /// 当前垂距（偏离路线的横向距离，米）
    public var crossTrackM: Double = 0
    public var offRoute: Bool = false
    public var arrived: Bool = false
    /// 本次定位是否被采纳（精度不足时为 false）
    public var fixUsable: Bool = false
    /// 所在 step 下标（-1 表示无）
    public var stepIndex: Int = -1

    public init() {}
}

// MARK: - 核心

public final class NavCore {
    public private(set) var config: NavConfig
    public private(set) var route: NavRoute?
    public private(set) var snapshot = NavSnapshot()

    /// 最近一次被采纳的进度（米）——用于单调性约束
    private var lastProgressM: Double = 0
    private var lastFixMs: Int64 = 0
    private var hasFix = false
    private var offRouteStreak = 0
    private var arrivalStreak = 0
    private var arrivedLatched = false

    public init(config: NavConfig = NavConfig()) {
        self.config = config
    }

    /// 设置（或替换）路线。会重置进度与确认计数。
    public func setRoute(_ route: NavRoute?) {
        self.route = route
        resetProgress()
        snapshot = NavSnapshot()
        if route != nil {
            snapshot.state = .active
            updateDerived(atProgressM: 0, crossTrackM: 0)
        } else {
            snapshot.state = .idle
        }
    }

    /// 仅清除路线前的状态（例如用户结束导航）
    public func clear() {
        setRoute(nil)
        snapshot.state = .noData
    }

    private func resetProgress() {
        lastProgressM = 0
        hasFix = false
        offRouteStreak = 0
        arrivalStreak = 0
        arrivedLatched = false
    }

    /// 处理一次定位，返回最新快照。
    ///
    /// - Parameters:
    ///   - fix: 已转换为 **GCJ-02** 的定位
    ///   - nowMs: 当前时间（毫秒）。传入便于测试注入
    @discardableResult
    public func handle(fix: NavFix, nowMs: Int64) -> NavSnapshot {
        guard let route = route, route.polyline.count >= 2 else {
            snapshot.state = .noData
            snapshot.fixUsable = false
            return snapshot
        }

        lastFixMs = fix.timestampMs
        hasFix = true

        // ---- 精度门槛：差定位不更新进度，但也不清空已有进度 ----
        let usable = fix.horizontalAccuracyM >= 0
            && fix.horizontalAccuracyM <= config.maximumUsableAccuracyM
        snapshot.fixUsable = usable

        if !usable {
            // 保持上次进度，状态仍可为 active（只是本次不用）
            refreshState(nowMs: nowMs)
            return snapshot
        }

        // ---- 投影到路线 ----
        guard let proj = project(point: fix.position, route: route) else {
            refreshState(nowMs: nowMs)
            return snapshot
        }

        // ---- 单调性：投影结果不应大幅回退 ----
        var progress = proj.alongM
        if progress < lastProgressM - config.backtrackAllowanceM {
            // 明显回退：视为 GPS 抖动/短暂错配，保持上次进度（偏航判定另行处理）
            progress = lastProgressM
        } else if progress < lastProgressM {
            // 轻微回退：允许，但不更新 lastProgress（避免来回抖动）
            progress = lastProgressM
        }
        lastProgressM = max(lastProgressM, progress)

        // ---- 偏航判定（连续确认）----
        let cross = proj.crossTrackM
        if cross > config.offRouteThresholdM {
            offRouteStreak += 1
        } else if cross < config.onRouteThresholdM {
            offRouteStreak = 0 // 恢复立即生效（好消息不延迟）
        }
        let offRoute = offRouteStreak >= config.offRouteConfirmations

        // ---- 到达判定（连续确认）----
        let remaining = max(0, route.totalDistanceM - lastProgressM)
        if remaining <= config.arrivalRadiusM {
            arrivalStreak += 1
        } else {
            arrivalStreak = 0
        }
        if arrivalStreak >= config.arrivalConfirmations {
            arrivedLatched = true
        }

        let followManeuver = followManeuverHint(fix: fix, route: route)

        updateDerived(atProgressM: lastProgressM, crossTrackM: cross)
        snapshot.offRoute = offRoute
        snapshot.arrived = arrivedLatched
        snapshot.crossTrackM = cross

        // ---- 状态优先级：到达 > 偏航(重算) > 正常 ----
        if arrivedLatched {
            snapshot.state = .arrived
            snapshot.maneuver = .arrive
            snapshot.distanceToManeuverM = 0
        } else if offRoute {
            snapshot.state = .rerouting
            // 偏航时**不给出转向指引**（可能已不适用），由调用方决定是否重算
            snapshot.maneuver = .unknown
            snapshot.distanceToManeuverM = nil
        } else {
            snapshot.state = .active
            if let fm = followManeuver {
                snapshot.maneuver = fm.maneuver
                snapshot.distanceToManeuverM = fm.distanceM
            }
        }
        return snapshot
    }

    /// 周期性调用（即使用户没动），用于把"数据过期"反映出来。
    @discardableResult
    public func tick(nowMs: Int64) -> NavSnapshot {
        refreshState(nowMs: nowMs)
        return snapshot
    }

    // MARK: 私有

    private func refreshState(nowMs: Int64) {
        guard route != nil else {
            snapshot.state = .noData
            return
        }
        if !hasFix {
            snapshot.state = .noData
            return
        }
        let age = nowMs - lastFixMs
        if age > config.staleAfterMs {
            // 过期：**停止显示旧指引**（与固件侧 §6.3 原则一致）
            snapshot.state = .stale
            snapshot.maneuver = .unknown
            snapshot.distanceToManeuverM = nil
            return
        }
        if arrivedLatched {
            snapshot.state = .arrived
            snapshot.maneuver = .arrive
            snapshot.distanceToManeuverM = 0
            return
        }
        if snapshot.offRoute {
            snapshot.state = .rerouting
            snapshot.maneuver = .unknown
            snapshot.distanceToManeuverM = nil
            return
        }
        snapshot.state = .active
    }

    /// 计算"当前应指引的动作 + 距该动作的距离"。
    ///
    /// 语义：我们引导用户**前往当前 step 的终点**（即下一个转向点），
    /// 动作取当前 step 的 maneuver。
    private func followManeuverHint(fix: NavFix, route: NavRoute)
        -> (maneuver: Maneuver, distanceM: Int)? {
        let idx = route.stepIndex(atProgressM: lastProgressM)
        guard idx >= 0 && idx < route.steps.count else { return nil }
        let step = route.steps[idx]
        let end = route.stepRanges[idx].end
        let d = max(0, end - lastProgressM)
        return (step.maneuver, Int(d.rounded()))
    }

    private func updateDerived(atProgressM progress: Double, crossTrackM cross: Double) {
        guard let route = route else { return }
        snapshot.routeProgressM = progress
        snapshot.remainingDistanceM = max(0, route.totalDistanceM - progress)
        snapshot.crossTrackM = cross
        snapshot.stepIndex = route.stepIndex(atProgressM: progress)
    }

    /// 全路线投影：取垂距最小的候选；同时受"单调窗口"约束。
    private func project(point: GeoPoint, route: NavRoute) -> (alongM: Double, crossTrackM: Double)? {
        let pts = route.polyline
        guard pts.count >= 2 else { return nil }

        let origin = point
        let p = NavMath.toLocalMeters(point, origin: origin) // 原点即自身 → (0,0)

        var best: (along: Double, cross: Double)?
        var bestConstrained: (along: Double, cross: Double)?
        let minAllowed = lastProgressM - config.backtrackAllowanceM

        for i in 0..<(pts.count - 1) {
            let a = NavMath.toLocalMeters(pts[i], origin: origin)
            let b = NavMath.toLocalMeters(pts[i + 1], origin: origin)
            let segLen = route.cumulativeM[i + 1] - route.cumulativeM[i]
            let (t, cross) = NavMath.projectToSegment(point: p, a: a, b: b)
            let along = route.cumulativeM[i] + t * segLen

            if best == nil || cross < best!.cross {
                best = (along, cross)
            }
            // 单调约束下的最优（排除明显回退的段）
            if along >= minAllowed {
                if bestConstrained == nil || cross < bestConstrained!.cross {
                    bestConstrained = (along, cross)
                }
            }
        }

        // 优先采用满足单调约束的解；若不存在（例如大幅跳变/重算后）则退回全局最优
        if let c = bestConstrained { return (alongM: c.along, crossTrackM: c.cross) }
        if let b = best { return (alongM: b.along, crossTrackM: b.cross) }
        return nil
    }
}
