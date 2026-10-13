import XCTest
final class BLEStateUITests: XCTestCase {
    func testBLEStateUpdatesWithoutPressingStart() {
        let app = XCUIApplication()
        app.launch()
        // 不点「开始驾驶」，只等界面自动刷新
        sleep(4)
        var texts: [String] = []
        for t in app.staticTexts.allElementsBoundByIndex { texts.append(t.label) }
        let joined = texts.joined(separator: " | ")
        print("@@UI@@ " + joined)
        // 关键断言：BLE 状态不应停留在"初始化中"
        XCTAssertFalse(joined.contains("初始化中"),
                       "BLE 状态不应停留在「初始化中」（objectWillChange 转发修复失效）")
        // 应出现扫描/就绪/已连接一类的终态
        let ok = joined.contains("蓝牙就绪") || joined.contains("已连接")
                  || joined.contains("扫描") || joined.contains("未授权")
                  || joined.contains("不支持")
        XCTAssertTrue(ok, "应显示 BLE 的确定状态，实际: \(joined)")
    }
}
