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
        print("结果: \(gPass) 通过, \(gFail) 失败")
        exit(gFail == 0 ? 0 : 1)
    }
}
