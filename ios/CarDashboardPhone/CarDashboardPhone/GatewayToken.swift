/**
 * GatewayToken.swift —— 网关令牌的**共享可观察持有者**（Keychain 支撑）
 *
 * ## 为什么需要它
 *
 * 令牌原先在两个视图里各写一份 `@AppStorage("navToken")`：
 * `ContentView`（设置页）与 `NavTabView`（导航页）。
 * `@AppStorage` 靠 UserDefaults 的 KVO 自动同步，所以两处能"神奇地一致"。
 *
 * 改用 Keychain 后没有这个自动同步，必须显式提供一个共享对象，
 * 否则会出现"设置页改了令牌，导航页还在用旧的"—— 而且是**静默**失败
 * （表现为"网关鉴权失败"，很难查）。
 *
 * 因此令牌**只存一份**（`GatewayToken.shared`），两个页面都观察它。
 * 写入即落 Keychain，读取时若内存为空会先尝试从 Keychain 载入。
 */
import Combine
import Foundation

final class GatewayToken: ObservableObject {

    static let shared = GatewayToken()

    /// 当前令牌。修改会立即写入 Keychain（空串 = 删除）。
    @Published var value: String {
        didSet {
            guard value != oldValue else { return }
            if value.isEmpty {
                KeychainTokenStore.delete()
            } else {
                KeychainTokenStore.save(value)
            }
        }
    }

    private init() {
        /*
         * 启动顺序：先迁移旧版明文（一次性），再从 Keychain 读。
         * 迁移必须在读取**之前** —— 否则首次升级启动会读到空串，
         * 用户会以为令牌丢了（虽然下一帧就能读到，但会闪一下"未填写"）。
         */
        KeychainTokenStore.migrateFromUserDefaultsIfNeeded()
        value = KeychainTokenStore.load() ?? ""
    }

    /// 供非 SwiftUI 环境（如测试）显式刷新
    func reload() {
        let v = KeychainTokenStore.load() ?? ""
        if v != value { value = v }
    }
}
