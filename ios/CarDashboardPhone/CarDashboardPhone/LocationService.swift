// LocationService.swift —— Core Location 封装（CLLocationManager）
//
// - 只在用户主动「开始驾驶」后启动持续定位（遵守省电与隐私要求）
// - 授权拒绝/定位失败/数据无效时置 locationValid=false，不伪造
// - 用公开 Core Location API，不读取 Apple 地图 / CarPlay 内部数据
import CoreLocation
import Combine
import Foundation

final class LocationService: NSObject, ObservableObject {
    @Published var authorizationStatus: CLAuthorizationStatus = .notDetermined
    @Published var lastLocation: CLLocation?
    @Published var lastError: String?

    private let manager = CLLocationManager()

    override init() {
        super.init()
        manager.delegate = self
        manager.desiredAccuracy = kCLLocationAccuracyBestForNavigation
        manager.distanceFilter = kCLDistanceFilterNone
        // .otherNavigation：导航类 activity，iOS 据此优化；进度到阶段4再配后台 allUpdating
        manager.activityType = .otherNavigation
        manager.pausesLocationUpdatesAutomatically = false // 避免自动暂停导致车速停止更新
        authorizationStatus = manager.authorizationStatus
    }

    func requestWhenInUse() {
        manager.requestWhenInUseAuthorization()
    }

    func start() {
        manager.startUpdatingLocation()
        // 注：航向用 CLLocation.course（地面行进方向），不用 startUpdatingHeading（那是磁罗盘方向）
    }

    func stop() {
        manager.stopUpdatingLocation()
    }

    /// 构造一条遥测（时间戳用调用方传入的 UTC，保证与 BLE 发送时刻一致）
    func makePacket(nowUtcMs: Int64) -> TelemetryPacket? {
        guard let loc = lastLocation else { return nil }
        return TelemetryPacket.from(location: loc, nowMs: nowUtcMs)
    }
}

extension LocationService: CLLocationManagerDelegate {
    func locationManagerDidChangeAuthorization(_ manager: CLLocationManager) {
        authorizationStatus = manager.authorizationStatus
        switch manager.authorizationStatus {
        case .notDetermined: break
        case .denied, .restricted: lastError = "定位权限被拒绝/受限"
        default: lastError = nil
        }
    }

    func locationManager(_ manager: CLLocationManager, didUpdateLocations locations: [CLLocation]) {
        lastLocation = locations.last
        lastError = nil
    }

    func locationManager(_ manager: CLLocationManager, didFailWithError error: Error) {
        lastError = "定位失败: \(error.localizedDescription)"
        // 失败时不清空 lastLocation；由上层按有效性/新鲜度判定是否发送
    }
}
