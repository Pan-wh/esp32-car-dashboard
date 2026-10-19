/**
 * BrightnessCurve.swift —— 亮度曲线（App → 设备）的模型与编码
 *
 * ## 设备侧对应实现
 *
 * 协议与判定逻辑必须与固件的 **纯 C** 版本严格一致：
 *   - `simulator/ui/auto_bright.h/.c`   —— 时段判定与亮度插值
 *   - `simulator/ui/bright_parse.h/.c`  —— 包解析（字段名/语义）
 *   - 测试：`tests/test_auto_bright.c` / `tests/test_bright_parse.c`
 *
 * 本文件的 `levelAt()` 是那份 C 实现的**镜像**，供 App 预览
 * （"现在会是什么亮度"）使用。**两边必须同步改** ——
 * 好在协议测试（`tools/test_brightness_curve.swift`）会交叉验证字段名
 * 与边界行为，改漏一边会被测出来。
 *
 * ## 为什么 App 也要有一份
 *
 * 用户拖动时间区间时，App 要能立刻显示"此刻会是多少亮度"，
 * 不能等设备回传（设备不回报算好的亮度，只回报"当前亮度"这一个值）。
 *
 * 本文件不依赖 CoreBluetooth，可在 macOS 上直接单测。
 */

import Foundation

/// 亮度曲线配置（与固件 `auto_bright_config_t` 字段一一对应）
struct BrightnessCurve: Equatable {

    /// 是否启用自动
    var enabled: Bool = true
    /// 白天开始（当天分钟数 0-1439；420 = 07:00）
    var dayStartMin: Int = 7 * 60
    /// 白天结束（当天分钟数 0-1439；1110 = 18:30）
    var dayEndMin: Int = 18 * 60 + 30
    /// 白天亮度 1-255
    var dayLevel: Int = 220
    /// 夜间亮度 1-255
    var nightLevel: Int = 40
    /// 过渡时长（分钟）
    var rampMin: Int = 30

    /**
     请求设备**恢复自动调节**（解除"手动优先"），即使曲线内容没变。

     ## 为什么需要

     用户点「按时段自动」切回自动时，曲线值往往**没变**（还是原来那些），
     设备会把包当成"重连重发"而保持用户手动设的亮度 —— 点了"自动"没反应。
     置此为 true 时协议会带 `reset:1`，设备据此明确恢复自动。

     发送后应立刻清回 false：它表达的是**一次动作**，不是持续状态。
     */
    var requestReset: Bool = false

    /// 协议里的类型标记（必须与固件 `bright_parse.c` 里的比较值一致）
    static let messageType = "bright"

    /// 时长上限（与固件 `bright_apply_packet` 的夹取范围一致）
    static let maxRampMin = 240

    // MARK: - 时间与时:分互转

    /// 分钟数 → "HH:mm"
    static func hhmm(_ minutes: Int) -> String {
        let m = ((minutes % 1440) + 1440) % 1440
        return String(format: "%02d:%02d", m / 60, m % 60)
    }

    /// "HH:mm" → 分钟数；解析失败返回 nil
    static func minutes(fromHHMM s: String) -> Int? {
        let parts = s.split(separator: ":")
        guard parts.count == 2,
              let h = Int(parts[0]), let m = Int(parts[1]),
              (0...23).contains(h), (0...59).contains(m) else { return nil }
        return h * 60 + m
    }

    /// 当天 0 点起的分钟数（用当前本地时间）
    static func nowMinutes(now: Date = Date(), calendar: Calendar = .current) -> Int {
        let c = calendar.dateComponents([.hour, .minute], from: now)
        return (c.hour ?? 0) * 60 + (c.minute ?? 0)
    }

    // MARK: - 判定（固件 auto_bright.c 的镜像）

    /// 归一化到 [0,1440)
    private static func wrap(_ m: Int) -> Int {
        let r = m % 1440
        return r < 0 ? r + 1440 : r
    }

    /// 夹取到 [1,255]（**不允许 0**：0 会让屏全黑，用户以为死机）
    private static func clampLevel(_ v: Int) -> Int {
        min(255, max(1, v))
    }

    /// 当前是否处于"白天"时段（含跨午夜区间）
    private func isDayPeriod(_ nowMin: Int) -> Bool {
        let start = Self.wrap(dayStartMin)
        let end = Self.wrap(dayEndMin)
        if start == end { return false } // 零长度白天 = 全天夜间
        if start < end { return nowMin >= start && nowMin < end }
        return nowMin >= start || nowMin < end // 跨午夜
    }

    /// 当前时段
    var isDaytime: (Int) -> Bool { { self.isDayPeriod($0) } }

    /// 算某时刻的亮度（与固件 `auto_bright_level_at` 同语义）
    func levelAt(_ nowMin: Int) -> Int {
        let day = Self.clampLevel(dayLevel)
        let night = Self.clampLevel(nightLevel)
        guard enabled else { return day } // 关闭时兜底给白天亮度
        if day == night { return day }

        let now = Self.wrap(nowMin)
        var ramp = max(0, min(rampMin, Self.maxRampMin))
        let inDay = isDayPeriod(now)
        if ramp == 0 { return inDay ? day : night }

        let start = Self.wrap(dayStartMin)
        let end = Self.wrap(dayEndMin)
        if start == end { return night } // 零长度白天 → 全天夜间

        /*
         * ★ 限制 ramp 不超过"**夜间段**长度的一半"，防止两个过渡窗口重叠。
         *
         * 天亮过渡在 [start-ramp, start)，天黑过渡在 [end, end+ramp)——
         * 两段都落在夜间段里。若 2*ramp 超过夜间段长度，两者重叠：
         * 同一时刻既算天亮又算天黑，而下面的 if/else 只取其一 →
         * **亮度突跳**（实测 ds=10:51 de=06:33 ramp=222 时，
         * 07:05 是 114，07:10 突然 200）。
         *
         * ⚠️ 必须与固件 `auto_bright.c` 的同名逻辑保持一致 ——
         *    否则 App 预览的亮度与设备实际亮度不符
         *    （由 `test_brightness_curve.swift` 交叉验证兜底）。
         */
        let dayLen = (start <= end) ? (end - start) : (1440 - start + end)
        let nightLen = 1440 - dayLen
        ramp = min(ramp, nightLen / 2)
        if ramp == 0 { return inDay ? day : night }

        let toStart = (start - now + 1440) % 1440
        let fromEnd = (now - end + 1440) % 1440

        if toStart > 0 && toStart <= ramp {
            let t = 1.0 - Double(toStart) / Double(ramp)
            return Self.clampLevel(Self.lerp(night, day, t))
        }
        if fromEnd >= 0 && fromEnd < ramp {
            let t = Double(fromEnd) / Double(ramp)
            return Self.clampLevel(Self.lerp(day, night, t))
        }
        return inDay ? day : night
    }

    private static func lerp(_ a: Int, _ b: Int, _ t: Double) -> Int {
        if t <= 0 { return a }
        if t >= 1 { return b }
        return Int((Double(a) + (Double(b) - Double(a)) * t).rounded())
    }

    // MARK: - 编码

    /**
     编码为设备能解析的 JSON（字段名必须与固件一致）。

     ```json
     {"t":"bright","v":1,"on":1,"ds":420,"de":1110,"day":220,"night":40,"ramp":30}
     ```
     */
    func encode() throws -> Data {
        var dict: [String: Any] = [
            "t": Self.messageType,
            "v": 1,
            "on": enabled ? 1 : 0,
            "ds": dayStartMin,
            "de": dayEndMin,
            "day": dayLevel,
            "night": nightLevel,
            "ramp": rampMin,
        ]
        /*
         * 只在需要时才带 reset（平时不带），避免把"一次动作"变成干扰画线的噪声。
         * 用 Int 而不是 Bool：固件解析器认整数（JSON 里 true 走的是 skip 分支）。
         */
        if requestReset { dict["reset"] = 1 }
        /*
         * 用 JSONSerialization 而不是 Codable：这里的键名是**协议字段**
         * （ds/de/day/night），与 Swift 命名习惯差别大，用字典更直观、
         * 也便于和固件的字段表逐条对照。
         * options 留空 = 紧凑无空白（与遥测包一致，省 BLE 带宽）。
         */
        return try JSONSerialization.data(withJSONObject: dict, options: [])
    }

    /// 供 UI 显示的一行摘要（如 "07:00-18:30 · 220/40"）
    var summary: String {
        "\(Self.hhmm(dayStartMin))-\(Self.hhmm(dayEndMin)) · \(dayLevel)/\(nightLevel)"
    }
}
