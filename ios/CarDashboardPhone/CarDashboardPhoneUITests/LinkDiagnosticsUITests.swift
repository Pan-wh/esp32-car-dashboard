// LinkDiagnosticsUITests.swift —— 模拟器定位场景下的链路诊断
//
// 目的：在不依赖真实 GPS 的情况下验证 iOS 侧数据生产链路：
//   simctl Freeway Drive → CoreLocation → 质量门槛 → 界面数值
//
// 运行前提（场景需在外部启动）：
//   xcrun simctl location <udid> run "Freeway Drive"
//
// 重要：App 内容是长表单，「实时数据」区默认在屏幕外，
//       XCUITest 默认只返回可见元素，因此必须先 swipeUp 再查询。
//       （首次写这个测试时因忽略这点，读到的全是"缺失"，误判为 bug。）
import XCTest

final class LinkDiagnosticsUITests: XCTestCase {

    /// 在已滚动到的界面中查找标签对应的值（SwiftUI row 渲染为相邻两个 staticText）
    private func value(_ app: XCUIApplication, _ label: String) -> String? {
        let all = app.staticTexts.allElementsBoundByIndex
        for (i, t) in all.enumerated() where t.label == label {
            if i + 1 < all.count { return all[i + 1].label }
        }
        return nil
    }

    func testLocationPipelineProducesSpeedAndCourse() {
        let app = XCUIApplication()
        app.launch()

        let start = app.buttons["开始驾驶"]
        XCTAssertTrue(start.waitForExistence(timeout: 10), "应显示「开始驾驶」")
        start.tap()
        XCTAssertTrue(app.buttons["结束驾驶"].waitForExistence(timeout: 10), "会话应已开始")

        // 等定位数据到达（Freeway Drive 场景通常几秒内就有）
        var got = false
        for _ in 0..<15 {
            app.swipeUp()                       // 暴露「实时数据」区
            if let s = value(app, "速度"), s != "--" { got = true; break }
            usleep(1_000_000)
        }

        app.swipeUp()
        let speed = value(app, "速度")
        let course = value(app, "航向")
        let raw = value(app, "speed 原始值")
        let quality = value(app, "速度质量")
        print("@@速度=\(speed ?? "缺失") 航向=\(course ?? "缺失") 原始=\(raw ?? "缺失") 质量=\(quality ?? "缺失")")

        XCTAssertTrue(got, "应收到定位数据（确认已运行 simctl Freeway Drive 场景）")
        XCTAssertNotNil(speed, "应有速度值")
        XCTAssertNotNil(course, "应有航向值")

        app.swipeDown()
        if app.buttons["结束驾驶"].exists { app.buttons["结束驾驶"].tap() }
    }
}
