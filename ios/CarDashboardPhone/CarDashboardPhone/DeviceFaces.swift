/**
 * DeviceFaces.swift —— 设备上报表盘列表的模型与解析
 *
 * ## 背景
 *
 * 此前 App 把表盘列表**硬编码**在 ContentView 的数组里，必须与固件手工保持
 * 一致 —— 已经漏过一次（固件加了 nav 表盘，App 选择器里没有）。
 * 现在改为**设备上报、App 渲染**（协议见 docs/ble.md 的 faces 上报）。
 *
 * ## 未知表盘的兜底（本设计的关键）
 *
 * 本地化按 `name`（稳定 ASCII 标识）查表；**查不到就用设备给的 `label` 显示**。
 * 因此设备新增表盘时，App **无需改动**也能正确列出（只是名字是 ASCII），
 * 这正是"上报表盘列表"要解决的问题。
 *
 * 本文件不依赖 CoreBluetooth，可在 macOS 上直接单测
 * （见 tools/test_ios_logic.swift）。
 */

import Foundation

// MARK: - 模型

/// 设备上的一个表盘
struct DeviceFace: Identifiable, Equatable {
    let id: Int
    /// 稳定 ASCII 标识（如 "analog"）——App 用它做本地化，**不要依赖 label**
    let name: String
    /// 设备给的 ASCII 显示名（App 未收录该 name 时的兜底）
    let label: String
    /// 分组 ASCII 名（如 "clock"）
    let group: String
    /// 是否启用（隐藏的表盘在仪表上滑动时会被跳过）
    var enabled: Bool
}

/// 一次上报的完整内容
struct DeviceFaceReport: Equatable {
    var faces: [DeviceFace] = []
    /// 设备因载荷上限截断了列表 —— App 应提示"列表不完整"，
    /// 而不是把缺失的表盘当成"不存在"（那会让开关状态显示错误）。
    var truncated: Bool = false
    /// **当前正在显示**的表盘 id（对应设备的 `cur` 字段；-1 = 未知）。
    /// 界面据此标注"正在用哪个"，避免选中态与设备实际状态不一致。
    var currentId: Int = -1

    // ── 设备显示状态（亮度 / 方向 / 息屏）────────────────────────
    //
    // 这三项**由设备拥有并上报**，App 只显示、不在连接时下发覆盖。
    //
    // 为什么必须这么设计：用户用 App 把亮度调成 70 → 断掉 App 装上车，
    // 亮度仍是 70；下次仪表单独上电（没开 App）也应保持。
    // 若 App 连接时下发自己记的值，就会把用户在设备上设好的值悄悄冲掉。
    //
    // 用 Optional 而非默认值，是为了区分**"设备没上报"**与**"设备上报了 0"**：
    //   nil → 设备未告知，App 保持当前显示值（不要伪造一个默认值）
    //   0   → 设备明确告知就是 0（息屏 / 亮度 0），必须照实显示
    // 把两者混为一谈正是"亮度被凭空改回默认值"那个 bug 的根源。

    /// 当前亮度 0-255；nil = 设备未上报
    var brightness: Int?
    /// 当前屏幕方向（度）；nil = 设备未上报
    var rotation: Int?
    /// 是否点亮（false = 息屏）；nil = 设备未上报
    var screenOn: Bool?

    // ── 行程统计（由**设备**累积，App 只显示）────────────────────
    //
    // 为什么不让 App 自己算：行程是"这一趟车"的累计值，由仪表持续累积。
    // App 可能被系统杀掉/重装（免费账号 7 天就要重装一次），
    // 本地算会一重启就归零 —— 与仪表上显示的数字对不上，比不显示更糟。
    //
    // 同样用 Optional 区分"没上报"与"确实是 0"（刚出发里程就是 0）。

    /// 累计里程（公里）；nil = 设备未上报
    var tripDistanceKm: Double?
    /// 最高速度（公里/小时）；nil = 设备未上报
    var tripMaxKmh: Double?
    /// 加权平均速度（公里/小时）；nil = 设备未上报
    var tripAvgKmh: Double?
    /// 有效行驶时长（秒）；nil = 设备未上报
    var tripTimeS: Int?

    /// 按分组聚合，保持设备给出的顺序（分组首次出现的先后）
    var grouped: [(group: String, faces: [DeviceFace])] {
        var order: [String] = []
        var bucket: [String: [DeviceFace]] = [:]
        for f in faces {
            if bucket[f.group] == nil { order.append(f.group) }
            bucket[f.group, default: []].append(f)
        }
        return order.map { ($0, bucket[$0] ?? []) }
    }

    /// 当前正在使用的表盘数（供界面提示）
    var enabledCount: Int { faces.filter { $0.enabled }.count }
}

// MARK: - 解析

enum DeviceFaceParser {
    /// 解析设备上报的 JSON。失败返回 nil（**不猜测**，避免显示错误的开关状态）。
    static func parse(_ data: Data) -> DeviceFaceReport? {
        guard let root = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else {
            return nil
        }
        /*
         * 判别字段（与遥测包共用通道时的分流依据）。
         *
         * ⚠️ **必须同时接受 "faces" 和 "f"**：紧凑格式为了省字节把 t 缩成了 "f"，
         *    若这里只认 "faces"，紧凑格式的上报会被整包丢弃 →
         *    **一个表盘都显示不出来**（实测踩过）。
         */
        let tag = root["t"] as? String
        guard tag == "faces" || tag == "f" else { return nil }

        /*
         * ⚠️ **两种格式都要支持**。
         *
         * 旧：`"faces":[{"id":0,"name":"analog","group":"clock","on":1}, ...]` —— 6 个表盘 371 字节
         * 新：`"f":["analog","digital","speed","color","nav","photo"]` —— 约 90 字节
         *
         * 为什么要压：iOS 单次 BLE 读取只有 **~184 字节**，旧格式必然被截断，
         * 结果**排在最后的照片表盘消失**（用户"看不到照片表盘"的真因）。
         * 新格式里 `id` = 下标、`group` 由名字映射、`on` 由 mask 推导。
         */
        if let names = root["f"] as? [String] {
            let mask: UInt32 = {
                if let m = root["m"] as? UInt32 { return m }
                if let m = root["mask"] as? UInt32 { return m }
                if let m = root["m"] as? Int { return UInt32(truncatingIfNeeded: m) }
                if let m = root["mask"] as? Int { return UInt32(truncatingIfNeeded: m) }
                return 0xFFFF_FFFF   // 没给 mask → 全部视为启用
            }()
            var list: [DeviceFace] = []
            for (i, name) in names.enumerated() {
                list.append(DeviceFace(id: i,
                                       name: name,
                                       label: name,
                                       group: (name == "speed" ? "speed"
                                               : name == "nav" ? "nav"
                                               : name == "photo" ? "photo"
                                               : "clock"),
                                       enabled: (mask >> UInt32(i)) & 1 != 0))
            }
            guard !list.isEmpty else { return nil }
            var rep = DeviceFaceReport()
            rep.faces = list
            rep.truncated = ((root["trunc"] as? Int) ?? (root["truncated"] as? Int) ?? 0) == 1
            rep.currentId = (root["u"] as? Int) ?? (root["cur"] as? Int) ?? -1
            rep.brightness = root["bright"] as? Int
            rep.rotation = root["rot"] as? Int
            if let sc = root["screen"] as? Int { rep.screenOn = (sc != 0) }
            return rep
        }

        guard let rawFaces = root["faces"] as? [[String: Any]] else { return nil }

        var faces: [DeviceFace] = []
        for item in rawFaces {
            guard let id = item["id"] as? Int else { continue }
            let name = (item["name"] as? String) ?? (item["n"] as? String) ?? ""
            // label 缺失时用 name 兜底，保证界面一定有可显示的文字
            /*
             * 兼容压缩前后的两种字段名：
             *   - 旧：name / label / group / count / mask / cur / truncated
             *   - 新：n / (无 label) / g / c / m / u / trunc
             * 压缩是为了让 6 个表盘塞进 iOS 单次 BLE 读取（≈184 字节）。
             */
            let label = (item["label"] as? String) ?? (item["l"] as? String) ?? name
            let group = (item["group"] as? String) ?? (item["g"] as? String) ?? "other"
            // on 可能是 1/0，也可能是 true/false（宽容处理）
            let enabled: Bool
            if let n = item["on"] as? Int { enabled = (n != 0) }
            else if let b = item["on"] as? Bool { enabled = b }
            else { enabled = true }
            faces.append(DeviceFace(id: id, name: name, label: label,
                                    group: group, enabled: enabled))
        }

        // 一个都没有时视为解析失败：宁可保持上一次的状态，也不清空界面
        guard !faces.isEmpty else { return nil }

        var report = DeviceFaceReport()
        report.faces = faces.sorted { $0.id < $1.id } // 按 id 排序即注册顺序
        report.truncated = (root["truncated"] as? Int) == 1
        report.currentId = (root["cur"] as? Int) ?? -1
        /*
         * 设备显示状态（可能缺省 → 保持 nil）。
         *
         * ⚠️ 这里**不能**用 `?? 默认值` 兜底：设备没上报时填一个 200，
         *    界面就会显示一个设备上根本不存在的亮度，用户一碰滑块反而把真值改掉。
         *    缺省就保持 nil，由上层决定"沿用当前显示值"。
         */
        report.brightness = root["bright"] as? Int
        report.rotation = root["rot"] as? Int
        if let s = root["screen"] as? Int { report.screenOn = (s != 0) }
        else if let b = root["screen"] as? Bool { report.screenOn = b }

        /*
         * 行程统计（可选）。JSONSerialization 对小数一律给 Double/NSNumber，
         * 用 as? Double 即可；整数（trip_s）单独取。
         */
        report.tripDistanceKm = root["trip_km"] as? Double
        report.tripMaxKmh = root["trip_max"] as? Double
        report.tripAvgKmh = root["trip_avg"] as? Double
        report.tripTimeS = root["trip_s"] as? Int
        return report
    }
}

// MARK: - 本地化（分组名 / 表盘名）

enum FaceDisplay {
    /// 分组 ASCII 名 → 中文标题；未收录的用原名（设备新增分组也能显示）
    static func groupTitle(_ key: String) -> String {
        switch key {
        case "clock": return "时钟"
        case "speed": return "速度表"
        case "nav": return "导航"
        case "photo": return "照片"
        default: return key
        }
    }

    /// 表盘图标（SF Symbol 名）。
    ///
    /// 同样按**稳定 `name`** 查表 —— 与 `faceTitle` 一致：
    /// 设备新增表盘时用通用图标兜底，不需要改 App。
    ///
    /// 为什么不用真实缩略图：那需要设备侧额外做"渲染 → 编码 → 分片传输"，
    /// 是另一个量级的功能；这里的目的是让人**一眼区分是哪种表盘**。
    static func faceSymbol(name: String) -> String {
        switch name {
        case "analog": return "clock"
        case "digital": return "number.square"
        case "color": return "paintpalette"
        case "speed": return "speedometer"
        case "nav": return "location.north.line"
        case "photo": return "photo"
        default: return "square.grid.2x2"
        }
    }

    /// 表盘：优先按稳定 `name` 本地化，**未收录则用设备给的 label**
    /// —— 这是设备新增表盘时 App 无需改动的关键。
    static func faceTitle(name: String, fallbackLabel: String) -> String {
        switch name {
        case "analog": return "模拟时钟"
        case "digital": return "数字时钟"
        case "speed": return "速度表"
        case "color": return "多彩时钟"
        case "nav": return "导航卡片"
        case "photo": return "照片"
        default: return fallbackLabel.isEmpty ? name : fallbackLabel
        }
    }


}
