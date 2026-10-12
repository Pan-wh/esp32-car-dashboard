// DriveSessionManager.swift —— 驾驶会话管理（开始/结束 + 定时发送遥测）
//
// 职责：用户点「开始驾驶」后 → 申请/启动定位 + 连接 BLE + 以 1-2Hz 定时发送遥测；
//      点「结束驾驶」或数据无效时停止。发送频率集中在此，不逐帧发送。
import Combine
import Foundation
import CoreLocation

final class DriveSessionManager: ObservableObject {
    @Published var isDriving = false

    let location = LocationService()
    let ble = BLEClient()

    private var sendTimer: Timer?

    static let sendInterval: TimeInterval = 0.5 // 2 Hz；实测后再调

    // 供 UI 绑定
    var locationStatus: CLAuthorizationStatus { location.authorizationStatus }
    var bleConnected: Bool { ble.isConnected }
    var lastLocation: CLLocation? { location.lastLocation }
    var locationError: String? { location.lastError }

    func requestPermissions() {
        location.requestWhenInUse()
    }

    func startDriving() {
        isDriving = true
        location.start()
        ble.scanAndConnect()
        sendTimer = Timer.scheduledTimer(withTimeInterval: Self.sendInterval, repeats: true) { [weak self] _ in
            self?.tick()
        }
    }

    func stopDriving() {
        isDriving = false
        sendTimer?.invalidate()
        sendTimer = nil
        location.stop()
    }

    private func tick() {
        let nowUtcMs = Int64(Date().timeIntervalSince1970 * 1000)
        guard let packet = location.makePacket(nowUtcMs: nowUtcMs) else { return }
        ble.send(packet: packet)
    }
}
