/**
 * GatewayClient.swift —— 网关客户端与路线数据（**App 与 Mac 模拟器共用**）
 *
 * ⚠️ 本文件是**单一真源**：`tools/nav_sim` 的构建命令会直接编译它
 *    （与 NavCore.swift / Coords.swift 同样处理），
 *    不要把这里的内容再抄一份到模拟器里 —— 那样两边会漂移。
 *
 * 内容：
 *   - RouteBundle*：网关返回的路线结构（与 gateway/src/normalize.js 对应）
 *   - GatewayClient：请求路线（**坐标一律 WGS84**，由网关转 GCJ-02）
 *   - RouteProvider / GatewayRouteProvider：路线来源抽象（便于单测用假实现）
 *   - RerouteDispatcher：把导航核心产出的"请求重算"命令变成实际取路线
 *
 * 迁移说明：本文件原先在 tools/nav_sim/NavSimSupport.swift 里，
 * 为让 App 接入导航时复用同一份已验证代码而移到这里（2026-10-14）。
 */

import Foundation

// MARK: - 网关返回结构（与 gateway/src/normalize.js 的 RouteBundle 对应）

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

// MARK: - 路线来源与重算调度

/// 路线来源抽象。**坐标一律 WGS84**（与网关约定一致）。
///
/// 抽出来是为了让"重算调度"可单测：测试用假实现即可，
/// 不需要真网关、不需要网络。
public protocol RouteProvider {
    func route(originWGS84: GeoPoint, destinationWGS84: GeoPoint) throws -> RouteBundle
}

/// 走网关的实现
public struct GatewayRouteProvider: RouteProvider {
    public let gateway: String
    public let token: String
    public let timeoutSec: Double

    public init(gateway: String, token: String, timeoutSec: Double = 15) {
        self.gateway = gateway
        self.token = token
        self.timeoutSec = timeoutSec
    }

    public func route(originWGS84: GeoPoint, destinationWGS84: GeoPoint) throws -> RouteBundle {
        try GatewayClient.fetchRoute(gateway: gateway, token: token,
                                     origin: originWGS84, destination: destinationWGS84,
                                     timeoutSec: timeoutSec)
    }
}

/// 重算调度：把核心产出的命令变成"取新路线并交回核心"这一副作用。
///
/// 放在这里（而不是塞在命令行主循环里）是为了**可测试** ——
/// 主循环是 `@main`，测试无法直接调用。
public enum RerouteDispatcher {
    public struct Outcome {
        /// 是否成功替换了路线
        public var applied = false
        /// 新路线（若成功）
        public var newRoute: NavRoute?
        /// 失败原因（若失败）
        public var errorText: String?
        /// 本次处理了几个重算请求
        public var handled = 0
    }

    /// **只取路线、不碰导航核心**（供 App 使用）。
    ///
    /// 为什么要拆出来：App 的网络请求必须在后台线程（`fetchRoute` 是同步实现），
    /// 而 `NavCore` **不是 Sendable**，不能跨线程传。所以 App 的流程是
    /// "后台 `fetchNewRoute` → 回主线程 `core.setRoute`"。
    ///
    /// 本函数与 `dispatch` 共用同一份**坐标转换与折线校验**逻辑 ——
    /// 这两步最容易写错（漏转换会让整条路线偏移数百米），必须只有一处实现。
    public static func fetchNewRoute(commands: [NavCommand],
                                     provider: RouteProvider) -> Outcome {
        var out = Outcome()
        for cmd in commands {
            guard case .requestRoute(let req) = cmd else { continue }
            out.handled += 1
            do {
                let bundle = try provider.route(
                    originWGS84: Coords.gcj02ToWgs84(req.origin),
                    destinationWGS84: Coords.gcj02ToWgs84(req.destination))
                let newRoute = bundle.toNavRoute()
                guard newRoute.polyline.count >= 2 else {
                    out.errorText = "新路线折线点不足（\(newRoute.polyline.count)）"
                    continue
                }
                out.newRoute = newRoute
            } catch {
                out.errorText = "\(error)"
            }
        }
        return out
    }

    /// 处理命令。成功取得新路线时会调用 `core.setRoute`。
    ///
    /// 注意坐标转换：核心内部是 **GCJ-02**，而 `RouteProvider` 约定收 **WGS84**
    /// （网关会转回 GCJ-02）。这一步不能省，否则整条新路线会偏移数百米。
    @discardableResult
    public static func dispatch(commands: [NavCommand],
                                provider: RouteProvider,
                                core: NavCore) -> Outcome {
        var out = fetchNewRoute(commands: commands, provider: provider)
        if let r = out.newRoute {
            core.setRoute(r)
            out.applied = true
        }
        return out
    }
}
