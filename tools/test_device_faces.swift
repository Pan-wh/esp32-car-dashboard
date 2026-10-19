/**
 * test_device_faces.swift —— 设备上报表盘列表的解析与本地化测试
 *
 * 运行（或 `cd tests && make test-device-faces`）：
 *   swiftc -O -o /tmp/test_device_faces \
 *     ios/CarDashboardPhone/CarDashboardPhone/DeviceFaces.swift \
 *     tools/test_device_faces.swift && /tmp/test_device_faces
 *
 * 直接编译 App 的真实源文件（不内联复制），故测的就是 App 用的那份逻辑。
 */

import Foundation

var gPass = 0
var gFail = 0
func check(_ cond: Bool, _ msg: @autoclosure () -> String) {
    if cond { gPass += 1 } else { gFail += 1; print("  ✗ \(msg())") }
}

/// 与固件 face_report_build() 输出同形状的样本（5 个表盘，全部启用）
let realShaped = """
{"t":"faces","v":1,"count":5,"mask":31,"faces":[\
{"id":0,"name":"analog","label":"Analog","group":"clock","on":1},\
{"id":1,"name":"digital","label":"Digital","group":"clock","on":1},\
{"id":2,"name":"speed","label":"Speed","group":"speed","on":1},\
{"id":3,"name":"color","label":"Color","group":"clock","on":1},\
{"id":4,"name":"nav","label":"Nav","group":"nav","on":1}]}
"""

func testParseBasic() {
    print("--- 解析 ---")
    guard let r = DeviceFaceParser.parse(Data(realShaped.utf8)) else {
        check(false, "应能解析合法上报"); return
    }
    check(r.faces.count == 5, "应有 5 个表盘，实际 \(r.faces.count)")
    check(r.truncated == false, "不应标记截断")
    check(r.enabledCount == 5, "应全部启用")
    check(r.faces.map { $0.id } == [0, 1, 2, 3, 4], "应按 id 升序（即注册顺序）")
    check(r.faces[4].name == "nav", "第 5 个应为 nav")
    check(r.faces[4].group == "nav", "nav 的分组应为 nav")
}

func testGrouping() {
    print("--- 分组 ---")
    guard let r = DeviceFaceParser.parse(Data(realShaped.utf8)) else {
        check(false, "解析失败"); return
    }
    let g = r.grouped
    check(g.count == 3, "应有 3 个分组（clock/speed/nav），实际 \(g.count)")
    check(g[0].group == "clock", "首个分组应为 clock")
    // ★ 用户的核心诉求：三个时钟类表盘归属同一分组
    check(g[0].faces.count == 3, "时钟组应含 3 个表盘，实际 \(g[0].faces.count)")
    check(g[0].faces.map { $0.name } == ["analog", "digital", "color"],
          "时钟组内容/顺序不对：\(g[0].faces.map { $0.name })")
    check(g[1].group == "speed" && g[1].faces.count == 1, "速度表组应 1 个")
    check(g[2].group == "nav" && g[2].faces.count == 1, "导航组应 1 个")
}

func testEnabledState() {
    print("--- 启用/隐藏状态 ---")
    let json = """
    {"t":"faces","v":1,"count":3,"mask":5,"faces":[\
    {"id":0,"name":"analog","label":"Analog","group":"clock","on":1},\
    {"id":1,"name":"digital","label":"Digital","group":"clock","on":0},\
    {"id":2,"name":"speed","label":"Speed","group":"speed","on":1}]}
    """
    guard let r = DeviceFaceParser.parse(Data(json.utf8)) else {
        check(false, "解析失败"); return
    }
    check(r.enabledCount == 2, "应有 2 个启用，实际 \(r.enabledCount)")
    check(r.faces[1].enabled == false, "digital 应为隐藏")
    // on 也接受布尔形式（宽容处理）
    let boolJson = """
    {"t":"faces","faces":[{"id":0,"name":"a","label":"A","group":"clock","on":true},\
    {"id":1,"name":"b","label":"B","group":"clock","on":false}]}
    """
    if let rb = DeviceFaceParser.parse(Data(boolJson.utf8)) {
        check(rb.faces[0].enabled && !rb.faces[1].enabled, "on 为布尔时应正确解析")
    } else {
        check(false, "on 为布尔时应能解析")
    }
}

func testUnknownFaceFallsBackToLabel() {
    print("--- ★ 未知表盘用设备 label 兜底（方案 A 的关键） ---")
    let json = """
    {"t":"faces","v":1,"count":2,"faces":[\
    {"id":0,"name":"analog","label":"Analog","group":"clock","on":1},\
    {"id":9,"name":"weather","label":"Weather","group":"weather","on":1}]}
    """
    guard let r = DeviceFaceParser.parse(Data(json.utf8)) else {
        check(false, "解析失败"); return
    }
    check(r.faces.count == 2, "App 未收录的表盘也必须列出来（否则新增表盘就看不见了）")
    // 已收录的用中文
    check(FaceDisplay.faceTitle(name: "analog", fallbackLabel: "Analog") == "模拟时钟",
          "已收录的 name 应本地化")
    // 未收录的用设备给的名字
    check(FaceDisplay.faceTitle(name: "weather", fallbackLabel: "Weather") == "Weather",
          "未收录的 name 应回退到设备 label，实际 \(FaceDisplay.faceTitle(name: "weather", fallbackLabel: "Weather"))")
    // label 也缺失时退回 name（不能显示空白）
    check(FaceDisplay.faceTitle(name: "weather", fallbackLabel: "") == "weather",
          "label 为空应退回 name")
    // 未收录的分组用原名（设备新增分组也能显示）
    check(FaceDisplay.groupTitle("weather") == "weather", "未收录分组应显示原名")
}

func testKnownLocalization() {
    print("--- 本地化 ---")
    check(FaceDisplay.faceTitle(name: "analog", fallbackLabel: "x") == "模拟时钟", "analog")
    check(FaceDisplay.faceTitle(name: "digital", fallbackLabel: "x") == "数字时钟", "digital")
    check(FaceDisplay.faceTitle(name: "speed", fallbackLabel: "x") == "速度表", "speed")
    check(FaceDisplay.faceTitle(name: "color", fallbackLabel: "x") == "多彩时钟", "color")
    check(FaceDisplay.faceTitle(name: "nav", fallbackLabel: "x") == "导航卡片", "nav")
    check(FaceDisplay.groupTitle("clock") == "时钟", "clock 组")
    check(FaceDisplay.groupTitle("speed") == "速度表", "speed 组")
    check(FaceDisplay.groupTitle("nav") == "导航", "nav 组")
    check(FaceDisplay.groupTitle("photo") == "照片", "photo 组")
}

func testTruncated() {
    print("--- 截断标记 ---")
    let json = """
    {"t":"faces","v":1,"count":20,"mask":1,"truncated":1,"faces":[\
    {"id":0,"name":"a","label":"A","group":"clock","on":1}]}
    """
    guard let r = DeviceFaceParser.parse(Data(json.utf8)) else {
        check(false, "解析失败"); return
    }
    check(r.truncated, "★ truncated=1 必须被识别（否则 App 会以为缺失的表盘不存在）")
}

func testRejectsBadInput() {
    print("--- 非法输入（宁可不更新，也不清空界面） ---")
    check(DeviceFaceParser.parse(Data("not json".utf8)) == nil, "非 JSON 应返回 nil")
    check(DeviceFaceParser.parse(Data("{}".utf8)) == nil, "缺 t 应返回 nil")
    // 判别字段不对：遥测包不能被当成表盘列表
    check(DeviceFaceParser.parse(Data("{\"t\":\"tel\",\"faces\":[]}".utf8)) == nil,
          "t 不是 faces 应返回 nil（同一通道分流）")
    check(DeviceFaceParser.parse(Data("{\"t\":\"faces\"}".utf8)) == nil, "缺 faces 数组应返回 nil")
    check(DeviceFaceParser.parse(Data("{\"t\":\"faces\",\"faces\":[]}".utf8)) == nil,
          "空列表应返回 nil（避免把界面清空）")
    // 被 MTU 截断的 JSON（这正是 App 需要重新 Read 的场景）
    let truncated = String(realShaped.prefix(120))
    check(DeviceFaceParser.parse(Data(truncated.utf8)) == nil,
          "★ 截断的 JSON 必须解析失败 —— App 据此判断需要重新 Read")
}

func testPartialFieldTolerance() {
    print("--- 字段缺失的宽容处理 ---")
    let json = """
    {"t":"faces","faces":[{"id":0,"name":"weird"},{"id":1}]}
    """
    guard let r = DeviceFaceParser.parse(Data(json.utf8)) else {
        check(false, "应能解析（缺字段用默认值）"); return
    }
    check(r.faces.count == 2, "两项都应保留")
    check(r.faces[0].group == "other", "缺 group 应为 other")
    check(r.faces[0].enabled, "缺 on 应默认启用")
    check(r.faces[1].label == "", "缺 label 且缺 name 时为空串（界面另有兜底）")
    // id 缺失的条目应被跳过而不是崩溃
    let noId = "{\"t\":\"faces\",\"faces\":[{\"name\":\"x\"},{\"id\":5,\"name\":\"y\"}]}"
    if let r2 = DeviceFaceParser.parse(Data(noId.utf8)) {
        check(r2.faces.count == 1 && r2.faces[0].id == 5, "缺 id 的条目应跳过，保留有效的")
    } else {
        check(false, "含缺 id 条目时应仍能解析出有效项")
    }
}

func testCurrentFace() {
    print("--- 当前表盘（cur）---")
    let json = """
    {"t":"faces","v":1,"count":3,"mask":7,"cur":2,"faces":[\
    {"id":0,"name":"analog","label":"Analog","group":"clock","on":1},\
    {"id":1,"name":"digital","label":"Digital","group":"clock","on":1},\
    {"id":2,"name":"speed","label":"Speed","group":"speed","on":1}]}
    """
    guard let r = DeviceFaceParser.parse(Data(json.utf8)) else {
        check(false, "解析失败"); return
    }
    check(r.currentId == 2, "应解析出 cur=2，实际 \(r.currentId)")
    // 缺 cur 时为 -1（未知），界面不应标错
    let noCur = "{\"t\":\"faces\",\"faces\":[{\"id\":0,\"name\":\"a\",\"group\":\"clock\"}]}"
    if let r2 = DeviceFaceParser.parse(Data(noCur.utf8)) {
        check(r2.currentId == -1, "缺 cur 应为 -1（未知）")
    } else {
        check(false, "应能解析")
    }
}

func testDisplayState() {
    print("--- 设备显示状态（亮度/方向/息屏）---")

    // 1) 正常上报：三个字段都应解析出来
    let full = """
    {"t":"faces","v":1,"count":1,"mask":1,"cur":0,"bright":70,"rot":90,"screen":1,\
    "faces":[{"id":0,"name":"analog","label":"Analog","group":"clock","on":1}]}
    """
    guard let r = DeviceFaceParser.parse(Data(full.utf8)) else {
        check(false, "解析失败"); return
    }
    check(r.brightness == 70, "应解析出亮度 70，实际 \(String(describing: r.brightness))")
    check(r.rotation == 90, "应解析出方向 90，实际 \(String(describing: r.rotation))")
    check(r.screenOn == true, "应解析出 screen=1（点亮）")

    // 2) ★ 缺省必须是 nil（"设备没说"），**不能**兜底成默认值。
    //    这正是"亮度被凭空改回 200"那个 bug 的根源：把"没说"当成"是 200"。
    let bare = "{\"t\":\"faces\",\"faces\":[{\"id\":0,\"name\":\"a\",\"group\":\"clock\"}]}"
    if let r2 = DeviceFaceParser.parse(Data(bare.utf8)) {
        check(r2.brightness == nil, "★ 缺 bright 应为 nil（不得伪造默认值），实际 \(String(describing: r2.brightness))")
        check(r2.rotation == nil, "★ 缺 rot 应为 nil")
        check(r2.screenOn == nil, "★ 缺 screen 应为 nil")
    } else {
        check(false, "应能解析")
    }

    // 3) ★ 0 是**有效值**，必须解析成 0 而不是 nil。
    //    把 0 与"未上报"混为一谈，App 就永远显示不出息屏/亮度 0。
    let zeros = """
    {"t":"faces","cur":0,"bright":0,"rot":0,"screen":0,\
    "faces":[{"id":0,"name":"a","group":"clock","on":1}]}
    """
    if let r3 = DeviceFaceParser.parse(Data(zeros.utf8)) {
        check(r3.brightness == 0, "★ 亮度 0 是有效值，应解析为 0（不是 nil），实际 \(String(describing: r3.brightness))")
        check(r3.rotation == 0, "★ 方向 0° 是有效值，应解析为 0")
        check(r3.screenOn == false, "★ screen=0 应解析为 false（息屏），不是 nil")
    } else {
        check(false, "应能解析")
    }

    // 4) 布尔形式也要宽容（不同固件版本可能发 true/false）
    let boolForm = """
    {"t":"faces","screen":true,"faces":[{"id":0,"name":"a","group":"clock","on":1}]}
    """
    if let r4 = DeviceFaceParser.parse(Data(boolForm.utf8)) {
        check(r4.screenOn == true, "screen 用布尔 true 也应解析")
    } else {
        check(false, "应能解析")
    }

    // 5) 旧固件（完全没有这三个字段）也应正常解析，只是状态未知
    let legacy = """
    {"t":"faces","v":1,"count":2,"mask":3,"cur":1,"faces":[\
    {"id":0,"name":"analog","label":"Analog","group":"clock","on":1},\
    {"id":1,"name":"digital","label":"Digital","group":"clock","on":1}]}
    """
    if let r5 = DeviceFaceParser.parse(Data(legacy.utf8)) {
        check(r5.faces.count == 2, "旧格式仍应解析出表盘（向后兼容）")
        check(r5.brightness == nil && r5.rotation == nil && r5.screenOn == nil,
              "旧格式下三项状态均为 nil（App 保持原显示值）")
    } else {
        check(false, "旧格式应能解析")
    }
}

func testTripStats() {
    print("--- 行程统计（P1-1）---")

    // 1) 完整上报：四个字段都应解析出来
    let full = """
    {"t":"faces","cur":0,"bright":200,"rot":0,"screen":1,\
    "trip_km":12.34,"trip_max":87.6,"trip_avg":45.25,"trip_s":977,\
    "faces":[{"id":0,"name":"analog","label":"Analog","group":"clock","on":1}]}
    """
    guard let r = DeviceFaceParser.parse(Data(full.utf8)) else {
        check(false, "解析失败"); return
    }
    check(r.tripDistanceKm == 12.34, "应解析里程 12.34，实际 \(String(describing: r.tripDistanceKm))")
    check(r.tripMaxKmh == 87.6, "应解析最高速 87.6")
    check(r.tripAvgKmh == 45.25, "应解析均速 45.25")
    check(r.tripTimeS == 977, "应解析时长 977s")

    // 2) ★ 缺省必须是 nil（"设备没说"），不能兜底成 0。
    //    否则刚启动时会显示"里程 0.00 km"——看起来像"这一趟白跑了"。
    let bare = "{\"t\":\"faces\",\"faces\":[{\"id\":0,\"name\":\"a\",\"group\":\"clock\"}]}"
    if let r2 = DeviceFaceParser.parse(Data(bare.utf8)) {
        check(r2.tripDistanceKm == nil, "★ 缺 trip_km 应为 nil（不得伪造 0）")
        check(r2.tripMaxKmh == nil, "缺 trip_max 应为 nil")
        check(r2.tripAvgKmh == nil, "缺 trip_avg 应为 nil")
        check(r2.tripTimeS == nil, "缺 trip_s 应为 nil")
    } else {
        check(false, "应能解析")
    }

    // 3) ★ 0 是**有效值**（刚出发里程就是 0），必须解析成 0 而非 nil
    let zeros = """
    {"t":"faces","cur":0,"trip_km":0.0,"trip_max":0.0,"trip_avg":0.0,"trip_s":0,\
    "faces":[{"id":0,"name":"a","group":"clock","on":1}]}
    """
    if let r3 = DeviceFaceParser.parse(Data(zeros.utf8)) {
        check(r3.tripDistanceKm == 0.0, "★ 里程 0 是有效值，应解析为 0（不是 nil）")
        check(r3.tripMaxKmh == 0.0, "最高速 0 是有效值")
        check(r3.tripTimeS == 0, "时长 0 是有效值")
    } else {
        check(false, "应能解析")
    }

    // 4) 整数形式的小数也应容忍（固件若发 "trip_km":12 而非 12.00）
    let intForm = """
    {"t":"faces","trip_km":12,"faces":[{"id":0,"name":"a","group":"clock","on":1}]}
    """
    if let r4 = DeviceFaceParser.parse(Data(intForm.utf8)) {
        check(r4.tripDistanceKm == 12.0,
              "整数形式的里程也应解析（实际 \(String(describing: r4.tripDistanceKm))）")
    } else {
        check(false, "应能解析")
    }
}

func testFaceSymbols() {
    print("--- 表盘图标映射 ---")
    // 已知表盘应有各自的图标（不能都一样，否则区分不了）
    let known = ["analog", "digital", "color", "speed", "nav"]
    let symbols = known.map { FaceDisplay.faceSymbol(name: $0) }
    check(symbols.allSatisfy { !$0.isEmpty }, "图标名不应为空")
    check(Set(symbols).count == known.count, "★ 每个表盘的图标应互不相同，实际 \(symbols)")
    // 未知表盘要有兜底图标（不能为空，否则界面上留白）
    check(FaceDisplay.faceSymbol(name: "weather") == "square.grid.2x2",
          "未收录表盘应回退到通用图标，实际 \(FaceDisplay.faceSymbol(name: "weather"))")
    check(!FaceDisplay.faceSymbol(name: "").isEmpty, "空 name 也要有图标")
}

@main
struct TestDeviceFaces {
    static func main() {
        print("=== 表盘列表解析与本地化 ===")
        testParseBasic()
        testGrouping()
        testEnabledState()
        testUnknownFaceFallsBackToLabel()
        testKnownLocalization()
        testTruncated()
        testRejectsBadInput()
        testPartialFieldTolerance()
        testCurrentFace()
        testDisplayState()
        testTripStats()
        testFaceSymbols()
        print("结果: \(gPass) 通过, \(gFail) 失败")
        exit(gFail == 0 ? 0 : 1)
    }
}
