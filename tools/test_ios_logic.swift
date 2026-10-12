// test_ios_logic.swift —— iOS 侧纯逻辑测试（可在 macOS 本机直接运行，无需 iOS 运行时）
//
// 覆盖 TelemetryPacket（协议契约）与 TelemetryQuality（§5.4 质量门槛）。
// 运行：swift tools/test_ios_logic.swift
//
// 说明：CLLocation 可在 macOS 上构造，故这两块逻辑无需模拟器/真机即可验证；
//      CoreLocation/CoreBluetooth 的**实际行为**仍需真机（见 docs/test-results.md）。

import CoreLocation
import Foundation

// ---- 被测代码（从 App 源码内联，保证与 App 同源语义；改动需同步） ----
// 若 App 侧改动，此文件必须同步，否则测试会与实现漂移。
// （Swift 无跨文件私有导入的轻量方案；这里保持最小复刻并加了同步提醒。）

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
        case latitude, longitude
        case horizontalAccuracyM = "horizontal_accuracy_m"
        case courseDeg = "course_deg"
        case locationValid = "location_valid"
        case speedValid = "speed_valid"
    }

    static let protocolVersion = 1

    func encode() throws -> Data {
        let e = JSONEncoder()
        e.outputFormatting = []
        return try e.encode(self)
    }

    static func from(location: CLLocation, nowMs: Int64) -> TelemetryPacket {
        let speed = location.speed
        let speedIsValid = TelemetryQuality.isSpeedUsable(location)
        let locationIsValid = TelemetryQuality.isLocationUsable(location)
        let course = location.course
        let courseIsValid = course >= 0

        var speedAccuracy: Double? = nil
        if #available(iOS 17.0, macOS 14.0, *) {
            let sa = location.speedAccuracy
            if sa >= 0 { speedAccuracy = sa }
        }
        return TelemetryPacket(
            v: protocolVersion, tsUtcMs: nowMs,
            speedMps: (speedIsValid && speed >= 0) ? speed : nil,
            speedAccuracyMps: speedAccuracy,
            latitude: location.coordinate.latitude,
            longitude: location.coordinate.longitude,
            horizontalAccuracyM: location.horizontalAccuracy,
            courseDeg: courseIsValid ? location.course : nil,
            locationValid: locationIsValid, speedValid: speedIsValid)
    }
}

struct TelemetryQuality {
    static let maxHorizontalAccuracyMeters: CLLocationAccuracy = 50
    static let maxSpeedAccuracyMPS: Double = 2.0
    static let minMeaningfulSpeedMPS: Double = 0.0

    static func isLocationUsable(_ l: CLLocation) -> Bool {
        guard l.horizontalAccuracy >= 0 else { return false }
        return l.horizontalAccuracy <= maxHorizontalAccuracyMeters
    }
    static func isSpeedUsable(_ l: CLLocation) -> Bool {
        guard l.speed >= minMeaningfulSpeedMPS else { return false }
        if #available(iOS 17.0, macOS 14.0, *) {
            let sa = l.speedAccuracy
            if sa >= 0 && sa > maxSpeedAccuracyMPS { return false }
        }
        return true
    }
}

// ---- 极简断言 ----
var pass = 0, fail = 0
func check(_ cond: Bool, _ msg: String) {
    if cond { pass += 1 } else { fail += 1; print("  ✗ \(msg)") }
}
func near(_ a: Double, _ b: Double, _ eps: Double = 0.001) -> Bool { abs(a - b) <= eps }

// ---- 构造 CLLocation 的辅助 ----
func makeLocation(lat: Double, lon: Double, speed: Double, course: Double, hacc: Double) -> CLLocation {
    return CLLocation(coordinate: CLLocationCoordinate2D(latitude: lat, longitude: lon),
                      altitude: 10, horizontalAccuracy: hacc, verticalAccuracy: 5,
                      course: course, speed: speed, timestamp: Date())
}

// ---- 测试 ----

// 1) 协议契约：字段名必须与固件 telemetry.c 完全一致
do {
    let p = TelemetryPacket(v: 1, tsUtcMs: 1791792803000, speedMps: 19.03, speedAccuracyMps: nil,
                            latitude: 1.3521, longitude: 103.8198, horizontalAccuracyM: 5.2,
                            courseDeg: 120.0, locationValid: true, speedValid: true)
    let json = String(data: try p.encode(), encoding: .utf8)!
    for key in ["\"v\"", "\"ts_utc_ms\"", "\"speed_mps\"", "\"latitude\"", "\"longitude\"",
                "\"horizontal_accuracy_m\"", "\"course_deg\"", "\"location_valid\"", "\"speed_valid\""] {
        check(json.contains(key), "JSON 应含字段 \(key)")
    }
    // nil 字段应被省略而非输出 null（减包体）
    check(!json.contains("speed_accuracy_mps"), "nil 的 speed_accuracy_mps 应省略")
    check(!json.contains("null"), "不应输出 null")
    print("  完整包: \(json)")
}

// 2) 质量门槛：定位精度
do {
    check(TelemetryQuality.isLocationUsable(makeLocation(lat: 0, lon: 0, speed: 0, course: 0, hacc: 8)),
          "hacc=8m 可用")
    check(TelemetryQuality.isLocationUsable(makeLocation(lat: 0, lon: 0, speed: 0, course: 0, hacc: 50)),
          "hacc=50m 恰好达标可用")
    check(!TelemetryQuality.isLocationUsable(makeLocation(lat: 0, lon: 0, speed: 0, course: 0, hacc: 51)),
          "hacc=51m 不可用")
    check(!TelemetryQuality.isLocationUsable(makeLocation(lat: 0, lon: 0, speed: 0, course: 0, hacc: -1)),
          "hacc<0（无效）不可用")
}

// 3) 质量门槛：速度
do {
    check(TelemetryQuality.isSpeedUsable(makeLocation(lat: 0, lon: 0, speed: 20, course: 0, hacc: 5)),
          "速度 20m/s 可用")
    check(!TelemetryQuality.isSpeedUsable(makeLocation(lat: 0, lon: 0, speed: -1, course: 0, hacc: 5)),
          "速度 -1（iOS 无效语义）不可用")
    // 速度 0 = 静止，是**合法**状态（不应被当成无效），仅负值才是 iOS 的无效语义
    check(TelemetryQuality.isSpeedUsable(makeLocation(lat: 0, lon: 0, speed: 0, course: 0, hacc: 5)),
          "速度 0（静止）应可用")
    check(!TelemetryQuality.isSpeedUsable(makeLocation(lat: 0, lon: 0, speed: -0.1, course: 0, hacc: 5)),
          "速度 -0.1 应不可用")
}

// 4) 构造：速度无效时不得把负速度发出去
do {
    let loc = makeLocation(lat: 31.2, lon: 121.4, speed: -1, course: -1, hacc: 10)
    let p = TelemetryPacket.from(location: loc, nowMs: 1000)
    check(p.speedMps == nil, "负速度 → speedMps 应为 nil（不发送负速度）")
    check(!p.speedValid, "负速度 → speedValid=false")
    check(p.courseDeg == nil, "负 course → courseDeg 应为 nil")
    check(p.locationValid, "hacc=10m → locationValid=true")
    let json = String(data: try p.encode(), encoding: .utf8)!
    check(!json.contains("-1"), "编码后不应出现负速度 -1")
}

// 5) 构造：精度差时 locationValid=false（但仍带经纬度，供固件兜底校验通过）
do {
    let loc = makeLocation(lat: 31.2, lon: 121.4, speed: 10, course: 90, hacc: 120)
    let p = TelemetryPacket.from(location: loc, nowMs: 1000)
    check(!p.locationValid, "hacc=120m → locationValid=false")
    check(p.latitude != nil && p.longitude != nil, "仍带经纬度（供固件 §7 校验）")
    check(p.speedValid, "速度本身仍可信")
}

// 6) 正常包：字段齐全且数值正确
do {
    let loc = makeLocation(lat: 31.2304, lon: 121.4737, speed: 25.0, course: 135, hacc: 5)
    let p = TelemetryPacket.from(location: loc, nowMs: 1791792803000)
    check(near(p.latitude!, 31.2304, 1e-9), "纬度保持")
    check(near(p.longitude!, 121.4737, 1e-9), "经度保持")
    check(near(p.speedMps!, 25.0), "速度保持")
    check(near(p.courseDeg!, 135.0), "航向保持")
    check(p.locationValid && p.speedValid, "均有效")
}

print("结果: \(pass) 通过, \(fail) 失败")
exit(fail == 0 ? 0 : 1)
