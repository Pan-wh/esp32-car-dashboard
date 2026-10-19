/**
 * test_nav_ui_state.swift —— 导航页状态机与展示格式化的单元测试
 *                          （macOS 本机运行，不需手机/仪表）
 *
 * 运行（或 `cd tests && make test-nav-ui`）：
 *   swiftc -O -o /tmp/test_nav_ui_state \
 *     ios/CarDashboardPhone/CarDashboardPhone/Nav/NavUIState.swift \
 *     tools/test_nav_ui_state.swift && /tmp/test_nav_ui_state
 *
 * 直接编译 App 的**真实源文件**（不内联复制），故测的就是 App 用的那份逻辑
 * —— 与 test_device_faces / test_ble_policy 的做法一致。
 *
 * 为什么值得测：这些规则全都是"只在真机上才看得出来"的体验问题 ——
 * 按钮该不该出现、取候选期间界面会不会闪回、ETA 算得对不对。
 * 抽成纯函数后可以在 PC 上穷举。
 *
 * ⚠️ **多文件 swiftc 不允许顶层语句**，所以下面用 `@main` 包起来
 *    （与 tools/test_ble_policy.swift 同一写法）。
 *    第一版我按普通脚本写，编译报 "expressions are not allowed at the top
 *    level" —— 而且报在**第一条顶层语句**上，看起来像语法问题，
 *    实际是"这个文件不是 main.swift"。
 */

import Foundation

/*
 * `NavUIState.swift` 里 `SavedDestination.geo` 用到 GeoPoint。
 * 真实定义在 App 的 GatewayClient.swift 里（依赖 URLSession 等，不便单独编译），
 * 这里给一个**同构的最小版本**，只为满足编译。
 *
 * ⚠️ 必须声明为 `public`：`SavedDestination.geo` 是 public 属性，而
 *    "public 属性不能用 internal 类型" —— 若这里是 internal，编译会失败，
 *    而且 Swift 报的是一个**误导性的**错误（"expressions are not allowed at
 *    the top level"），跟真正的原因毫无关系。踩过一次，记在这里。
 */
public struct GeoPoint: Codable, Equatable {
    public var lon: Double
    public var lat: Double
    public init(lon: Double, lat: Double) { self.lon = lon; self.lat = lat }
}

typealias S = NavUIState
typealias L = NavUIStateLogic
typealias F = NavCandidateFormat
typealias D = SavedDestination
typealias DL = SavedDestinationLogic

// ---- 极简断言（与 tools/test_ble_policy.swift 风格一致）----
var gPass = 0
var gFail = 0
func check(_ cond: Bool, _ msg: @autoclosure () -> String) {
    if cond { gPass += 1 } else { gFail += 1; print("  ✗ \(msg())") }
}
func eq<T: Equatable>(_ a: T, _ b: T, _ msg: @autoclosure () -> String) {
    if a == b { gPass += 1 } else { gFail += 1; print("  ✗ \(msg())（实际 \(a)，期望 \(b)）") }
}

/// 固定时钟与时区，保证 ETA 测试可重复
let fixedNow = Date(timeIntervalSince1970: 1_800_000_000) // 2027-01-15 前后，UTC
var fixedCal: Calendar = {
    var c = Calendar(identifier: .gregorian)
    c.timeZone = TimeZone(identifier: "Asia/Shanghai")!
    return c
}()

@main
struct TestNavUIState {
    static func main() {
    print("=== 导航页状态机 ===")

    // ─────────────────────────────────────────────────────────────
    // 1) 穷举：四个布尔输入的 16 种组合都必须落在确定的状态上
    // ─────────────────────────────────────────────────────────────
    print("--- 穷举 16 种组合 ---")
    var seen = Set<String>()
    for nav in [false, true] {
        for cand in [false, true] {
            for empty in [false, true] {
                for loading in [false, true] {
                    let st = L.state(isNavigating: nav, hasCandidates: cand,
                                     queryIsEmpty: empty, isLoading: loading)
                    seen.insert("\(st)")
                    /* 不变量 1：导航中永远压过其它一切 */
                    if nav { check(st == .navigating, "导航中优先（nav=true 时必须 navigating）") }
                    /* 不变量 2：未导航但有候选 → 一定是选路线 */
                    if !nav && cand { check(st == .pickingRoute, "有候选时必须停在选路线") }
                    /* 不变量 3：未导航无候选，但正在查询 → 不能停在"准备" */
                    if !nav && !cand && loading {
                        check(st != .ready, "查询期间不能闪回「准备」（否则用户以为没生效）")
                    }
                    /* 不变量 4：未导航、无候选、没输入、没查询 → 准备 */
                    if !nav && !cand && empty && !loading { check(st == .ready, "空输入空闲时是准备") }
                    /* 不变量 5：状态判定绝不会因为「有候选」而被导航中覆盖掉导航态之外的东西 */
                    if !nav && !cand && !empty { check(st == .pickingPlace, "有输入无候选是选地点") }
                }
            }
        }
    }
    check(seen.count == 4, "16 种组合只产生 4 种状态（实际 \(seen.count) 种：\(seen.sorted())）")
    /* 加上到达维度后：未到达分支仍是 4 种，到达分支另有 arrived（见 2b）*/

    // ─────────────────────────────────────────────────────────────
    // 2) 按钮可见性：P0-2 的验收标准
    // ─────────────────────────────────────────────────────────────
    print("--- 按钮语义唯一（P0-2）---")
    check(L.showsStartButton(.pickingRoute), "选路线时显示「开始导航」")
    check(!L.showsStartButton(.ready), "准备时**不**显示「开始导航」")
    check(!L.showsStartButton(.pickingPlace), "选地点时**不**显示「开始导航」")
    check(!L.showsStartButton(.navigating), "导航中**不**显示「开始导航」（避免重启导航）")

    check(L.showsStopButton(.navigating), "导航中显示「结束导航」")
    check(!L.showsStopButton(.pickingRoute), "选路线时不显示「结束导航」")

    check(L.showsSearchField(.ready) && L.showsSearchField(.pickingPlace),
          "准备/选地点显示搜索框")
    check(!L.showsSearchField(.pickingRoute), "选路线时不显示搜索框（此步已不需要输入）")
    check(!L.showsSearchField(.navigating), "导航中不显示搜索框")

    // ─────────────────────────────────────────────────────────────
    // 2b) 「已到达」状态（P0-2：导航流程必须有终点）
    // ─────────────────────────────────────────────────────────────
    print("--- 已到达（P0-2）---")
    /* 到达是既成事实：即便 isNavigating 仍为 true（NavSession 还没 stop），
       也必须切到到达态 —— 否则回到"到了却还显示导航中"的老问题 */
    check(L.state(isNavigating: true, hasCandidates: false, queryIsEmpty: true,
                  isLoading: false, hasArrived: true) == .arrived,
          "★ 已到达时即使仍在导航也必须显示到达态")
    check(L.state(isNavigating: false, hasCandidates: false, queryIsEmpty: true,
                  isLoading: false, hasArrived: true) == .arrived,
          "未导航 + 已到达 → 到达态")
    /* 但用户明确点「换一条路线」时不结束：到达优先于导航，却让位于主动换路 */
    check(L.state(isNavigating: true, hasCandidates: true, queryIsEmpty: true,
                  isLoading: false, isChoosingRoute: true, hasArrived: true) == .pickingRoute,
          "★ 已到达但用户主动换路线 → 进选路线（不把他锁死在到达页）")
    /* 未到达时行为不变（向后兼容） */
    check(L.state(isNavigating: true, hasCandidates: false, queryIsEmpty: true,
                  isLoading: false, hasArrived: false) == .navigating,
          "未到达时导航态不变")

    /* 按钮可见性：到达态给"结束行程"，**不给**"结束导航"（语义不同，避免两个都点） */
    check(L.showsArrivalCard(.arrived), "到达态显示到达卡片")
    check(!L.showsArrivalCard(.navigating), "导航中不显示到达卡片")
    check(!L.showsStopButton(.arrived),
          "★ 到达态不显示「结束导航」（那时该显示「结束行程」，否则两个按钮语义重叠）")
    check(!L.showsSearchField(.arrived),
          "到达态不显示搜索框（要先点「继续导航到附近」才回到搜索）")

    // ─────────────────────────────────────────────────────────────
    // 2c) 剩余摘要（P1-2：开车时最常问的"还有多久到"）
    // ─────────────────────────────────────────────────────────────
    print("--- 剩余摘要（P1-2）---")
    /* 固定时刻，避免依赖真实时钟（测试必须可重复）*/
    let t0 = Date(timeIntervalSince1970: 1_791_792_000)  // 固定基准
    let full = NavCandidateFormat.remainingSummaryText(distanceM: 12300, durationS: 1080, now: t0, calendar: fixedCal)
    check(full.contains("12.3 km"), "★ 剩余摘要应含距离，实际：\(full)")
    check(full.contains("18 分钟"), "★ 剩余摘要应含时长，实际：\(full)")
    check(full.contains("到"), "★ 剩余摘要应含到达时刻，实际：\(full)")
    check(full.components(separatedBy: " · ").count == 3,
          "三段信息用 ' · ' 连接（不换行、好扫），实际：\(full)")

    /* 拿不到总时长（网关没给 duration）时退化为只有距离 —— 不能显示 "nil" 或崩 */
    let distOnly = NavCandidateFormat.remainingSummaryText(distanceM: 800, durationS: nil, now: t0, calendar: fixedCal)
    check(distOnly.contains("800 m"), "无时长时仍应显示距离，实际：\(distOnly)")
    check(!distOnly.contains("nil"), "★ 不得把 nil 显示给用户，实际：\(distOnly)")
    check(!distOnly.contains("·"), "无时长时不应留下多余的分隔符，实际：\(distOnly)")

    /* 异常时长（负 / 非有限）也要退化，不能算出 "0 分钟" 或崩溃 */
    let negDur = NavCandidateFormat.remainingSummaryText(distanceM: 800, durationS: -5, now: t0, calendar: fixedCal)
    check(!negDur.contains("·"), "★ 负时长应退化为只有距离，实际：\(negDur)")
    let nanDur = NavCandidateFormat.remainingSummaryText(distanceM: 800, durationS: .nan, now: t0, calendar: fixedCal)
    check(!nanDur.contains("nan"), "★ NaN 不得泄漏到界面，实际：\(nanDur)")

    // ─────────────────────────────────────────────────────────────
    // 3) 策略人话（P1-3）
    // ─────────────────────────────────────────────────────────────
    print("--- 策略名给人话 ---")
    eq(F.strategyLabel(0, isFastest: true), "推荐", "最快的一条叫「推荐」而不是「速度优先」")
    eq(F.strategyLabel(0, isFastest: false), "速度优先", "非最快的策略 0 保留原名")
    eq(F.strategyLabel(2), "最短", "策略 2 = 距离最短 → 「最短」")
    eq(F.strategyLabel(6), "不走高速", "策略 6 = 不走高速")
    eq(F.strategyLabel(99), "方案 99", "未知策略号要有兜底，不能显示空")

    // ─────────────────────────────────────────────────────────────
    // 4) 距离 / 耗时格式化
    // ─────────────────────────────────────────────────────────────
    print("--- 距离/耗时格式化 ---")
    eq(F.distanceText(0), "0 m", "0 米")
    eq(F.distanceText(999), "999 m", "999 米仍用米")
    eq(F.distanceText(1000), "1.0 km", "1000 米切到公里")
    eq(F.distanceText(16847), "16.8 km", "16.8 公里（保留一位）")

    eq(F.durationText(0), "不到 1 分钟", "0 秒不显示「0 分钟」")
    eq(F.durationText(30), "不到 1 分钟", "30 秒同样")
    eq(F.durationText(60), "1 分钟", "1 分钟")
    eq(F.durationText(2349), "39 分钟", "2349 秒 ≈ 39 分钟")
    eq(F.durationText(3600), "1 小时", "整点小时不带「0 分」")
    eq(F.durationText(5400), "1 小时 30 分", "1.5 小时")
    eq(F.durationText(10758), "2 小时 59 分", "长途")

    // ─────────────────────────────────────────────────────────────
    // 5) ETA（注入固定时钟，结果确定）
    // ─────────────────────────────────────────────────────────────
    print("--- ETA ---")
    let eta = F.etaText(durationS: 2349, now: fixedNow, calendar: fixedCal)
    check(eta.hasSuffix(" 到"), "ETA 以「到」结尾（实际 \(eta)）")
    /* 自行按同一时区算一遍做交叉验证，避免只信被测代码 */
    do {
        let f = DateFormatter()
        f.calendar = fixedCal; f.timeZone = fixedCal.timeZone
        f.locale = Locale(identifier: "zh_CN"); f.dateFormat = "HH:mm"
        let expect = f.string(from: fixedNow.addingTimeInterval(2349)) + " 到"
        eq(eta, expect, "ETA 与独立计算一致")
    }
    check(F.etaText(durationS: 0, now: fixedNow, calendar: fixedCal)
            == F.etaText(durationS: 0, now: fixedNow, calendar: fixedCal),
          "同样的输入给出同样的 ETA（无隐藏状态）")

    let prim = F.primaryText(durationS: 2349, now: fixedNow, calendar: fixedCal)
    check(prim.contains("分钟") && prim.contains(" 到"), "主文案同时含耗时与 ETA（实际 \(prim)）")
    check(prim.split(separator: "·").count == 2, "主文案是「耗时 · ETA」两段")

    let sec = F.secondaryText(distanceM: 16847, strategy: 0, isFastest: true)
    eq(sec, "16.8 km · 推荐", "副文案是「距离 · 策略」")

    // ─────────────────────────────────────────────────────────────
    // 6) 常用 / 最近目的地（P2-2）
    // ─────────────────────────────────────────────────────────────
    print("--- 常用/最近 ---")
    let home = D(name: "家", lon: 116.1, lat: 39.1, isFavorite: true)
    let south = D(name: "北京南站", lon: 116.37, lat: 39.86, isFavorite: false)
    let airport = D(name: "首都机场", lon: 116.6, lat: 40.08, isFavorite: false)

    /* 新记录插入最前 */
    var recents = DL.recording([], south)
    eq(recents.map(\.name), ["北京南站"], "首次记录只有一条")
    recents = DL.recording(recents, airport)
    eq(recents.map(\.name), ["首都机场", "北京南站"], "新记录排到最前")

    /* 同名去重并提前 */
    recents = DL.recording(recents, south)
    eq(recents.map(\.name), ["北京南站", "首都机场"], "重复使用同名地点不产生第二条，且提到最前")
    eq(recents.count, 2, "去重后条数正确")

    /* 同坐标去重（名字不同） */
    let sameSpotRenamed = D(name: "南站南口", lon: south.lon, lat: south.lat, isFavorite: false)
    let dedup = DL.recording(recents, sameSpotRenamed)
    eq(dedup.count, 2, "同名**或**同坐标都算同一地点")
    eq(dedup.first?.name, "南站南口", "按最近一次的名字显示")

    /* 上限 */
    var many: [D] = []
    for i in 0..<10 {
        many = DL.recording(many, D(name: "地点\(i)", lon: Double(i), lat: Double(i), isFavorite: false))
    }
    eq(many.count, DL.maxRecents, "最近列表不超过上限 \(DL.maxRecents)")
    eq(many.first?.name, "地点9", "保留最新的；丢弃最旧的")

    /* 收藏标记在同名重复时被保留 */
    let faved = DL.recording([home], D(name: "家", lon: 116.2, lat: 39.2, isFavorite: false))
    eq(faved.count, 1, "重复的「家」只留一条")
    check(faved[0].isFavorite, "用户收藏过的地点，即使又搜了一次也不丢收藏标记")

    /* 合并展示：收藏在前，去重，受 maxShown 限制 */
    let merged = DL.merged(favorites: [home], recents: [south, airport], maxShown: 2)
    eq(merged.map(\.name), ["家", "北京南站"], "收藏在前、最近在后、按上限截断")
    let mergedAll = DL.merged(favorites: [home], recents: [home, south])
    eq(mergedAll.count, 2, "收藏与最近重复时只显示一条")

    print("\n结果: \(gPass) 通过, \(gFail) 失败")
    exit(gFail == 0 ? 0 : 1)
    }
}
