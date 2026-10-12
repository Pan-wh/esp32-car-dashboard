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

    var body: some View {
        NavigationView {
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

                Section("实时数据") {
                    if let loc = session.lastLocation {
                        let speedKmh = loc.speed < 0 ? nil : loc.speed * 3.6
                        row("速度", speedKmh.map { String(format: "%.1f km/h", $0) } ?? "--")
                        row("航向", loc.course >= 0 ? String(format: "%.0f°", loc.course) : "--")
                        row("定位精度", String(format: "±%.0f m", loc.horizontalAccuracy))
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
