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
        if let cur = deviceFaces.first(where: { $0.id == session.ble.faces.currentId }) {
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
    /// 仪表侧走 LVGL 软件旋转（本屏为圆形，旋转无长宽比副作用），
    /// 并会持久化到 NVS，重启后保持。
    private let rotations = [0, 90, 180, 270]
    @State private var selectedRotation = 0

    @State private var selectedFace = 2
    /// 当前选中的**分组**（不是表盘）。分组来自设备上报。
    @State private var selectedGroup = ""
    @State private var brightness: Double = 200
    @State private var screenOn = true
    /// 防止 onAppear 重复触发自动连接（SwiftUI 可能多次调用）
    @State private var didAutoConnect = false

    var body: some View {
        NavigationView {
            ScrollViewReader { proxy in
            Form {
                if session.needsPermissionExplanation {
                    Section("为什么需要权限") {
                        Text("定位：驾驶会话中读取车速、航向与位置，用于车载仪表显示。")
                        Text("蓝牙：与车载仪表（CarDashboard）连接并发送这些数据。")
                        Text("数据只在本机与仪表之间通过蓝牙传输，不上传云端。")
                            .font(.footnote).foregroundColor(.secondary)
                        Button("我了解了，申请权限") { session.requestPermissions() }
                    }
                }

                Section("仪表连接") {
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
                }

                Section("驾驶会话") {
                    Button(session.isDriving ? "结束驾驶" : "开始驾驶") {
                        if session.isDriving { session.stopDriving() }
                        else { session.requestPermissions(); session.startDriving() }
                    }
                    .foregroundColor(session.isDriving ? .red : .blue)

                    row("累计发送", "\(session.sentCount) 包")
                    row("最近发送", timeText(session.lastSendTime))
                    row("定位数据时间", timeText(session.lastLocationTime))
                }

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
                        ForEach(facesInSelectedGroup) { f in
                            HStack {
                                Button {
                                    selectedFace = f.id
                                    _ = session.ble.setFace(f.id)
                                } label: {
                                    HStack(spacing: 8) {
                                        Image(systemName: f.id == selectedFace
                                              ? "checkmark.circle.fill" : "circle")
                                            .foregroundColor(f.id == selectedFace
                                                             ? .accentColor : .secondary)
                                        Text(FaceDisplay.faceTitle(name: f.name,
                                                                   fallbackLabel: f.label))
                                            .foregroundColor(.primary)
                                    }
                                }
                                .buttonStyle(.plain)

                                Spacer()

                                Toggle("", isOn: Binding(
                                    get: { f.enabled },
                                    set: { on in _ = session.ble.setFaceEnabled(f.id, on) }
                                ))
                                .labelsHidden()
                                .accessibilityLabel("显示\(FaceDisplay.faceTitle(name: f.name, fallbackLabel: f.label))")
                            }
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
                            Text("点名称切换到该表盘（✓ 为当前）；开关控制是否出现在滑动序列中。")
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

                Section("实时数据") {
                    if let loc = session.lastLocation {
                        let speedKmh = loc.speed < 0 ? nil : loc.speed * 3.6
                        row("速度", speedKmh.map { String(format: "%.1f km/h", $0) } ?? "--")
                        row("航向", loc.course >= 0 ? String(format: "%.0f°", loc.course) : "--")
                        row("定位精度", String(format: "±%.0f m", loc.horizontalAccuracy))

                        // ---- 诊断：速度为何被判无效（AI_TASK §5 质量门槛）----
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
                    } else {
                        Text("等待定位数据…").foregroundColor(.secondary)
                    }
                    if let err = session.locationError {
                        Text(err).foregroundColor(.red)
                    }
                }
            }
            .navigationTitle("CarDashboard")
            #if DEBUG
            /*
             * 演示模式自动滚动到表盘分组。
             * 目的：模拟器不支持蓝牙、连不上真机，若还要手动滚动才能看到
             * 「分组 + 开关」，这个演示模式就失去了"验收 UI"的意义。
             * 仅 Debug 构建 + -demoFaces 启动参数时生效。
             */
            .onAppear {
                guard BLEClient.demoModeEnabled else { return }
                DispatchQueue.main.asyncAfter(deadline: .now() + 1.2) {
                    withAnimation { proxy.scrollTo("faceGroupsAnchor", anchor: .top) }
                }
            }
            #endif
            // 设备上报变化时校正本地选择：
            //   - 选中的分组没了（设备改版）→ 退回第一个分组
            //   - 当前表盘以**设备上报的 cur 为准**（设备可能因隐藏而自动切换过）
            // 当前表盘以**设备上报的 cur 为准**：设备可能因隐藏而自动切换过，
            // 若只信本地状态，勾选标记就会指向一个已经不是当前的表盘。
            // （分组选中态是派生值，不需要在这里同步。）
            .onChange(of: session.ble.faces.currentId) { _, cur in
                if cur >= 0 { selectedFace = cur }
            }
            }
        }
        /*
         * App 启动即尝试连接仪表（#bug 修复）：
         * 原先只有点「开始驾驶」才会 scanAndConnect，导致重开 App 后
         * BLE 状态一直停留在初始态、必须手动点一下才刷新。
         * 这里在界面出现时自动扫描一次，用户无需先开始驾驶。
         */
        .onAppear {
                #if DEBUG
                if BLEClient.demoModeEnabled && !session.ble.facesReady {
                    session.ble.loadDemoFaces()
                }
                #endif
            guard !didAutoConnect else { return }
            didAutoConnect = true
            session.ble.scanAndConnect()
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
