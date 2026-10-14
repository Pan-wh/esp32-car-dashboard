// ContentView.swift —— 前台 MVP UI（连接 / 状态 / 驾驶按钮 / 调试值）
//
// 任务书 §5.2 最小范围：
//   连接页 / 状态区域（蓝牙 + 定位 + 最后数据时间）/ 按钮（连接、开始驾驶、结束驾驶）/ 调试值
// 要求：用户首次启动时先解释为何需要蓝牙与定位权限，不要在无解释的情况下连续弹权限。
import SwiftUI
import CoreLocation
import CoreBluetooth

struct ContentView: View {
    @StateObject private var session = DriveSessionManager()

    /// 表盘列表**来自设备上报**（不再硬编码）。
    ///
    /// 这样设备新增表盘时 App 无需改动；未收录的名字用设备给的 label 显示。
    /// 协议见 docs/ble.md 的「表盘列表上报」。
    private var deviceFaces: [DeviceFace] { session.ble.faces.faces }

    /// 设备实际存在的分组（通常 3 个：时钟 / 速度表 / 导航）
    private var groups: [String] { session.ble.faces.grouped.map { $0.group } }

    /// 当前正在显示的表盘 id（**以设备上报的 `cur` 为准**）。
    ///
    /// 为什么不存本地状态：设备可能因"隐藏了当前表盘"而自动切到别的表盘，
    /// 本地状态不会知道；只信本地就会出现"勾选指向一个已经不是当前的表盘"。
    private var currentFaceId: Int { session.ble.faces.currentId }

    /// **有效分组**：派生值，不靠 onChange 去"同步"。
    ///
    /// 之前把 selectedGroup 当独立状态、在 onChange 里跟上报对齐，结果
    /// 上报到达时选中值仍是空串 → 表盘列表**空白**（界面看起来像坏了）。
    /// 改为派生：选中值无效（空 / 设备改版后消失）就退回第一个分组，
    /// 单一数据源，不存在"两份状态对不上"的可能。
    private var effectiveGroup: String {
        if !selectedGroup.isEmpty && groups.contains(selectedGroup) { return selectedGroup }
        // 首次打开时优先显示**当前表盘所在的分组** —— 否则默认停在第一个分组，
        // 而当前表盘在别的分组里，用户会看不到 ✓ 标记、以为"没有当前表盘"。
        if let cur = deviceFaces.first(where: { $0.id == currentFaceId }) {
            return cur.group
        }
        return groups.first ?? ""
    }

    /// **只列当前分组**的表盘 —— 这是界面层级的关键：
    /// 一级是分组，二级才是组内表盘，不要把所有表盘平铺在一起。
    private var facesInSelectedGroup: [DeviceFace] {
        let g = effectiveGroup
        guard !g.isEmpty else { return [] }
        return deviceFaces.filter { $0.group == g }
    }
    /// 屏幕方向选项（度）—— 只列 90 的整数倍，与固件 control.c 的严格校验一致。
    ///
    /// 固件**按角度自动选最合适的旋转机制**（App 不需关心）：
    ///   - 0° / 180° → LVGL 软件旋转（路径便宜，实测最丝滑）
    ///   - 90° / 270° → **面板硬件旋转**（MADCTL）：LVGL 在这两个角度要做
    ///     4 向旋转，渲染耗时超过面板一帧（16.78 ms）→ 会撕裂，故改走硬件
    /// 方向持久化到仪表 NVS，重启后保持。
    private let rotations = [0, 90, 180, 270]
    @State private var selectedRotation = 0

    // ── 导航页的状态 ────────────────────────────────────────
    /// 网关地址/令牌用 @AppStorage 持久化：开发期反复改会烦，且退出 App 后应保留。
    /// 默认地址是开发机的局域网 IP —— 手机访问不到电脑的 127.0.0.1，必须用局域网地址。
    @AppStorage("navGateway") private var navGateway = "http://192.168.5.188:8080"
    @AppStorage("navToken") private var navToken = ""
    /// 目的地坐标输入（中文地名或"纬度,经度"，WGS84）。不持久化：每次目的地都不同。
    @State private var navDestination = ""
    /// 坐标格式错误 / 查询失败 / 无结果的提示
    @State private var navInputError: String?
    /// 地名查询命中的候选（多个时让用户选）
    @State private var navCandidates: [GeocodeResult] = []
    /// 是否正在查询地名（禁用按钮、显示进度）
    @State private var navLookingUp = false

    /// 当前选中的**分组**（不是表盘）。分组来自设备上报。
    ///
    /// 注：「当前表盘」**不设本地状态** —— 它同样以设备上报的 `cur` 为准
    /// （见 `currentFaceId`）。之前用 @State + onChange 同步，结果 onChange
    /// 在上报到达时没触发，勾选标记指不到当前表盘。
    /// 教训一致：**能从数据派生的，就不要另存一份状态去同步**。
    @State private var selectedGroup = ""
    @State private var brightness: Double = 200
    @State private var screenOn = true
    /// 防止 onAppear 重复触发自动连接（SwiftUI 可能多次调用）
    @State private var didAutoConnect = false
    /// 当前标签页（0 仪表 / 1 数据 / 2 导航）
    @State private var selectedTab = 0

    var body: some View {
        // 三个标签页：配置仪表 / 观察数据 / 导航（预留）
        //
        // 为什么分页：此前所有内容挤在一页，开车时要看的"速度、航向"被
        // 亮度、表盘开关等**配置项**挤到下面，每次都得滚动。
        // 按"配置"与"观察"分成两页后，各页都短到一屏可见。
        TabView(selection: $selectedTab) {
            instrumentTab
                .tag(0)
                .tabItem { Label("仪表", systemImage: "gauge.medium") }
            dataTab
                .tag(1)
                .tabItem { Label("数据", systemImage: "location.fill") }
            navTab
                .tag(2)
                .tabItem { Label("导航", systemImage: "location.north.line") }
        }
        /*
         * App 启动即尝试连接仪表（#bug 修复）：
         * 原先只有点「开始驾驶」才会 scanAndConnect，导致重开 App 后
         * BLE 状态一直停留在初始态、必须手动点一下才刷新。
         * 这里在界面出现时自动扫描一次，用户无需先开始驾驶。
         */
        .onAppear {
            #if DEBUG
            // 便于截图/验收：-tab 0|1|2 指定初始标签页（正常运行由用户点切换）
            let args = ProcessInfo.processInfo.arguments
            if let i = args.firstIndex(of: "-tab"), i + 1 < args.count,
               let t = Int(args[i + 1]), (0...2).contains(t) {
                selectedTab = t
            }
            if BLEClient.demoModeEnabled && !session.ble.facesReady {
                session.ble.loadDemoFaces()
            }
            #endif
            guard !didAutoConnect else { return }
            didAutoConnect = true
            session.ble.scanAndConnect()
        }
    }

    // MARK: - 标签页 1：仪表（配置：连接 / 屏幕 / 表盘）

    private var instrumentTab: some View {
        NavigationView {
            ScrollViewReader { proxy in
                Form {
                    permissionSection
                    connectionSection
                    screenSection
                    faceSection
                }
                .navigationTitle("仪表")
                .toolbar { connectionIndicator }
                #if DEBUG
                /*
                 * 演示模式自动滚动到表盘分组。
                 * 目的：模拟器不支持蓝牙、连不上真机，若还要手动滚动才能看到
                 * 「分组 + 开关」，这个演示模式就失去了"验收 UI"的意义。
                 * 仅 Debug 构建 + -demoFaces 启动参数时生效。
                 */
                .onAppear {
                    // 只演示"表盘"这一屏时自动滚过去；看别的标签页时不要乱滚
                    guard BLEClient.demoModeEnabled, selectedTab == 0 else { return }
                    DispatchQueue.main.asyncAfter(deadline: .now() + 1.2) {
                        withAnimation { proxy.scrollTo("faceGroupsAnchor", anchor: .top) }
                    }
                }
                #endif
            }
        }
        .navigationViewStyle(.stack)
    }

    // MARK: - 标签页 2：数据（观察：发送状态 / 定位 / 诊断）

    private var dataTab: some View {
        NavigationView {
            Form {
                sendStatusSection
                liveDataSection
            }
            .navigationTitle("数据")
            .toolbar { connectionIndicator }
        }
        .navigationViewStyle(.stack)
    }

    // MARK: - 标签页 3：导航

    private var navTab: some View {
        NavigationView {
            Form {
                // ── 实时状态 ─────────────────────────────────
                Section("导航状态") {
                    HStack {
                        Label(session.nav.statusText,
                              systemImage: navStateIcon)
                        Spacer()
                        if session.nav.isNavigating {
                            Text("\(Int(session.nav.snapshot.remainingDistanceM)) m")
                                .font(.footnote).foregroundStyle(.secondary)
                        }
                    }
                    if let s = session.nav.routeSummary {
                        Text("全程 \(s)").font(.footnote).foregroundStyle(.secondary)
                    }
                    if session.nav.rerouteCount > 0 {
                        Text("已重新规划 \(session.nav.rerouteCount) 次")
                            .font(.footnote).foregroundStyle(.secondary)
                    }
                    if let e = session.nav.lastError {
                        Text(e).font(.footnote).foregroundStyle(.red)
                    }
                }

                // ── 目的地 ───────────────────────────────────
                Section {
                    TextField("地名（中文）或 纬度,经度", text: $navDestination)
                        .autocorrectionDisabled()
                    if !navCandidates.isEmpty {
                        // 地名可能命中多个（如"外滩"在多个城市）→ 让用户选
                        ForEach(navCandidates) { c in
                            Button {
                                navCandidates = []
                                navDestination = c.name
                                session.nav.start(destinationWGS84: c.geo)
                            } label: {
                                VStack(alignment: .leading, spacing: 2) {
                                    Text(c.name).font(.subheadline)
                                    Text(String(format: "%.5f, %.5f", c.lat, c.lon))
                                        .font(.caption).foregroundStyle(.secondary)
                                }
                            }
                        }
                    }
                    if let e = navInputError {
                        Text(e).font(.footnote).foregroundStyle(.red)
                    }
                } header: {
                    Text("目的地")
                } footer: {
                    Text("两种都支持：\n• 中文地名，如「北京南站」—— 会向网关查询坐标\n• 经纬度，如 39.9087,116.3974（**WGS84**）\n\n查询到的坐标由网关转成高德的 GCJ-02，无需你操心。")
                }

                // ── 开始 / 停止 ──────────────────────────────
                Section {
                    if session.nav.isNavigating {
                        Button(role: .destructive) {
                            session.nav.stop()
                        } label: {
                            Label("停止导航", systemImage: "stop.circle")
                        }
                    } else {
                        Button {
                            startNavigation()
                        } label: {
                            Label("开始导航", systemImage: "location.north.line")
                        }
                        .disabled(!session.bleConnected)
                    }
                    if !session.bleConnected {
                        Text("仪表未连接：请先在「仪表」页连接，导航指引才能下发。")
                            .font(.footnote).foregroundStyle(.secondary)
                    }
                }

                // ── 网关（开发期配置）────────────────────────
                Section {
                    TextField("网关地址", text: $navGateway)
                        .autocorrectionDisabled()
                        .textInputAutocapitalization(.never)
                        .keyboardType(.URL)
                    SecureField("网关令牌", text: $navToken)
                    Text("令牌用于向网关证明请求来自本机（网关要求认证，CORS 不是认证）。")
                        .font(.footnote).foregroundStyle(.secondary)
                } header: {
                    Text("网关")
                } footer: {
                    Text("开发期指向你电脑上的 gateway；手机与电脑需在同一局域网。\n默认 http://192.168.5.188:8080 —— 换成你电脑当前的局域网 IP。")
                }
                .onChange(of: navGateway) { _, v in
                    session.nav.configure(gateway: v, token: navToken)
                }
                .onChange(of: navToken) { _, v in
                    session.nav.configure(gateway: navGateway, token: v)
                }
            }
            .navigationTitle("导航")
            .toolbar { connectionIndicator }
            .onAppear {
                // 界面出现时把已保存的网关配置推给 NavSession（否则首次启动是空的）
                session.nav.configure(gateway: navGateway, token: navToken)
            }
        }
        .navigationViewStyle(.stack)
    }

    /// 导航状态对应的图标（一眼能看出是"在导航"还是"偏航/到达"）
    private var navStateIcon: String {
        switch session.nav.snapshot.state {
        case .active:    return session.nav.snapshot.offRoute ? "exclamationmark.triangle" : "location.north.line"
        case .rerouting: return "arrow.triangle.2.circlepath"
        case .arrived:   return "checkmark.circle"
        case .stale:     return "wifi.exclamationmark"
        case .idle, .noData: return "location.slash"
        }
    }

    /// 解析并开始导航。输入可以是：
    ///   - "纬度,经度"（WGS84）→ 直接开始
    ///   - 中文地名 → 先向网关查坐标；命中 1 个直接开始，多个则列出让用户选
    private func startNavigation() {
        let text = navDestination.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !text.isEmpty else {
            navInputError = "请填写目的地"
            return
        }
        session.nav.configure(gateway: navGateway, token: navToken)

        // 先按"纬度,经度"试解析
        let parts = text.split(separator: ",").map { $0.trimmingCharacters(in: .whitespaces) }
        if parts.count == 2, let lat = Double(parts[0]), let lon = Double(parts[1]),
           (-90...90).contains(lat), (-180...180).contains(lon) {
            navInputError = nil
            navCandidates = []
            session.nav.start(destinationWGS84: GeoPoint(lon: lon, lat: lat))
            return
        }

        // 否则当地名去查
        navInputError = nil
        navCandidates = []
        navLookingUp = true
        Task {
            defer { navLookingUp = false }
            do {
                let rs = try await session.nav.lookup(address: text)
                if rs.isEmpty {
                    navInputError = "没找到「\(text)」。换个说法，或直接填「纬度,经度」。"
                } else if rs.count == 1 {
                    navDestination = rs[0].name
                    session.nav.start(destinationWGS84: rs[0].geo)
                } else {
                    navCandidates = rs // 多个候选 → 界面列出让用户点选
                }
            } catch {
                navInputError = "查询失败：\(error)"
            }
        }
    }

    // MARK: - 各节内容

    @ViewBuilder
    private var screenSection: some View {
        // ── 全局显示设置 ────────────────────────────────────
        // 旋转 / 亮度 / 息屏都是**整机**行为，对所有表盘生效，
        // 不属于某个表盘 —— 因此独立成节，不要放进「表盘」里，
        // 否则会让人以为"只影响当前那个表盘"。
        Section {
            Picker("屏幕方向", selection: $selectedRotation) {
                ForEach(rotations, id: \.self) { deg in
                    Text("\(deg)°").tag(deg)
                }
            }
            .pickerStyle(.segmented)
            .onChange(of: selectedRotation) { _, deg in
                _ = session.ble.setRotation(deg)
            }
            // 备注（用户要求写在这里）：说明 90°/270° 为何与另两个角度走不同机制。
            //
            // 措辞依据**固件当前实际行为**（dashboard.ino 的 set_display_rotation）：
            //   const bool hw = (deg == 90 || deg == 270);
            // 即只有 90°/270° 走面板硬件旋转，0°/180° 仍走 LVGL。
            // ⚠️ 不要写"若固件是旧版本会撕裂"这类话 —— 固件已修复，那样只会让人误解。
            Text("90° / 270° 由面板硬件完成旋转（MADCTL），不会撕裂；0° / 180° 由 LVGL 完成，渲染开销最低。方向保存在仪表中，重启后保持。")
                .font(.footnote)
                .foregroundStyle(.secondary)

            HStack {
                Text("亮度")
                Slider(value: $brightness, in: 1...255, step: 1) { editing in
                    // 拖动结束才发送，避免高频写 BLE
                    if !editing { _ = session.ble.setBrightness(Int(brightness)) }
                }
                Text("\(Int(brightness))").monospacedDigit().frame(width: 34)
            }

            Toggle("屏幕点亮", isOn: $screenOn)
                .onChange(of: screenOn) { _, on in
                    _ = session.ble.setScreen(on: on)
                }
        } header: {
            Text("屏幕显示")
        } footer: {
            Text("对所有表盘生效，不是单个表盘的设置。")
        }
    }

    @ViewBuilder
    private var faceSection: some View {
        // ── 表盘：先选分组，再看该分组内的表盘 ──────────────
        //
        // 层级：分组（时钟/速度表/导航…）→ 组内表盘
        // 分组来自设备上报的 group 字段，只列出设备实际有的分组。
        Section {
            if deviceFaces.isEmpty {
                Text(session.ble.isConnected
                     ? "尚未读到设备的表盘列表"
                     : "表盘列表读取中…（连接仪表后自动获取）")
                    .font(.footnote).foregroundColor(.secondary)
            } else {
                Color.clear.frame(height: 0).id("faceGroupsAnchor")

                // 一级：分组（设备实际有的，通常 3 个）
                Picker("分组", selection: Binding(
                    get: { effectiveGroup },
                    set: { selectedGroup = $0 }
                )) {
                    ForEach(groups, id: \.self) { g in
                        Text(FaceDisplay.groupTitle(g)).tag(g)
                    }
                }
                .pickerStyle(.segmented)

                // 二级：**只列出当前分组**的表盘
                // 行布局：左=表盘图标（区分是哪种表盘）｜中=名称
                //         右=「切到这个表盘」按钮 + 「是否在滑动序列中」开关
                ForEach(facesInSelectedGroup) { f in
                    faceRow(f)
                }

                if let notice = session.ble.lastNotice {
                    Text(notice).font(.footnote).foregroundColor(.secondary)
                }
            }
        } header: {
            Text("表盘")
        } footer: {
            if !deviceFaces.isEmpty {
                VStack(alignment: .leading, spacing: 4) {
                    Text("→ 切到该表盘（✓ 为当前显示的）；开关控制是否出现在滑动序列中。")
                    if session.ble.faces.enabledCount <= 1 {
                        Text("至少保留一个表盘，最后一个无法隐藏。")
                    }
                    if session.ble.faces.truncated {
                        Text("设备上报的列表不完整，部分开关可能显示不准。")
                            .foregroundColor(.orange)
                    }
                }
            }
        }
    }


    @ViewBuilder
    private var permissionSection: some View {
        if session.needsPermissionExplanation {
            Section("为什么需要权限") {
                Text("定位：驾驶会话中读取车速、航向与位置，用于车载仪表显示。")
                Text("蓝牙：与车载仪表（CarDashboard）连接并发送这些数据。")
                Text("数据只在本机与仪表之间通过蓝牙传输，不上传云端。")
                    .font(.footnote).foregroundColor(.secondary)
                Button("我了解了，申请权限") { session.requestPermissions() }
            }
        }
    }

    /// 连接 + 开始驾驶。
    ///
    /// 「开始驾驶」放在这里（而不是数据页）：它是**动作**不是数据，
    /// 与"连接"同页才能形成 连接 → 开始驾驶 的顺畅流程。
    /// 发送统计属于"观察"，放在数据页。
    @ViewBuilder
    private var connectionSection: some View {
        Section {
            HStack {
                Text("BLE 状态")
                Spacer()
                Text(session.ble.stateText).foregroundColor(bleConnected ? .green : .orange)
            }
            if let notice = session.ble.lastNotice {
                Text(notice).font(.footnote).foregroundColor(.secondary)
            }
            HStack {
                Text("定位权限")
                Spacer()
                Text(locationPermissionText)
            }
            Button(bleConnected ? "已连接 CarDashboard" : "连接仪表") {
                session.ble.scanAndConnect()
            }
            .disabled(bleConnected)

            Button(session.isDriving ? "结束驾驶" : "开始驾驶") {
                if session.isDriving { session.stopDriving() }
                else { session.requestPermissions(); session.startDriving() }
            }
            .foregroundColor(session.isDriving ? .red : .blue)
        } header: {
            Text("仪表连接")
        } footer: {
            Text("开始驾驶后，本机会持续把定位与车速通过蓝牙发送给仪表。")
        }
    }

    /// 发送状态（原「驾驶会话」的统计部分）
    @ViewBuilder
    private var sendStatusSection: some View {
        Section("发送状态") {
            row("累计发送", "\(session.sentCount) 包")
            row("最近发送", timeText(session.lastSendTime))
            row("定位数据时间", timeText(session.lastLocationTime))
            if !session.isDriving {
                Text("未在发送：到「仪表」页点开始驾驶。")
                    .font(.footnote).foregroundColor(.secondary)
            }
        }
    }

    /// 实时定位：**只放开车时真正要看的**（速度 / 航向 / 精度）。
    /// 质量门槛、原始值、经纬度等排查用信息收进「诊断详情」，默认收起。
    @ViewBuilder
    private var liveDataSection: some View {
        Section("实时数据") {
            if let loc = session.lastLocation {
                let speedKmh = loc.speed < 0 ? nil : loc.speed * 3.6
                row("速度", speedKmh.map { String(format: "%.1f km/h", $0) } ?? "--")
                row("航向", loc.course >= 0 ? String(format: "%.0f°", loc.course) : "--")
                row("定位精度", String(format: "±%.0f m", loc.horizontalAccuracy))

                DisclosureGroup("诊断详情") {
                    // ---- 诊断：速度为何被判无效（§5 质量门槛）----
                    // speed_valid 由 TelemetryQuality.isSpeedUsable() 决定，
                    // 关键门槛：speed >= 0 且 speedAccuracy <= 2.0 m/s
                    let speedOK = TelemetryQuality.isSpeedUsable(loc)
                    let locOK = TelemetryQuality.isLocationUsable(loc)
                    row("速度质量", speedOK ? "通过" : "被拦（不发送）")
                    if #available(iOS 17.0, *) {
                        let sa = loc.speedAccuracy
                        row("测速精度", sa >= 0 ? String(format: "±%.2f m/s", sa) : "未知（负值）")
                    }
                    row("定位质量", locOK ? "通过" : "被拦（不发送）")
                    row("speed 原始值", String(format: "%.2f m/s", loc.speed))
                    row("纬度", String(format: "%.4f", loc.coordinate.latitude))
                    row("经度", String(format: "%.4f", loc.coordinate.longitude))
                    row("高度", String(format: "%.0f m", loc.altitude))
                }
                .font(.subheadline)
            } else {
                Text("等待定位数据…").foregroundColor(.secondary)
            }
            if let err = session.locationError {
                Text(err).foregroundColor(.red)
            }
        }
    }

    /// 连接状态指示器（三个标签页都显示）。
    ///
    /// 为什么每页都要：数据是否有效**完全取决于连接**。
    /// 若只在「仪表」页显示，用户在「数据」页看到速度不动时，
    /// 无法判断是"没定位"还是"压根没连上"，会白排查。
    @ToolbarContentBuilder
    private var connectionIndicator: some ToolbarContent {
        ToolbarItem(placement: .navigationBarTrailing) {
            HStack(spacing: 5) {
                Circle()
                    .fill(bleConnected ? Color.green : Color.orange)
                    .frame(width: 8, height: 8)
                Text(bleConnected ? "已连接" : "未连接")
                    .font(.caption)
                    .foregroundColor(.secondary)
            }
        }
    }

    /// 单个表盘行。
    ///
    /// 布局取舍（按用户反馈调整）：
    ///   - **左侧放表盘图标**，不是勾选标记 —— 勾选放在名称前既占位又不表意，
    ///     图标才能一眼区分"这是哪种表盘"。
    ///   - **切换**与**显隐**是两个动作，各自有明确控件：
    ///       右侧「→ / ✓」按钮 = 切到这个表盘（✓ 表示已经是当前，置灰不可点）
    ///       最右开关        = 是否出现在滑动序列中
    ///     之前把"切换"藏在"点名称"里，不显眼也不易发现。
    @ViewBuilder
    private func faceRow(_ f: DeviceFace) -> some View {
        let title = FaceDisplay.faceTitle(name: f.name, fallbackLabel: f.label)
        let isCurrent = (f.id == currentFaceId)

        HStack(spacing: 12) {
            // 左：表盘图标（隐藏时同时变淡，一眼看出不在序列里）
            Image(systemName: FaceDisplay.faceSymbol(name: f.name))
                .font(.system(size: 17, weight: .medium))
                .foregroundColor(f.enabled ? .accentColor : .secondary)
                .frame(width: 34, height: 34)
                .background(
                    RoundedRectangle(cornerRadius: 9)
                        .fill(Color.accentColor.opacity(f.enabled ? 0.14 : 0.05))
                )
                .accessibilityHidden(true)

            Text(title)
                .foregroundColor(f.enabled ? .primary : .secondary)

            Spacer(minLength: 8)

            // 右一：切到这个表盘。已在当前 / 已隐藏时禁用（点它没有意义）
            Button {
                // 不设本地状态：切完由设备重新上报 cur，界面自然收敛到真值
                _ = session.ble.setFace(f.id)
            } label: {
                Image(systemName: isCurrent ? "checkmark.circle.fill" : "arrow.right.circle")
                    .font(.system(size: 21))
                    .foregroundColor(isCurrent ? .accentColor : .secondary)
            }
            .buttonStyle(.plain)
            .disabled(!f.enabled || isCurrent)
            .accessibilityLabel(isCurrent ? "\(title)（当前显示的）" : "切换到\(title)")

            // 右二：是否出现在滑动序列中
            Toggle("", isOn: Binding(
                get: { f.enabled },
                set: { on in _ = session.ble.setFaceEnabled(f.id, on) }
            ))
            .labelsHidden()
            .accessibilityLabel("在滑动序列中显示\(title)")
        }
    }

    private func row(_ label: String, _ value: String) -> some View {
        HStack { Text(label); Spacer(); Text(value).monospacedDigit() }
    }

    private func timeText(_ d: Date?) -> String {
        guard let d = d else { return "--" }
        let f = DateFormatter()
        f.dateFormat = "HH:mm:ss"
        return f.string(from: d)
    }

    private var bleConnected: Bool { session.bleConnected }
    private var locationPermissionText: String {
        switch session.locationStatus {
        case .authorizedAlways: return "始终"
        case .authorizedWhenInUse: return "使用期间"
        case .denied: return "已拒绝"
        case .restricted: return "受限"
        case .notDetermined: return "待授权"
        @unknown default: return "未知"
        }
    }
}
