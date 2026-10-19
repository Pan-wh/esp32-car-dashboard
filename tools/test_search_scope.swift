/**
 * test_search_scope.swift —— 搜索范围降级策略测试（macOS 本机运行，无需联网）
 *
 * 运行（或 `cd tests && make test-search-scope`）：
 *   swiftc -O -o /tmp/test_search_scope \
 *     ios/CarDashboardPhone/CarDashboardPhone/Nav/SearchScope.swift \
 *     tools/test_search_scope.swift && /tmp/test_search_scope
 *
 * 直接编译 App 的**真实源文件**（不内联复制），故测的就是 App 用的逻辑。
 *
 * 为什么值得测：这层逻辑决定"发几个请求"和"结果从哪来"，
 * 而配额有限（网关有用量计数器）。写错会导致每次联想都发 3 个请求，
 * 或直辖市重复查同一层 —— 都只有真机联网才能发现，代价高。
 */

import Foundation

var gPass = 0
var gFail = 0
func check(_ cond: Bool, _ msg: @autoclosure () -> String) {
    if cond { gPass += 1 } else { gFail += 1; print("  ✗ \(msg())") }
}
func eq<T: Equatable>(_ a: T, _ b: T, _ msg: @autoclosure () -> String) {
    if a == b { gPass += 1 } else { gFail += 1; print("  ✗ \(msg())（实际 \(a)，期望 \(b)）") }
}

typealias P = SearchScopePolicy
typealias R = SearchScopePolicy.Region

// ─────────────────────────────────────────────────────────────
// 1) 一般城市：本市 → 本省 → 全国
// ─────────────────────────────────────────────────────────────
func testNormalCity() {
    let r = R(city: "杭州市", province: "浙江省")
    check(!r.isMunicipality, "杭州不是直辖市")
    eq(P.scopes(for: r), [.city, .province, .nationwide],
       "一般城市应为 本市→本省→全国")

    // city 参数取值：全国层必须传**空串**（高德语义=不限城市）
    eq(P.cityParameter(for: .city, region: r), "杭州市", "本市层应传市名")
    eq(P.cityParameter(for: .province, region: r), "浙江省", "本省层应传省名")
    eq(P.cityParameter(for: .nationwide, region: r), "", "★ 全国层必须传空串（不限城市）")
}

// ─────────────────────────────────────────────────────────────
// 2) 直辖市：省==市，必须跳过省级（否则白烧一次配额）
// ─────────────────────────────────────────────────────────────
func testMunicipality() {
    let shanghai = R(city: "上海市", province: "上海市")
    check(shanghai.isMunicipality, "上海市的省==市，应识别为直辖市")
    eq(P.scopes(for: shanghai), [.city, .nationwide],
       "★ 直辖市应跳过省级（省==市，重复请求纯浪费配额）")

    let beijing = R(city: "北京市", province: "北京市")
    eq(P.scopes(for: beijing), [.city, .nationwide], "北京同样跳过省级")

    let chongqing = R(city: "重庆市", province: "重庆市")
    eq(P.scopes(for: chongqing), [.city, .nationwide], "重庆同样跳过省级")

    // 带/不带"市"字都应识别为同一地（高德返回格式可能不一致）
    let variant = R(city: "上海市", province: "上海")
    check(variant.isMunicipality, "★ 忽略「市/省」后缀后相同也应识别为直辖市")
}

// ─────────────────────────────────────────────────────────────
// 3) 无定位信息：只能全国（不能崩、不能返回空列表）
// ─────────────────────────────────────────────────────────────
func testNoRegion() {
    eq(P.scopes(for: R()), [.nationwide], "★ 没有城市信息时只能查全国（不得返回空）")
    eq(P.scopes(for: R(city: "", province: "浙江省")), [.nationwide],
       "只有省没有市 → 仍退化为全国（无法定位到市，省过滤意义不大）")

    // 空白字符也算"没有"
    eq(P.scopes(for: R(city: "   ", province: "浙江省")), [.nationwide],
       "★ 纯空白 city 应视为缺失")
}

// ─────────────────────────────────────────────────────────────
// 4) 有市无省：跳过省级
// ─────────────────────────────────────────────────────────────
func testCityWithoutProvince() {
    let r = R(city: "苏州市", province: "")
    eq(P.scopes(for: r), [.city, .nationwide], "只有市没有省 → 跳过省级")
}

// ─────────────────────────────────────────────────────────────
// 5) 降级终止：本层有结果就停（这是"本市优先"的核心语义）
// ─────────────────────────────────────────────────────────────
func testStopOnFirstHit() {
    check(P.shouldStop(after: .city, resultCount: 3), "★ 本市有结果就应停止降级")
    check(!P.shouldStop(after: .city, resultCount: 0), "本市无结果 → 继续降级")
    check(P.shouldStop(after: .province, resultCount: 1), "本省有结果就停")
    check(!P.shouldStop(after: .province, resultCount: 0), "本省无结果 → 继续")
    check(P.shouldStop(after: .nationwide, resultCount: 5), "全国层是最后一层")
}

// ─────────────────────────────────────────────────────────────
// 6) 来源提示：本市不提示（默认预期），降级后要提示
// ─────────────────────────────────────────────────────────────
func testNotice() {
    check(P.notice(for: .city, resultCount: 3) == nil,
          "本市结果属默认预期，不应啰嗦提示")
    check(P.notice(for: .province, resultCount: 2)?.contains("全省") == true,
          "本省结果应提示用户")
    check(P.notice(for: .nationwide, resultCount: 2)?.contains("全国") == true,
          "全国结果应提示用户")
    // 没结果时不提示（界面自己会说"没找到"，避免两条矛盾文案）
    check(P.notice(for: .province, resultCount: 0) == nil, "无结果时不提示来源")
    check(P.notice(for: .nationwide, resultCount: 0) == nil, "无结果时不提示来源")
}

// ─────────────────────────────────────────────────────────────
// 7) 穷举不变量：任何输入下，scopes 都非空、无重复、以全国结尾
//    （兜住"将来加分支漏了兜底"的回归）
// ─────────────────────────────────────────────────────────────
func testExhaustiveInvariants() {
    let cities = ["", "  ", "杭州市", "上海市", "北京市"]
    let provinces = ["", "浙江省", "上海市", "北京"]
    var cases = 0
    var violations = 0

    for c in cities {
        for p in provinces {
            cases += 1
            let scopes = P.scopes(for: R(city: c, province: p))

            // 不变量 1：必须非空（否则联想功能直接失效）
            if scopes.isEmpty { violations += 1 }
            // 不变量 2：不得有重复（重复=多烧一次配额）
            if Set(scopes).count != scopes.count { violations += 1 }
            // 不变量 3：必须以全国结尾（保证总有兜底，不会"什么都没查"）
            if scopes.last != .nationwide { violations += 1 }
            // 不变量 4：直辖市不得出现省级
            let r = R(city: c, province: p)
            if r.isMunicipality && scopes.contains(.province) { violations += 1 }
        }
    }
    check(cases == 20, "应枚举 20 种组合（实际 \(cases)）")
    check(violations == 0, "20 种组合中存在 \(violations) 处违反不变量")
    print("  已穷举 \(cases) 种行政区组合，不变量违例 \(violations) 处")
}

@main
struct TestSearchScope {
    static func main() {
        print("=== 地点联想搜索范围（本市 → 本省 → 全国）===")
        testNormalCity()
        testMunicipality()
        testNoRegion()
        testCityWithoutProvince()
        testStopOnFirstHit()
        testNotice()
        testExhaustiveInvariants()
        print("结果: \(gPass) 通过, \(gFail) 失败")
        exit(gFail == 0 ? 0 : 1)
    }
}
