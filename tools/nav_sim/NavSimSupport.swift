/**
 * NavSimSupport.swift —— Mac 模拟器**专用**的纯逻辑部分（可离线测试）
 *
 * ⚠️ 网关客户端、RouteBundle、RouteProvider、RerouteDispatcher 已移到
 *    ios/CarDashboardPhone/CarDashboardPhone/Nav/GatewayClient.swift，
 *    由构建命令一起编译（两处共用一份，勿在本文件里重复定义）。
 *
 * 本文件只保留**模拟器专用**的内容：DriveSimulator（沿路线推进的位置发生器）。
 * 不含 CoreBluetooth（见 NavSimBLE.swift），因此可在任何平台编译与测试。
 */

import Foundation

// MARK: - 行车模拟

// MARK: - 行车模拟

/// 沿路线匀速推进的位置发生器（纯逻辑，可单测）
public struct DriveSimulator {
    public let route: NavRoute
    /// 速度（米/秒）
    public var speedMps: Double
    /// 已行驶距离（米）
    public private(set) var traveledM: Double = 0

    /// 偏离参数：从 `offRouteFromM` 起，把位置横向偏移 `offRouteOffsetM` 米
    public var offRouteFromM: Double?
    public var offRouteOffsetM: Double = 0

    public init(route: NavRoute, speedMps: Double) {
        self.route = route
        self.speedMps = speedMps
    }

    public var finished: Bool { traveledM >= route.totalDistanceM }

    /// 推进 dt 秒，返回"车辆真实位置"（**GCJ-02**，与路线同一坐标系）
    public mutating func advance(dt: Double) -> GeoPoint? {
        traveledM = min(route.totalDistanceM, traveledM + speedMps * dt)
        guard let base = route.pointAt(distanceM: traveledM) else { return nil }
        guard let from = offRouteFromM, traveledM >= from, offRouteOffsetM != 0 else {
            return base
        }
        // 沿**当前航向的右侧法向**偏移，保证垂距 ≈ 指定值且保持稳定
        let heading = routeHeadingDeg(at: traveledM)
        return DriveSimulator.offset(base, bearingDeg: heading + 90, meters: offRouteOffsetM)
    }

    /// 把一点沿**指定方位角**偏移若干米。
    ///
    /// 注意不要用"固定朝东偏移"来制造偏航：路线一转向，东向偏移就变成
    /// **沿路线**方向了，垂距会忽大忽小（实测 43.9 → 12.6 → 3.5 m），
    /// 既不稳定也可能永远触发不了偏航判定。必须用**当前航向的法向**。
    ///
    /// - Parameters:
    ///   - bearingDeg: 方位角（0=北，90=东），与 `routeHeadingDeg` 相加即得法向
    ///   - meters: 偏移距离（正数沿该方位）
    public static func offset(_ p: GeoPoint, bearingDeg: Double, meters: Double) -> GeoPoint {
        let rad = bearingDeg * .pi / 180
        let dNorth = meters * cos(rad)
        let dEast = meters * sin(rad)
        let dLatDeg = dNorth / 111_320.0
        let dLonDeg = dEast / (111_320.0 * cos(p.lat * .pi / 180))
        return GeoPoint(lon: p.lon + dLonDeg, lat: p.lat + dLatDeg)
    }

    /// 路线在 `distanceM` 处的航向角（度，0=北，90=东）。
    /// 用前后各取一小段来估算，避免在折点处抖动。
    public func routeHeadingDeg(at distanceM: Double) -> Double {
        let d0 = max(0, distanceM - 5)
        let d1 = min(route.totalDistanceM, distanceM + 5)
        guard let a = route.pointAt(distanceM: d0),
              let b = route.pointAt(distanceM: d1) else { return 0 }
        let dNorth = (b.lat - a.lat) * 111_320.0
        let dEast = (b.lon - a.lon) * 111_320.0 * cos(a.lat * .pi / 180)
        if abs(dNorth) < 1e-9 && abs(dEast) < 1e-9 { return 0 }
        let deg = atan2(dEast, dNorth) * 180 / .pi
        return deg < 0 ? deg + 360 : deg
    }
}
