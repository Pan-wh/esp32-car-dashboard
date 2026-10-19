/**
 * NavUIState.swift —— 导航页的**界面状态判定**与**展示格式化**（纯函数）
 *
 * ## 为什么单独抽出来
 *
 * 2026-10-15 的产品复盘发现导航页最严重的问题（P0-1）是：
 * **「开始导航」按钮在不同时刻含义不同** —— 有目的地时它是"取候选"，
 * 有候选时它又变多余。根因是判定散落在 `navTab` 的多个 `if` 里，
 * 没人能一眼看全"此刻到底该显示什么"。
 *
 * 因此把"此刻处于哪个状态"抽成一个**没有副作用**的纯函数：
 *   - `ContentView` 只负责把当前输入喂进来、按返回值决定显示什么；
 *   - 测试可以在 macOS 上穷举各种组合（见 `tools/test_nav_ui_state.swift`）。
 *
 * 本文件不依赖 SwiftUI/CoreLocation，可在 macOS 上直接单测。
 *
 * 设计文档：`docs/nav-app-ux.md` §二、§三。
 */

import Foundation

/// 导航页的界面状态（文档 §二 的状态机）
public enum NavUIState: Equatable, Sendable {
    /// 准备：搜索目的地（未输入、未导航）
    case ready
    /// 选地点：有输入或正在查询，还没有候选路线
    case pickingPlace
    /// 选路线：已取到候选路线，等待用户确认
    case pickingRoute
    /// 导航中
    case navigating
    /**
     已到达目的地（2026-10-16 新增，P0-2）。

     ## 为什么必须是一个独立状态

     此前 `NavCore` 早就有 `.arrived`、`statusText` 也会变成"已到达目的地"，
     但**界面状态机完全不认它** —— 于是到达后界面仍停在「导航中」：
     继续显示"距下一动作 -- 米"、继续占着定位、也没有明确的"结束"入口。
     用户到了地方看着一个还在"导航中"的界面，不知道要不要手动点结束
     —— 这是导航流程**没有终点**，体验上像"没做完"。

     独立成状态后，到达可以给一个明确的收尾卡片（结束行程 / 继续导航到附近），
     并且可以自动停掉定位省电。注意**不能**沿用导航态然后把按钮换掉 ——
     那样"导航中"和"已到达"就无法区分，测试也穷举不出来。
     */
    case arrived
}

public enum NavUIStateLogic {

    /**
     判定当前界面状态。

     ⚠️ **`isLoading` 也算「选地点」**：若把它忽略，取候选的那 1~3 秒里
     界面会因为"还没有候选"而闪回「准备」，用户会觉得刚才的操作没生效 ——
     这正是改造前"点了候选没反应"的观感来源之一。

     - Parameters:
       - isNavigating: 是否正在导航
       - hasCandidates: 是否已取到候选路线（且未开始导航）
       - queryIsEmpty: 搜索框是否为空
       - isLoading: 是否正在查询（取候选 / 搜地点）
     */
    public static func state(isNavigating: Bool,
                             hasCandidates: Bool,
                             queryIsEmpty: Bool,
                             isLoading: Bool,
                             isChoosingRoute: Bool = false,
                             hasArrived: Bool = false) -> NavUIState {
        /*
         * 「已到达」优先于一切（除"主动换路线"之外）。
         *
         * 顺序理由：到达是**既成事实**，此时即便 isNavigating 仍为 true
         * （NavSession 尚未 stop），界面也必须切到到达态 —— 否则就回到
         * "到达了却还在显示导航中"的老问题。
         *
         * 但 `isChoosingRoute` 仍要优先：用户明确点了「换一条路线」，
         * 说明他不想结束（比如目的地停车场没找到，想换个地方）。
         */
        if isChoosingRoute && hasCandidates { return .pickingRoute }
        if hasArrived { return .arrived }
        if isNavigating { return .navigating }
        if hasCandidates { return .pickingRoute }
        if !queryIsEmpty || isLoading { return .pickingPlace }
        return .ready
    }

    /// 「开始导航」按钮**只应在选路线状态出现**（P0-2 的验收标准）
    public static func showsStartButton(_ s: NavUIState) -> Bool {
        s == .pickingRoute
    }

    /// 只有导航中才显示「结束导航」；**到达态不给它** —— 那时该显示「结束行程」
    public static func showsStopButton(_ s: NavUIState) -> Bool {
        s == .navigating
    }

    /// 到达态才显示到达卡片（结束行程 / 继续导航到附近）
    public static func showsArrivalCard(_ s: NavUIState) -> Bool {
        s == .arrived
    }

    /// 只有准备/选地点状态才需要搜索框（选路线、导航中、已到达都不该再输入）
    public static func showsSearchField(_ s: NavUIState) -> Bool {
        s == .ready || s == .pickingPlace
    }
}

// MARK: - 路线候选的展示（P1-3：给人话，不给参数）

public enum NavCandidateFormat {

    /**
     策略号 → **人话**。

     实测有效的三个策略（见 `docs/nav-backend.md` §5.4）：
     0 速度优先、2 距离最短、6 不走高速。

     ⚠️ 不要直接显示"速度优先" —— 用户面对三个选项时的真实问题是
     **"我该选哪个"**，所以第一条（最快的）应该叫**「推荐」**。
     */
    public static func strategyLabel(_ strategy: Int, isFastest: Bool = false) -> String {
        if isFastest { return "推荐" }
        switch strategy {
        case 0: return "速度优先"
        case 2: return "最短"
        case 6: return "不走高速"
        default: return "方案 \(strategy)"
        }
    }

    /// 距离：< 1km 用米，否则用公里（保留一位）
    public static func distanceText(_ meters: Double) -> String {
        if meters < 1000 { return String(format: "%.0f m", meters) }
        return String(format: "%.1f km", meters / 1000)
    }

    /// 耗时：< 1 分钟显示"不到 1 分钟"，< 60 分钟用分钟，否则用"x 小时 y 分"
    public static func durationText(_ seconds: Double) -> String {
        /*
         * ⚠️ "< 1 分钟" 必须先判，且用**原始秒数**判 ——
         *    第一版先 `(seconds/60).rounded()` 再判，于是 30 秒被四舍五入成
         *    "1 分钟"，反而比 59 秒（"1 分钟"）更不准确。单测抓到了这个。
         */
        if seconds < 60 { return "不到 1 分钟" }
        let min = Int((seconds / 60).rounded())
        if min < 60 { return "\(min) 分钟" }
        let h = min / 60, m = min % 60
        return m == 0 ? "\(h) 小时" : "\(h) 小时 \(m) 分"
    }

    /**
     ETA（预计到达时刻）—— 产品复盘里用户最关心的其实是「几点到」。

     - Parameters:
       - durationS: 预计耗时（秒）
       - now: 当前时刻（测试可注入，避免依赖真实时钟）
       - calendar: 用于格式化（测试可固定时区）
     */
    public static func etaText(durationS: Double,
                               now: Date = Date(),
                               calendar: Calendar = .current) -> String {
        let arrive = now.addingTimeInterval(durationS)
        let f = DateFormatter()
        f.calendar = calendar
        f.timeZone = calendar.timeZone
        f.locale = Locale(identifier: "zh_CN")
        f.dateFormat = "HH:mm"
        return f.string(from: arrive) + " 到"
    }

    /**
     候选列表的一行主文案：`17 分钟 · 14:32 到`。

     把 ETA 放在**同行**而不是另起一行 —— 圆屏/小屏上每多一行都是成本，
     而这两条信息用户会一起读。
     */
    public static func primaryText(durationS: Double,
                                   now: Date = Date(),
                                   calendar: Calendar = .current) -> String {
        "\(durationText(durationS)) · \(etaText(durationS: durationS, now: now, calendar: calendar))"
    }

    /// 候选列表的一行副文案：`16.8 km · 推荐`
    public static func secondaryText(distanceM: Double,
                                     strategy: Int,
                                     isFastest: Bool) -> String {
        "\(distanceText(distanceM)) · \(strategyLabel(strategy, isFastest: isFastest))"
    }

    /**
     导航中的"剩余"摘要（P1-2）：`12.3 km · 18 分钟 · 14:32 到`。

     ## 为什么要单独做一个

     此前导航中把**全程剩余**用 `.subheadline` + 灰色显示，
     视觉层级低于"距下一动作"。但驾驶员最常想知道的是"还有多久到" ——
     次要信息被弱化得过分了。

     现在把它提炼成一行可复用的文案，界面上用更大的字号呈现。
     三段信息用 `·` 连接：短、好扫、不换行。

     - Parameters:
       - distanceM: 剩余距离（米）
       - durationS: 剩余预计耗时（秒）；nil 时省略时长与到达时刻
     */
    public static func remainingSummaryText(distanceM: Double,
                                            durationS: Double?,
                                            now: Date = Date(),
                                            calendar: Calendar = .current) -> String {
        let dist = distanceText(distanceM)
        guard let d = durationS, d.isFinite, d >= 0 else { return dist }
        return "\(dist) · \(durationText(d)) · \(etaText(durationS: d, now: now, calendar: calendar))"
    }
}

// MARK: - 常用 / 最近目的地（P2-2）

/// 一条已保存的目的地
public struct SavedDestination: Codable, Identifiable, Equatable, Sendable {
    public let name: String
    public let lon: Double
    public let lat: Double
    /// 是否为用户手动收藏（否则是"最近"自动记录）
    public let isFavorite: Bool

    public var id: String { "\(name)|\(lon)|\(lat)" }
    public var geo: GeoPoint { GeoPoint(lon: lon, lat: lat) }
}

/**
 常用 / 最近目的地的**纯逻辑**（存储由 `SavedDestinationStore` 负责）。

 放在这里是为了可单测 —— 去重、上限、排序这些规则最容易写错，
 而它们又只能在真机上发现（比如"同一个地点出现三次"）。
 */
public enum SavedDestinationLogic {

    /// 最近列表的上限（超过就丢弃最旧的）
    public static let maxRecents = 5

    /**
     记录一次使用：若已存在（同名或同坐标）则提到最前，否则插入最前。

     - Parameters:
       - list: 现有列表（`isFavorite == false` 的部分）
       - new: 本次使用的地点
     */
    public static func recording(_ list: [SavedDestination],
                                 _ new: SavedDestination,
                                 max: Int = maxRecents) -> [SavedDestination] {
        /* 同名**或**同坐标都视为同一个地点：地名可能一样（"家"）但坐标不同，
           也可能名字不同但坐标相同（用户先后搜了"北京南站"和"北京南站(南进站口)"）。*/
        let sameKey: (SavedDestination) -> Bool = { e in
            e.name == new.name || (abs(e.lon - new.lon) < 1e-6 && abs(e.lat - new.lat) < 1e-6)
        }
        var out = list.filter { !sameKey($0) }
        var entry = new
        /* 若旧条目是收藏，保留收藏标记 */
        if let old = list.first(where: sameKey), old.isFavorite {
            entry = SavedDestination(name: new.name, lon: new.lon, lat: new.lat, isFavorite: true)
        }
        out.insert(entry, at: 0)
        if out.count > max { out = Array(out.prefix(max)) }
        return out
    }

    /// 合并"收藏"与"最近"用于展示：收藏在前，最近在后，不重复
    public static func merged(favorites: [SavedDestination],
                              recents: [SavedDestination],
                              maxShown: Int = 6) -> [SavedDestination] {
        var seen = Set<String>()
        var out: [SavedDestination] = []
        for e in favorites + recents {
            if seen.contains(e.id) { continue }
            seen.insert(e.id)
            out.append(e)
            if out.count >= maxShown { break }
        }
        return out
    }
}
