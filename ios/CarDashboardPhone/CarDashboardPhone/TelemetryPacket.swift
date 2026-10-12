// TelemetryPacket.swift —— BLE 遥测数据包（**协议字段名必须与固件 telemetry.c 完全一致**）
//
// 字段名/语义见 docs/ble.md 与 AI_TASK_IPHONE_BLE_DASHBOARD.md §4.1。
// 固件端用受控扁平 JSON 解析（simulator/ui/telemetry.c），只认这些 key：
//   v / ts_utc_ms / speed_mps / speed_accuracy_mps / latitude / longitude /
//   horizontal_accuracy_m / course_deg / location_valid / speed_valid
//
// 规则：
//   - ts_utc_ms 为 UTC Unix epoch 毫秒（不传本地字符串）
//   - speed_mps 米/秒；<0 视为无效（speed_valid 必须 false）
//   - 经纬度、精度、航向可为 nil（编码时省略，固件以 has_* 标志区分，不伪装 0）
import Foundation
import CoreLocation

struct TelemetryPacket: Codable {
    var v: Int
    var tsUtcMs: Int64
    var speedMps: Double?
    var speedAccuracyMps: Double?
    var latitude: Double?
    var longitude: Double?
    var horizontalAccuracyM: Double?
    var courseDeg: Double?
    var locationValid: Bool
    var speedValid: Bool

    enum CodingKeys: String, CodingKey {
        case v
        case tsUtcMs = "ts_utc_ms"
        case speedMps = "speed_mps"
        case speedAccuracyMps = "speed_accuracy_mps"
        case latitude
        case longitude
        case horizontalAccuracyM = "horizontal_accuracy_m"
        case courseDeg = "course_deg"
        case locationValid = "location_valid"
        case speedValid = "speed_valid"
    }

    static let protocolVersion = 1

    /// 编码为紧凑 JSON；nil 的可选字段用 encodeIfPresent 省略，减小包体。
    func encode() throws -> Data {
        let encoder = JSONEncoder()
        encoder.outputFormatting = [] // 无空白，最紧凑（利于塞进 BLE MTU）
        return try encoder.encode(self)
    }

    /// 从一次 CoreLocation 更新构造。
    /// iOS 的 CLLocation.speed < 0 表示无效。
    static func from(location: CLLocation, nowMs: Int64) -> TelemetryPacket {
        let speed = location.speed
        let speedIsValid = speed >= 0
        let course = location.course
        let courseIsValid = course >= 0

        return TelemetryPacket(
            v: protocolVersion,
            tsUtcMs: nowMs,
            speedMps: speedIsValid ? location.speed : nil,
            speedAccuracyMps: nil, // speedAccuracy 需 iOS 17+，见 LocationService 注释
            latitude: location.coordinate.latitude,
            longitude: location.coordinate.longitude,
            horizontalAccuracyM: location.horizontalAccuracy,
            courseDeg: courseIsValid ? location.course : nil,
            locationValid: true,
            speedValid: speedIsValid
        )
    }
}
