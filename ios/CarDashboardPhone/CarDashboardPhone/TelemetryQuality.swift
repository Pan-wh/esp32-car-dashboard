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

    /**
     速度精度上限（米/秒，iOS 17+ speedAccuracy）—— **按速度分档**。

     ## 为什么不能用一个固定阈值（真机踩坑）

     GPS 测速是**位置差分**得来的（速度 = 位置变化 / 时间），因此测速误差
     大致正比于 `位置误差 / 采样间隔`，**与速度本身强相关**：

     | 场景 | speedAccuracy 实测常值 |
     |------|----------------------|
     | 高速巡航 60 km/h | 0.3 – 1.5 m/s |
     | 中速 30 km/h | 1 – 3 m/s |
     | **低速 5 km/h** | **3 – 10 m/s** |
     | 刚启动定位前 10–30 秒 | 很大（还在收敛）|

     低速时误差被**放大**是因为相对误差爆炸（分母趋近 0）。任务书 §5.4
     也点明了这点：*"低速时 GPS 速度可能跳动"*。

     原先用固定 `2.0` 的后果：**低速/起步/堵车时几乎必然被拦**，
     仪表频繁从 LIVE 掉到 `--`，而高速时反而一切正常 —— 用户看到的正是
     "速度质量经常被拦、定位质量却通过"（两者是 iOS 给的独立指标）。

     ## 为什么也不该简单放大成一个更大的固定值

     放大到 5.0 会让**高速时把漂移当真实车速**（表显 80 实际 60），
     这比显示 `--` 危险得多。所以按速度分档：低速放宽、高速保持严格。

     ## 分档依据

     高速段维持 2.0（GPS 在高速下精度本就很好，没必要放宽）；
     低速段放宽到 5.0（此时测速本就不精确，且即便判为无效，
     仪表显示 0 也不影响驾驶判断）。中速取中间值平滑过渡。

     ⚠️ 改这些值前先看 `SpeedAccuracyStats` 的实测分布（诊断界面已显示），
        不要凭感觉调。
     */
    static let maxSpeedAccuracyMPS: Double = 2.0 // 高速段（向后兼容的默认值）

    /// 低速段（< 10 km/h）的放宽阈值
    static let maxSpeedAccuracyLowSpeedMPS: Double = 5.0
    /// 中速段（10–40 km/h）的阈值
    static let maxSpeedAccuracyMidSpeedMPS: Double = 2.5
    /// 分档边界（米/秒）
    static let lowSpeedBandMPS: Double = 10.0 / 3.6  // 10 km/h
    static let midSpeedBandMPS: Double = 40.0 / 3.6  // 40 km/h

    /**
     取当前速度对应的测速精度上限（分档）。

     - Parameter speedMPS: 当前速度（米/秒）
     - Returns: 该速度档位允许的最大 `speedAccuracy`
     */
    static func speedAccuracyLimit(forSpeed speedMPS: Double) -> Double {
        if speedMPS < lowSpeedBandMPS { return maxSpeedAccuracyLowSpeedMPS }
        if speedMPS < midSpeedBandMPS { return maxSpeedAccuracyMidSpeedMPS }
        return maxSpeedAccuracyMPS
    }

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
    ///
    /// 精度门槛**按当前速度分档**（见 `speedAccuracyLimit`）—— 低速放宽、
    /// 高速保持严格。用一个固定阈值会让低速时几乎必然被拦（真机踩坑）。
    static func isSpeedUsable(_ location: CLLocation) -> Bool {
        guard location.speed >= minMeaningfulSpeedMPS else { return false } // 负值=无效
        if #available(iOS 17.0, *) {
            let sa = location.speedAccuracy
            /*
             * sa < 0 表示 iOS 也没有精度估计（Apple 文档：负值=无效）。
             * 此时**放行**：总比把全部数据都拦掉好，且 speed 本身仍受
             * minMeaningfulSpeedMPS 约束。这一分支必须保留 —— 早先若写成
             * `sa <= limit` 会把"未知"误判为"不可信"。
             */
            if sa >= 0 && sa > speedAccuracyLimit(forSpeed: location.speed) { return false }
        }
        return true
    }
}

/**
 `SpeedAccuracyStats` —— 测速精度分布统计（**用于按实测数据调阈值**）

 ## 为什么需要它

 上面的分档阈值（5.0 / 2.5 / 2.0）是**依据 GPS 原理推出的合理起点**，
 但每个地区、每台设备的实际分布不同（城市峡谷 vs 开阔高速差异很大）。
 凭感觉调阈值容易两头出错：

 - 调太严 → 仪表频繁闪 `--`（本次要解决的问题）
 - 调太松 → 把漂移当真实车速（表显 80 实际 60，**比闪 `--` 更危险**）

 所以把实测分布显示出来，用数据决定档位。

 ## 设计

 按**速度分档**分别统计，因为误差与速度强相关（低速误差大）。
 纯值类型 + 纯函数，可在 macOS 上单测，不依赖 CoreBluetooth。
 */
struct SpeedAccuracyStats {
    /// 一个速度档位的统计
    struct Band {
        /// 样本数（含精度未知的样本）
        var count = 0
        /// 被判定为"速度不可信"的次数
        var rejected = 0
        /// **有效**精度样本数（speedAccuracy >= 0 才计入）
        ///
        /// ⚠️ 必须与 `count` 分开：`sumAccuracy` 只累加有效样本，
        ///    若均值仍除以 `count`，那些"精度未知"的样本会把均值**拉低**，
        ///    调阈值时就会误以为"实际精度很好"而错误收紧门槛。
        var accuracyCount = 0
        /// 观察到的最大 speedAccuracy（m/s）
        var maxAccuracy: Double = 0
        /// speedAccuracy 累加（求均值用）
        var sumAccuracy: Double = 0
        /// 速度累加（求均值用）
        var sumSpeed: Double = 0

        /// 平均 speedAccuracy（只按**有精度估计**的样本算；无样本时为 0）
        var avgAccuracy: Double { accuracyCount > 0 ? sumAccuracy / Double(accuracyCount) : 0 }
        /// 平均速度（km/h，便于人读）
        var avgSpeedKmh: Double { count > 0 ? (sumSpeed / Double(count)) * 3.6 : 0 }
        /// 被拦比例（0-1）
        var rejectRate: Double { count > 0 ? Double(rejected) / Double(count) : 0 }
    }

    /// 低速档（< 10 km/h）
    var low = Band()
    /// 中速档（10–40 km/h）
    var mid = Band()
    /// 高速档（≥ 40 km/h）
    var high = Band()

    /// 总样本数
    var total: Int { low.count + mid.count + high.count }
    /// 总被拦数
    var totalRejected: Int { low.rejected + mid.rejected + high.rejected }
    /// 总体被拦比例
    var rejectRate: Double { total > 0 ? Double(totalRejected) / Double(total) : 0 }

    /// 记录一次采样（`speedAccuracy` 为负表示 iOS 未提供，不计入精度统计但计入次数）
    mutating func record(speedMPS: Double, speedAccuracy: Double, usable: Bool) {
        let band: WritableKeyPath<SpeedAccuracyStats, Band>
        if speedMPS < TelemetryQuality.lowSpeedBandMPS { band = \.low }
        else if speedMPS < TelemetryQuality.midSpeedBandMPS { band = \.mid }
        else { band = \.high }

        self[keyPath: band].count += 1
        self[keyPath: band].sumSpeed += speedMPS
        if !usable { self[keyPath: band].rejected += 1 }
        // speedAccuracy < 0 表示"未知"，不参与精度分布（避免污染均值）
        if speedAccuracy >= 0 {
            self[keyPath: band].accuracyCount += 1
            self[keyPath: band].sumAccuracy += speedAccuracy
            self[keyPath: band].maxAccuracy = max(self[keyPath: band].maxAccuracy, speedAccuracy)
        }
    }

    /// 重置（重新开始一段行程时调用）
    mutating func reset() { self = SpeedAccuracyStats() }

    /// 供界面显示的一行摘要
    static func describe(_ name: String, _ b: Band) -> String {
        guard b.count > 0 else { return "\(name)：无样本" }
        return String(format: "%@：%.0f km/h  均±%.2f 最大±%.2f 被拦 %.0f%%（%d/%d）",
                      name, b.avgSpeedKmh, b.avgAccuracy, b.maxAccuracy,
                      b.rejectRate * 100, b.rejected, b.count)
    }
}
