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

    /**
     * 路线折线在设备侧的点数上限（与固件 `NAV_MAP_MAX_ROUTE_PTS` 一致）。
     *
     * 2000m 前视距离 ÷ 96 点 ≈ 21m 一点：在 0.78 px/m 的缩放下约 16px 一段，
     * 足够平滑，同时保证白线一定超出屏幕边缘。
     */
    public static let maxRoutePoints = 96
    /// 单条消息的点数上限（与固件 `NAV_MAP_PTS_PER_FEATURE` 一致）
    public static let maxPointsPerFeature = 16

    /**
     折线的点会被切成若干条消息，每条**实际字节数**不超过 `maxMessageBytes`。
     返回点的下标区间 `[start, end]`（含），相邻区间**重叠一个点**，
     这样拆开的折线在设备上是连续的（不会出现一格空隙）。
     */
    static func pointRanges(prefix: String, pts: [(x: Int, y: Int)]) -> [(start: Int, end: Int)] {
        guard pts.count >= 2 else { return [] }
        var out: [(start: Int, end: Int)] = []
        var start = 0
        while start < pts.count - 1 {
            var end = start + 1
            /*
             * ⚠️ 必须用**真实坐标**来量长度。
             * 曾经用下标当坐标去试探（"[i,i]"），实际坐标是大数字（如 [-800,-500]），
             * 于是切成 194~276 字节的包 —— 超出 BLE 单包上限、被静默丢弃。
             * 教训：判断"放不放得下"只能按**最终要发出去的那个字符串**来算。
             */
            while end + 1 < pts.count {
                let probe = prefix + pointsJSON(Array(start...(end + 1)).map { pts[$0] }) + "}"
                if probe.utf8.count > maxMessageBytes { break }
                end += 1
            }
            out.append((start, end))
            if end >= pts.count - 1 { break }
            start = end /* 重叠一个点，保证连续 */
        }
        return out
    }

    /// 按点区间生成一条消息
    static func message(prefix: String, pts: [(x: Int, y: Int)], start: Int, end: Int) -> String {
        prefix + pointsJSON(Array(start...end).map { pts[$0] }) + "}"
    }

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

        /*
         * ⚠️ 每条消息都必须**放在一个 BLE 包里**（约 185 字节）。
         *
         * 曾经踩过的坑：把路线点数从 64 提到 96 时忘了这一点 —— 一条 96 点的
         * `mp` 消息达到 **892 字节**（超限 4.8 倍），BLE 栈直接丢弃，
         * 表现是"底图有、白线没有"。所以这里改成按**实际序列化字节数**切分，
         * 而不是按固定点数上限。
         */
        for r in bundle.roads {
            let pts = r.p.compactMap { a -> (x: Int, y: Int)? in
                a.count >= 2 ? (a[0], a[1]) : nil
            }
            guard pts.count >= 2 else { continue }
            let capped = decimate(pts, to: maxPointsPerFeature)
            let prefix = "{\"t\":\"mr\",\"c\":\(r.c),\"p\":"
            for rg in pointRanges(prefix: prefix, pts: capped) {
                out.append(message(prefix: prefix, pts: capped, start: rg.start, end: rg.end))
            }
        }

        for b in bundle.buildings {
            let pts = b.p.compactMap { a -> (x: Int, y: Int)? in
                a.count >= 2 ? (a[0], a[1]) : nil
            }
            guard pts.count >= 3 else { continue } /* 多边形至少三点 */
            /*
             * 建筑是**闭合轮廓**，不能像折线那样在中间切开（会破坏形状）。
             * 它本身点数就少（网关已限到 16），若仍超限就进一步降采样。
             */
            var capped = decimate(pts, to: maxPointsPerFeature)
            let prefix = "{\"t\":\"mb\",\"p\":"
            while capped.count > 3,
                  prefix.utf8.count + pointsJSON(capped).utf8.count + 1 > maxMessageBytes {
                capped = decimate(capped, to: max(3, capped.count - 2))
            }
            out.append(prefix + pointsJSON(capped) + "}")
        }

        /* 路线：投影到窗口原点 → 降采样 → 按字节切段（设备端会把多段首尾相接）*/
        if routeGCJ.count >= 2 {
            let local = routeGCJ.map { LocalProjection.toMeters($0, origin: bundle.centerGcj) }
            let sliced = decimate(local, to: maxRoutePoints)
            let prefix = "{\"t\":\"mp\",\"p\":"
            for rg in pointRanges(prefix: prefix, pts: sliced) {
                out.append(message(prefix: prefix, pts: sliced, start: rg.start, end: rg.end))
            }
        }

        out.append("{\"t\":\"me\"}")
        return out
    }
}
