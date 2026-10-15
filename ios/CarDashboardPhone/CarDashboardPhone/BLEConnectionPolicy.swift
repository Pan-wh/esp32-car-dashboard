/**
 * BLEConnectionPolicy.swift —— 自动连接的**决策逻辑**（纯函数，可在 macOS 单测）
 *
 * ## 为什么单独抽出来
 *
 * "蓝牙就绪就自动连、掉线自动重连、但用户手动断开后不许自动连回去"
 * 这三条规则凑在一起会互相打架，而它们又散落在 `CBCentralManagerDelegate`
 * 的多个回调里（状态变化 / 发现设备 / 连接成功 / 连接失败 / 掉线）。
 * 如果每条回调里各写一遍 if，很容易出现"断开按钮点了没用"这类 bug ——
 * 而且**只有真机+真仪表才能发现**，代价很高。
 *
 * 因此把"此刻该不该发起扫描"抽成一个**没有副作用**的纯函数：
 *   - `BLEClient` 只负责把当前状态喂进来、按返回值执行动作；
 *   - 测试可以在 macOS 上穷举各种状态组合（见 `tools/test_ble_policy.swift`）。
 *
 * 本文件不依赖 CoreBluetooth，可在 macOS 上直接单测。
 */

import Foundation

/// 自动连接策略：把"是否该发起扫描"与"UI 该显示什么"集中在一处
enum BLEConnectionPolicy {

    /// 决策所需的全部输入（都是值类型，便于穷举测试）
    struct Inputs: Equatable {
        /// CoreBluetooth 的 manager 状态是否已 `.poweredOn`
        var bluetoothReady: Bool
        /// 当前是否已连接
        var isConnected: Bool
        /// 是否已经在扫描（避免重复启动扫描）
        var isScanning: Bool
        /// 用户是否允许自动连接（点过「断开连接」则为 false）
        var autoConnectEnabled: Bool

        init(bluetoothReady: Bool,
             isConnected: Bool = false,
             isScanning: Bool = false,
             autoConnectEnabled: Bool = true) {
            self.bluetoothReady = bluetoothReady
            self.isConnected = isConnected
            self.isScanning = isScanning
            self.autoConnectEnabled = autoConnectEnabled
        }
    }

    /// 决策结果：要么发起扫描，要么什么都不做（并给出原因/提示）
    enum Decision: Equatable {
        /// 应调用 `scanForPeripherals` 开始扫描
        case startScan
        /// 已在扫描：保持不动（**不要**重复调用，也不要覆盖提示文案）
        case alreadyScanning
        /// 已经连着：什么都不用做
        case alreadyConnected
        /// 用户主动断开：**绝不**自动连回去
        case userDisconnected
        /// 蓝牙未就绪（关闭 / 未授权 / 不支持）
        case bluetoothNotReady
    }

    /**
     核心决策：此刻该不该开始扫描？

     ⚠️ 判断顺序很关键，不能调换：
       1. **用户意图优先** —— 用户说断开就断开，哪怕蓝牙就绪、也没连着。
          放在最后判断的话，会被"蓝牙就绪 → 扫描"抢先，断开按钮直接失效。
       2. 已连接 / 已在扫描 → 无事可做（重复 scan 不报错但白耗电，
          还会把提示文案覆盖成"扫描中…"，与"已连接"自相矛盾）。
       3. 最后才看蓝牙是否就绪。
     */
    static func decide(_ i: Inputs) -> Decision {
        // 1) 用户主动断开是最高优先级：任何自动路径都不得越过它
        guard i.autoConnectEnabled else { return .userDisconnected }
        // 2) 已连接 → 不重复扫描
        if i.isConnected { return .alreadyConnected }
        // 3) 已在扫描 → 不重复启动
        if i.isScanning { return .alreadyScanning }
        // 4) 蓝牙就绪才能扫描
        guard i.bluetoothReady else { return .bluetoothNotReady }
        return .startScan
    }

    /// 掉线后是否应自动重连（等价于"用户没要求断开"）
    ///
    /// 单独给一个语义化入口，是为了让 `didDisconnectPeripheral` 里
    /// 的意图一目了然：**掉线重连的前提是用户还想要连接**。
    static func shouldReconnectAfterDrop(autoConnectEnabled: Bool) -> Bool {
        autoConnectEnabled
    }

    /// 界面状态文案（与 `BLEClient.stateText` 同源，便于单测穷举）
    ///
    /// - Parameters:
    ///   - bluetoothReady: 蓝牙是否就绪
    ///   - isConnected: 是否已连接
    ///   - autoConnectEnabled: 用户是否允许自动连接
    static func stateText(bluetoothReady: Bool,
                          isConnected: Bool,
                          autoConnectEnabled: Bool) -> String {
        guard bluetoothReady else { return "蓝牙未就绪" }
        if isConnected { return "已连接" }
        /*
         * 蓝牙就绪但没连上时，必须区分"正在自动连接"（过渡态，等着就好）
         * 与"用户主动断开"（稳定态，等用户点按钮）—— 否则用户不知道
         * 到底还会不会自动连上。
         */
        return autoConnectEnabled ? "正在连接…" : "已断开"
    }
}
