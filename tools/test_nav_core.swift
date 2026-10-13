/**
 * test_nav_core.swift —— 导航核心测试（macOS 本机运行，无需 iOS 运行时/真机）
 *
 * 运行：
 *   swiftc -O -o /tmp/test_nav_core \
 *     ios/CarDashboardPhone/CarDashboardPhone/Nav/NavCore.swift tools/test_nav_core.swift \
 *     && /tmp/test_nav_core
 *   （或 `cd tests && make test-nav-core`）
 *
 * ⚠️ 与 tools/test_ios_logic.swift 的**关键区别**：那个文件是"内联复制"被测逻辑，
 *    存在与实现漂移的风险。本文件**直接编译 App 的真实源文件**，
 *    因此测的就是将要跑在 iPhone 上的那份代码。
 */

import Foundation

// MARK: - 极简断言

var gPass = 0
var gFail = 0

func check(_ cond: Bool, _ msg: @autoclosure () -> String) {
    if cond {
        gPass += 1
    } else {
        gFail += 1
        print("  ✗ \(msg())")
    }
}

func approx(_ a: Double, _ b: Double, _ tol: Double) -> Bool { abs(a - b) <= tol }

// MARK: - 测试用几何
//
// 用正南正北的直线路线，便于手算期望值：
//   纬度每 0.01 度 ≈ 1111.95 米（地球半径 6371008.8）

let lon0 = 116.0

func pt(_ lat: Double, lon: Double? = nil) -> GeoPoint {
    GeoPoint(lon: lon ?? lon0, lat: lat)
}

/// 两段直线：A(39.00) → B(39.01) → C(39.02)，每段约 1111.95 m
func straightRoute() -> NavRoute {
    let a = pt(39.00)
    let b = pt(39.01)
    let c = pt(39.02)
    let segM = NavMath.distanceM(a, b) // ≈ 1111.95
    return NavRoute(steps: [
        NavStep(maneuver: .left, distanceM: segM, roadName: "第一段", polyline: [a, b]),
        NavStep(maneuver: .right, distanceM: segM, roadName: "第二段", polyline: [b, c]),
    ])
}

/// 构造一次定位（注意：名字刻意不叫 fix，以免与 handle(fix:) 的标签混淆）
func mkFix(_ p: GeoPoint, acc: Double = 5, t: Int64) -> NavFix {
    NavFix(position: p, horizontalAccuracyM: acc, courseDeg: 0, timestampMs: t)
}

// MARK: - 测试

func testRouteConstruction() {
    print("--- 路线构造 ---")
    let r = straightRoute()
    let segM = NavMath.distanceM(pt(39.00), pt(39.01))

    check(r.polyline.count == 3, "拼接后应有 3 个点（去掉重复首点），实际 \(r.polyline.count)")
    check(approx(r.totalDistanceM, segM * 2, 1), "总长应约 \(segM * 2)，实际 \(r.totalDistanceM)")
    check(r.cumulativeM.count == r.polyline.count, "累计距离应与点数一致")
    check(approx(r.cumulativeM[0], 0, 0.001), "首点累计为 0")
    check(approx(r.cumulativeM[1], segM, 1), "第二点累计约 \(segM)")

    check(r.stepRanges.count == 2, "应有 2 个 step 区间")
    check(approx(r.stepRanges[0].start, 0, 0.001), "step0 起点为 0")
    check(approx(r.stepRanges[0].end, segM, 1), "step0 终点约 \(segM)")
    check(approx(r.stepRanges[1].start, segM, 1), "step1 起点接续 step0")
    check(approx(r.stepRanges[1].end, r.totalDistanceM, 1), "step1 终点 = 总长（浮点兜底生效）")

    // 空路线与单点路线不应崩溃
    let empty = NavRoute(steps: [])
    check(empty.polyline.isEmpty && empty.totalDistanceM == 0, "空路线安全")
    check(empty.stepIndex(atProgressM: 0) == -1, "空路线 stepIndex = -1")

    let single = NavRoute(steps: [NavStep(maneuver: .straight, distanceM: 10, polyline: [pt(39.0)])])
    check(single.totalDistanceM == 0, "单点折线总长为 0")
}

func testProjection() {
    print("--- 投影 ---")
    let r = straightRoute()
    let segM = NavMath.distanceM(pt(39.00), pt(39.01))

    let core = NavCore()
    core.setRoute(r)

    // 路线上一点（39.005）→ 进度应约半段
    _ = core.handle(fix: mkFix(pt(39.005), t: 1000), nowMs: 1000)
    check(approx(core.snapshot.routeProgressM, segM / 2, 5),
          "39.005 处进度应约 \(segM / 2)，实际 \(core.snapshot.routeProgressM)")
    check(core.snapshot.crossTrackM < 1, "在路线上垂距应≈0，实际 \(core.snapshot.crossTrackM)")
    check(core.snapshot.stepIndex == 0, "应处于 step0，实际 \(core.snapshot.stepIndex)")
    check(core.snapshot.maneuver == .left, "动作应取当前 step 的 left")

    // 距下一转向：step0 终点在 segM，进度半段 → 约半段
    check(approx(Double(core.snapshot.distanceToManeuverM ?? -1), segM / 2, 5),
          "距下一转向应约 \(segM / 2)，实际 \(String(describing: core.snapshot.distanceToManeuverM))")
}

func testStepTransition() {
    print("--- 步骤切换 ---")
    let r = straightRoute()
    let segM = NavMath.distanceM(pt(39.00), pt(39.01))
    let core = NavCore()
    core.setRoute(r)

    _ = core.handle(fix: mkFix(pt(39.005), t: 1000), nowMs: 1000)
    check(core.snapshot.stepIndex == 0 && core.snapshot.maneuver == .left, "前半段：step0/left")

    _ = core.handle(fix: mkFix(pt(39.015), t: 2000), nowMs: 2000)
    check(core.snapshot.stepIndex == 1, "超过 39.01 后应进入 step1，实际 \(core.snapshot.stepIndex)")
    check(core.snapshot.maneuver == .right, "动作应切换为 right")
    check(approx(Double(core.snapshot.distanceToManeuverM ?? -1), segM * 0.5, 20),
          "距 step1 终点应约半段，实际 \(String(describing: core.snapshot.distanceToManeuverM))")
}

func testMonotonicProgress() {
    print("--- 进度单调性 ---")
    let r = straightRoute()
    let core = NavCore()
    core.setRoute(r)

    _ = core.handle(fix: mkFix(pt(39.015), t: 1000), nowMs: 1000)
    let p1 = core.snapshot.routeProgressM

    // GPS 抖动导致投影稍微回退 → 进度不应倒退
    _ = core.handle(fix: mkFix(pt(39.0149), t: 2000), nowMs: 2000)
    check(core.snapshot.routeProgressM >= p1 - 0.001,
          "轻微回退不应导致进度倒退（前 \(p1) 后 \(core.snapshot.routeProgressM)）")

    // 明显回退（超过 backtrackAllowance）→ 保持上次进度并交由偏航判定处理
    _ = core.handle(fix: mkFix(pt(39.001), t: 3000), nowMs: 3000)
    check(core.snapshot.routeProgressM >= p1 - 0.001,
          "大幅回退应被单调约束挡住（实际 \(core.snapshot.routeProgressM)）")
}

func testOffRouteConfirmations() {
    print("--- 偏航判定与确认次数 ---")
    let r = straightRoute()
    var cfg = NavConfig()
    cfg.offRouteConfirmations = 3
    cfg.offRouteThresholdM = 45
    cfg.onRouteThresholdM = 25
    let core = NavCore(config: cfg)
    core.setRoute(r)

    // 在路线东侧约 86 m（0.001 度经度 @39°）
    let off = pt(39.005, lon: lon0 + 0.001)
    let crossExpected = NavMath.distanceM(pt(39.005), off)
    check(crossExpected > cfg.offRouteThresholdM,
          "测试点垂距应超过阈值（实际 \(crossExpected)）")

    _ = core.handle(fix: mkFix(off, t: 1000), nowMs: 1000)
    check(core.snapshot.state == .active, "第 1 次偏航候选不应立刻判定（避免抖动误报）")
    _ = core.handle(fix: mkFix(off, t: 2000), nowMs: 2000)
    check(core.snapshot.state == .active, "第 2 次仍不判定")
    _ = core.handle(fix: mkFix(off, t: 3000), nowMs: 3000)
    check(core.snapshot.state == .rerouting, "第 3 次应确认为偏航（rerouting）")
    check(core.snapshot.offRoute, "offRoute 标志应为 true")
    check(core.snapshot.maneuver == .unknown, "偏航时不得给出转向指引")
    check(core.snapshot.distanceToManeuverM == nil, "偏航时距离应为 nil")
}

func testOffRouteRecoveryImmediate() {
    print("--- 偏航恢复（立即生效） ---")
    let r = straightRoute()
    var cfg = NavConfig()
    cfg.offRouteConfirmations = 2
    let core = NavCore(config: cfg)
    core.setRoute(r)

    let off = pt(39.005, lon: lon0 + 0.001)
    _ = core.handle(fix: mkFix(off, t: 1000), nowMs: 1000)
    _ = core.handle(fix: mkFix(off, t: 2000), nowMs: 2000)
    check(core.snapshot.offRoute, "应已判定偏航")

    // 回到路线 → 立即恢复（好消息不延迟）
    _ = core.handle(fix: mkFix(pt(39.006), t: 3000), nowMs: 3000)
    check(!core.snapshot.offRoute, "回到路线应立即恢复，不等确认次数")
    check(core.snapshot.state == .active, "状态应回到 active")
    check(core.snapshot.maneuver != .unknown, "恢复后应重新给出指引")
}

func testArrival() {
    print("--- 到达判定 ---")
    let r = straightRoute()
    var cfg = NavConfig()
    cfg.arrivalRadiusM = 20
    cfg.arrivalConfirmations = 2
    let core = NavCore(config: cfg)
    core.setRoute(r)

    // 接近终点（39.02 是终点）
    _ = core.handle(fix: mkFix(pt(39.0199), t: 1000), nowMs: 1000)
    check(core.snapshot.state == .active, "第 1 次到达候选不判定")
    _ = core.handle(fix: mkFix(pt(39.0199), t: 2000), nowMs: 2000)
    check(core.snapshot.state == .arrived, "第 2 次应判定到达")
    check(core.snapshot.arrived, "arrived 标志为 true")
    check(core.snapshot.maneuver == .arrive, "动作应为 arrive")
    check(core.snapshot.distanceToManeuverM == 0, "到达时距离为 0")

    // 到达是"锁存"的：不应因轻微移动又变回导航中
    _ = core.handle(fix: mkFix(pt(39.0180), t: 3000), nowMs: 3000)
    check(core.snapshot.state == .arrived, "到达状态应锁存")
}

func testStale() {
    print("--- 数据过期 ---")
    let r = straightRoute()
    var cfg = NavConfig()
    cfg.staleAfterMs = 5000
    let core = NavCore(config: cfg)
    core.setRoute(r)

    _ = core.handle(fix: mkFix(pt(39.005), t: 1000), nowMs: 1000)
    check(core.snapshot.state == .active, "正常导航中")

    // 无新定位，时间推进超过阈值
    _ = core.tick(nowMs: 3000)
    check(core.snapshot.state == .active, "未超时仍为 active")

    _ = core.tick(nowMs: 7000)
    check(core.snapshot.state == .stale, "超过阈值应变为 stale")
    check(core.snapshot.maneuver == .unknown, "过期时不得显示旧指引")
    check(core.snapshot.distanceToManeuverM == nil, "过期时距离为 nil")
}

func testAccuracyGate() {
    print("--- 精度门槛 ---")
    let r = straightRoute()
    var cfg = NavConfig()
    cfg.maximumUsableAccuracyM = 50
    let core = NavCore(config: cfg)
    core.setRoute(r)

    _ = core.handle(fix: mkFix(pt(39.005), t: 1000), nowMs: 1000)
    let good = core.snapshot.routeProgressM
    check(good > 0, "正常定位应更新进度")

    // 差定位（精度 200m）→ 不采纳，进度保持不变
    core.handle(fix: mkFix(pt(39.015), acc: 200, t: 2000), nowMs: 2000)
    check(core.snapshot.fixUsable == false, "差定位应标记 fixUsable=false")
    check(approx(core.snapshot.routeProgressM, good, 0.001),
          "差定位不得带偏进度（应保持 \(good)，实际 \(core.snapshot.routeProgressM)）")

    // 恢复良好定位 → 继续正常更新
    _ = core.handle(fix: mkFix(pt(39.012), t: 3000), nowMs: 3000)
    check(core.snapshot.fixUsable == true, "好定位应标记可用")
    check(core.snapshot.routeProgressM > good, "好定位应继续推进")
}

func testNoRouteAndClear() {
    print("--- 无路线 / 清除 ---")
    let core = NavCore()
    check(core.snapshot.state == .noData, "初始状态应为 noData")

    var snap = core.handle(fix: mkFix(pt(39.0), t: 1000), nowMs: 1000)
    check(snap.state == .noData, "无路线时处理定位应为 noData")

    core.setRoute(straightRoute())
    snap = core.handle(fix: mkFix(pt(39.005), t: 2000), nowMs: 2000)
    check(snap.state == .active, "设置路线后应为 active")

    core.clear()
    check(core.snapshot.state == .noData, "clear 后应回到 noData")
}

func testManeuverEnumMatchesProtocol() {
    print("--- 枚举与协议一致性 ---")
    // 这些数值必须与固件 simulator/ui/nav.h、gateway/src/maneuver.js 一致
    check(Maneuver.unknown.rawValue == 0, "unknown = 0")
    check(Maneuver.straight.rawValue == 1, "straight = 1")
    check(Maneuver.left.rawValue == 2, "left = 2")
    check(Maneuver.right.rawValue == 3, "right = 3")
    check(Maneuver.uTurn.rawValue == 4, "uTurn = 4")
    check(Maneuver.keepLeft.rawValue == 5, "keepLeft = 5")
    check(Maneuver.keepRight.rawValue == 6, "keepRight = 6")
    check(Maneuver.arrive.rawValue == 7, "arrive = 7")

    check(NavState.idle.rawValue == 0, "idle = 0")
    check(NavState.active.rawValue == 1, "active = 1")
    check(NavState.rerouting.rawValue == 2, "rerouting = 2")
    check(NavState.arrived.rawValue == 3, "arrived = 3")
    check(NavState.stale.rawValue == 4, "stale = 4")
    check(NavState.noData.rawValue == 5, "noData = 5")
}

func testStepRangeScaling() {
    print("--- step 距离与几何长度不一致时的处理 ---")
    // 高德上报 2000m，但几何只有约 1111.95m（差异 80%，超过容差 5%）
    let a = pt(39.00)
    let b = pt(39.01)
    let r = NavRoute(steps: [
        NavStep(maneuver: .left, distanceM: 2000, polyline: [a, b]),
    ])
    // 应改用几何长度，避免"上报长度与折线进度不自洽"
    check(approx(r.totalDistanceM, NavMath.distanceM(a, b), 1),
          "总长应以几何长度为准，实际 \(r.totalDistanceM)")
    check(approx(r.stepRanges[0].end, r.totalDistanceM, 0.001),
          "step 区间须与几何总长对齐")

    // 差异在容差内时，应优先采用上报长度（更贴近手机导航显示）
    let geom = NavMath.distanceM(a, b)
    let r2 = NavRoute(steps: [
        NavStep(maneuver: .left, distanceM: geom * 1.02, polyline: [a, b]),
    ])
    check(approx(r2.stepRanges[0].end, r2.totalDistanceM, 0.001),
          "容差内也应与几何总长对齐（浮点兜底）")
}

func testPointAt() {
    print("--- 按距离取点（模拟行车用） ---")
    let r = straightRoute()               // A(39.00) → B(39.01) → C(39.02)
    let segM = NavMath.distanceM(pt(39.00), pt(39.01))

    // 起点 / 中点 / 终点
    if let p0 = r.pointAt(distanceM: 0) {
        check(approx(p0.lat, 39.00, 1e-6), "起点纬度应为 39.00，实际 \(p0.lat)")
    } else { check(false, "起点不应为 nil") }

    if let pm = r.pointAt(distanceM: segM / 2) {
        check(approx(pm.lat, 39.005, 1e-4), "半段处纬度应约 39.005，实际 \(pm.lat)")
    } else { check(false, "中点不应为 nil") }

    if let pe = r.pointAt(distanceM: r.totalDistanceM) {
        check(approx(pe.lat, 39.02, 1e-6), "终点纬度应为 39.02，实际 \(pe.lat)")
    } else { check(false, "终点不应为 nil") }

    // 越界钳制
    if let pn = r.pointAt(distanceM: -100) {
        check(approx(pn.lat, 39.00, 1e-6), "负距离应钳制到起点")
    }
    if let pf = r.pointAt(distanceM: r.totalDistanceM + 5000) {
        check(approx(pf.lat, 39.02, 1e-6), "超界应钳制到终点")
    }

    // 取出的点再投影回去，进度应基本一致（往返自洽）
    let core = NavCore()
    core.setRoute(r)
    let d: Double = segM * 0.75
    if let p = r.pointAt(distanceM: d) {
        _ = core.handle(fix: mkFix(p, t: 1000), nowMs: 1000)
        check(approx(core.snapshot.routeProgressM, d, 3),
              "取点再投影应回到同一进度（期望 \(d)，实际 \(core.snapshot.routeProgressM)）")
        check(core.snapshot.crossTrackM < 1, "取出的点应在路线上（垂距≈0）")
    }

    // 退化情形
    let empty = NavRoute(steps: [])
    check(empty.pointAt(distanceM: 0) == nil, "空路线返回 nil")
    let single = NavRoute(steps: [NavStep(maneuver: .straight, distanceM: 0, polyline: [pt(39.0)])])
    check(single.pointAt(distanceM: 0)?.lat == 39.0, "单点路线返回该点")
}

func testCrossLanguageCoords() {
    print("--- 坐标转换：与 JS 实现交叉验证 ---")

    // 向量由 gateway/test/gen-coord-vectors.js 生成（JS 实现）。
    // 本测试用 Swift 实现重算同一批点并逐点比对 —— 这是防止两份实现漂移的
    // 唯一自动化手段（语言不同，无法共享代码）。
    // 可用环境变量 COORD_VECTORS 指定路径；否则用仓库内的默认位置。
    let path = ProcessInfo.processInfo.environment["COORD_VECTORS"]
        ?? "gateway/test/fixtures/coord-vectors.json"

    guard let data = FileManager.default.contents(atPath: path) else {
        // 找不到就明确跳过（不误报失败），并提示如何生成
        print("  ⚠️ 跳过：未找到 \(path)")
        print("     生成：node gateway/test/gen-coord-vectors.js > gateway/test/fixtures/coord-vectors.json")
        check(true, "跳过（文件缺失）")
        return
    }

    guard let root = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
          let vectors = root["vectors"] as? [[String: Any]], !vectors.isEmpty else {
        check(false, "向量文件格式不符：\(path)")
        return
    }

    func point(_ obj: Any?) -> GeoPoint? {
        guard let d = obj as? [String: Any],
              let lon = d["lon"] as? Double, let lat = d["lat"] as? Double else { return nil }
        return GeoPoint(lon: lon, lat: lat)
    }

    let tol = 1e-9 // 两份实现运算顺序一致，差异应远小于此
    var nChina = 0, nAbroad = 0

    for v in vectors {
        let note = (v["note"] as? String) ?? "?"
        guard let w = point(v["wgs84"]),
              let expectGcj = point(v["gcj02"]),
              let expectBack = point(v["roundTrip"]) else {
            check(false, "向量缺少字段：\(note)")
            continue
        }

        let gotGcj = Coords.wgs84ToGcj02(w)
        check(approx(gotGcj.lon, expectGcj.lon, tol) && approx(gotGcj.lat, expectGcj.lat, tol),
              "\(note) WGS84→GCJ02 不一致：Swift(\(gotGcj.lon),\(gotGcj.lat)) vs JS(\(expectGcj.lon),\(expectGcj.lat))")

        let gotBack = Coords.gcj02ToWgs84(expectGcj)
        check(approx(gotBack.lon, expectBack.lon, 1e-7) && approx(gotBack.lat, expectBack.lat, 1e-7),
              "\(note) GCJ02→WGS84 不一致：Swift(\(gotBack.lon),\(gotBack.lat)) vs JS(\(expectBack.lon),\(expectBack.lat))")

        if Coords.outOfChina(lon: w.lon, lat: w.lat) { nAbroad += 1 } else { nChina += 1 }
    }

    // 向量本身应同时覆盖境内与境外，否则测试覆盖无意义
    check(nChina >= 5, "向量应覆盖境内点（实际 \(nChina)）")
    check(nAbroad >= 3, "向量应覆盖境外点（实际 \(nAbroad)）")
    print("     比对 \(vectors.count) 个点（境内 \(nChina) / 境外 \(nAbroad)）")
}

// MARK: - 入口

@main
struct TestNavCore {
    static func main() {
        print("=== 导航核心（NavCore）===")
        testRouteConstruction()
        testProjection()
        testStepTransition()
        testMonotonicProgress()
        testOffRouteConfirmations()
        testOffRouteRecoveryImmediate()
        testArrival()
        testStale()
        testAccuracyGate()
        testNoRouteAndClear()
        testManeuverEnumMatchesProtocol()
        testStepRangeScaling()
        testPointAt()
        testCrossLanguageCoords()

        print("结果: \(gPass) 通过, \(gFail) 失败")
        exit(gFail == 0 ? 0 : 1)
    }
}
