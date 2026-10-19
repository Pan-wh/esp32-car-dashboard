/**
 * KeychainTokenStore.swift —— 网关令牌的**安全存储**（Keychain）
 *
 * ## 为什么要改（2026-10-16 产品评审 P0-1）
 *
 * 原实现用 `@AppStorage("navToken")`，而 `@AppStorage` 底层是 **UserDefaults**，
 * 也就是 App 沙盒里的一个**明文 plist**。UI 上却用 `SecureField` 显示成圆点，
 * 给人"已加密"的错觉 —— 实际上：
 *
 *   - 越狱设备 / 未加密的 iTunes 备份可直接读出
 *   - 配合网关是**明文 HTTP**（`http://192.168.x.x:8080`），同一 Wi-Fi 下
 *     任何人抓到 `Authorization: Bearer <token>` 就能**盗用你的高德 Key 配额**
 *
 * 令牌是这台设备上**唯一有价值的凭据**（高德 Key 在服务端，不进 App），
 * 所以它值得放进 Keychain。
 *
 * ## 设计要点
 *
 * - `kSecAttrAccessibleAfterFirstUnlock`：**锁屏后仍可用**。
 *   这是刻意的 —— 导航要在后台/锁屏时继续请求网关，
 *   若用 `WhenUnlocked` 会在锁屏后全部失败。
 * - 读写失败**不抛错**（返回 nil / 静默失败）：令牌读不到只是"要重新填"，
 *   不该让 App 崩或卡住。但失败会 `NSLog` 出来便于排查。
 * - 提供一次性**迁移**：老的 UserDefaults 里有值就搬进 Keychain 并删除，
 *   避免用户升级后要重填一遍。
 *
 * 本文件只依赖 Security.framework 与 Foundation，可在 macOS 上单测
 * （测试用独立的 service 名，不污染真实凭据）。
 */

import Foundation
import Security

enum KeychainTokenStore {

    /// Keychain 里的服务名（同一 App 的所有令牌共用）
    static let service = "com.cardashboard.CarDashboardPhone.navToken"

    /// 旧版 `@AppStorage` 用的 UserDefaults 键（仅用于一次性迁移）
    static let legacyDefaultsKey = "navToken"

    /**
     读取令牌。

     - Returns: 令牌；未设置或读取失败时返回 nil（**不抛错**）。
     */
    static func load(service: String = KeychainTokenStore.service) -> String? {
        let query: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
            kSecReturnData as String: true,
            kSecMatchLimit as String: kSecMatchLimitOne,
        ]
        var item: CFTypeRef?
        let status = SecItemCopyMatching(query as CFDictionary, &item)
        guard status == errSecSuccess, let data = item as? Data else {
            if status != errSecItemNotFound {
                NSLog("[Keychain] 读取令牌失败: %d", status)
            }
            return nil
        }
        return String(data: data, encoding: .utf8)
    }

    /**
     写入（或更新）令牌。

     - Returns: 是否成功。失败时**不应**阻断导航功能（只是下次要重填）。
     */
    @discardableResult
    static func save(_ token: String, service: String = KeychainTokenStore.service) -> Bool {
        let data = Data(token.utf8)

        /*
         * 先尝试"更新已有项"。若直接 add，已存在时会返回 errSecDuplicateItem，
         * 于是每次改令牌都得先删再加 —— 中间有个"没有令牌"的窗口。
         */
        let query: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
        ]
        let attrs: [String: Any] = [
            kSecValueData as String: data,
            kSecAttrAccessible as String: kSecAttrAccessibleAfterFirstUnlock,
        ]

        let updateStatus = SecItemUpdate(query as CFDictionary, attrs as CFDictionary)
        if updateStatus == errSecSuccess { return true }

        guard updateStatus == errSecItemNotFound else {
            NSLog("[Keychain] 更新令牌失败: %d", updateStatus)
            return false
        }

        var addQuery = query
        addQuery[kSecValueData as String] = data
        addQuery[kSecAttrAccessible as String] = kSecAttrAccessibleAfterFirstUnlock
        let addStatus = SecItemAdd(addQuery as CFDictionary, nil)
        if addStatus != errSecSuccess {
            NSLog("[Keychain] 写入令牌失败: %d", addStatus)
            return false
        }
        return true
    }

    /// 删除令牌（用户清空输入框时调用）
    @discardableResult
    static func delete(service: String = KeychainTokenStore.service) -> Bool {
        let query: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
        ]
        let status = SecItemDelete(query as CFDictionary)
        return status == errSecSuccess || status == errSecItemNotFound
    }

    /**
     一次性迁移：把旧版存在 UserDefaults 里的令牌搬进 Keychain，并**删除旧值**。

     ⚠️ 必须删除旧值 —— 否则明文令牌永远留在 plist 里，这次改造就白做了。

     幂等：Keychain 已有值时不覆盖（那是更新的数据），但仍会清掉遗留的明文。
     */
    static func migrateFromUserDefaultsIfNeeded(
        defaults: UserDefaults = .standard,
        service: String = KeychainTokenStore.service) {
        let legacy = defaults.string(forKey: legacyDefaultsKey) ?? ""
        defer {
            /* 无论迁移是否成功都要清掉明文，避免它一直留在 plist 里 */
            if defaults.object(forKey: legacyDefaultsKey) != nil {
                defaults.removeObject(forKey: legacyDefaultsKey)
                NSLog("[Keychain] 已清除 UserDefaults 中的明文令牌")
            }
        }
        guard !legacy.isEmpty else { return }

        if let existing = load(service: service), !existing.isEmpty {
            /* Keychain 已有更新的值：不覆盖，只清明文 */
            return
        }
        if save(legacy, service: service) {
            NSLog("[Keychain] 已把旧版令牌迁移到 Keychain")
        }
    }
}
