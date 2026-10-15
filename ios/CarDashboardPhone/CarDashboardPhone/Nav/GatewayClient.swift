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
    /// 顶层这条是第几条候选（网关 `/routes` 才有意义）
    public let pathIndex: Int?
    /// 候选总数
    public let pathCount: Int?

    /**
     显式初始化。

     为什么要它：`RouteBundle` 全是 `let`，默认的 memberwise init 是 internal 且
     参数必须写全 8 个 —— 从"候选路线"构造时（`RouteCandidate.toBundle()`）
     既啰嗦又容易被字段增删改坏。这里给全默认值，只写关心的字段。
     */
    public init(routeId: String? = nil,
                distanceM: Double? = nil,
                durationS: Double? = nil,
                origin: RouteBundlePoint? = nil,
                destination: RouteBundlePoint? = nil,
                steps: [RouteBundleStep] = [],
                polyline: [RouteBundlePoint] = [],
                alternativeCount: Int? = nil,
                pathIndex: Int? = nil,
                pathCount: Int? = nil) {
        self.routeId = routeId
        self.distanceM = distanceM
        self.durationS = durationS
        self.origin = origin
        self.destination = destination
        self.steps = steps
        self.polyline = polyline
        self.alternativeCount = alternativeCount
        self.pathIndex = pathIndex
        self.pathCount = pathCount
    }

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

// MARK: - 地理编码（中文地名 → 坐标）

/// 一条地理编码结果。**坐标是 WGS84**（网关已从高德的 GCJ-02 转回）。
public struct GeocodeResult: Codable, Identifiable, Sendable {
    public let name: String
    public let address: String
    public let lon: Double
    public let lat: Double
    public var id: String { "\(name)|\(lon)|\(lat)" }
    public var geo: GeoPoint { GeoPoint(lon: lon, lat: lat) }
}

private struct GeocodeResponse: Codable {
    let results: [GeocodeResult]
}

public extension GatewayClient {
    /// 地址文本 → 坐标（支持中文地名）。
    ///
    /// 为什么必须有它：否则用户只能手填经纬度 —— 这不是能用的体验。
    /// 网关侧限制地址长度并做了限流；这里只负责发请求与解包。
    ///
    /// - Returns: 候选列表（可能多个，如"外滩"在不同城市）；空数组表示没找到。
    /// - Throws: 网络/HTTP/解码错误。**"没找到"不是错误**，返回空数组。
    static func geocode(gateway: String,
                        token: String,
                        address: String,
                        city: String = "",
                        timeoutSec: Double = 10) async throws -> [GeocodeResult] {
        guard let url = URL(string: gateway.hasSuffix("/") ? gateway + "geocode" : gateway + "/geocode") else {
            throw GatewayError.badURL(gateway)
        }
        var req = URLRequest(url: url)
        req.httpMethod = "POST"
        req.setValue("application/json", forHTTPHeaderField: "content-type")
        req.setValue("Bearer \(token)", forHTTPHeaderField: "authorization")
        req.timeoutInterval = timeoutSec
        var payload: [String: Any] = ["address": address]
        if !city.isEmpty { payload["city"] = city }
        req.httpBody = try JSONSerialization.data(withJSONObject: payload)

        let (data, resp) = try await URLSession.shared.data(for: req)
        let code = (resp as? HTTPURLResponse)?.statusCode ?? 0
        guard code == 200 else {
            let detail = String(data: data, encoding: .utf8) ?? ""
            throw GatewayError.transport("HTTP \(code)：\(detail.prefix(300))")
        }
        do {
            return try JSONDecoder().decode(GeocodeResponse.self, from: data).results
        } catch {
            throw GatewayError.decode("\(error)")
        }
    }
}

// MARK: - 周边地图窗口

public extension GatewayClient {
    /**
     取当前位置周边的地图窗口（道路 + 建筑几何）。

     与 `fetchRoute` 不同，这里用 **async URLSession**：调用方是 `NavSession`
     的时钟回调，可以 `await`，不会阻塞主线程。

     - Parameter positionWGS84: 手机的 WGS84 定位。**网关内部会转成 GCJ-02**，
       返回的 `centerGcj` 才是局部坐标原点（WGS84 与 GCJ-02 在北京差约 556m，
       不统一会让整张底图偏移一个窗口的距离）。
     */
    static func fetchMapWindow(gateway: String,
                               token: String,
                               positionWGS84: GeoPoint,
                               radiusM: Double = 500,
                               timeoutSec: Double = 10) async throws -> MapWindowBundle {
        guard let url = URL(string: gateway.hasSuffix("/") ? gateway + "mapwindow"
                                                           : gateway + "/mapwindow") else {
            throw GatewayError.badURL(gateway)
        }
        var req = URLRequest(url: url)
        req.httpMethod = "POST"
        req.setValue("application/json", forHTTPHeaderField: "content-type")
        req.setValue("Bearer \(token)", forHTTPHeaderField: "authorization")
        req.timeoutInterval = timeoutSec
        req.httpBody = try JSONSerialization.data(withJSONObject: [
            "lon": positionWGS84.lon,
            "lat": positionWGS84.lat,
            "radiusM": radiusM,
        ])

        let (data, resp) = try await URLSession.shared.data(for: req)
        let code = (resp as? HTTPURLResponse)?.statusCode ?? 0
        guard code == 200 else {
            let detail = String(data: data, encoding: .utf8) ?? ""
            throw GatewayError.transport("HTTP \(code)：\(detail.prefix(300))")
        }
        do {
            return try JSONDecoder().decode(MapWindowBundle.self, from: data)
        } catch {
            throw GatewayError.decode("\(error)")
        }
    }
}

// MARK: - 路线候选（多方案选择）

/// 一条候选路线（与网关 `/routes` 的 `candidates[]` 对应）
public struct RouteCandidate: Codable, Identifiable, Sendable {
    /// 高德策略号（0 速度优先 / 2 距离最短 / 6 不走高速）
    public let strategy: Int
    public let distanceM: Double
    public let durationS: Double
    public let steps: [RouteBundleStep]
    public let polyline: [RouteBundlePoint]

    public var id: Int { strategy }

    /// 策略的中文说明（UI 显示用）
    public var label: String {
        switch strategy {
        case 0: return "速度优先"
        case 2: return "距离最短"
        case 6: return "不走高速"
        default: return "方案 \(strategy)"
        }
    }

    /// 转成 RouteBundle，便于复用既有的 toNavRoute()
    public func toBundle() -> RouteBundle {
        RouteBundle(distanceM: distanceM, durationS: durationS,
                    steps: steps, polyline: polyline)
    }
}

private struct RoutesResponse: Codable {
    let candidates: [RouteCandidate]
}

public extension GatewayClient {
    /**
     取**多方案**路线（一次请求拿到若干条真正不同的路）。

     ## 为什么需要单独一个端点

     高德 v3 驾车 API **一次只返回 1 条**路径（实测 strategy 0/1/2/6/8/35/36/37
     全是 `paths.length == 1`），所以"多方案对比"没法靠它一次给。
     但**换 strategy 会得到真正不同的路线** —— 于是网关用几个策略各取一次、
     按"距离+耗时"去重后合并返回。**一次 App 请求 = 网关上 3 次高德请求**，
     换来的是"真正可选的多条路 + 不需要二次请求"。

     - Throws: 网络/解码错误。**所有策略都失败**时抛 `.transport`。
     */
    static func fetchRouteCandidates(gateway: String,
                                     token: String,
                                     originWGS84: GeoPoint,
                                     destinationWGS84: GeoPoint,
                                     timeoutSec: Double = 25) async throws -> [RouteCandidate] {
        guard let url = URL(string: gateway.hasSuffix("/") ? gateway + "routes" : gateway + "/routes") else {
            throw GatewayError.badURL(gateway)
        }
        var req = URLRequest(url: url)
        req.httpMethod = "POST"
        req.setValue("application/json", forHTTPHeaderField: "content-type")
        req.setValue("Bearer \(token)", forHTTPHeaderField: "authorization")
        /* 网关要串行请求 3 个策略并留间隔（避开高德 QPS），超时给足 */
        req.timeoutInterval = timeoutSec
        req.httpBody = try JSONSerialization.data(withJSONObject: [
            "origin": ["lon": originWGS84.lon, "lat": originWGS84.lat],
            "destination": ["lon": destinationWGS84.lon, "lat": destinationWGS84.lat],
        ])

        let (data, resp) = try await URLSession.shared.data(for: req)
        let code = (resp as? HTTPURLResponse)?.statusCode ?? 0
        guard code == 200 else {
            let detail = String(data: data, encoding: .utf8) ?? ""
            throw GatewayError.transport("HTTP \(code)：\(detail.prefix(300))")
        }
        do {
            return try JSONDecoder().decode(RoutesResponse.self, from: data).candidates
        } catch {
            throw GatewayError.decode("\(error)")
        }
    }
}

// MARK: - 目的地搜索（输入提示 / 周边搜索）

/**
 一条地点候选（`/tips` 与 `/around` 共用给 UI 的结构）。

 ⚠️ 注意：网关这两个端点的**响应字段并不相同**（见下面两个私有解码结构），
 这里只是把两者**归一**成 UI 好用的形状。
 */
public struct PlaceSuggestion: Codable, Identifiable, Sendable {
    public let name: String
    public let address: String
    public let lon: Double
    public let lat: Double
    /// 距当前位置的距离（米）。`/around` 有，`/tips` 没有。
    public let distanceM: Double?

    public var id: String { "\(name)|\(lon)|\(lat)" }
    public var geo: GeoPoint { GeoPoint(lon: lon, lat: lat) }
}

/*
 * ⚠️ **字段名必须照网关实际返回写**，不能凭印象。
 *    这一版第一稿就是凭印象写的（以为两者都返回 `places`、元素是
 *    `address` + `lon/lat`），结果真机上一调就全解不开 ——
 *    "查询失败"只是表象，实际是 JSONDecoder 抛错。
 *    `/tips` 与 `/around` 的元素字段**不一样**，所以这里分开定义。
 */

/// `/tips` 的元素：`{tips:[{name, district, adcode, lon, lat}]}`
private struct TipItem: Codable {
    let name: String
    let district: String?
    let adcode: String?
    let lon: Double
    let lat: Double
}
private struct TipsResponse: Codable { let tips: [TipItem] }

/// `/around` 的元素：`{pois:[{name, type, distanceM, lon, lat, address}]}`
private struct PoiItem: Codable {
    let name: String
    let type: String?
    let distanceM: Double?
    let lon: Double
    let lat: Double
    let address: String?
}
private struct AroundResponse: Codable { let pois: [PoiItem] }

public extension GatewayClient {
    /**
     输入提示：边打字边给候选（比"打完再查"体验好得多）。

     网关入参：`{keywords, city?, location?}`
     网关返回：`{tips:[{name, district, adcode, lon, lat}]}`

     - Returns: 候选列表（可能为空，表示没有匹配）。
     */
    static func fetchInputTips(gateway: String,
                               token: String,
                               keywords: String,
                               city: String = "",
                               near: GeoPoint? = nil,
                               timeoutSec: Double = 10) async throws -> [PlaceSuggestion] {
        var body: [String: Any] = ["keywords": keywords]
        if !city.isEmpty { body["city"] = city }
        if let n = near { body["location"] = ["lon": n.lon, "lat": n.lat] }

        let data = try await postJSON(path: "tips", gateway: gateway, token: token,
                                      body: body, timeoutSec: timeoutSec)
        let r: TipsResponse
        do { r = try JSONDecoder().decode(TipsResponse.self, from: data) }
        catch { throw GatewayError.decode("tips: \(error)") }
        return r.tips.map {
            PlaceSuggestion(name: $0.name,
                            address: $0.district ?? "",
                            lon: $0.lon, lat: $0.lat, distanceM: nil)
        }
    }

    /**
     周边搜索：按关键词在坐标附近找地点（如"加油站""停车场"）。

     ⚠️ 网关要求 `lon`/`lat` 在**顶层**（不是嵌套的 `location` 对象）——
     第一稿写成了 `location:{...}`，网关直接返回 `invalid_coordinates`。

     网关返回：`{pois:[{name, type, distanceM, lon, lat, address}]}`
     */
    static func fetchAround(gateway: String,
                            token: String,
                            keywords: String,
                            near: GeoPoint?,
                            radiusM: Double = 3000,
                            timeoutSec: Double = 12) async throws -> [PlaceSuggestion] {
        guard let n = near else {
            throw GatewayError.transport("还没有定位，无法周边搜索")
        }
        let body: [String: Any] = [
            "lon": n.lon, "lat": n.lat,
            "keywords": keywords, "radius": radiusM,
        ]
        let data = try await postJSON(path: "around", gateway: gateway, token: token,
                                      body: body, timeoutSec: timeoutSec)
        let r: AroundResponse
        do { r = try JSONDecoder().decode(AroundResponse.self, from: data) }
        catch { throw GatewayError.decode("around: \(error)") }
        return r.pois.map {
            PlaceSuggestion(name: $0.name,
                            address: $0.address ?? $0.type ?? "",
                            lon: $0.lon, lat: $0.lat, distanceM: $0.distanceM)
        }
    }
}

// MARK: - 逆地理编码 / 路况

/**
 逆地理编码结果（坐标 → 可读地址）。

 ⚠️ 网关返回的是**扁平对象**：`{formattedAddress, road, poiName, district,
 city, province}` —— 不是 `{result:{...}}`。第一稿按后者写，解不开。
 */
public struct ReverseGeocodeResult: Codable, Sendable {
    public let formattedAddress: String?
    public let road: String?
    public let poiName: String?
    public let district: String?
    public let city: String?
    public let province: String?

    /// 适合显示的一行文字：优先 POI 名，其次道路名，最后完整地址
    public var displayText: String {
        for s in [poiName, road, formattedAddress] {
            if let v = s, !v.isEmpty { return v }
        }
        return ""
    }
}

/**
 路况。

 ⚠️ 同样是**扁平对象**：`{status, statusCode, description, roads:[...]}`。
 车速在各路段里（`roads[].speedKmh`），顶层没有 —— 第一稿按
 `{traffic:{status, speedKmh}}` 写，解不开。
 */
public struct TrafficStatus: Codable, Sendable {
    public let status: String
    public let statusCode: String?
    public let description: String?
    public let roads: [Road]

    public struct Road: Codable, Sendable {
        public let name: String?
        public let status: String?
        public let speedKmh: Double?
        public let lengthM: Double?
    }

    /// 各路段平均车速（无有效数据时为 nil）
    public var averageSpeedKmh: Double? {
        let vals = roads.compactMap { $0.speedKmh }
        guard !vals.isEmpty else { return nil }
        return vals.reduce(0, +) / Double(vals.count)
    }
}

public extension GatewayClient {
    /**
     坐标 → 可读地址（WGS84 入参，网关内部转 GCJ-02）。
     网关返回**扁平对象**（见 `ReverseGeocodeResult`）。
     */
    static func fetchRegeo(gateway: String, token: String,
                           positionWGS84: GeoPoint,
                           timeoutSec: Double = 10) async throws -> ReverseGeocodeResult {
        let data = try await postJSON(path: "regeo", gateway: gateway, token: token,
                                      body: ["lon": positionWGS84.lon, "lat": positionWGS84.lat],
                                      timeoutSec: timeoutSec)
        do { return try JSONDecoder().decode(ReverseGeocodeResult.self, from: data) }
        catch { throw GatewayError.decode("regeo: \(error)") }
    }

    /// 查询某点周边的路况（网关返回扁平对象，见 `TrafficStatus`）
    static func fetchTraffic(gateway: String, token: String,
                             positionWGS84: GeoPoint,
                             timeoutSec: Double = 10) async throws -> TrafficStatus {
        let data = try await postJSON(path: "traffic", gateway: gateway, token: token,
                                      body: ["lon": positionWGS84.lon, "lat": positionWGS84.lat],
                                      timeoutSec: timeoutSec)
        do { return try JSONDecoder().decode(TrafficStatus.self, from: data) }
        catch { throw GatewayError.decode("traffic: \(error)") }
    }

    /// 共用的 POST → Data。**解析错误要抛出并带上下文**，
    /// 不能像第一稿那样静默返回 nil（那样只会表现为"查询失败"，查不到原因）。
    private static func postJSON(path: String, gateway: String, token: String,
                                 body: [String: Any], timeoutSec: Double) async throws -> Data {
        guard let url = URL(string: gateway.hasSuffix("/") ? gateway + path : gateway + "/" + path) else {
            throw GatewayError.badURL(gateway)
        }
        var req = URLRequest(url: url)
        req.httpMethod = "POST"
        req.setValue("application/json", forHTTPHeaderField: "content-type")
        req.setValue("Bearer \(token)", forHTTPHeaderField: "authorization")
        req.timeoutInterval = timeoutSec
        req.httpBody = try JSONSerialization.data(withJSONObject: body)
        let (data, resp) = try await URLSession.shared.data(for: req)
        let code = (resp as? HTTPURLResponse)?.statusCode ?? 0
        guard code == 200 else {
            let detail = String(data: data, encoding: .utf8) ?? ""
            throw GatewayError.transport("HTTP \(code)：\(detail.prefix(200))")
        }
        return data
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
