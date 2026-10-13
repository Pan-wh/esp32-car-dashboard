/**
 * NavSimMain.swift —— Mac 侧"模拟手机"：用真实路线驱动真机仪表
 *
 * ## 它解决什么问题
 *
 * iPhone 在导航链路里负责：定位 → 匹配路线 → 算进度/偏航 → 经 BLE 下发。
 * 本工具在 Mac 上**完整扮演这个角色**（CBPeripheral 侧由真机承担），
 * 因此**不需要 iPhone、不需要装车**，就能验证：
 *
 *   网关（真实高德路线） → 导航核心（真实 Swift 实现） → BLE → 固件 UI
 *
 * ## 可验证与不可验证
 *
 * ✅ 可验证：网关取路线、坐标转换、导航核心的进度/偏航/到达判定、
 *           BLE 协议与固件解析、仪表 UI 随行程更新
 * ❌ 不可验证：真实 CoreLocation 的精度/频率/后台行为、App 界面（需 iPhone）
 *
 * ## 用法
 *
 *   # 用网关的真实路线驱动（需要网关在跑）
 *   swiftc ... && ./nav_sim --from 116.397428,39.90923 --to 116.461000,39.908700
 *
 *   # 离线：直接读网关响应体的 JSON
 *   ./nav_sim --route-file /tmp/route.json
 *
 *   # 只看会发什么，不连设备
 *   ./nav_sim --route-file /tmp/route.json --dry-run
 *
 *   # 人为制造偏航（验证 rerouting 分支）
 *   ./nav_sim --route-file /tmp/route.json --offroute-from 300 --offroute-offset 80
 */

import Foundation

// MARK: - 参数解析

struct Options {
    var from: String?
    var to: String?
    var routeFile: String?
    var gateway = "http://127.0.0.1:8080"
    var token = ProcessInfo.processInfo.environment["GATEWAY_TOKEN"] ?? ""
    var speedKmh = 40.0
    var interval = 1.0
    var offRouteFrom: Double?
    var offRouteOffset = 0.0
    var dryRun = false
    var maxTicks = 10_000
    var deviceName = "CarDashboard"
}

let USAGE = """
用法：
  nav_sim --from <lon,lat> --to <lon,lat> [选项]        # 经网关取真实路线
  nav_sim --route-file <route.json> [选项]              # 离线：读网关响应体

选项：
  --gateway <url>        网关地址（默认 http://127.0.0.1:8080）
  --token <token>        网关令牌（默认取环境变量 GATEWAY_TOKEN）
  --speed <km/h>         模拟车速（默认 40）
  --interval <秒>        下发间隔（默认 1.0）
  --offroute-from <米>   从该里程起人为偏航
  --offroute-offset <米> 偏航横向偏移（正数向东，默认 0）
  --max-ticks <n>        最多推进多少步（默认 10000）
  --device <名称>        BLE 广播名（默认 CarDashboard）
  --dry-run              不连 BLE，只打印将要发送的内容
  -h, --help             显示本帮助

说明：
  --from/--to 是 **WGS84**（手机定位坐标系），网关会转 GCJ-02。
  导航消息走 Telemetry 特征（7D2A0002），协议见 docs/ble.md。
"""

func parseArgs(_ argv: [String]) -> Options? {
    var o = Options()
    var i = 0
    func next() -> String? { i += 1; return i < argv.count ? argv[i] : nil }

    while i < argv.count {
        let a = argv[i]
        switch a {
        case "--from": o.from = next()
        case "--to": o.to = next()
        case "--route-file": o.routeFile = next()
        case "--gateway": o.gateway = next() ?? o.gateway
        case "--token": o.token = next() ?? o.token
        case "--speed": o.speedKmh = Double(next() ?? "") ?? o.speedKmh
        case "--interval": o.interval = Double(next() ?? "") ?? o.interval
        case "--offroute-from": o.offRouteFrom = Double(next() ?? "")
        case "--offroute-offset": o.offRouteOffset = Double(next() ?? "") ?? 0
        case "--max-ticks": o.maxTicks = Int(next() ?? "") ?? o.maxTicks
        case "--device": o.deviceName = next() ?? o.deviceName
        case "--dry-run": o.dryRun = true
        case "-h", "--help": return nil
        default:
            FileHandle.standardError.write("未知参数：\(a)\n\n".data(using: .utf8)!)
            return nil
        }
        i += 1
    }
    return o
}

/// 解析 "lon,lat"
func parseLonLat(_ s: String) -> GeoPoint? {
    let parts = s.split(separator: ",")
    guard parts.count == 2,
          let lon = Double(parts[0]), let lat = Double(parts[1]),
          lon >= -180, lon <= 180, lat >= -90, lat <= 90 else { return nil }
    return GeoPoint(lon: lon, lat: lat)
}

/// 右侧补空格（Swift 的 String(format:"%@") 不会按宽度填充，故手工处理）
func pad(_ s: String, _ n: Int) -> String {
    s.count >= n ? s : s + String(repeating: " ", count: n - s.count)
}

/// 左侧补空格
func padLeft(_ s: String, _ n: Int) -> String {
    s.count >= n ? s : String(repeating: " ", count: n - s.count) + s
}

let maneuverNames = ["unknown", "straight", "left", "right", "u_turn",
                     "keep_left", "keep_right", "arrive"]
let stateNames = ["idle", "active", "rerouting", "arrived", "stale", "no_data"]

func maneuverName(_ m: Maneuver) -> String {
    let i = m.rawValue
    return (i >= 0 && i < maneuverNames.count) ? maneuverNames[i] : "unknown"
}
func stateName(_ s: NavState) -> String {
    let i = s.rawValue
    return (i >= 0 && i < stateNames.count) ? stateNames[i] : "no_data"
}

// MARK: - 主流程

@main
struct NavSim {
    static func main() {
        let argv = Array(CommandLine.arguments.dropFirst())
        guard let opts = parseArgs(argv) else {
            print(USAGE)
            exit(argv.contains("-h") || argv.contains("--help") ? 0 : 1)
        }

        // ---- 1) 取路线 ----
        let bundle: RouteBundle
        do {
            if let f = opts.routeFile {
                bundle = try GatewayClient.loadRouteFile(f)
                print("[路线] 从文件载入：\(f)")
            } else {
                guard let fs = opts.from, let ts = opts.to,
                      let origin = parseLonLat(fs), let dest = parseLonLat(ts) else {
                    print("需要 --from/--to（格式 lon,lat）或 --route-file。\n")
                    print(USAGE)
                    exit(1)
                }
                guard !opts.token.isEmpty else {
                    print("缺少网关令牌：用 --token 或环境变量 GATEWAY_TOKEN")
                    exit(1)
                }
                print("[路线] 请求网关 \(opts.gateway) …")
                bundle = try GatewayClient.fetchRoute(gateway: opts.gateway,
                                                      token: opts.token,
                                                      origin: origin, destination: dest)
            }
        } catch {
            print("[失败] \(error)")
            exit(2)
        }

        let navRoute = bundle.toNavRoute()
        guard navRoute.polyline.count >= 2 else {
            print("[失败] 路线折线点数不足（\(navRoute.polyline.count)），无法模拟")
            exit(2)
        }
        print("[路线] 总长 \(Int(navRoute.totalDistanceM)) m，\(navRoute.steps.count) 段，折线 \(navRoute.polyline.count) 点")

        // ---- 2) 初始化模拟与导航核心 ----
        var sim = DriveSimulator(route: navRoute, speedMps: opts.speedKmh / 3.6)
        sim.offRouteFromM = opts.offRouteFrom
        sim.offRouteOffsetM = opts.offRouteOffset

        let core = NavCore()
        core.setRoute(navRoute)

        // ---- 3) 连接设备（除非 dry-run）----
        var ble: NavSimBLE?
        if opts.dryRun {
            print("[BLE] --dry-run：不连接设备，仅打印将要发送的内容")
        } else {
            let link = NavSimBLE(deviceName: opts.deviceName)
            guard link.connect() else {
                print("[失败] BLE 未就绪。确认设备已上电、未被其它程序（如串口监视器）占用。")
                exit(3)
            }
            ble = link
        }

        // ---- 4) 模拟行车 ----
        let eta = navRoute.totalDistanceM / max(0.1, sim.speedMps)
        print(String(format: "[模拟] %.0f km/h，间隔 %.1fs，预计 %.0f 秒走完\n",
                     opts.speedKmh, opts.interval, eta))
        print("   t(s)  里程(m)  状态        动作          距转向    垂距")
        print("  ─────────────────────────────────────────────────────────────")

        var t = 0.0
        var tick = 0
        var sentArrived = 0

        while tick < opts.maxTicks {
            guard let gcjTrue = sim.advance(dt: opts.interval) else { break }
            // 先推进时间，让打印的 t 与里程对应（t=1.0 时才是走完第 1 秒的里程）
            t += opts.interval

            // ★ 刻意走一遍"手机上的真实路径"：
            //   车辆真实位置在 GCJ-02 → 转成 WGS84（模拟 CoreLocation 的输出）
            //   → 再转回 GCJ-02（模拟 App 的处理）→ 喂给导航核心。
            //   这样能顺带验证坐标往返**不会破坏路线匹配**（垂距应保持≈0）。
            let wgs = Coords.gcj02ToWgs84(gcjTrue)
            let gcjFix = Coords.wgs84ToGcj02(wgs)

            let nowMs = Int64(t * 1000)
            let fix = NavFix(position: gcjFix, horizontalAccuracyM: 5,
                             courseDeg: nil, timestampMs: nowMs)
            let snap = core.handle(fix: fix, nowMs: nowMs)

            // 构造 BLE 消息：字段与 docs/ble.md 一致
            var json = "{\"t\":\"nav\",\"state\":\(snap.state.rawValue),\"maneuver\":\(snap.maneuver.rawValue)"
            if let d = snap.distanceToManeuverM {
                json += ",\"dist_m\":\(d)"
            } else {
                json += ",\"dist_m\":null"   // 明确无效（与"字段缺失"语义不同）
            }
            json += "}"

            if tick % 2 == 0 || snap.state != .active {   // 每 2 秒打一行，状态变化时必打
                let dm = snap.distanceToManeuverM.map { "\($0) m" } ?? "--"
                print("  " + padLeft(String(format: "%.0f", t), 5)
                      + padLeft(String(format: "%.0f", sim.traveledM), 8)
                      + "  " + pad(stateName(snap.state), 11)
                      + pad(maneuverName(snap.maneuver), 13)
                      + pad(dm, 9)
                      + String(format: "%.1f m", snap.crossTrackM))
            }

            if let b = ble {
                if !b.send(json) { print("  [BLE] 发送失败（已累计 \(b.failCount) 次）") }
            } else if tick < 3 {
                print("    → \(json)")
            }

            if snap.state == .arrived {
                sentArrived += 1
                if sentArrived >= 3 { break }   // 到达后多发几条，确保固件稳定显示
            }

            tick += 1
            if !opts.dryRun { Thread.sleep(forTimeInterval: opts.interval) }
        }

        // ---- 5) 收尾 ----
        print("  ─────────────────────────────────────────────────────────────")
        if let b = ble {
            print("[结果] 发送 \(b.sentCount) 条，失败 \(b.failCount) 条")
            // 保持连接一小会儿，确保最后几条写入完成后再断开
            Thread.sleep(forTimeInterval: 1.5)
            b.disconnect()
        } else {
            print("[结果] dry-run 完成（共 \(tick) 步）")
        }
        print("[结果] 模拟结束：里程 \(Int(sim.traveledM)) m / \(Int(navRoute.totalDistanceM)) m")
        exit(0)
    }
}
