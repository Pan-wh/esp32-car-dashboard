/**
 * NavTabView.swift —— 导航标签页（四状态机）
 *
 * ## 为什么从 ContentView 里抽出来
 *
 * 2026-10-15 的产品复盘（`docs/nav-app-ux.md`）发现导航页的问题是
 * **"把功能清单直接铺成了界面"**：每接一个网关端点就加一个 Section，
 * 攒到 7 个 Section、214 行，没人能一眼看全"此刻该显示什么"。
 *
 * 本文件把它重写成一个**显式的状态机**：
 *
 *     [准备] ──打字──▶ [选地点] ──点一个──▶ [选路线] ──开始导航──▶ [导航中]
 *        ▲                 │                  │                    │
 *        └─────────────────┴──────────────────┘                    │
 *                                                            结束导航
 *
 * 每个状态只做一件事、只显示该做的事；状态的判定在
 * `Nav/NavUIState.swift` 里（纯函数 + 穷举单测）。
 *
 * 设计文档：`docs/nav-app-ux.md`
 */

import SwiftUI
import CoreLocation

// MARK: - 统一的地点行（tips / geocode / around 归一）

/// 三种来源归一后的列表项 —— 用户不需要知道它们来自哪个端点。
struct NavPlaceRow: Identifiable {
    let id: String
    let name: String
    /// 副标题：地址 / 行政区 / POI 类型
    let detail: String
    /// 距当前位置的距离（米）。拿不到定位时为 nil。
    let distanceM: Double?
    let geo: GeoPoint
}

// MARK: - 导航标签页

struct NavTabView: View {

    @ObservedObject var session: DriveSessionManager

    /// 网关配置（与设置页共用同一个 @AppStorage 键）
    @AppStorage("navGateway") private var navGateway = "http://192.168.5.188:8080"
    /// 令牌来自 Keychain（与设置页共享同一个持有者，见 KeychainTokenStore.swift）
    @ObservedObject private var tokenStore = GatewayToken.shared

    // ── 状态 1/2：搜索 ────────────────────────────────
    /// 搜索框内容
    @State private var query = ""
    /// 正在查询地点（tips / geocode / around）
    @State private var searching = false
    /// 输入提示的防抖任务
    @State private var tipsTask: Task<Void, Never>?
    /// 输入提示
    @State private var tips: [PlaceSuggestion] = []
    /// 地名解析结果（/geocode 多结果）
    @State private var geocoded: [GeocodeResult] = []
    /// 周边搜索结果（/around）
    @State private var nearby: [PlaceSuggestion] = []
    /// 当前生效的快捷筛选（nil = 没在用）
    @State private var quickFilter: String?

    // ── 通用 ────────────────────────────────────────
    @State private var errorText: String?
    /// 常用 + 最近目的地
    @State private var saved: [SavedDestination] = []
    /// 「结束导航」时是否一并结束行程记录（P2-3 解耦）
    @State private var endDrivingToo = false
    /**
     本次导航是否**由我们顺带**启动了驾驶会话。

     ⚠️ 与改造前保持一致的一条规则：**「开始导航」等同于「开始驾驶」** ——
     导航要定位、遥测要发速度，两者都归驾驶会话管。若不在这里启动，
     用户会看到"导航开始了但仪表上速度不动"。
     记住是谁开的，是为了结束导航时能对称收尾：驾驶本来是用户自己开的，
     就不要替他关掉。
     */
    @State private var startedDriving = false

    private static let quickFilters = ["加油站", "停车场", "充电站"]

    // MARK: 状态判定（纯函数，见 Nav/NavUIState.swift）

    private var uiState: NavUIState {
        NavUIStateLogic.state(
            isNavigating: session.nav.isNavigating,
            hasCandidates: !session.nav.routeCandidates.isEmpty,
            queryIsEmpty: query.trimmingCharacters(in: .whitespaces).isEmpty,
            isLoading: searching || session.nav.candidatesLoading,
            isChoosingRoute: session.nav.isChoosingRoute,
            /* 到达是 NavCore 判定的既成事实（连续确认后才置位）*/
            hasArrived: session.nav.snapshot.state == .arrived)
    }

    // MARK: - body

    var body: some View {
        NavigationView {
            Form {
                switch uiState {
                case .ready, .pickingPlace:
                    searchSections
                case .pickingRoute:
                    routeSection
                case .navigating:
                    navigatingSection
                case .arrived:
                    arrivalSection
                }
                if let e = errorText ?? session.nav.lastError {
                    Section { Text(e).font(.footnote).foregroundStyle(.red) }
                }
            }
            .navigationTitle(navigationTitle)
            .onAppear {
                /* 进入本页就把网关配置推给 NavSession（设置页也可能改过）*/
                session.nav.configure(gateway: navGateway, token: tokenStore.value)
                saved = SavedDestinationStore.load()
                refreshCurrentPlace()
                #if DEBUG
                /*
                 * 便于截图/复现：`-navQuery 北京南` 预填搜索框。
                 *
                 * ⚠️ 令牌**不再**能用 `-navToken xxx` 传入：它已从 UserDefaults
                 *    （有"参数域"这个后门）移到 Keychain，而 Keychain 不认启动参数。
                 *    调试时请在「设置」页填一次令牌（会持久化到 Keychain）。
                 *    网关地址仍是 `@AppStorage`，故 `-navGateway http://...` 依旧可用。
                 */
                let args = ProcessInfo.processInfo.arguments
                if let i = args.firstIndex(of: "-navQuery"), i + 1 < args.count, query.isEmpty {
                    query = args[i + 1]
                }
                #endif
            }
            .onChange(of: query) { _, v in scheduleTips(for: v) }
        }
        .navigationViewStyle(.stack)
    }

    private var navigationTitle: String {
        switch uiState {
        case .navigating: return "导航中"
        case .arrived: return "已到达"
        case .pickingRoute: return "选择路线"
        default: return "导航"
        }
    }

    // MARK: - 状态 1 & 2：准备 / 选地点

    @ViewBuilder
    private var searchSections: some View {
        // ── 搜索框 ─────────────────────────────────────
        Section {
            HStack {
                Image(systemName: "magnifyingglass").foregroundStyle(.secondary)
                TextField("搜索目的地，或输入 纬度,经度", text: $query)
                    .autocorrectionDisabled()
                if !query.isEmpty {
                    Button {
                        query = ""; tips = []; geocoded = []; nearby = []; tipsScopeNotice = nil
                        quickFilter = nil; errorText = nil
                    } label: { Image(systemName: "xmark.circle.fill") }
                    .buttonStyle(.plain)
                    .foregroundStyle(.secondary)
                }
            }
        }

        // ── 当前位置（P1-4：兼作定位自检）─────────────
        Section {
            HStack {
                Label(currentPlaceText, systemImage: "location.fill")
                    .font(.subheadline)
                Spacer()
                if session.nav.currentPositionWGS84 == nil {
                    Button("重试") { refreshCurrentPlace() }
                        .font(.footnote)
                }
            }
        } header: {
            Text("当前位置")
        } footer: {
            if session.nav.currentPositionWGS84 == nil {
                Text("定位不可用时无法规划路线。室内通常也能定位（iOS 会融合 Wi-Fi/基站），若一直失败请检查系统定位权限。")
            }
        }

        // ── 常用 / 最近（P2-2）────────────────────────
        if query.isEmpty && !saved.isEmpty {
            Section("常用 / 最近") {
                ForEach(saved) { d in
                    Button { pick(d.name, geo: d.geo) } label: {
                        HStack {
                            Image(systemName: d.isFavorite ? "star.fill" : "clock")
                                .foregroundStyle(d.isFavorite ? .yellow : .secondary)
                            Text(d.name).font(.subheadline)
                            Spacer()
                            Image(systemName: "chevron.right")
                                .font(.caption).foregroundStyle(.tertiary)
                        }
                    }
                    .swipeActions {
                        Button(role: .destructive) {
                            saved.removeAll { $0.id == d.id }
                            SavedDestinationStore.save(saved)
                        } label: { Label("删除", systemImage: "trash") }
                    }
                }
            }
        }

        // ── 结果列表（三类统一）───────────────────────
        let rows = placeRows
        if searching {
            Section { HStack { ProgressView(); Text("查询中…") } }
        } else if !rows.isEmpty {
            Section {
                ForEach(rows) { r in
                    Button { pick(r.name, geo: r.geo) } label: { rowView(r) }
                }
            } header: {
                Text(quickFilter == nil ? "搜索结果" : "附近的\(quickFilter!)")
            } footer: {
                /*
                 * 结果来自本市时**不提示**（那是默认预期，提示反而啰嗦）；
                 * 只有降级到省/全国时才说，避免用户以为"本市就这几条"。
                 */
                if let n = tipsScopeNotice {
                    Text(n).font(.footnote).foregroundStyle(.secondary)
                }
            }
        } else if !query.isEmpty && quickFilter == nil && errorText == nil {
            /*
             * 只有**确实查完且没有失败**时才说"没找到"。
             * 有失败时上面那个红色 Section 已经说清了原因 ——
             * 否则会把"网关连不上"伪装成"没有这个地方"。
             */
            Section {
                Text("没找到「\(query)」").font(.footnote).foregroundStyle(.secondary)
            }
        }

        // ── 快捷筛选（原「周边搜索」降级到此）─────────
        Section {
            HStack(spacing: 8) {
                ForEach(Self.quickFilters, id: \.self) { k in
                    Button {
                        quickFilter = (quickFilter == k) ? nil : k
                        if quickFilter != nil { Task { await searchNearby(k) } }
                        else { nearby = [] }
                    } label: {
                        Text(k)
                            .font(.footnote)
                            .padding(.horizontal, 10).padding(.vertical, 5)
                            .background(quickFilter == k ? Color.accentColor.opacity(0.25)
                                                         : Color.secondary.opacity(0.12))
                            .clipShape(Capsule())
                    }
                    .buttonStyle(.plain)
                }
            }
        } header: {
            Text("附近找")
        } footer: {
            Text("在当前位置附近按关键词找地点（网关 /around）。")
        }
    }

    private func rowView(_ r: NavPlaceRow) -> some View {
        HStack(alignment: .top) {
            VStack(alignment: .leading, spacing: 2) {
                Text(r.name).font(.subheadline)
                if !r.detail.isEmpty {
                    Text(r.detail).font(.caption).foregroundStyle(.secondary).lineLimit(1)
                }
            }
            Spacer()
            if let d = r.distanceM {
                Text(NavCandidateFormat.distanceText(d))
                    .font(.caption).foregroundStyle(.secondary).monospacedDigit()
            }
        }
    }

    /// 三类来源归一成一个列表（P1-2）
    private var placeRows: [NavPlaceRow] {
        var out: [NavPlaceRow] = []
        var seen = Set<String>()
        func add(_ r: NavPlaceRow) {
            guard !seen.contains(r.id) else { return }
            seen.insert(r.id)
            out.append(r)
        }
        for t in tips {
            add(NavPlaceRow(id: t.id, name: t.name, detail: t.address,
                            distanceM: distanceTo(t.geo), geo: t.geo))
        }
        for c in geocoded {
            let key = "\(c.name)|\(c.lon)|\(c.lat)"
            add(NavPlaceRow(id: key, name: c.name, detail: "",
                            distanceM: distanceTo(c.geo), geo: c.geo))
        }
        for p in nearby {
            add(NavPlaceRow(id: p.id, name: p.name, detail: p.address,
                            distanceM: p.distanceM ?? distanceTo(p.geo), geo: p.geo))
        }
        return out
    }

    // MARK: - 状态 3：选路线

    @ViewBuilder
    private var routeSection: some View {
        let cands = session.nav.routeCandidates
        /* 默认选中耗时最短的一条（列表已由网关按耗时排序）*/
        let fastestId = cands.min(by: { $0.durationS < $1.durationS })?.strategy

        Section {
            ForEach(cands) { c in
                Button {
                    beginNavigation(with: c)
                } label: {
                    HStack(alignment: .center, spacing: 10) {
                        Image(systemName: c.strategy == fastestId
                              ? "checkmark.circle.fill" : "circle")
                            .foregroundStyle(c.strategy == fastestId ? .green : .secondary)
                        VStack(alignment: .leading, spacing: 3) {
                            Text(NavCandidateFormat.primaryText(durationS: c.durationS))
                                .font(.headline).monospacedDigit()
                            Text(NavCandidateFormat.secondaryText(
                                    distanceM: c.distanceM, strategy: c.strategy,
                                    isFastest: c.strategy == fastestId))
                                .font(.footnote).foregroundStyle(.secondary)
                        }
                        Spacer()
                    }
                }
                .buttonStyle(.plain)
            }
        } header: {
            Text("选择路线")
        } footer: {
            Text("来自网关 /routes：用「速度优先 / 距离最短 / 不走高速」三个策略各取一条并去重，一次请求就拿到真正不同的几条路。")
        }

        /* 「开始导航」只在此状态出现 —— 按钮语义唯一（P0-2）*/
        Section {
            Button {
                if let fastest = cands.first(where: { $0.strategy == fastestId }) {
                    beginNavigation(with: fastest)
                }
            } label: {
                Label("开始导航", systemImage: "location.north.line").bold()
            }
            Button {
                session.nav.cancelChoosingRoute()
                session.nav.clearCandidates()
                errorText = nil
            } label: {
                Label(session.nav.isNavigating ? "保留当前路线" : "返回搜索",
                      systemImage: "chevron.left")
            }
        }
    }

    // MARK: - 状态 5：已到达（P0-2，导航流程的收尾）

    /**
     到达卡片。

     ## 为什么要有这个状态

     此前到达后界面仍停在「导航中」：继续显示"距下一动作 -- 米"、
     继续占着定位、也没有明确的结束入口 —— 用户不知道要不要手动点结束。
     导航流程**没有终点**，体验上像"没做完"。

     ## 设计取舍

     - **不自动结束**：用户可能还要"继续导航到附近"（比如停车场没找到），
       自动结束会把目的地一起清掉，反而更难操作。所以给两个明确选项。
     - **自动停导航指引但保留定位**：到了地方就不用再算偏航/重算了，
       但"继续导航到附近"还需要当前位置，所以不能连定位一起停。
     */
    @ViewBuilder
    private var arrivalSection: some View {
        Section {
            HStack(spacing: 12) {
                Image(systemName: "checkmark.circle.fill")
                    .font(.system(size: 38))
                    .foregroundStyle(.green)
                VStack(alignment: .leading, spacing: 3) {
                    Text("已到达目的地").font(.title3).bold()
                    if !query.isEmpty {
                        Text(query).font(.footnote).foregroundStyle(.secondary)
                    }
                }
            }
        }

        Section {
            Button {
                endNavigation()
            } label: {
                Label("结束行程", systemImage: "flag.checkered")
            }

            /*
             * "继续导航到附近"：把搜索框让出来，让用户直接搜下一个地方
             * （常见场景：目的地是商圈/景区，到了还要找具体停车位）。
             * 这里只退出到达态、**不清目的地**，用户重新搜索即可。
             */
            Button {
                session.nav.leaveArrivedState()
                query = ""
                tips = []; geocoded = []; nearby = []; tipsScopeNotice = nil
                errorText = nil
            } label: {
                Label("继续导航到附近", systemImage: "magnifyingglass")
            }
        } footer: {
            Text("导航指引已结束。选择「继续导航到附近」可以直接搜索下一个目的地。")
        }
    }

    // MARK: - 状态 4：导航中

    @ViewBuilder
    private var navigatingSection: some View {
        let s = session.nav.snapshot

        /*
         * 顶部：**下一动作**（最大）+ **剩余摘要**（次大）。
         *
         * P1-2 之前，剩余信息是 `.subheadline` + 灰色，层级被压得过低 ——
         * 而开车时被问得最多的就是"还有多久到"。现在：
         *   - 下一动作保持 title2（驾驶动作优先，不能弱化）
         *   - 剩余摘要提到 headline（醒目但不超过动作）
         * 两行都在，一眼可见，不用滚动。
         */
        Section {
            HStack(spacing: 12) {
                Image(systemName: Self.maneuverIcon(s.maneuver))
                    .font(.system(size: 34))
                    .frame(width: 44)
                VStack(alignment: .leading, spacing: 2) {
                    Text(s.distanceToManeuverM.map { "\($0) m" } ?? "--")
                        .font(.title2).bold().monospacedDigit()
                    Text(session.nav.statusText)
                        .font(.footnote).foregroundStyle(.secondary)
                }
            }
            /*
             * 剩余摘要独立成行（不塞进 VStack）——否则会被上面的 HStack
             * 挤成窄列，中文会折行。
             */
            if !s.arrived {
                Text(session.nav.remainingSummaryText)
                    .font(.headline).monospacedDigit()
            }
        }

        /*
         * 行程细节。
         *
         * ⚠️ 剩余距离与 ETA **已提到顶部摘要行**（P1-2），此处不再重复 ——
         *    同一屏出现两遍相同信息会让驾驶员多花时间分辨哪行是"当前"。
         *    这里只留"顶部没覆盖到"的内容：重新规划次数。
         */
        if session.nav.rerouteCount > 0 {
            Section("行程") {
                Text("已重新规划 \(session.nav.rerouteCount) 次")
                    .font(.footnote).foregroundStyle(.secondary)
            }
        }

        if session.nav.currentRoadName != nil || session.nav.trafficText != nil {
            Section("当前道路 / 路况") {
                if let r = session.nav.currentRoadName {
                    Label(r, systemImage: "road.lanes")
                }
                if let t = session.nav.trafficText {
                    Label(t, systemImage: "car.2")
                }
            }
        }

        Section {
            /* P2-4：不用停止导航就能换路线 */
            Button {
                session.nav.beginChoosingRoute()
                session.nav.clearCandidates()
                /* 换路线要重新请求网关：路况随时在变，旧候选可能已经堵了 */
                guard let dest = session.nav.currentDestinationGeo else {
                    errorText = "拿不到原目的地，无法重新规划"
                    return
                }
                Task { await loadCandidates(for: dest, name: query.isEmpty ? "目的地" : query) }
            } label: {
                Label("换一条路线", systemImage: "arrow.triangle.2.circlepath")
            }
        }

        Section {
            Toggle("同时结束行程记录", isOn: $endDrivingToo)
            Button(role: .destructive) {
                endNavigation()
            } label: {
                Label("结束导航", systemImage: "stop.circle")
            }
        } footer: {
            Text("不勾选「同时结束行程记录」时，只停止导航指引，仍会继续记录里程与车速。")
        }
    }

    private var currentPlaceText: String {
        if let p = currentPlaceName { return p }
        if session.nav.currentPositionWGS84 == nil { return "定位中…（点击右侧重试）" }
        return "已定位"
    }
    @State private var currentPlaceName: String?
    /**
     当前所在行政区（市/省）—— 用于把联想搜索限定在**本市优先**。

     来自同一次逆地理编码（`/regeo` 已返回 city/province，此前只取了显示文本
     而把这两个字段丢掉了）。拿不到时为空 Region，搜索会自动退化为全国。
     */
    @State private var currentRegion = SearchScopePolicy.Region()
    /// 本次联想结果来自哪一层（本市/本省/全国），用于界面提示
    @State private var tipsScopeNotice: String?

    // MARK: - 动作

    /// 取当前所在位置的可读名称（P1-4：让用户确认"我在哪"）
    private func refreshCurrentPlace() {
        Task {
            guard let p = await session.nav.ensurePositionWGS84() else { return }
            let r = try? await GatewayClient.fetchRegeo(
                gateway: navGateway, token: tokenStore.value, positionWGS84: p)
            await MainActor.run {
                errorText = nil
                let t = r?.displayText ?? ""
                currentPlaceName = t.isEmpty ? "已定位" : t
                /*
                 * 记下行政区，供联想搜索限定范围（本市→本省→全国）。
                 * `/regeo` 一直都返回了 city/province，此前只是没接住 ——
                 * 所以这次改造**不需要改网关**。
                 */
                currentRegion = SearchScopePolicy.Region(city: r?.city ?? "",
                                                         province: r?.province ?? "")
            }
        }
    }

    /**
     输入提示防抖（300ms）+ **地名解析回落**。

     ## 这里踩过两次坑，都记下来

     1. **`/geocode` 回落被漏掉**：重写界面时只保留了 `/tips`，
        `geocoded` 变成"只被清空、从不赋值"的死变量。而 `/tips` 对**门牌地址**
        之类关键词常常给不出结果（它偏 POI），于是用户看到"没找到" ——
        而他记得的"联想"恰恰是旧版点「开始导航」时那次 `/geocode`。
        **所以 tips 为空时必须回落到 geocode。**

     2. **`try?` 把所有错误吞了**：失败时 `tips` 只是空数组，
        界面显示"没找到「xxx」" —— **把网络/令牌/解码问题伪装成"没有这个地方"**。
        这类"把失败说成没结果"的写法最难查，一律改为**把原因显示出来**。
     */
    private func scheduleTips(for text: String) {
        tipsTask?.cancel()
        let q = text.trimmingCharacters(in: .whitespacesAndNewlines)
        quickFilter = nil
        nearby = []
        /* 太短不查（噪音大）；纯坐标("39.9,116.4")走另一条路，不查地名 */
        guard q.count >= 2, !q.contains(",") else {
            tips = []; geocoded = []; tipsScopeNotice = nil
            if errorText?.hasPrefix("查询") == true { errorText = nil }
            return
        }
        tipsTask = Task {
            try? await Task.sleep(nanoseconds: 300_000_000)
            if Task.isCancelled { return }

            /* 网关没配置：明确说出来，不要伪装成"没找到" */
            guard session.nav.isConfigured else {
                await MainActor.run {
                    tips = []; geocoded = []; tipsScopeNotice = nil
                    errorText = "网关未配置：请到「设置」填写地址与令牌"
                }
                return
            }

            var newTips: [PlaceSuggestion] = []
            var newGeo: [GeocodeResult] = []
            var failure: String?
            var notice: String?

            /*
             * ── 分层检索：本市 → 本省 → 全国 ──────────────────────
             *
             * 原先直接查全国，搜"人民路"会返回几十个城市的结果，
             * 用户本地要找的那条被淹没。改为**由近及远**：
             * 本层有结果就立即停（不再请求下一层），省配额也更快。
             *
             * 层级由 SearchScopePolicy 决定（含直辖市跳过省级等规则，
             * 已离线穷举单测）。这里的循环只负责"依次试、命中即止"。
             */
            for scope in SearchScopePolicy.scopes(for: currentRegion) {
                let cityParam = SearchScopePolicy.cityParameter(for: scope,
                                                                region: currentRegion)
                do {
                    newTips = try await GatewayClient.fetchInputTips(
                        gateway: navGateway, token: tokenStore.value,
                        keywords: q, city: cityParam)
                } catch {
                    if failure == nil { failure = "输入提示失败：\(error)" }
                    continue /* 本层失败 → 试下一层（全国兜底仍可能成功）*/
                }
                if Task.isCancelled { return }
                if SearchScopePolicy.shouldStop(after: scope, resultCount: newTips.count) {
                    /* 告诉用户"这是退而求其次的结果"，避免他以为本市只有这些 */
                    notice = SearchScopePolicy.notice(for: scope, resultCount: newTips.count)
                    break
                }
            }

            /* tips 给不出时回落到 /geocode（地址类关键词 tips 常为空）*/
            if newTips.isEmpty {
                do {
                    newGeo = try await GatewayClient.geocode(
                        gateway: navGateway, token: tokenStore.value, address: q)
                } catch {
                    if failure == nil { failure = "地名查询失败：\(error)" }
                }
            }

            if Task.isCancelled { return }
            await MainActor.run {
                tips = newTips
                geocoded = newGeo
                tipsScopeNotice = notice
                /* 两者都空时：**有失败就报失败**，真没结果才留空让界面说"没找到" */
                if newTips.isEmpty && newGeo.isEmpty { errorText = failure }
                else { errorText = nil }
            }
        }
    }

    /// 周边搜索
    private func searchNearby(_ keyword: String) async {
        errorText = nil
        searching = true
        defer { searching = false }
        guard let near = await session.nav.ensurePositionWGS84() else {
            errorText = session.nav.lastError ?? "拿不到定位，无法搜索附近"
            return
        }
        do {
            nearby = try await GatewayClient.fetchAround(
                gateway: navGateway, token: tokenStore.value, keywords: keyword, near: near)
            if nearby.isEmpty { errorText = "附近没找到「\(keyword)」" }
        } catch {
            errorText = "周边搜索失败：\(error)"
        }
    }

    /// 用户选定一个地点 → **自动进入选路线**（P0-3，不再要求额外确认）
    private func pick(_ name: String, geo: GeoPoint) {
        query = name
        tips = []; geocoded = []; nearby = []; quickFilter = nil; tipsScopeNotice = nil
        Task { await loadCandidates(for: geo, name: name) }
    }

    /// 取多方案路线
    private func loadCandidates(for destination: GeoPoint, name: String) async {
        errorText = nil
        session.nav.configure(gateway: navGateway, token: tokenStore.value)
        let n = await session.nav.loadRouteCandidates(destinationWGS84: destination)
        if n == 0 { errorText = session.nav.lastError }
    }

    /**
     开始导航：**同时**启动驾驶会话（若尚未启动）。

     顺序很重要：先确保驾驶会话起来（它负责定位与 BLE 遥测），再让导航核心
     开始跑 —— 与改造前 `beginNavigation` 的语义一致。
     */
    private func beginNavigation(with c: RouteCandidate) {
        if !session.isDriving {
            session.startDriving()
            startedDriving = true
        }
        endDrivingToo = startedDriving
        session.nav.start(with: c)
        rememberDestination(c)
    }

    /// 开始导航后把目的地记进「最近」（P2-2）
    private func rememberDestination(_ c: RouteCandidate) {
        let name = query.isEmpty ? "目的地" : query
        guard let geo = session.nav.currentDestinationGeo else { return }
        saved = SavedDestinationLogic.recording(
            saved, SavedDestination(name: name, lon: geo.lon, lat: geo.lat, isFavorite: false))
        SavedDestinationStore.save(saved)
    }

    private func endNavigation() {
        session.nav.stop()
        /*
         * 对称收尾：只在"本次驾驶是随导航一起启动的"、且用户确认要一起结束时
         * 才停驾驶。用户自己开的驾驶会话不替他关。
         */
        if endDrivingToo && startedDriving { session.stopDriving() }
        endDrivingToo = false
        startedDriving = false
        query = ""; tips = []; geocoded = []; nearby = []; quickFilter = nil; tipsScopeNotice = nil
        errorText = nil
    }

    private func distanceTo(_ geo: GeoPoint) -> Double? {
        guard let me = session.nav.currentPositionWGS84 else { return nil }
        return Self.haversineM(me, geo)
    }

    /// 两点球面距离（米）。用于列表里的"距我 x km"。
    static func haversineM(_ a: GeoPoint, _ b: GeoPoint) -> Double {
        let r = 6_371_000.0
        let dLat = (b.lat - a.lat) * .pi / 180
        let dLon = (b.lon - a.lon) * .pi / 180
        let la1 = a.lat * .pi / 180, la2 = b.lat * .pi / 180
        let h = sin(dLat / 2) * sin(dLat / 2)
              + cos(la1) * cos(la2) * sin(dLon / 2) * sin(dLon / 2)
        return 2 * r * asin(min(1, sqrt(h)))
    }

    static func maneuverIcon(_ m: Maneuver) -> String {
        switch m {
        case .left, .keepLeft:   return "arrow.turn.up.left"
        case .right, .keepRight: return "arrow.turn.up.right"
        case .uTurn:             return "arrow.uturn.left"
        case .arrive:            return "flag.checkered"
        case .straight:          return "arrow.up"
        default:                 return "location.north.line"
        }
    }
}

// MARK: - 常用 / 最近目的地的持久化（P2-2）

/// 简单的 `UserDefaults` 存储。
///
/// 只存本地：这是**自用**场景，跨设备同步没有价值，反而要引入账号体系。
enum SavedDestinationStore {
    private static let key = "navSavedDestinations"

    static func load() -> [SavedDestination] {
        guard let data = UserDefaults.standard.data(forKey: key) else { return [] }
        return (try? JSONDecoder().decode([SavedDestination].self, from: data)) ?? []
    }

    static func save(_ list: [SavedDestination]) {
        guard let data = try? JSONEncoder().encode(list) else { return }
        UserDefaults.standard.set(data, forKey: key)
    }
}
