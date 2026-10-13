/**
 * ble_speed_probe.swift —— 持续注入变速遥测，用于实测「指针是否卡顿」
 *
 * ## 为什么需要
 *
 * 心跳行里的"重绘/s"只在**有数据变化**时才有意义：手机没连、速度不变时
 * 重绘率恒为 0，看不出流畅度。要判断指针卡顿，必须**让指针持续动起来**，
 * 同时观察设备侧的渲染耗时。
 *
 * 本工具按真实 App 的频率（默认 2Hz）持续下发**连续变化**的车速，
 * 配合串口抓取 `[运行]` / `渲染最大` 行，即可量化：
 *   - 指针移动期间的重绘率
 *   - 单帧渲染耗时峰值（是否出现 >100ms 的卡顿帧）
 *
 * ## 车速轨迹（三段，覆盖不同动态特性）
 *
 *   1. 慢扫   0 → 120 km/h      观察**连续平滑**移动
 *   2. 快扫   120 → 0 → 120     观察急速变化时指针能否跟上（固件有时间斜坡）
 *   3. 抖动   ±3 km/h 随机      观察小抖动是否被抑制（不该抖成噪声）
 *
 * ## 用法
 *
 *   swiftc -O -o /tmp/ble_speed_probe \
 *     tools/nav_sim/NavSimBLE.swift tools/ble_speed_probe.swift
 *   /tmp/ble_speed_probe [总秒数] [Hz]
 *
 * 注：**只写 BLE，不碰固件构建**，因此可与其他构建任务并行。
 */
import CoreBluetooth
import Foundation

// MARK: - 遥测包（字段名与固件 telemetry.c 严格一致）

struct ProbeTelemetry {
    var speedMps: Double
    var tsUtcMs: Int64

    /// 构造 JSON。质量字段填"可用"，避免被 §5.4 门槛拦掉（那样指针根本不会动）。
    func json() -> String {
        let lat = 31.230416
        let lon = 121.473701
        return """
        {"v":1,"ts_utc_ms":\(tsUtcMs),\
        "speed_mps":\(String(format: "%.3f", speedMps)),\
        "speed_accuracy_mps":0.15,\
        "latitude":\(lat),"longitude":\(lon),\
        "horizontal_accuracy_m":5.0,\
        "course_deg":90.0,\
        "location_valid":true,"speed_valid":true}
        """
    }
}

/// 车速轨迹：返回第 t 秒（0 起）应发的车速 km/h
func speedProfile(elapsed: Double) -> (kmh: Double, phase: String) {
    if elapsed < 20 {
        // 1) 慢扫 0 → 120，20 秒走完；用平滑曲线避免生硬的匀速
        let p = elapsed / 20.0
        let eased = p * p * (3 - 2 * p)          // smoothstep
        return (eased * 120.0, "慢扫")
    } else if elapsed < 32 {
        // 2) 快扫 120 → 0 → 120，12 秒内两次满量程，考验指针跟随
        let p = (elapsed - 20.0) / 12.0          // 0..1
        let tri = p < 0.5 ? (1 - p * 2) : ((p - 0.5) * 2)
        return (tri * 120.0, "快扫")
    } else {
        // 3) 抖动：在 60 km/h 附近 ±3 km/h 随机走，检验小抖动抑制
        let p = (elapsed - 32.0) * 3.0
        let jitter = sin(p) * 2.0 + sin(p * 2.7) * 1.0
        return (60.0 + jitter, "抖动")
    }
}

final class Probe: NSObject {
    private let link = NavSimBLE()
    private let totalSec: Double
    private let hz: Double

    init(totalSec: Double, hz: Double) {
        self.totalSec = totalSec
        self.hz = hz
        super.init()
    }

    func run() -> Int32 {
        guard link.connect() else {
            print("[失败] BLE 未就绪（设备是否已上电 / 被其它程序占用？）")
            return 3
        }

        let interval = 1.0 / hz
        let start = Date()
        var sent = 0
        var lastPhase = ""

        print(String(format: "[探测] 共 %.0f 秒，%.1f Hz；车速分三段：慢扫 20s → 快扫 12s → 抖动",
                     totalSec, hz))
        print("   t(s)   车速(km/h)  段")
        print("  ───────────────────────────")

        while true {
            let elapsed = Date().timeIntervalSince(start)
            if elapsed >= totalSec { break }

            let (kmh, phase) = speedProfile(elapsed: elapsed)
            let t = ProbeTelemetry(speedMps: kmh / 3.6,
                                   tsUtcMs: Int64(Date().timeIntervalSince1970 * 1000))
            if link.send(t.json()) { sent += 1 }

            if phase != lastPhase {
                print(String(format: "  %5.0f   %8.1f  %@ ← 进入", elapsed, kmh, phase))
                lastPhase = phase
            } else if sent % Int(hz * 4) == 0 {
                print(String(format: "  %5.0f   %8.1f  %@", elapsed, kmh, phase))
            }

            // 按固定节拍发送（扣掉本帧耗时，避免累积漂移）
            let target = start.addingTimeInterval(Double(sent) * interval)
            let sleep = target.timeIntervalSinceNow
            if sleep > 0 { Thread.sleep(forTimeInterval: sleep) }
        }

        print("  ───────────────────────────")
        print("[结果] 发送 \(sent) 条，失败 \(link.failCount) 条，用时 \(Int(Date().timeIntervalSince(start)))s")
        // 给最后几包写入完成的时间，再断开
        Thread.sleep(forTimeInterval: 1.0)
        link.disconnect()
        return 0
    }
}

@main
struct SpeedProbe {
    static func main() {
        // 注意：多文件编译时顶层表达式只允许出现在 main.swift；
        // 本文件不叫 main.swift，故用 @main 入口。
        let args = CommandLine.arguments
        let total = args.count > 1 ? (Double(args[1]) ?? 60) : 60
        let hz = args.count > 2 ? (Double(args[2]) ?? 2) : 2
        exit(Probe(totalSec: max(5, total), hz: max(0.2, hz)).run())
    }
}
