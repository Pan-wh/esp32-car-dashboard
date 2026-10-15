/**
 * test_ble_policy.swift —— 自动连接决策的单元测试（macOS 本机运行，不需真机/真仪表）
 *
 * 运行（或 `cd tests && make test-ble-policy`）：
 *   swiftc -O -o /tmp/test_ble_policy \
 *     ios/CarDashboardPhone/CarDashboardPhone/BLEConnectionPolicy.swift \
 *     tools/test_ble_policy.swift && /tmp/test_ble_policy
 *
 * 直接编译 App 的**真实源文件**（不内联复制），故测的就是 App 用的那份逻辑
 * —— 与 test_device_faces / test_nav_core 的做法一致。
 *
 * 为什么值得测：这套规则**只能在真机+真仪表上验证**，而最容易错的组合
 * （"用户点了断开，但蓝牙随后就绪 → 又自动连上了"）恰恰是用户一眼能看到的
 * 体验 bug。抽成纯函数后这些组合可以在 PC 上穷举。
 */

import Foundation

// ---- 极简断言（与 tools/test_device_faces.swift 风格一致）----
var gPass = 0
var gFail = 0
func check(_ cond: Bool, _ msg: @autoclosure () -> String) {
    if cond { gPass += 1 } else { gFail += 1; print("  ✗ \(msg())") }
}
func eq<T: Equatable>(_ a: T, _ b: T, _ msg: @autoclosure () -> String) {
    if a == b { gPass += 1 } else { gFail += 1; print("  ✗ \(msg())（实际 \(a)，期望 \(b)）") }
}

typealias P = BLEConnectionPolicy
typealias D = BLEConnectionPolicy.Decision
typealias I = BLEConnectionPolicy.Inputs

// ─────────────────────────────────────────────────────────────
// 1) 核心：蓝牙一就绪就自动扫描（本次优化的主目标）
// ─────────────────────────────────────────────────────────────
func testAutoConnectOnReady() {
    // 刚启动：蓝牙就绪、没连、没在扫描、用户没拒绝 → 必须自动开始扫描
    eq(P.decide(I(bluetoothReady: true)), .startScan,
       "蓝牙就绪应立即自动开始扫描（无需用户手动点连接）")

    // 用户没点过任何按钮（autoConnectEnabled 默认 true）
    check(I(bluetoothReady: true).autoConnectEnabled, "默认应允许自动连接")
}

// ─────────────────────────────────────────────────────────────
// 2) 手动断开必须被尊重 —— 这是最容易写错的一条
// ─────────────────────────────────────────────────────────────
func testManualDisconnectIsRespected() {
    // 用户点了「断开连接」：蓝牙仍然就绪，但**绝不**自动连回去
    eq(P.decide(I(bluetoothReady: true, autoConnectEnabled: false)), .userDisconnected,
       "用户断开后即使蓝牙就绪也不得自动扫描（否则断开按钮形同虚设）")

    // 断开时正在扫描（之前那轮还没结束）→ 同样不扫描
    eq(P.decide(I(bluetoothReady: true, isScanning: true, autoConnectEnabled: false)),
       .userDisconnected,
       "用户断开后即使残留扫描标志也不得自动扫描")

    // 断开时竟然还连着（cancelPeripheralConnection 尚未回调）→ 依然听用户的
    eq(P.decide(I(bluetoothReady: true, isConnected: true, autoConnectEnabled: false)),
       .userDisconnected,
       "用户断开优先于已连接状态（判断顺序不得调换）")

    // 掉线重连的前提：用户还想要连接
    check(P.shouldReconnectAfterDrop(autoConnectEnabled: true), "用户未拒绝 → 掉线应自动重连")
    check(!P.shouldReconnectAfterDrop(autoConnectEnabled: false), "用户已断开 → 掉线不得自动重连")
}

// ─────────────────────────────────────────────────────────────
// 3) 幂等：重复调用不得重复扫描、不得覆盖状态
// ─────────────────────────────────────────────────────────────
func testIdempotent() {
    eq(P.decide(I(bluetoothReady: true, isScanning: true)), .alreadyScanning,
       "已在扫描 → 不重复启动扫描（避免状态文案被反复覆盖）")

    eq(P.decide(I(bluetoothReady: true, isConnected: true)), .alreadyConnected,
       "已连接 → 不再扫描")

    // 已连接 + 还在扫描（didConnect 前扫描已停，但为防御）→ 以"已连接"为准
    eq(P.decide(I(bluetoothReady: true, isConnected: true, isScanning: true)), .alreadyConnected,
       "已连接优先于已在扫描（避免已连接时又去扫描）")
}

// ─────────────────────────────────────────────────────────────
// 4) 蓝牙未就绪：不得扫描（此前会静默什么都不做）
// ─────────────────────────────────────────────────────────────
func testBluetoothNotReady() {
    eq(P.decide(I(bluetoothReady: false)), .bluetoothNotReady, "蓝牙未就绪 → 不扫描")
    eq(P.decide(I(bluetoothReady: false, autoConnectEnabled: false)), .userDisconnected,
       "用户断开优先于蓝牙未就绪（顺序固定）")
}

// ─────────────────────────────────────────────────────────────
// 5) 状态文案：能区分"正在自动连接"与"用户主动断开"
// ─────────────────────────────────────────────────────────────
func testStateText() {
    eq(P.stateText(bluetoothReady: true, isConnected: true, autoConnectEnabled: true),
       "已连接", "已连接文案")
    eq(P.stateText(bluetoothReady: true, isConnected: false, autoConnectEnabled: true),
       "正在连接…", "自动连接中是过渡态，文案应表明正在连")
    eq(P.stateText(bluetoothReady: true, isConnected: false, autoConnectEnabled: false),
       "已断开", "用户断开是稳定态，不得显示成「正在连接」")
    // 已连接时文案不受 autoConnectEnabled 影响（连接是事实）
    eq(P.stateText(bluetoothReady: true, isConnected: true, autoConnectEnabled: false),
       "已连接", "已连接文案优先于用户意图")
    eq(P.stateText(bluetoothReady: false, isConnected: false, autoConnectEnabled: true),
       "蓝牙未就绪", "蓝牙未就绪文案")
}

// ─────────────────────────────────────────────────────────────
// 6) 穷举：任何输入组合下，不该扫描的情形都不得产生 startScan
//    （用暴力枚举兜住"将来有人加分支忘了顺序"的回归）
// ─────────────────────────────────────────────────────────────
func testExhaustiveInvariants() {
    var violations = 0
    var cases = 0
    for ready in [true, false] {
        for conn in [true, false] {
            for scanning in [true, false] {
                for allow in [true, false] {
                    cases += 1
                    let d = P.decide(I(bluetoothReady: ready, isConnected: conn,
                                       isScanning: scanning, autoConnectEnabled: allow))
                    // 不变量 1：用户未允许自动连接 → 永不扫描
                    if !allow && d == .startScan { violations += 1 }
                    // 不变量 2：已连接 → 永不扫描
                    if conn && d == .startScan { violations += 1 }
                    // 不变量 3：已在扫描 → 永不重复扫描
                    if scanning && d == .startScan { violations += 1 }
                    // 不变量 4：蓝牙未就绪 → 永不扫描
                    if !ready && d == .startScan { violations += 1 }
                    // 不变量 5：只有"就绪 + 未连 + 未扫描 + 允许"才扫描
                    if d == .startScan && !(ready && !conn && !scanning && allow) { violations += 1 }
                }
            }
        }
    }
    check(cases == 16, "应枚举全部 16 种组合（实际 \(cases)）")
    check(violations == 0, "16 种组合中存在 \(violations) 处违反自动连接不变量")
    print("  已穷举 \(cases) 种状态组合，不变量违例 \(violations) 处")
}

@main
struct TestBLEPolicy {
    static func main() {
        print("=== BLE 自动连接决策（自动连接 / 手动断开 / 重连）===")
        testAutoConnectOnReady()
        testManualDisconnectIsRespected()
        testIdempotent()
        testBluetoothNotReady()
        testStateText()
        testExhaustiveInvariants()
        print("结果: \(gPass) 通过, \(gFail) 失败")
        exit(gFail == 0 ? 0 : 1)
    }
}
