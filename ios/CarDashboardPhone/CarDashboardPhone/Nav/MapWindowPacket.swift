/**
 * MapWindowPacket.swift —— 周边地图窗口：解码、坐标投影、BLE 消息编码
 *
 * ## 数据流
 *
 * ```
 * 网关 /mapwindow ──▶ MapWindowBundle ──▶ BLE 文本消息 ──▶ 圆屏
 *   （GCJ-02 局部米）      （本文件）        mw/mr/mb/mp/me
 * ```
 *
 * ## 坐标系（**最容易错的地方**）
 *
 * 网关把 OSM（WGS84）与高德路线（GCJ-02）**统一到 GCJ-02** 后投影成
 * **以窗口中心为原点的局部米坐标**。实测北京两点坐标差约 **556 米** ——
 * 若不统一，整张图会整体偏移一个窗口的距离。
 *
 * 因此本文件的所有投影都相对 `bundle.centerGcj`，而**不是** `center`。
 *
 * ## 为什么不做分片
 *
 * 一条道路 / 一栋建筑 / 一段路线各发一条独立消息，每条都远小于 BLE 单包
 * （约 185 字节）。这样复用既有的"文本消息 + 攒到 `}` 就解析"通路，
 * 固件无需任何重组逻辑。见 docs/ble.md。
 */

import Foundation

// MARK: - 网关返回的窗口

/// 网关 `/mapwindow` 的响应。
public struct MapWindowBundle: Codable, Sendable {
    /// 请求时用的 WGS84 中心（原样回显，仅用于排查）
    public let center: GeoPoint
    /// **局部坐标原点**（GCJ-02）。自车偏移必须相对它计算。
    public let centerGcj: GeoPoint
    /// 几何所用坐标系（应为 "GCJ-02"）
    public let crs: String
    public let radiusM: Double
    public let roads: [Road]
    public let buildings: [Building]
    /// 署名（OSM ODbL）—— 必须展示，见 decisions.md D-010
    public let attribution: String
    /// 是否因预算被裁剪（仅诊断用）
    public let truncated: Truncated?

    public struct Road: Codable, Sendable {
        /// 道路等级 0..5，越小越主要（与固件 `nav_map.h` 一致）
        public let c: Int
        /// 点序列，局部米 `[x, y]`
        public let p: [[Int]]
    }

    public struct Building: Codable, Sendable {
        public let p: [[Int]]
    }

    public struct Truncated: Codable, Sendable {
        public let roads: Bool
        public let buildings: Bool
    }
}

// MARK: - 局部坐标投影

/// 与网关 `mapwindow.js` 的 `toLocalMeters` **必须一致**。
///
/// 等距圆柱近似：500m 量级误差 < 0.1m。两端若用了不同公式，
/// 路线与底图会错位 —— 这是"两条数据在同一张图上"的隐含契约。
public enum LocalProjection {
    /// 每度纬度对应的米数（与网关同值）
    public static let metersPerDegLat = 111320.0

    /// 经纬度 → 以 `origin` 为原点的局部米坐标（四舍五入到米）
    public static func toMeters(_ p: GeoPoint, origin: GeoPoint) -> (x: Int, y: Int) {
        let mPerDegLon = metersPerDegLat * cos(origin.lat * .pi / 180)
        let x = (p.lon - origin.lon) * mPerDegLon
        let y = (p.lat - origin.lat) * metersPerDegLat
        return (Int(x.rounded()), Int(y.rounded()))
    }
}

// MARK: - BLE 消息编码

public enum MapWindowCodec {
    /// 单条消息的字节上限。iOS 实测 `maximumWriteValueLength` 约 185 字节，
    /// 这里留出余量；网关已把单条要素限制在 16 点（约 120 字节）。
    public static let maxMessageBytes = 180

    /// 路线折线在设备侧的点数上限（与固件 `NAV_MAP_MAX_ROUTE_PTS` 一致）
    public static let maxRoutePoints = 64
    /// 单条消息的点数上限（与固件 `NAV_MAP_PTS_PER_FEATURE` 一致）
    public static let maxPointsPerFeature = 16

    /// 把点序列序列化成 `[[x,y],[x,y]]`
    static func pointsJSON(_ pts: [(x: Int, y: Int)]) -> String {
        var s = "["
        for (i, p) in pts.enumerated() {
            if i > 0 { s += "," }
            s += "[\(p.x),\(p.y)]"
        }
        return s + "]"
    }

    /// 均匀降采样到 `maxN` 点（保留首尾）—— 与网关的 `decimate` 同一取舍：
    /// 宁可形状粗一点，也不能因为超长而丢掉整条要素。
    static func decimate(_ pts: [(x: Int, y: Int)], to maxN: Int) -> [(x: Int, y: Int)] {
        guard pts.count > maxN, maxN >= 2 else { return pts }
        var out: [(x: Int, y: Int)] = [pts[0]]
        let step = Double(pts.count - 1) / Double(maxN - 1)
        for k in 1..<(maxN - 1) {
            out.append(pts[Int((Double(k) * step).rounded())])
        }
        out.append(pts[pts.count - 1])
        return out
    }

    /**
     把窗口 + 路线编码成按顺序下发的 BLE 文本消息。

     - Parameters:
       - bundle: 网关返回的窗口（局部米坐标）
       - routeGCJ: **GCJ-02** 的路线折线（来自导航核心）。函数内部投影到窗口原点。
     - Returns: 依次应下发的消息；`mw` 开头、`me` 结尾。
     */
    public static func messages(bundle: MapWindowBundle, routeGCJ: [GeoPoint]) -> [String] {
        var out: [String] = []
        out.reserveCapacity(2 + bundle.roads.count + bundle.buildings.count + 2)
        out.append("{\"t\":\"mw\"}")

        for r in bundle.roads {
            var pts = r.p.compactMap { a -> (x: Int, y: Int)? in
                a.count >= 2 ? (a[0], a[1]) : nil
            }
            guard pts.count >= 2 else { continue }
            pts = decimate(pts, to: maxPointsPerFeature)
            let msg = "{\"t\":\"mr\",\"c\":\(r.c),\"p\":\(pointsJSON(pts))}"
            out.append(msg)
        }

        for b in bundle.buildings {
            var pts = b.p.compactMap { a -> (x: Int, y: Int)? in
                a.count >= 2 ? (a[0], a[1]) : nil
            }
            guard pts.count >= 3 else { continue } /* 多边形至少三点 */
            pts = decimate(pts, to: maxPointsPerFeature)
            out.append("{\"t\":\"mb\",\"p\":\(pointsJSON(pts))}")
        }

        /* 路线：投影到窗口原点，再按预算降采样 */
        if routeGCJ.count >= 2 {
            let local = routeGCJ.map { LocalProjection.toMeters($0, origin: bundle.centerGcj) }
            let sliced = decimate(local, to: maxRoutePoints)
            out.append("{\"t\":\"mp\",\"p\":\(pointsJSON(sliced))}")
        }

        out.append("{\"t\":\"me\"}")
        return out
    }
}
