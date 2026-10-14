/**
 * NavPacket.swift —— 下发给仪表的导航消息
 *
 * 协议（与固件 `nav_parse.c` 的解析字段、网关/BLE 协议文档 §3.4 一致）：
 *
 *     {"t":"nav","state":N,"maneuver":N,"dist_m":N}
 *
 * ⚠️ 要点：
 *   1. **`t` 必须是 "nav"** —— 固件用它与遥测包分流（两者共用同一个特征）。
 *      若漏了 `t`，固件会当成遥测去解析，必然失败并计入 `解析错`。
 *   2. `state` / `maneuver` 的数值必须与固件 `nav_state_t` / `nav_maneuver_t`
 *      逐值一致（已在 `NavCore.swift` 的枚举注释里声明；本文件直接复用那两个枚举）。
 *   3. **路名不下发**：固件的 nav 表盘当前只显示"距离 + 箭头"（用户决策 j2），
 *      这样也就不需要中文字模。将来若要显示路名，须先解决字模与带宽。
 *   4. `dist_m` 未知时**省略该字段**（而不是发 0）—— 固件据此显示 "--"，
 *      避免把"未知"伪装成"距离为 0"（0 会被读成"马上要转向"）。
 */

import Foundation

/// 导航消息。字段名用 CodingKeys 映射为固件期望的下划线风格。
struct NavPacket: Codable {
    /// 消息类型标识，恒为 "nav"
    var t: String = "nav"
    /// 导航状态（NavState.rawValue）
    var state: Int
    /// 下一动作（Maneuver.rawValue）
    var maneuver: Int
    /// 距下一动作的距离（米）；nil = 未知，序列化时字段整个省略
    var distM: Int?

    enum CodingKeys: String, CodingKey {
        case t
        case state
        case maneuver
        case distM = "dist_m"
    }

    /// 从导航快照构造（这是唯一的正常来源）
    init(snapshot: NavSnapshot) {
        self.state = snapshot.state.rawValue
        self.maneuver = snapshot.maneuver.rawValue
        self.distM = snapshot.distanceToManeuverM
    }

    /// 直接构造（测试/调试用）
    init(state: NavState, maneuver: Maneuver, distM: Int?) {
        self.state = state.rawValue
        self.maneuver = maneuver.rawValue
        self.distM = distM
    }

    /// 最紧凑的 JSON（利于塞进 BLE MTU；与 TelemetryPacket.encode 同一策略）
    func encode() throws -> Data {
        let encoder = JSONEncoder()
        encoder.outputFormatting = []
        return try encoder.encode(self)
    }
}
