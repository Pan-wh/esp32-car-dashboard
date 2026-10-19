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
    static let maxSpeedAccuracyLowSpeedMPS: Double = 5.0
    static let maxSpeedAccuracyMidSpeedMPS: Double = 2.5
    static let lowSpeedBandMPS: Double = 10.0 / 3.6
    static let midSpeedBandMPS: Double = 40.0 / 3.6

    static func speedAccuracyLimit(forSpeed speedMPS: Double) -> Double {
        if speedMPS < lowSpeedBandMPS { return maxSpeedAccuracyLowSpeedMPS }
        if speedMPS < midSpeedBandMPS { return maxSpeedAccuracyMidSpeedMPS }
        return maxSpeedAccuracyMPS
    }

    static func isLocationUsable(_ l: CLLocation) -> Bool {
        guard l.horizontalAccuracy >= 0 else { return false }
        return l.horizontalAccuracy <= maxHorizontalAccuracyMeters
    }
    static func isSpeedUsable(_ l: CLLocation) -> Bool {
        guard l.speed >= 0 else { return false }
        if #available(iOS 17.0, macOS 14.0, *) {
            let sa = l.speedAccuracy
            if sa >= 0 && sa > speedAccuracyLimit(forSpeed: l.speed) { return false }
        }
        return true
    }
}

/// 测速精度分布统计（与 App 侧 TelemetryQuality.swift 同源语义；改动需同步）
struct SpeedAccuracyStats {
    struct Band {
        var count = 0
        var rejected = 0
        var accuracyCount = 0
        var maxAccuracy: Double = 0
        var sumAccuracy: Double = 0
        var sumSpeed: Double = 0
        var avgAccuracy: Double { accuracyCount > 0 ? sumAccuracy / Double(accuracyCount) : 0 }
        var avgSpeedKmh: Double { count > 0 ? (sumSpeed / Double(count)) * 3.6 : 0 }
        var rejectRate: Double { count > 0 ? Double(rejected) / Double(count) : 0 }
    }
    var low = Band()
    var mid = Band()
    var high = Band()
    var total: Int { low.count + mid.count + high.count }
    var totalRejected: Int { low.rejected + mid.rejected + high.rejected }
    var rejectRate: Double { total > 0 ? Double(totalRejected) / Double(total) : 0 }

    mutating func record(speedMPS: Double, speedAccuracy: Double, usable: Bool) {
        let band: WritableKeyPath<SpeedAccuracyStats, Band>
        if speedMPS < TelemetryQuality.lowSpeedBandMPS { band = \.low }
        else if speedMPS < TelemetryQuality.midSpeedBandMPS { band = \.mid }
        else { band = \.high }
        self[keyPath: band].count += 1
        self[keyPath: band].sumSpeed += speedMPS
        if !usable { self[keyPath: band].rejected += 1 }
        if speedAccuracy >= 0 {
            self[keyPath: band].accuracyCount += 1
            self[keyPath: band].sumAccuracy += speedAccuracy
            self[keyPath: band].maxAccuracy = max(self[keyPath: band].maxAccuracy, speedAccuracy)
        }
    }
    mutating func reset() { self = SpeedAccuracyStats() }
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

// 7) 测速精度门槛**按速度分档**（真机问题驱动：低速时几乎必然被拦）
//
// 背景：GPS 测速是位置差分，误差与速度强相关（低速相对误差爆炸）。
// 原先固定 2.0 m/s，导致起步/堵车时 speed_valid 频繁为 false，
// 仪表从 LIVE 掉到 `--`，而高速时反而正常。
//
// 注：CLLocation.speedAccuracy 是只读的、无法在测试里构造，故这里直接
//     验证**纯决策函数** speedAccuracyLimit(forSpeed:) —— 分档逻辑全在这里，
//     isSpeedUsable 只是取 limit 后做一次比较。
do {
    func kmh(_ k: Double) -> Double { k / 3.6 }

    // 分档边界必须与注释一致：<10 km/h 低速、10–40 中速、≥40 高速
    check(TelemetryQuality.speedAccuracyLimit(forSpeed: 0) == 5.0, "静止应属低速档（±5.0）")
    check(TelemetryQuality.speedAccuracyLimit(forSpeed: kmh(5)) == 5.0, "5 km/h 属低速档")
    check(TelemetryQuality.speedAccuracyLimit(forSpeed: kmh(9.9)) == 5.0, "9.9 km/h 仍在低速档")
    check(TelemetryQuality.speedAccuracyLimit(forSpeed: kmh(10)) == 2.5, "10 km/h 应进入中速档（±2.5）")
    check(TelemetryQuality.speedAccuracyLimit(forSpeed: kmh(30)) == 2.5, "30 km/h 属中速档")
    check(TelemetryQuality.speedAccuracyLimit(forSpeed: kmh(39.9)) == 2.5, "39.9 km/h 仍在中速档")
    check(TelemetryQuality.speedAccuracyLimit(forSpeed: kmh(40)) == 2.0, "40 km/h 应进入高速档（±2.0）")
    check(TelemetryQuality.speedAccuracyLimit(forSpeed: kmh(80)) == 2.0, "80 km/h 属高速档")
    check(TelemetryQuality.speedAccuracyLimit(forSpeed: kmh(120)) == 2.0, "120 km/h 属高速档")

    // 单调性：速度越高，门槛越**严**（绝不能反过来 —— 反过来会在高速放过漂移）
    let speeds = stride(from: 0.0, through: 140.0, by: 5.0).map { kmh($0) }
    let limits = speeds.map { TelemetryQuality.speedAccuracyLimit(forSpeed: $0) }
    var monotonic = true
    for i in 1..<limits.count where limits[i] > limits[i - 1] { monotonic = false }
    check(monotonic, "★ 门槛必须随速度单调收紧（否则高速会放过漂移数据）")

    // ★ 核心回归：低速的"坏"精度值现在应被放行（原先固定 2.0 时全被拦）
    // ±4.0 m/s 在 5 km/h 时：旧门槛拦掉，新门槛放行
    check(4.0 > 2.0, "前提：4.0 确实超过旧的固定门槛 2.0")
    check(4.0 <= TelemetryQuality.speedAccuracyLimit(forSpeed: kmh(5)),
          "★ 低速下 ±4.0 应放行（这正是用户报的『低速总被拦』场景）")

    // ★ 反向保护：高速时不能放宽（避免把漂移当真实车速）
    check(4.0 > TelemetryQuality.speedAccuracyLimit(forSpeed: kmh(80)),
          "★ 高速下 ±4.0 仍必须被拦（表显 80 实际 60 比显示 -- 更危险）")
}

// 8) 测速精度分布统计（调阈值的事实依据）
do {
    var s = SpeedAccuracyStats()
    check(s.total == 0 && s.rejectRate == 0, "初始应为空且不除零")

    // 低速样本：被拦
    s.record(speedMPS: 1.0, speedAccuracy: 6.0, usable: false)
    // 低速样本：通过
    s.record(speedMPS: 2.0, speedAccuracy: 1.0, usable: true)
    // 高速样本：通过
    s.record(speedMPS: 25.0, speedAccuracy: 0.5, usable: true)

    check(s.total == 3, "总样本应为 3，实际 \(s.total)")
    check(s.low.count == 2, "低速档应有 2 个样本")
    check(s.mid.count == 0, "中速档无样本")
    check(s.high.count == 1, "高速档应有 1 个样本")
    check(s.totalRejected == 1, "应记录 1 次被拦")
    check(abs(s.rejectRate - 1.0 / 3.0) < 1e-9, "被拦比例应为 1/3")
    check(abs(s.low.maxAccuracy - 6.0) < 1e-9, "低速档最大值应为 6.0")
    check(abs(s.low.avgAccuracy - 3.5) < 1e-9, "低速档均值应为 (6+1)/2=3.5")

    // speedAccuracy 为负（iOS 未提供）不应污染精度均值，但仍计入次数与被拦
    //
    // ⚠️ 这里曾写出一个 bug：均值除以 count（含未知精度的样本），
    //    导致"精度未知"的样本把均值**拉低** —— 调阈值时会误判为
    //    "实际精度很好"而错误收紧门槛。现在均值只除以 accuracyCount。
    let beforeAvg = s.low.avgAccuracy
    let beforeCount = s.low.count
    s.record(speedMPS: 1.0, speedAccuracy: -1, usable: false)
    check(s.low.count == beforeCount + 1, "未知精度也应计入样本数")
    check(abs(s.low.avgAccuracy - beforeAvg) < 1e-9,
          "★ 负精度不得污染均值（均值应只按有精度估计的样本算）")
    check(s.low.rejected == 2, "未知精度的样本若被拦也应计入")

    s.reset()
    check(s.total == 0, "reset 后应清空")
    check(s.rejectRate == 0, "reset 后不应除零")
}

print("结果: \(pass) 通过, \(fail) 失败")
exit(fail == 0 ? 0 : 1)
