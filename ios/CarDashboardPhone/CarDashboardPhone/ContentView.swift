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

    // ── 导航网关（设置页与导航页共用同一个 @AppStorage 键）──────
    /// 用 @AppStorage 持久化：开发期反复改会烦，且退出 App 后应保留。
    /// 默认地址是开发机的局域网 IP —— 手机访问不到电脑的 127.0.0.1，必须用局域网地址。
    @AppStorage("navGateway") private var navGateway = "http://192.168.5.188:8080"
    @AppStorage("navToken") private var navToken = ""
    /// 「测试连接」的结果文字（nil = 还没测过）
    @State private var gatewayTestResult: String?

    /// 当前选中的**分组**（不是表盘）。分组来自设备上报。
    ///
    /// 注：「当前表盘」**不设本地状态** —— 它同样以设备上报的 `cur` 为准
    /// （见 `currentFaceId`）。之前用 @State + onChange 同步，结果 onChange
    /// 在上报到达时没触发，勾选标记指不到当前表盘。
    /// 教训一致：**能从数据派生的，就不要另存一份状态去同步**。
    @State private var selectedGroup = ""
    /**
     亮度 / 息屏：**以设备上报为准**，这里只是"设备还没上报时的暂存值"。

     ⚠️ 不要把它当成"App 记住的用户设置"：设备才是亮度/息屏的所有者
        （App 关掉、仪表单独上电都要保持）。设备一上报就覆盖这里。

     之前直接用 `@State brightness = 200` 当初始显示值 → 重开 App 显示 200
     （并非设备真值），用户一碰滑块还把设备改成 200。现在改为：
     未连接时用本地暂存值显示，连上后立刻被设备上报刷新。
     */
    @State private var brightness: Double = 200
    /**
     演示速度（仅用于"看速度表盘的动画"）。

     ⚠️ 它是**演示**，不是数据通路：固件会显示 DEMO 标记，且收到真实遥测
        或 BLE 断开就自动退出。这里不持久化 —— 每次进入 App 都从"未演示"开始。
     */
    @State private var demoSpeed: Double = 0
    @State private var demoOn = false
    /**
     上次把演示速度发给仪表的时刻（毫秒）。

     ⚠️ 拖动时 `onChange` 会**按刻度**触发（一秒几十次），每次都写 BLE 会让
        连接层来不及发（写入被丢弃/合并），表现就是"演示速度卡顿、跟不上手"。
        所以拖动期间**限频**，松手时再补发一次末值确保一定送达。
     */
    @State private var lastDemoSendMs: UInt64 = 0
    /// 演示速度的发送间隔（毫秒）。100ms ≈ 10Hz，比人手拖动平滑。
    private static let demoSendIntervalMs: UInt64 = 100
    @State private var screenOn = true
    /// 屏幕方向：同上，设备上报为准（设备侧持久化在 NVS，重开 App 也要显示真值）
    @State private var selectedRotation = 0
    /**
     正在把**设备上报的值**写进上面的本地显示状态。

     ⚠️ 必须有这个标志：写入会触发 `onChange`，若不拦截就会把刚收到的值
        当成"用户操作"**再发回设备**（无效往返；更糟的是可能形成回环）。
        判断依据是"这次变化来自设备还是来自用户"，而不是比较数值 ——
        比较数值在"用户改成与设备相同的值"时会漏发。
     */
    @State private var applyingDeviceState = false
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
            NavTabView(session: session)
                .tag(2)
                .tabItem { Label("导航", systemImage: "location.north.line") }
            settingsTab
                .tag(3)
                .tabItem { Label("设置", systemImage: "gearshape") }
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
            #if DEBUG
            /*
             * ⚠️ 必须在 scanAndConnect() **之后**：那个函数会把 autoConnectEnabled
             *    置回 true（"App 启动应当自动连接"），先设就会被它覆盖掉，
             *    界面看起来跟没断过一样 —— 演示模式也就失去意义。
             */
            if BLEClient.demoDisconnectedEnabled { session.ble.enterDemoDisconnectedState() }
            #endif
        }
        /*
         * 设备上报到达 → 用**设备的真值**刷新亮度/方向/息屏的显示。
         *
         * 这是本次修复的核心：此前这三项用 `@State` 的凭空默认值，App 重开
         * 显示 200（不是设备的真实亮度），用户一碰滑块还把设备改成 200。
         * 改为"设备上报 → App 显示"后，重开 App 看到的就是设备上的真值。
         *
         * 只在设备**确实上报了**该字段时才覆盖（nil 表示设备没说，保持现值）。
         */
        .onChange(of: session.ble.faces) { _, report in
            /*
             * 标记"这批变化来自设备"，让下面的 onChange 不要把它们当成
             * 用户操作再发回设备（否则就是收到什么就回发什么的无效往返）。
             */
            applyingDeviceState = true
            if let b = report.brightness { brightness = Double(b) }
            if let r = report.rotation { selectedRotation = r }
            if let s = report.screenOn { screenOn = s }
            /*
             * SwiftUI 的 onChange 在**本次视图更新批次内**同步触发，因此这里
             * 复位是安全的（下面的回调已经跑完）。用 async 再兜一层，
             * 防止将来把同步改成异步时悄悄失效。
             */
            DispatchQueue.main.async { applyingDeviceState = false }
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

    /**
     测一下网关通不通。

     为什么值得做成按钮：手机连不上电脑的原因有好几种（不在同一 Wi-Fi、
     令牌写错、网关只监听 127.0.0.1、网关没启动），而它们的**界面表现完全一样**
     —— 都是"什么都搜不到"。调用 `/health` 能把它们区分开，而且**不消耗高德额度**。
     */
    private func testGateway() async {
        let base = navGateway.hasSuffix("/") ? String(navGateway.dropLast()) : navGateway
        guard let url = URL(string: base + "/health") else {
            gatewayTestResult = "❌ 网关地址不合法：\(navGateway)"
            return
        }
        var req = URLRequest(url: url)
        req.timeoutInterval = 8
        if !navToken.isEmpty { req.setValue("Bearer \(navToken)", forHTTPHeaderField: "authorization") }
        do {
            let (data, resp) = try await URLSession.shared.data(for: req)
            let code = (resp as? HTTPURLResponse)?.statusCode ?? 0
            if code == 200 {
                let t = String(data: data, encoding: .utf8) ?? ""
                gatewayTestResult = "✅ 网关可达（HTTP 200）\(t.isEmpty ? "" : "\n\(t.prefix(160))")"
            } else if code == 401 || code == 403 {
                gatewayTestResult = "❌ 网关拒绝（HTTP \(code)）：令牌不对。请核对网关 .env 里的 GATEWAY_TOKEN"
            } else {
                gatewayTestResult = "❌ 网关返回 HTTP \(code)"
            }
        } catch {
            gatewayTestResult = "❌ 连不上网关：\(error.localizedDescription)\n请确认：① 手机与电脑在同一 Wi-Fi ② 网关已启动 ③ 网关监听 0.0.0.0（不是 127.0.0.1） ④ 地址填的是电脑的局域网 IP"
        }
    }

    // MARK: - 标签页 4：设置（开发期配置不占主流程）

    /**
     设置页。

     ⚠️ 为什么把网关配置挪到这里（P2-1）：
     它是**开发期一次性配置**，却在改造前占着导航页整整一个 Section ——
     用户每次导航都要从它旁边划过去。设置页只在需要时打开。
     */
    private var settingsTab: some View {
        NavigationView {
            Form {
                Section {
                    TextField("网关地址", text: $navGateway)
                        .autocorrectionDisabled()
                        .textInputAutocapitalization(.never)
                        .keyboardType(.URL)
                    SecureField("网关令牌", text: $navToken)
                } header: {
                    Text("导航网关")
                } footer: {
                    Text("开发期指向你电脑上的 gateway；手机与电脑需在同一局域网。\n默认 http://192.168.5.188:8080 —— 换成你电脑当前的局域网 IP。\n\n⚠️ 网关必须监听 0.0.0.0（默认只监听 127.0.0.1，手机访问不到）。")
                }
                .onChange(of: navGateway) { _, v in
                    session.nav.configure(gateway: v, token: navToken)
                }
                .onChange(of: navToken) { _, v in
                    session.nav.configure(gateway: navGateway, token: v)
                }

                Section {
                    /*
                     * ⚠️ 这两行是**排查必需**：手机连不上网关时，界面若只显示
                     * "没找到地点"，用户完全无从判断是"没这个地方"还是
                     * "令牌没填" / "不在同一 Wi-Fi"。把事实摆出来。
                     */
                    LabeledContent("令牌",
                                   value: navToken.isEmpty
                                          ? "未填写" : "已填写（\(navToken.count) 字符）")
                    LabeledContent("导航会话",
                                   value: session.nav.isNavigating ? "导航中" : "空闲")
                    Button {
                        Task { await testGateway() }
                    } label: {
                        Label("测试连接", systemImage: "antenna.radiowaves.left.and.right")
                    }
                    if let r = gatewayTestResult {
                        Text(r)
                            .font(.footnote)
                            .foregroundStyle(r.hasPrefix("✅") ? .green : .red)
                    }
                } header: {
                    Text("状态")
                } footer: {
                    Text("「测试连接」会调用网关的 /health（**不消耗高德额度**），用来确认手机与电脑在同一局域网、且令牌正确。")
                }
            }
            .navigationTitle("设置")
            .toolbar { connectionIndicator }
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
                // 来自设备上报的同步 → 不要回发给设备（那是无效往返）
                guard !applyingDeviceState else { return }
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
                    /*
                     * 拖动结束才发送，避免高频写 BLE。
                     * ⚠️ 必须排除"设备上报同步"引起的变化，否则收到一次上报
                     *    就会回发一次亮度命令。
                     */
                    if !editing && !applyingDeviceState {
                        _ = session.ble.setBrightness(Int(brightness))
                    }
                }
                Text("\(Int(brightness))").monospacedDigit().frame(width: 34)
            }

            /*
             * ── 演示：模拟速度测试（用户需求）──
             *
             * 拖动速度条即可让仪表上的速度表盘动起来，用来演示/验收表盘动画，
             * 不必真去开车。放在亮度下方，与其它"整机设置"在一起。
             *
             * 退出演示有三重保证（缺一不可）：
             *   ① 用户关掉这个开关
             *   ② **点「开始驾驶」时 App 主动退出**（用户明确要求的时点）
             *   ③ 固件侧：收到真实遥测 / BLE 断开 → 自己退出
             *      —— App 的命令可能丢，所以安全必须由固件兜底。
             */
            /*
             * ⚠️ 演示速度必须**经由仪表**显示，所以要先把"必须已连接"摆在明面上。
             *
             * 之前的版本没连上时只是静默失败：开关能打开、滑条能拖，但仪表毫无反应，
             * 用户完全看不出是"没连接"还是"功能坏了"（真实反馈就是
             * "滑动速度并没有显示，仪表盘也没有任何速度动作"）。
             */
            HStack {
                Text("模拟速度测试")
                Spacer()
                if !session.bleConnected {
                    Text("未连接仪表").font(.caption2).bold()
                        .padding(.horizontal, 6).padding(.vertical, 2)
                        .background(Color.red.opacity(0.15))
                        .foregroundStyle(.red)
                        .clipShape(Capsule())
                } else if demoOn {
                    Text("DEMO").font(.caption2).bold()
                        .padding(.horizontal, 6).padding(.vertical, 2)
                        .background(Color.orange.opacity(0.25))
                        .foregroundStyle(.orange)
                        .clipShape(Capsule())
                }
            }
            Toggle("启用演示", isOn: $demoOn)
                .disabled(!session.bleConnected)
                .onChange(of: demoOn) { _, on in
                    _ = session.ble.sendDemoSpeed(on ? demoSpeed : -1)
                }
            if !session.bleConnected {
                Text("演示速度要显示在**仪表**上，因此需要先连接仪表（点上方「连接仪表」，或在仪表页点「开始驾驶」）。未连接时开关是禁用的。")
                    .font(.footnote).foregroundStyle(.secondary)
            }
            if demoOn {
                HStack {
                    Text("速度")
                    Slider(value: $demoSpeed, in: 0...240, step: 1) { editing in
                        /*
                         * 松手时补发末值：拖动期间是限频的，最后一个刻度可能
                         * 恰好被跳过；这里保证"松手后仪表一定停在滑条的位置"。
                         */
                        if !editing {
                            lastDemoSendMs = 0
                            _ = session.ble.sendDemoSpeed(demoSpeed)
                        }
                    }
                    .onChange(of: demoSpeed) { _, v in
                        /* 拖动期间限频（见 lastDemoSendMs 的说明）；松手时上面会补发末值 */
                        let now = DispatchTime.now().uptimeNanoseconds / 1_000_000
                        if now &- lastDemoSendMs >= Self.demoSendIntervalMs {
                            lastDemoSendMs = now
                            _ = session.ble.sendDemoSpeed(v)
                        }
                    }
                    Text("\(Int(demoSpeed))").monospacedDigit().frame(width: 34)
                }
                Text("仅用于演示表盘动画：仪表状态行会显示 DEMO（琥珀色）。开始驾驶、收到真实定位或蓝牙断开时会自动退出。")
                    .font(.footnote).foregroundStyle(.secondary)
            }

            Toggle("屏幕点亮", isOn: $screenOn)
                .onChange(of: screenOn) { _, on in
                    guard !applyingDeviceState else { return }
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
            /*
             * 连接 / 断开是一对**互斥动作**，同一位置切换，而不是让"连接"按钮
             * 在已连接时变灰（那样用户找不到断开入口）。
             *
             * 自动连接（BLEClient.connectIfPossible）让用户通常无需点这里；
             * 但"手动断开"必须留给用户 —— 例如想让仪表断开独占、或省电时。
             */
            if bleConnected {
                Button(role: .destructive) {
                    session.ble.disconnect()
                } label: {
                    Label("断开连接", systemImage: "xmark.circle")
                }
            } else {
                Button {
                    session.ble.scanAndConnect()
                } label: {
                    Label("连接仪表", systemImage: "antenna.radiowaves.left.and.right")
                }
            }
            /*
             * 用户主动断开后，自动连接是关着的 —— 必须明确告诉他"现在不会自动连"，
             * 否则他会以为 App 坏了（此前会自动重连，断开后行为变了）。
             */
            if !bleConnected && !session.ble.autoConnectEnabled {
                Text("已停止自动连接。点「连接仪表」可重新连接（之后会自动保持连接）。")
                    .font(.footnote).foregroundColor(.orange)
            }

            Button(session.isDriving ? "结束驾驶" : "开始驾驶") {
                if session.isDriving {
                    session.stopDriving()
                } else {
                    /*
                     * ⚠️ 开始驾驶前**必须退出演示**（用户明确要求）：
                     * 否则演示速度会和真实遥测抢同一个指针（固件虽有兜底，
                     * 但主动退出能让切换立刻生效）。
                     */
                    if demoOn {
                        demoOn = false
                        _ = session.ble.sendDemoSpeed(-1)
                    }
                    session.requestPermissions()
                    session.startDriving()
                }
            }
            .foregroundColor(session.isDriving ? .red : .blue)
        } header: {
            Text("仪表连接")
        } footer: {
            Text("蓝牙就绪后本机会自动连接仪表并在掉线后自动重连，无需手动操作；如需断开请点「断开连接」。\n开始驾驶后，本机会持续把定位与车速通过蓝牙发送给仪表。")
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
