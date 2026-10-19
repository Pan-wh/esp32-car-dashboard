/**
 * test_brightness_curve.swift —— App 侧亮度曲线：协议契约 + 与固件交叉验证
 *
 * 运行（或 `cd tests && make test-brightness-curve`）：
 *   swiftc -O -o /tmp/test_bc \
 *     ios/CarDashboardPhone/CarDashboardPhone/BrightnessCurve.swift \
 *     tools/test_brightness_curve.swift && /tmp/test_bc
 *
 * 直接编译 App 的**真实源文件**（不内联复制），故测的就是 App 用的逻辑。
 *
 * ## 为什么这个测试特别重要
 *
 * App（Swift）与固件（C）各有一份"算亮度"的实现：
 *   - App：`BrightnessCurve.levelAt()` —— 用于界面预览
 *   - 固件：`auto_bright_level_at()`（C）—— 真正驱动面板
 *
 * 两份实现**必须给出同样的结果**，否则会出现"App 预览说 220，
 * 设备实际是 40"这种最令人困惑的 bug。本测试把 C 侧的关键用例
 * **在 Swift 侧重放一遍**，任何一边改漏都会被这里的断言挡住。
 *
 * 同时验证 JSON 字段名与固件 `bright_parse.c` 一致（协议契约）。
 */

import Foundation

var gPass = 0
var gFail = 0
func check(_ cond: Bool, _ msg: @autoclosure () -> String) {
    if cond { gPass += 1 } else { gFail += 1; print("  ✗ \(msg())") }
}
func eq<T: Equatable>(_ a: T, _ b: T, _ msg: @autoclosure () -> String) {
    if a == b { gPass += 1 } else { gFail += 1; print("  ✗ \(msg())（实际 \(a)，期望 \(b)）") }
}

func hhmm(_ h: Int, _ m: Int) -> Int { h * 60 + m }

/// 与固件测试 (test_auto_bright.c) 用同一套配置，便于逐项对照
func basicCurve() -> BrightnessCurve {
    var c = BrightnessCurve()
    c.enabled = true
    c.dayStartMin = hhmm(7, 0)
    c.dayEndMin = hhmm(18, 30)
    c.dayLevel = 220
    c.nightLevel = 40
    c.rampMin = 30
    return c
}

// ─────────────────────────────────────────────────────────────
// 1) 协议契约：JSON 字段名必须与固件 bright_parse.c 完全一致
// ─────────────────────────────────────────────────────────────
func testProtocolContract() {
    print("--- 协议契约（字段名）---")
    let c = basicCurve()
    guard let data = try? c.encode(),
          let json = String(data: data, encoding: .utf8),
          let obj = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else {
        check(false, "编码失败"); return
    }

    // 固件 bright_parse.c 认这些键：t / on / ds / de / day / night / ramp
    eq(obj["t"] as? String, "bright", "★ 类型标记必须是 bright（固件靠它分流）")
    eq(obj["on"] as? Int, 1, "on 字段名/值")
    eq(obj["ds"] as? Int, hhmm(7, 0), "ds = 白天开始（分钟）")
    eq(obj["de"] as? Int, hhmm(18, 30), "de = 白天结束（分钟）")
    eq(obj["day"] as? Int, 220, "day = 白天亮度")
    eq(obj["night"] as? Int, 40, "night = 夜间亮度")
    eq(obj["ramp"] as? Int, 30, "ramp = 过渡分钟")

    // 紧凑编码（省 BLE 带宽，与遥测包一致）
    check(!json.contains(" "), "★ 不应有空格（紧凑编码省带宽），实际：\(json)")
    check(json.count < 120, "单包应远小于 BLE MTU（实际 \(json.count) 字节）")

    // 关闭时 on 必须是 0（**不是省略**）—— 0 是有效值，固件靠它区分开关
    var off = c
    off.enabled = false
    if let d2 = try? off.encode(),
       let o2 = try? JSONSerialization.jsonObject(with: d2) as? [String: Any] {
        eq(o2["on"] as? Int, 0, "★ 关闭时 on=0（不能省略，否则固件当成'未提供'）")
    } else {
        check(false, "关闭态编码失败")
    }
}

// ─────────────────────────────────────────────────────────────
// 2) 与固件 C 实现交叉验证（同一组用例）
//    ⚠️ 这些期望值与 tests/test_auto_bright.c 里的断言**一一对应**。
//       改任何一边的算法，这里都会失败 —— 这正是我们要的。
// ─────────────────────────────────────────────────────────────
func testMirrorsFirmware() {
    print("--- 与固件 C 实现交叉验证 ---")
    let c = basicCurve()

    eq(c.levelAt(hhmm(12, 0)), 220, "正午 = 白天亮度")
    eq(c.levelAt(hhmm(23, 0)), 40, "深夜 = 夜间亮度")
    eq(c.levelAt(hhmm(3, 0)), 40, "凌晨 = 夜间亮度")
    eq(c.levelAt(hhmm(8, 0)), 220, "08:00 = 白天亮度")
    eq(c.levelAt(hhmm(18, 0)), 220, "18:00 = 白天亮度")

    // 过渡段边界
    eq(c.levelAt(hhmm(7, 0) - 30), 40, "★ 天亮过渡起点 = 夜间亮度（固件同）")
    eq(c.levelAt(hhmm(7, 0)), 220, "★ 天亮起点 = 白天亮度（固件同）")
    let mid = c.levelAt(hhmm(7, 0) - 15)
    check(mid > 40 && mid < 220, "★ 过渡中点应介于两者之间（实际 \(mid)）")
    eq(c.levelAt(hhmm(18, 30) + 30), 40, "★ 天黑过渡结束 = 夜间亮度（固件同）")

    // 单调性（与固件一致，防止亮度来回抖）
    var prev = -1
    var mono = true
    for m in (hhmm(7, 0) - 30)...hhmm(7, 0) {
        let lv = c.levelAt(m)
        if prev >= 0 && lv < prev { mono = false }
        prev = lv
    }
    check(mono, "★ 天亮过渡单调不降")

    prev = -1
    mono = true
    for m in hhmm(18, 30)...(hhmm(18, 30) + 30) {
        let lv = c.levelAt(m)
        if prev >= 0 && lv > prev { mono = false }
        prev = lv
    }
    check(mono, "★ 天黑过渡单调不升")
}

// ─────────────────────────────────────────────────────────────
// 3) 用户滑动调整区间
// ─────────────────────────────────────────────────────────────
func testUserAdjustsRange() {
    print("--- 用户左右滑区间 ---")
    var c = basicCurve()

    // 起点往左拖到 05:30
    c.dayStartMin = hhmm(5, 30)
    eq(c.levelAt(hhmm(6, 0)), 220, "★ 起点拖到 05:30 后 06:00 = 白天亮度")
    eq(c.levelAt(hhmm(4, 20)), 40, "起点提前后 04:20 仍是夜间")

    // 终点往右拖到 21:00
    var c2 = basicCurve()
    c2.dayEndMin = hhmm(21, 0)
    eq(c2.levelAt(hhmm(20, 30)), 220, "★ 终点拖到 21:00 后 20:30 = 白天亮度")
    eq(basicCurve().levelAt(hhmm(20, 30)), 40, "默认 18:30 结束时 20:30 = 夜间亮度")
}

// ─────────────────────────────────────────────────────────────
// 4) 跨午夜区间（与固件同语义）
// ─────────────────────────────────────────────────────────────
func testCrossMidnight() {
    print("--- 跨午夜（20:00→06:00 算白天）---")
    var c = basicCurve()
    c.dayStartMin = hhmm(20, 0)
    c.dayEndMin = hhmm(6, 0)
    c.rampMin = 30

    eq(c.levelAt(hhmm(23, 0)), 220, "★ 跨午夜区间内 23:00 = 白天亮度")
    eq(c.levelAt(hhmm(2, 0)), 220, "★ 跨午夜区间内 02:00 = 白天亮度")
    eq(c.levelAt(hhmm(12, 0)), 40, "★ 区间外 12:00 = 夜间亮度")

    // 全天不得越界
    var ok = true
    for m in 0..<1440 {
        let lv = c.levelAt(m)
        if lv < 1 || lv > 255 { ok = false; print("      越界于 \(m): \(lv)"); break }
    }
    check(ok, "★ 跨午夜全天采样 1-255")
}

// ─────────────────────────────────────────────────────────────
// 4b) 过渡窗口重叠（属性测试在 C 侧抓到的 bug，两边必须一致）
// ─────────────────────────────────────────────────────────────
func testTransitionOverlap() {
    print("--- 过渡窗口重叠（跨午夜 + 大 ramp）---")
    var c = BrightnessCurve()
    c.enabled = true
    c.dayStartMin = 651   // 10:51
    c.dayEndMin = 393     // 06:33
    c.dayLevel = 100
    c.nightLevel = 200
    c.rampMin = 222       // 远大于夜间段一半(129) → 应被夹取

    /*
     * 修复前：07:05 -> 114，07:10 -> 200（一步跳 84），
     * 因为两个过渡窗口在夜间段里重叠，if/else 只取了一个。
     * 修复后 ramp 被夹到 129，曲线平滑。
     */
    var worst = 0
    for m in 0..<1440 {
        let a = c.levelAt(m)
        let b = c.levelAt((m + 1) % 1440)
        worst = max(worst, abs(b - a))
    }
    check(worst <= 3, "★ 跨午夜 + 大 ramp 不得突跳（最大相邻差 \(worst)，期望 ≤3）")

    // 与固件的期望值对齐（C 侧同样算出 121/125/129/133/136）
    eq(c.levelAt(7 * 60), 121, "07:00 亮度应与固件一致")
    eq(c.levelAt(7 * 60 + 5), 125, "07:05 亮度应与固件一致")
    eq(c.levelAt(7 * 60 + 10), 129, "07:10 亮度应与固件一致")

    // 分段边界仍要正确
    for m in 0..<1440 {
        let v = c.levelAt(m)
        if v < 1 || v > 255 {
            check(false, "越界于 \(m): \(v)")
            break
        }
    }
    check(true, "全天亮度在 1-255")
}

// ─────────────────────────────────────────────────────────────
// 5) 安全边界（与固件同）
// ─────────────────────────────────────────────────────────────
func testSafetyBounds() {
    print("--- 安全边界 ---")
    let c = basicCurve()

    // ★ 永不为 0：0 会让屏全黑，用户以为死机
    var ok = true
    for m in stride(from: 0, to: 1440, by: 7) {
        let lv = c.levelAt(m)
        if lv < 1 || lv > 255 { ok = false; break }
    }
    check(ok, "★ 全天采样亮度均在 1-255")

    var z = basicCurve()
    z.dayLevel = 0; z.nightLevel = 0
    eq(z.levelAt(hhmm(12, 0)), 1, "★ 亮度 0 应夹到 1")
    eq(z.levelAt(hhmm(23, 0)), 1, "★ 夜间 0 也应夹到 1")

    var hi = basicCurve()
    hi.dayLevel = 999
    eq(hi.levelAt(hhmm(12, 0)), 255, "超上限应夹到 255")

    // ramp = 0：硬切换
    var r0 = basicCurve()
    r0.rampMin = 0
    eq(r0.levelAt(hhmm(7, 0) - 1), 40, "ramp=0 起点前一分钟 = 夜间")
    eq(r0.levelAt(hhmm(7, 0)), 220, "ramp=0 起点 = 白天")

    // 零长度白天 → 全天夜间
    var eqz = basicCurve()
    eqz.dayStartMin = hhmm(12, 0)
    eqz.dayEndMin = hhmm(12, 0)
    eq(eqz.levelAt(hhmm(12, 0)), 40, "★ 零长度白天 = 全天夜间")

    // 同值 → 恒为该值
    var same = basicCurve()
    same.dayLevel = 128
    same.nightLevel = 128
    eq(same.levelAt(hhmm(3, 0)), 128, "同值时深夜 = 该值")
    eq(same.levelAt(hhmm(12, 0)), 128, "同值时正午 = 该值")

    // 关闭 → 兜底白天亮度
    var off = basicCurve()
    off.enabled = false
    eq(off.levelAt(hhmm(23, 0)), 220, "关闭自动时兜底给白天亮度")

    // 越界分钟数应环形归一
    eq(c.levelAt(-60), c.levelAt(1380), "负分钟数应环形归一（-60 == 1380）")
    eq(c.levelAt(1440 + 60), c.levelAt(60), "超 1440 也应归一")
}

// ─────────────────────────────────────────────────────────────
// 6) 时间格式化与解析（UI 用）
// ─────────────────────────────────────────────────────────────
func testTimeHelpers() {
    print("--- 时间互转 ---")
    eq(BrightnessCurve.hhmm(0), "00:00", "0 分钟 → 00:00")
    eq(BrightnessCurve.hhmm(hhmm(7, 0)), "07:00", "420 → 07:00")
    eq(BrightnessCurve.hhmm(hhmm(18, 30)), "18:30", "1110 → 18:30")
    eq(BrightnessCurve.hhmm(hhmm(23, 59)), "23:59", "1439 → 23:59")
    eq(BrightnessCurve.hhmm(-60), "23:00", "负值应环形归一")
    eq(BrightnessCurve.hhmm(1440), "00:00", "1440 应归一为 00:00")

    eq(BrightnessCurve.minutes(fromHHMM: "07:00"), hhmm(7, 0), "解析 07:00")
    eq(BrightnessCurve.minutes(fromHHMM: "23:59"), hhmm(23, 59), "解析 23:59")
    check(BrightnessCurve.minutes(fromHHMM: "24:00") == nil, "24:00 应拒绝（越界）")
    check(BrightnessCurve.minutes(fromHHMM: "7:0") == hhmm(7, 0), "单位数也应能解析")
    check(BrightnessCurve.minutes(fromHHMM: "abc") == nil, "非法输入应返回 nil")
    check(BrightnessCurve.minutes(fromHHMM: "") == nil, "空串应返回 nil")

    // 默认值合理性
    let d = BrightnessCurve()
    check(d.enabled, "默认应启用自动")
    check(d.dayStartMin < d.dayEndMin, "默认白天区间应有序")
    check(d.dayLevel > d.nightLevel, "★ 白天亮度应高于夜间（否则逻辑反了）")
    check(d.nightLevel >= 1 && d.dayLevel <= 255, "默认亮度应在合法范围")
    check(d.rampMin > 0, "默认应有过渡")
}

@main
struct TestBrightnessCurve {
    static func main() {
        print("=== 亮度曲线（App 侧）：协议契约 + 与固件交叉验证 ===")
        testProtocolContract()
        testMirrorsFirmware()
        testUserAdjustsRange()
        testCrossMidnight()
        testTransitionOverlap()
        testSafetyBounds()
        testTimeHelpers()
        print("结果: \(gPass) 通过, \(gFail) 失败")
        exit(gFail == 0 ? 0 : 1)
    }
}
