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

    /// 选择器里只列**已启用**的表盘 —— 隐藏的表盘在仪表上滑动时会被跳过，
    /// 列进来会让用户以为能切过去。
    private var selectableFaces: [DeviceFace] { deviceFaces.filter { $0.enabled } }
    /// 屏幕方向选项（度）—— 只列 90 的整数倍，与固件 control.c 的严格校验一致。
    /// 仪表侧走 LVGL 软件旋转（本屏为圆形，旋转无长宽比副作用），
    /// 并会持久化到 NVS，重启后保持。
    private let rotations = [0, 90, 180, 270]
    @State private var selectedRotation = 0

    @State private var selectedFace = 2
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

                Section("仪表控制") {
                    if selectableFaces.isEmpty {
                        Text(session.ble.facesReady
                             ? "设备未上报可用的表盘"
                             : "表盘列表读取中…（连接仪表后自动获取）")
                            .font(.footnote).foregroundColor(.secondary)
                    } else {
                        Picker("表盘", selection: $selectedFace) {
                            ForEach(selectableFaces, id: \.id) { f in
                                Text(FaceDisplay.faceTitle(name: f.name,
                                                           fallbackLabel: f.label)).tag(f.id)
                            }
                        }
                        .pickerStyle(.segmented)
                        .onChange(of: selectedFace) { _, newValue in
                            _ = session.ble.setFace(newValue)
                        }
                    }

                    // 表盘分组与启用/隐藏（设备上报 → 按分组展示 → 开关写回设备）
                    // .id 供演示模式自动滚动定位（仅 Debug，见 body 末尾）
                    if !deviceFaces.isEmpty {
                        Color.clear.frame(height: 0).id("faceGroupsAnchor")
                        ForEach(session.ble.faces.grouped, id: \.group) { grp in
                            Section {
                                ForEach(grp.faces) { f in
                                    Toggle(isOn: Binding(
                                        get: { f.enabled },
                                        set: { on in _ = session.ble.setFaceEnabled(f.id, on) }
                                    )) {
                                        Text(FaceDisplay.faceTitle(name: f.name,
                                                                   fallbackLabel: f.label))
                                    }
                                }
                            } header: {
                                Text(FaceDisplay.groupTitle(grp.group))
                            } footer: {
                                if session.ble.faces.enabledCount <= 1 {
                                    Text("至少保留一个表盘，最后一个无法隐藏。")
                                }
                            }
                        }
                    } else if session.ble.isConnected {
                        Section("表盘分组") {
                            Text("尚未读到设备的表盘列表").font(.footnote)
                                .foregroundColor(.secondary)
                        }
                    }

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

                    if let notice = session.ble.lastNotice {
                        Text(notice).font(.footnote).foregroundColor(.secondary)
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
