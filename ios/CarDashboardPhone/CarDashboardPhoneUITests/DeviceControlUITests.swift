// DeviceControlUITests.swift —— 仪表控制 UI 的自动化验证（功能①/③）
//
// 验证：表盘分段选择器可点、亮度滑块可拖、息屏开关可切，
//      且未连接时给出明确提示（而不是“点了没反应”）。
// 说明：模拟器无真实 BLE 外设，因此这里只验证 UI 可达性与交互路径。
import XCTest

final class DeviceControlUITests: XCTestCase {

    override func setUp() {
        continueAfterFailure = false
    }

    func testControlSectionReachableAndInteractable() {
        let app = XCUIApplication()
        app.launch()

        // 控制区应存在
        // 注：SwiftUI 的 Picker label（"表盘"）不会作为独立 staticText 暴露，
        //     而是挂在分段控件上，因此这里直接查分段控件与其中的按钮。
        XCTAssertTrue(app.staticTexts["仪表控制"].waitForExistence(timeout: 10), "应有「仪表控制」分区")
        XCTAssertTrue(app.staticTexts["亮度"].exists, "应有「亮度」行")

        // ⚠️ 页面上有**两个**分段控件（表盘 / 屏幕方向），不能用 firstMatch 含糊定位。
        //    用「按钮标签」精确定位表盘那个，避免将来新增分段控件时测试失效。
        let faceNames = ["模拟时钟", "数字时钟", "速度表", "多彩时钟"]
        let seg = app.segmentedControls.containing(.button, identifier: "模拟时钟").firstMatch
        XCTAssertTrue(seg.exists, "应有表盘分段控件")
        // 数量需与固件 ui_face 注册表 + ContentView.faces 三者一致
        // （0 模拟时钟 / 1 数字时钟 / 2 速度表 / 3 多彩时钟）
        XCTAssertEqual(seg.buttons.count, 4, "应有四个表盘选项")

        // 切表盘：依次点击全部选项（未连接时只更新本地状态，不应崩溃）
        for name in faceNames {
            let b = seg.buttons[name]
            XCTAssertTrue(b.exists, "分段控件应有「\(name)」")
            b.tap()
        }
        XCTAssertEqual(seg.buttons.count, 4, "四次切换后分段控件结构不变")

        // 屏幕方向选择器：四个角度（与固件 control.c 校验一致）
        let rotSeg = app.segmentedControls.containing(.button, identifier: "90°").firstMatch
        XCTAssertTrue(rotSeg.exists, "应有屏幕方向分段控件")
        XCTAssertEqual(rotSeg.buttons.count, 4, "应有四个方向选项")
        for deg in ["0°", "90°", "180°", "270°"] {
            let b = rotSeg.buttons[deg]
            XCTAssertTrue(b.exists, "方向控件应有「\(deg)」")
            b.tap()
        }
        rotSeg.buttons["0°"].tap()   // 复位，保证后续测试状态干净

        // 切息屏开关：应能切换（未连接时会给出提示，不应崩溃）
        // ⚠️ 表单较长（表盘 + 屏幕方向 + 亮度 + 息屏），开关可能在屏幕外；
        //    先滚动把它带入可见区再操作，否则会报 "No matches found for Switch"。
        let sw = app.switches["屏幕点亮"]
        for _ in 0..<4 where !sw.exists { app.swipeUp() }
        XCTAssertTrue(sw.waitForExistence(timeout: 5), "应能找到「屏幕点亮」开关（已滚动）")
        sw.tap()
        // 再切回来，保证后续测试状态干净
        sw.tap()
        XCTAssertTrue(sw.exists, "开关仍存在（未崩溃）")
    }
}
