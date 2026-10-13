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
        // 判别字段：只认 t == "faces"（与遥测包共用通道时的分流依据）
        guard (root["t"] as? String) == "faces" else { return nil }

        guard let rawFaces = root["faces"] as? [[String: Any]] else { return nil }

        var faces: [DeviceFace] = []
        for item in rawFaces {
            guard let id = item["id"] as? Int else { continue }
            let name = (item["name"] as? String) ?? ""
            // label 缺失时用 name 兜底，保证界面一定有可显示的文字
            let label = (item["label"] as? String) ?? name
            let group = (item["group"] as? String) ?? "other"
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

    /// 表盘：优先按稳定 `name` 本地化，**未收录则用设备给的 label**
    /// —— 这是设备新增表盘时 App 无需改动的关键。
    static func faceTitle(name: String, fallbackLabel: String) -> String {
        switch name {
        case "analog": return "模拟时钟"
        case "digital": return "数字时钟"
        case "speed": return "速度表"
        case "color": return "多彩时钟"
        case "nav": return "导航卡片"
        default: return fallbackLabel.isEmpty ? name : fallbackLabel
        }
    }
}
