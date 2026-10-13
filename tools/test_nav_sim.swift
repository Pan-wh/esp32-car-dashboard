/**
 * test_nav_sim.swift —— Mac 模拟器的纯逻辑测试（离线，不需要设备/网络）
 *
 * 运行（或 `cd tests && make test-nav-sim`）：
 *   swiftc -O -o /tmp/test_nav_sim \
 *     ios/CarDashboardPhone/CarDashboardPhone/Nav/NavCore.swift \
 *     ios/CarDashboardPhone/CarDashboardPhone/Nav/Coords.swift \
 *     tools/nav_sim/NavSimSupport.swift \
 *     tools/test_nav_sim.swift && /tmp/test_nav_sim
 *
 * 覆盖：RouteBundle 解码、行车推进、法向偏航、以及"模拟循环产出的消息序列"。
 * 不含 CoreBluetooth（那部分只能对真机验证，见 docs/test-results.md）。
 */

import Foundation

var gPass = 0
var gFail = 0
func check(_ cond: Bool, _ msg: @autoclosure () -> String) {
    if cond { gPass += 1 } else { gFail += 1; print("  ✗ \(msg())") }
}
func approx(_ a: Double, _ b: Double, _ tol: Double) -> Bool { abs(a - b) <= tol }

/// 夹具相对路径（从仓库根目录运行即可命中；也可用环境变量覆盖）
let fixturePath = "tools/nav_sim/testdata/route-bundle.json"

func loadBundle() -> RouteBundle? {
    // 允许用环境变量覆盖（便于从不同工作目录运行）
    let p = ProcessInfo.processInfo.environment["NAV_SIM_FIXTURE"] ?? fixturePath
    guard let d = FileManager.default.contents(atPath: p) else { return nil }
    return try? JSONDecoder().decode(RouteBundle.self, from: d)
}

func testDecode() {
    print("--- RouteBundle 解码 ---")
    guard let b = loadBundle() else {
        // ⚠️ 夹具缺失必须**判失败**，不能静默跳过。
        //    曾出现过：从错误的工作目录运行时找不到夹具，
        //    4 项检查全部"跳过"却显示"通过"—— 等于没测。
        let p = ProcessInfo.processInfo.environment["NAV_SIM_FIXTURE"] ?? fixturePath
        check(false, "找不到路线夹具：\(p)（请设 NAV_SIM_FIXTURE 或在仓库根目录运行）")
        return
    }
    check(b.steps.count == 9, "应有 9 段，实际 \(b.steps.count)")
    check(b.distanceM == 1260, "总距离 1260，实际 \(String(describing: b.distanceM))")
    check(b.polyline.count > 100, "折线点应较多，实际 \(b.polyline.count)")

    let r = b.toNavRoute()
    check(r.steps.count == 9, "转换后 step 数一致")
    check(r.totalDistanceM > 0, "转换后总长 > 0")
    // 动作序列应与夹具一致
    let expect = [2, 2, 5, 3, 4, 2, 6, 3, 7]
    check(r.steps.map { $0.maneuver.rawValue } == expect,
          "动作序列应为 \(expect)，实际 \(r.steps.map { $0.maneuver.rawValue })")
}

func testDriveSimulator() {
    print("--- 行车推进 ---")
    guard let b = loadBundle() else { check(false, "夹具缺失（见上文提示）"); return }
    let r = b.toNavRoute()

    var sim = DriveSimulator(route: r, speedMps: 10) // 10 m/s
    check(sim.traveledM == 0, "初始里程为 0")
    check(!sim.finished, "初始未完成")

    _ = sim.advance(dt: 1)
    check(approx(sim.traveledM, 10, 1e-9), "1 秒后应行驶 10 m，实际 \(sim.traveledM)")

    _ = sim.advance(dt: 2)
    check(approx(sim.traveledM, 30, 1e-9), "再 2 秒后应 30 m")

    // 一路走到底：不应超出总长
    for _ in 0..<1000 { _ = sim.advance(dt: 1) }
    check(sim.finished, "长时间推进后应完成")
    check(approx(sim.traveledM, r.totalDistanceM, 1e-6),
          "里程应钳制到总长（实际 \(sim.traveledM) / \(r.totalDistanceM)）")

    // 越界后仍能取点（钳制到终点），不返回 nil
    check(sim.advance(dt: 1) != nil, "完成后再推进仍应返回位置（钳制）")
}

func testOffRoutePerpendicular() {
    print("--- 法向偏航 ---")
    guard let b = loadBundle() else { check(false, "夹具缺失（见上文提示）"); return }
    let r = b.toNavRoute()

    // 航向函数：应返回 0..360
    let h = sim0Heading(r)
    check(h >= 0 && h < 360, "航向应在 0..360，实际 \(h)")

    // 偏航注入：垂距应接近指定值（这是修复"固定朝东偏移"后应有的行为）
    var sim = DriveSimulator(route: r, speedMps: 20)
    sim.offRouteFromM = 200
    sim.offRouteOffsetM = 80

    let core = NavCore()
    core.setRoute(r)

    var sawOffRoute = false
    var maxCross = 0.0
    var t: Int64 = 0
    for _ in 0..<60 {
        t += 1000
        guard let gcj = sim.advance(dt: 1) else { break }
        let snap = core.handle(
            fix: NavFix(position: Coords.wgs84ToGcj02(Coords.gcj02ToWgs84(gcj)),
                        horizontalAccuracyM: 5, courseDeg: nil, timestampMs: t),
            nowMs: t)
        if sim.traveledM >= 210 {
            maxCross = max(maxCross, snap.crossTrackM)
            if snap.state == .rerouting { sawOffRoute = true }
        }
        if snap.state == .arrived { break }
    }
    check(maxCross > 60 && maxCross < 100,
          "偏航后垂距应接近 80 m（实际峰值 \(String(format: "%.1f", maxCross))）—— 若用固定方向偏移会忽大忽小")
    check(sawOffRoute, "应触发 rerouting（连续确认后）")
}

func sim0Heading(_ r: NavRoute) -> Double {
    let s = DriveSimulator(route: r, speedMps: 1)
    return s.routeHeadingDeg(at: 0)
}

func testMessageSequence() {
    print("--- 模拟循环产出的消息 ---")
    guard let b = loadBundle() else { check(false, "夹具缺失（见上文提示）"); return }
    let r = b.toNavRoute()

    var sim = DriveSimulator(route: r, speedMps: 15)
    let core = NavCore()
    core.setRoute(r)

    var messages: [String] = []
    var states = Set<String>()
    var maneuvers = Set<Int>()
    var t: Int64 = 0

    for _ in 0..<200 {
        t += 1000
        guard let gcj = sim.advance(dt: 1) else { break }
        let snap = core.handle(
            fix: NavFix(position: Coords.wgs84ToGcj02(Coords.gcj02ToWgs84(gcj)),
                        horizontalAccuracyM: 5, courseDeg: nil, timestampMs: t),
            nowMs: t)

        // 与 NavSimMain 相同的构造方式
        var json = "{\"t\":\"nav\",\"state\":\(snap.state.rawValue),\"maneuver\":\(snap.maneuver.rawValue)"
        if let d = snap.distanceToManeuverM { json += ",\"dist_m\":\(d)" }
        else { json += ",\"dist_m\":null" }
        json += "}"
        messages.append(json)
        states.insert("\(snap.state.rawValue)")
        maneuvers.insert(snap.maneuver.rawValue)
        if snap.state == .arrived { break }
    }

    check(!messages.isEmpty, "应产出消息")
    check(messages.allSatisfy { $0.hasPrefix("{\"t\":\"nav\"") }, "每条都应是导航消息")
    check(messages.allSatisfy { $0.count <= 480 }, "单条长度应在固件 512B 重组上限内")

    // 必须覆盖导航中与到达两种状态（否则模拟不完整）
    check(states.contains("1"), "应出现 active 状态")
    check(states.contains("3"), "应出现 arrived 状态")

    // 必须出现多个不同动作（说明步骤切换生效）
    check(maneuvers.count >= 3, "应出现至少 3 种动作，实际 \(maneuvers.sorted())")

    // 最后一条应为到达
    check(messages.last?.contains("\"state\":3") == true, "最后一条应为到达状态")

    // 可被 JSON 解析（固件侧同款手写解析器要求严格 JSON）
    if let d = messages.last?.data(using: .utf8),
       let obj = try? JSONSerialization.jsonObject(with: d) as? [String: Any] {
        check(obj["t"] as? String == "nav", "最后一条可被 JSON 解析且 t=nav")
        check(obj["state"] as? Int == 3, "state 为 3")
    } else {
        check(false, "最后一条不是合法 JSON")
    }
    print("     产出 \(messages.count) 条消息，动作集合 \(maneuvers.sorted())")
}

@main
struct TestNavSim {
    static func main() {
        print("=== Mac 模拟器（纯逻辑）===")
        testDecode()
        testDriveSimulator()
        testOffRoutePerpendicular()
        testMessageSequence()
        print("结果: \(gPass) 通过, \(gFail) 失败")
        exit(gFail == 0 ? 0 : 1)
    }
}
