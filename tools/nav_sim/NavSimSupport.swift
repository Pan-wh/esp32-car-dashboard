/**
 * NavSimSupport.swift —— Mac 模拟器的纯逻辑部分（可离线测试）
 *
 * 包含：网关客户端、RouteBundle 解码、行车模拟。
 * 不含 CoreBluetooth（见 NavSimBLE.swift），因此本文件可在任何平台编译与测试。
 */

import Foundation

// MARK: - 网关返回结构（与 gateway/src/normalize.js 的 RouteBundle 对应）

public struct RouteBundleStep: Codable {
    public let index: Int
    public let maneuver: Int
    public let distanceM: Double?
    public let roadName: String?
    public let instruction: String?
    public let durationS: Double?
    public let polyline: [RouteBundlePoint]
}

public struct RouteBundlePoint: Codable {
    public let lon: Double
    public let lat: Double
    public var geo: GeoPoint { GeoPoint(lon: lon, lat: lat) }
}

public struct RouteBundle: Codable {
    public let routeId: String?
    public let distanceM: Double?
    public let durationS: Double?
    public let origin: RouteBundlePoint?
    public let destination: RouteBundlePoint?
    public let steps: [RouteBundleStep]
    public let polyline: [RouteBundlePoint]
    public let alternativeCount: Int?

    /// 转换为导航核心使用的 NavRoute。
    ///
    /// 动作数值与固件/网关逐值一致（0..7），越界一律降级为"未知"，
    /// 与网关侧 `maneuver.js`、固件侧 `nav_clamp_maneuver` 保持同一策略。
    public func toNavRoute() -> NavRoute {
        let steps: [NavStep] = self.steps.map { s in
            NavStep(maneuver: Maneuver(rawValue: s.maneuver) ?? .unknown,
                    distanceM: s.distanceM,
                    roadName: s.roadName,
                    polyline: s.polyline.map { $0.geo })
        }
        return NavRoute(steps: steps)
    }
}

// MARK: - 网关客户端

public enum GatewayError: Error, CustomStringConvertible {
    case badURL(String)
    case http(Int)
    case transport(String)
    case decode(String)

    public var description: String {
        switch self {
        case .badURL(let u): return "网关地址不合法：\(u)"
        case .http(let c): return "网关返回 HTTP \(c)"
        case .transport(let m): return "连接网关失败：\(m)"
        case .decode(let m): return "解析网关响应失败：\(m)"
        }
    }
}

public enum GatewayClient {
    /// 请求路线。坐标是 **WGS84**（手机定位坐标系），由网关转 GCJ-02。
    ///
    /// 同步实现：模拟器是命令行工具，用信号量等待即可，逻辑更直观。
    public static func fetchRoute(gateway: String,
                                 token: String,
                                 origin: GeoPoint,
                                 destination: GeoPoint,
                                 timeoutSec: Double = 15) throws -> RouteBundle {
        guard let url = URL(string: gateway.hasSuffix("/") ? gateway + "route" : gateway + "/route") else {
            throw GatewayError.badURL(gateway)
        }

        var req = URLRequest(url: url)
        req.httpMethod = "POST"
        req.setValue("application/json", forHTTPHeaderField: "content-type")
        req.setValue("Bearer \(token)", forHTTPHeaderField: "authorization")
        req.timeoutInterval = timeoutSec
        req.httpBody = try JSONSerialization.data(withJSONObject: [
            "origin": ["lon": origin.lon, "lat": origin.lat],
            "destination": ["lon": destination.lon, "lat": destination.lat],
        ])

        let sem = DispatchSemaphore(value: 0)
        var result: Result<RouteBundle, Error>!

        URLSession.shared.dataTask(with: req) { data, resp, err in
            defer { sem.signal() }
            if let err = err {
                result = .failure(GatewayError.transport(err.localizedDescription))
                return
            }
            let code = (resp as? HTTPURLResponse)?.statusCode ?? 0
            guard let data = data else {
                result = .failure(GatewayError.http(code))
                return
            }
            guard code == 200 else {
                // 网关的错误响应是 JSON（含 error/info/infocode/hint），尽量透出
                let detail = String(data: data, encoding: .utf8) ?? ""
                result = .failure(GatewayError.transport("HTTP \(code)：\(detail.prefix(300))"))
                return
            }
            do {
                result = .success(try JSONDecoder().decode(RouteBundle.self, from: data))
            } catch {
                result = .failure(GatewayError.decode("\(error)"))
            }
        }.resume()

        if sem.wait(timeout: .now() + timeoutSec + 1) == .timedOut {
            throw GatewayError.transport("请求超时（\(Int(timeoutSec))s）")
        }
        return try result.get()
    }

    /// 从本地 JSON 文件读路线（离线调试用；文件内容即网关的响应体）
    public static func loadRouteFile(_ path: String) throws -> RouteBundle {
        let data = try Data(contentsOf: URL(fileURLWithPath: path))
        return try JSONDecoder().decode(RouteBundle.self, from: data)
    }
}

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
