// ContentView.swift —— 前台 MVP UI（连接 / 状态 / 驾驶按钮 / 调试值）
//
// 任务书 §5.2 最小范围：
//   连接页 / 状态区域（蓝牙 + 定位）/ 按钮（连接、开始驾驶、结束驾驶）/ 调试值
import SwiftUI
import CoreLocation
import CoreBluetooth

struct ContentView: View {
    @StateObject private var session = DriveSessionManager()

    var body: some View {
        NavigationView {
            Form {
                Section("仪表连接") {
                    HStack {
                        Text("BLE 状态")
                        Spacer()
                        Text(bleStateText).foregroundColor(bleConnected ? .green : .orange)
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
            .onAppear {
                // 首次进入先解释权限，不连续弹窗
                session.requestPermissions()
            }
        }
    }

    private func row(_ label: String, _ value: String) -> some View {
        HStack { Text(label); Spacer(); Text(value).monospacedDigit() }
    }

    private var bleConnected: Bool { session.bleConnected }
    private var bleStateText: String { bleConnected ? "已连接" : "未连接/未授权" }
    private var locationPermissionText: String {
        switch session.locationStatus {
        case .authorizedAlways, .authorizedWhenInUse: return "已授权"
        case .denied: return "已拒绝"
        case .restricted: return "受限"
        case .notDetermined: return "待授权"
        @unknown default: return "未知"
        }
    }
}
