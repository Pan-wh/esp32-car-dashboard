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

    /// 驾驶会话开始时调用：升级到「始终」授权并开启后台定位（阶段 4）。
    /// 需 Info.plist 配置 NSLocationAlwaysAndWhenInUseUsageDescription 并在
    /// Capabilities 勾选 Background Modes → Location updates。
    func requestBackground() {
        manager.requestWhenInUseAuthorization()
        manager.requestAlwaysAuthorization()
#if os(iOS)
        manager.allowsBackgroundLocationUpdates = true // 后台持续定位（iOS only）
        if #available(iOS 17.0, *) {
            manager.showsBackgroundLocationIndicator = true // 后台定位状态指示
        }
#endif
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
        /*
         * 只发送**新鲜**的位置：权限被撤销、定位停止、或系统长时间不给回调时，
         * lastLocation 会变成过期值——绝不能拿它冒充当前数据（§6.3）。
         */
        guard let loc = lastLocation else { return nil }
        guard Date().timeIntervalSince(loc.timestamp) <= 3.0 else { return nil }
        return TelemetryPacket.from(location: loc, nowMs: nowUtcMs)
    }
}

extension LocationService: CLLocationManagerDelegate {
    func locationManagerDidChangeAuthorization(_ manager: CLLocationManager) {
        authorizationStatus = manager.authorizationStatus
        switch manager.authorizationStatus {
        case .notDetermined:
            break
        case .denied, .restricted:
            /*
             * 权限被撤销时必须**立即失效**已缓存的位置。
             *
             * 真机踩坑：原先只设置 lastError，未清 lastLocation，
             * 而 tick() 只判断 lastLocation 非空 → 权限被拒后仪表仍以 2Hz
             * 收到**过期坐标**并显示"实时速度"，违反 §6.3「不得用旧值冒充当前值」。
             */
            manager.stopUpdatingLocation()
            lastLocation = nil
            lastError = "定位权限被拒绝/受限"
        default:
            lastError = nil
        }
    }

    func locationManager(_ manager: CLLocationManager, didUpdateLocations locations: [CLLocation]) {
        lastLocation = locations.last
        lastError = nil
    }

    /// 位置是否仍然"新鲜"（超过该时长视为失效，避免发送过期坐标）
    func hasFreshLocation(maxAge: TimeInterval = 3.0) -> Bool {
        guard let loc = lastLocation else { return false }
        return Date().timeIntervalSince(loc.timestamp) <= maxAge
    }

    func locationManager(_ manager: CLLocationManager, didFailWithError error: Error) {
        lastError = "定位失败: \(error.localizedDescription)"
        // 失败时不清空 lastLocation；由上层按有效性/新鲜度判定是否发送
    }
}
