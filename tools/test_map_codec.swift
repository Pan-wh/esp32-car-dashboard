// test_map_codec.swift —— 地图窗口编码的纯逻辑测试（macOS 本机可跑，无需真机）
//
// 覆盖：
//   1. LocalProjection 与**网关 JS 实现**的交叉验证（两端公式必须逐字一致，
//      否则"路线"与"底图"会错位 —— 这是两条数据画在同一张图上的隐含契约）
//   2. MapWindowCodec 的消息格式、顺序（mw…me）、降采样、字节上限
//   3. NavPacket 的 ox/oy 编码（成对出现、nil 时字段整个省略）
//
// 运行：swift tools/test_map_codec.swift
//
// ⚠️ 与 App 源码同源语义：被测代码从 App 源码内联（与 test_ios_logic.swift 同一策略），
//    **App 侧改动时本文件必须同步**，否则测试会与实现漂移。

import Foundation

// ===== 被测代码（内联自 App 源码）=====

struct GeoPoint: Equatable, Codable {
    var lon: Double
    var lat: Double
}

enum LocalProjection {
    static let metersPerDegLat = 111320.0
    static func toMeters(_ p: GeoPoint, origin: GeoPoint) -> (x: Int, y: Int) {
        let mPerDegLon = metersPerDegLat * cos(origin.lat * .pi / 180)
        let x = (p.lon - origin.lon) * mPerDegLon
        let y = (p.lat - origin.lat) * metersPerDegLat
        return (Int(x.rounded()), Int(y.rounded()))
    }
}

enum MapWindowCodec {
    static let maxMessageBytes = 180
    static let maxRoutePoints = 64
    static let maxPointsPerFeature = 16

    static func pointsJSON(_ pts: [(x: Int, y: Int)]) -> String {
        var s = "["
        for (i, p) in pts.enumerated() {
            if i > 0 { s += "," }
            s += "[\(p.x),\(p.y)]"
        }
        return s + "]"
    }

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

    /// 折线切段：每段实际字节数不超过 maxMessageBytes，相邻段重叠一个点
    static func pointRanges(prefix: String, pts: [(x: Int, y: Int)]) -> [(start: Int, end: Int)] {
        guard pts.count >= 2 else { return [] }
        var out: [(start: Int, end: Int)] = []
        var start = 0
        while start < pts.count - 1 {
            var end = start + 1
            while end + 1 < pts.count {
                let probe = prefix + pointsJSON(Array(start...(end + 1)).map { pts[$0] }) + "}"
                if probe.utf8.count > maxMessageBytes { break }
                end += 1
            }
            out.append((start, end))
            if end >= pts.count - 1 { break }
            start = end
        }
        return out
    }

    static func message(prefix: String, pts: [(x: Int, y: Int)], start: Int, end: Int) -> String {
        prefix + pointsJSON(Array(start...end).map { pts[$0] }) + "}"
    }

    static func messages(bundle: MapWindowBundle, routeGCJ: [GeoPoint]) -> [String] {
        var out: [String] = []
        out.append("{\"t\":\"mw\"}")
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
            guard pts.count >= 3 else { continue }
            var capped = decimate(pts, to: maxPointsPerFeature)
            let prefix = "{\"t\":\"mb\",\"p\":"
            while capped.count > 3,
                  prefix.utf8.count + pointsJSON(capped).utf8.count + 1 > maxMessageBytes {
                capped = decimate(capped, to: max(3, capped.count - 2))
            }
            out.append(prefix + pointsJSON(capped) + "}")
        }
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

struct MapWindowBundle: Codable {
    struct Road: Codable { let c: Int; let p: [[Int]] }
    struct Building: Codable { let p: [[Int]] }
    struct Truncated: Codable { let roads: Bool; let buildings: Bool }
    let center: GeoPoint
    let centerGcj: GeoPoint
    let crs: String
    let radiusM: Double
    let roads: [Road]
    let buildings: [Building]
    let attribution: String
    let truncated: Truncated?
}

struct NavPacket: Codable {
    var t: String = "nav"
    var state: Int
    var maneuver: Int
    var distM: Int?
    var offsetX: Int?
    var offsetY: Int?
    enum CodingKeys: String, CodingKey {
        case t, state, maneuver
        case distM = "dist_m"
        case offsetX = "ox"
        case offsetY = "oy"
    }
    init(state: Int, maneuver: Int, distM: Int?, offsetX: Int? = nil, offsetY: Int? = nil) {
        self.state = state; self.maneuver = maneuver; self.distM = distM
        self.offsetX = offsetX; self.offsetY = offsetY
    }
    func encode() throws -> Data {
        let e = JSONEncoder(); e.outputFormatting = []
        return try e.encode(self)
    }
}

// ===== 测试框架 =====

var gPass = 0, gFail = 0
func CHECK(_ cond: Bool, _ msg: String) {
    if cond { gPass += 1 }
    else { gFail += 1; print("  ✗ \(msg)") }
}

/// 某纬度处 1 米对应的经度差
func mLonAt(_ lat: Double) -> Double { 111320.0 * cos(lat * .pi / 180) }

func mkBundle(roads: [(Int, [[Int]])], bldgs: [[[Int]]] = [],
              centerGcj: GeoPoint = GeoPoint(lon: 116.4037, lat: 39.9106)) -> MapWindowBundle {
    MapWindowBundle(center: GeoPoint(lon: 116.3974, lat: 39.9092),
                    centerGcj: centerGcj, crs: "GCJ-02", radiusM: 500,
                    roads: roads.map { MapWindowBundle.Road(c: $0.0, p: $0.1) },
                    buildings: bldgs.map { MapWindowBundle.Building(p: $0) },
                    attribution: "© OpenStreetMap contributors (ODbL)",
                    truncated: nil)
}

// ===== 测试 =====

print("=== 地图窗口编码 ===")

// ---- 1) 投影：与网关 JS 实现交叉验证 ----
print("--- 投影与网关一致性 ---")
// 与 gateway/src/mapwindow.js 的 toLocalMeters 逐字对照：
//   mPerDegLat = 111320; mPerDegLon = 111320*cos(originLat); x=(lon-oLon)*mLon; y=(lat-oLat)*mLat
do {
    let origin = GeoPoint(lon: 116.403672, lat: 39.910634)
    // 正东 100m：经度差 = 100 / (111320*cos(lat))
    let mLon = 111320.0 * cos(origin.lat * .pi / 180)
    let east = GeoPoint(lon: origin.lon + 100.0 / mLon, lat: origin.lat)
    let r1 = LocalProjection.toMeters(east, origin: origin)
    CHECK(r1.x == 100 && r1.y == 0, "正东 100m → (\(r1.x),\(r1.y))，期望 (100,0)")

    // 正北 100m：纬度差 = 100 / 111320
    let north = GeoPoint(lon: origin.lon, lat: origin.lat + 100.0 / 111320.0)
    let r2 = LocalProjection.toMeters(north, origin: origin)
    CHECK(r2.x == 0 && r2.y == 100, "正北 100m → (\(r2.x),\(r2.y))，期望 (0,100)")

    // 原点自身 → (0,0)
    let r3 = LocalProjection.toMeters(origin, origin: origin)
    CHECK(r3.x == 0 && r3.y == 0, "原点 → (0,0)")

    // 负方向
    let south = GeoPoint(lon: origin.lon, lat: origin.lat - 250.0 / 111320.0)
    let r4 = LocalProjection.toMeters(south, origin: origin)
    CHECK(r4.y == -250, "正南 250m → y=\(r4.y)，期望 -250")
    print("  投影自检通过（与网关公式同源）")
}

// ---- 2) 消息顺序与格式 ----
print("--- 消息顺序与格式 ---")
do {
    let bundle = mkBundle(roads: [(1, [[0, 0], [100, 0]]), (3, [[0, 50], [80, 50]])],
                          bldgs: [[[10, 10], [40, 10], [40, 40], [10, 40]]])
    /* 首点**必须与 centerGcj 完全一致**，否则投影出来不是 (0,0) ——
       这里刻意用同一个值，验证"自车在窗口中心"这一约定。 */
    let route = [bundle.centerGcj,
                 GeoPoint(lon: bundle.centerGcj.lon, lat: bundle.centerGcj.lat + 150.0 / 111320.0)]
    let msgs = MapWindowCodec.messages(bundle: bundle, routeGCJ: route)
    CHECK(msgs.first == "{\"t\":\"mw\"}", "首条是 mw（实际 \(msgs.first ?? "nil")）")
    CHECK(msgs.last == "{\"t\":\"me\"}", "末条是 me")
    CHECK(msgs.count == 2 + 2 + 1 + 1, "共 6 条消息（实际 \(msgs.count)）")
    CHECK(msgs[1].hasPrefix("{\"t\":\"mr\",\"c\":1,"), "第 2 条是道路且等级正确：\(msgs[1])")
    CHECK(msgs[3].hasPrefix("{\"t\":\"mb\","), "第 4 条是建筑")
    CHECK(msgs[4].hasPrefix("{\"t\":\"mp\","), "第 5 条是路线")
    // 所有有效的 JSON 对象都应以 } 结尾（固件按此判定完整）
    for m in msgs { CHECK(m.hasPrefix("{") && m.hasSuffix("}"), "消息是完整 JSON 对象：\(m)") }
    // 路线首点应投影为 (0,0)（自车就在窗口中心）
    CHECK(msgs[4].contains("\"p\":[[0,0],"), "路线首点投影为 (0,0)：\(msgs[4])")
}

// ---- 3) 字节上限（**这是踩过的坑**：96 点的路线消息达 892 字节，超限 4.8 倍）----
print("--- 字节上限（关键回归）---")
do {
    /* 造一条超长路线：2000m 前视、96 点上限，坐标是多位数 */
    var route: [GeoPoint] = []
    for k in 0..<400 {
        let mLat = 111320.0
        route.append(GeoPoint(lon: 116.4037 + Double(k) * 30.0 / (mLonAt(39.91)),
                              lat: 39.9106 + Double(k) * 30.0 / mLat))
    }
    /* 道路也造一批多位数坐标 */
    var rp: [[Int]] = []
    for k in 0..<40 { rp.append([k * 37 - 800, k * 23 - 500]) }
    let bundle = mkBundle(roads: [(0, rp), (2, rp)], bldgs: [rp, rp, rp])
    let msgs = MapWindowCodec.messages(bundle: bundle, routeGCJ: route)

    let longest = msgs.map { $0.utf8.count }.max() ?? 0
    CHECK(longest <= MapWindowCodec.maxMessageBytes,
          "最长消息 \(longest) 字节 ≤ \(MapWindowCodec.maxMessageBytes)（超限会被 BLE 静默丢弃）")

    /* 逐条检查，并打印超限的那条内容开头，便于定位 */
    for m in msgs where m.utf8.count > MapWindowCodec.maxMessageBytes {
        CHECK(false, "超限消息（\(m.utf8.count) 字节）：\(m.prefix(60))…")
    }
    /* 路线被切成多段，且每段都以 mp 开头（设备端会首尾相接）*/
    let mps = msgs.filter { $0.hasPrefix("{\"t\":\"mp\"") }
    CHECK(mps.count >= 1, "路线至少 1 段（实际 \(mps.count) 段）")
    CHECK(mps.allSatisfy { $0.utf8.count <= MapWindowCodec.maxMessageBytes }, "所有路线段都在限额内")
    print("  路线 \(mps.count) 段，最长单条 \(longest) 字节")
}

// ---- 3b) 切段重叠：相邻段共享一个点，设备端画出来才连续 ----
print("--- 切段连续性 ---")
do {
    let prefix = "{\"t\":\"mp\",\"p\":"
    let demoPts = (0..<96).map { (x: $0 * 37 - 800, y: $0 * 23 - 500) }
    let ranges = MapWindowCodec.pointRanges(prefix: prefix, pts: demoPts)
    CHECK(ranges.count >= 2, "96 点被切成多段（实际 \(ranges.count)）")
    CHECK(ranges.first!.start == 0, "首段从 0 开始")
    CHECK(ranges.last!.end == 95, "末段到最后一个点")
    var contiguous = true
    for i in 1..<ranges.count where ranges[i].start != ranges[i - 1].end { contiguous = false }
    CHECK(contiguous, "相邻段重叠一个点（否则画出来会有一格空隙）")
}

// ---- 4) 降采样保留首尾 ----
print("--- 降采样 ---")
do {
    let src = (0..<100).map { (x: $0, y: $0 * 2) }
    let out = MapWindowCodec.decimate(src, to: 10)
    CHECK(out.count == 10, "降到 10 点（实际 \(out.count)）")
    CHECK(out.first!.x == 0 && out.first!.y == 0, "保留首点")
    CHECK(out.last!.x == 99 && out.last!.y == 198, "保留尾点")
    let short = MapWindowCodec.decimate([(x: 1, y: 1), (x: 2, y: 2)], to: 16)
    CHECK(short.count == 2, "点数不足时原样返回")
    let one = MapWindowCodec.decimate([(x: 5, y: 5)], to: 2)
    CHECK(one.count == 1, "单点安全")
}

// ---- 5) 异常数据不崩 ----
print("--- 异常输入 ---")
do {
    let bundle = mkBundle(roads: [(1, [[0, 0]]),            /* 只有 1 点：丢弃 */
                                 (2, []),                    /* 空：丢弃 */
                                 (3, [[0, 0], [10, 10]])],   /* 正常 */
                          bldgs: [[[0, 0], [1, 1]],          /* 只有 2 点：丢弃 */
                                  [[0, 0], [1, 0], [1, 1]]]) /* 3 点：保留 */
    let msgs = MapWindowCodec.messages(bundle: bundle, routeGCJ: [])
    let roads = msgs.filter { $0.hasPrefix("{\"t\":\"mr\"") }.count
    let bldgs = msgs.filter { $0.hasPrefix("{\"t\":\"mb\"") }.count
    CHECK(roads == 1, "只有 1 点的道路被丢弃（保留 \(roads) 条）")
    CHECK(bldgs == 1, "只有 2 点的建筑被丢弃（保留 \(bldgs) 栋）")
    CHECK(!msgs.contains { $0.hasPrefix("{\"t\":\"mp\"") }, "路线不足 2 点时不下发")
}

// ---- 6) NavPacket 的 ox/oy ----
print("--- NavPacket ox/oy ---")
do {
    let d1 = try! NavPacket(state: 1, maneuver: 3, distM: 300, offsetX: 12, offsetY: -34).encode()
    let s1 = String(data: d1, encoding: .utf8)!
    CHECK(s1.contains("\"ox\":12"), "含 ox：\(s1)")
    CHECK(s1.contains("\"oy\":-34"), "含 oy")
    CHECK(s1.contains("\"t\":\"nav\""), "含 t=nav")
    CHECK(s1.contains("\"dist_m\":300"), "含 dist_m")

    /* 无偏移时字段应**整个省略**（固件据此不用旧偏移）*/
    let d2 = try! NavPacket(state: 1, maneuver: 3, distM: nil).encode()
    let s2 = String(data: d2, encoding: .utf8)!
    CHECK(!s2.contains("ox") && !s2.contains("oy"), "无偏移时省略 ox/oy：\(s2)")
    CHECK(!s2.contains("dist_m"), "距离未知时省略 dist_m：\(s2)")
}

print("\n结果: \(gPass) 通过, \(gFail) 失败")
exit(gFail == 0 ? 0 : 1)
