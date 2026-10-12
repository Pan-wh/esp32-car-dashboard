// TelemetryQuality.swift —— 遥测质量门槛（AI_TASK §5.4）
//
// 要求：「结合 horizontalAccuracy、speedAccuracy 设置合理质量门槛；
//        阈值应集中配置、可调，并在文档中解释」。
//
// 设计：阈值**集中在本文件**，App 内唯一来源；判定结果写入
//      TelemetryPacket.speedValid / locationValid，仪表端据此显示有效/失效，
//      不需要在固件里重复这套业务规则（固件只做 §7 的超范围兜底校验）。
//
// 调参建议：先把阈值放宽观察实际行驶数据，再逐步收紧；
//          过严会导致仪表频繁闪 "NO FIX"，过松会把漂移当真实车速。
import CoreLocation
import Foundation

struct TelemetryQuality {

    /// 水平定位精度上限（米）。超过则视为定位不可用。
    /// GPS 开阔地通常 5-10m；城市峡谷/树荫下 20-50m。默认 50m：宁可显示不可用，也不显示漂移位置。
    static let maxHorizontalAccuracyMeters: CLLocationAccuracy = 50

    /// 速度精度上限（米/秒，iOS 17+ speedAccuracy）。超过则速度不可信。
    /// 1 m/s ≈ 3.6 km/h；默认 2.0 m/s（≈7 km/h）作为"可信速度"的门槛。
    static let maxSpeedAccuracyMPS: Double = 2.0

    /// 速度下限（米/秒）：小于此值视为静止/噪声，speed_valid 仍为 true 但速度按 0 处理由仪表决定。
    /// 这里不强制置零，避免掩盖低速蠕行。
    static let minMeaningfulSpeedMPS: Double = 0.0

    /// 判定定位是否可用
    static func isLocationUsable(_ location: CLLocation) -> Bool {
        // 负值表示无效
        guard location.horizontalAccuracy >= 0 else { return false }
        return location.horizontalAccuracy <= maxHorizontalAccuracyMeters
    }

    /// 判定速度是否可信（iOS 17+ 用 speedAccuracy；更早版本仅看 speed 符号）
    static func isSpeedUsable(_ location: CLLocation) -> Bool {
        guard location.speed >= minMeaningfulSpeedMPS else { return false } // 负值=无效
        if #available(iOS 17.0, *) {
            let sa = location.speedAccuracy
            if sa >= 0 && sa > maxSpeedAccuracyMPS { return false }
        }
        return true
    }
}
