/**
 * test_keychain_token.swift —— 网关令牌安全存储测试（macOS 本机运行）
 *
 * 运行（或 `cd tests && make test-keychain-token`）：
 *   swiftc -O -o /tmp/test_keychain \
 *     ios/CarDashboardPhone/CarDashboardPhone/KeychainTokenStore.swift \
 *     tools/test_keychain_token.swift && /tmp/test_keychain
 *
 * 直接编译 App 的**真实源文件**（不内联复制）。
 *
 * ⚠️ 本测试**真的会读写 macOS 钥匙串**，但用的是独立 service 名
 *    （`...test.<随机>`），跑完即删，不碰真实凭据。
 *
 * 为什么值得测：令牌是设备上唯一有价值的凭据，而"读不到令牌"
 * 只会表现为"网关鉴权失败"—— 极难定位。另外合并迁移若写错
 * （比如没删干净明文），这次改造等于白做。
 */

import Foundation

var gPass = 0
var gFail = 0
func check(_ cond: Bool, _ msg: @autoclosure () -> String) {
    if cond { gPass += 1 } else { gFail += 1; print("  ✗ \(msg())") }
}

/// 每次运行用独立 service，避免与真实凭据或上次残留互相影响
let SVC = "com.cardashboard.test.navToken.\(UUID().uuidString)"

/**
 本机 Keychain 是否可用。

 ⚠️ **在没有签名的命令行进程里 Keychain 通常不可用**：写入会返回
    `100001 (UNIX[Operation not permitted])`，因为 Keychain 访问需要
    `keychain-access-groups` 授权，而 `swiftc` 直接产出的可执行文件
    是 **ad-hoc 签名、无 entitlements**。实测连系统自带的
    `security add-generic-password` 在本环境也会失败。

    这不影响真机 App —— 它带 `keychain-access-groups = [TEAM.*]`
    授权（可在 `embedded.mobileprovision` 里核对）。

 因此：**能跑就跑真断言；跑不了就明确报告"跳过"而不是假装通过**
（把失败说成成功比不测更危险）。
*/
func keychainAvailable() -> Bool {
    let probe = "\(SVC).probe"
    let ok = KeychainTokenStore.save("probe", service: probe)
    KeychainTokenStore.delete(service: probe)
    return ok
}

let KC_OK = keychainAvailable()

// ─────────────────────────────────────────────────────────────
// 1) 基本读写
// ─────────────────────────────────────────────────────────────
func testSaveLoadDelete() {
    guard KC_OK else { return }
    check(KeychainTokenStore.load(service: SVC) == nil, "初始应为空（nil）")

    check(KeychainTokenStore.save("tok-abc-123", service: SVC), "写入应成功")
    check(KeychainTokenStore.load(service: SVC) == "tok-abc-123", "应读回刚写入的令牌")

    // 覆盖写入（走 SecItemUpdate 分支）—— 不能退化成新增第二项
    check(KeychainTokenStore.save("tok-xyz-999", service: SVC), "覆盖写入应成功")
    check(KeychainTokenStore.load(service: SVC) == "tok-xyz-999",
          "★ 覆盖后应读到新值（若误用 SecItemAdd 会残留旧值或报重复）")

    check(KeychainTokenStore.delete(service: SVC), "删除应成功")
    check(KeychainTokenStore.load(service: SVC) == nil, "删除后应读不到")

    // 幂等：重复删除不报错
    check(KeychainTokenStore.delete(service: SVC), "重复删除应幂等成功")
}

// ─────────────────────────────────────────────────────────────
// 2) 特殊字符 / 中文 / 长串（令牌可能是任意字节）
// ─────────────────────────────────────────────────────────────
func testOddValues() {
    guard KC_OK else { return }
    let odd = "令牌-with-符号!@#$%^&*()_+-=[]{}|;':\",./<>?`~ 空格"
    check(KeychainTokenStore.save(odd, service: SVC), "含特殊字符应能写入")
    check(KeychainTokenStore.load(service: SVC) == odd, "★ 特殊字符应原样读回（编码不能丢）")

    let long = String(repeating: "a", count: 4096)
    check(KeychainTokenStore.save(long, service: SVC), "长令牌应能写入")
    check(KeychainTokenStore.load(service: SVC) == long, "★ 长令牌应完整读回（不得截断）")

    KeychainTokenStore.delete(service: SVC)
}

// ─────────────────────────────────────────────────────────────
// 3) 空串语义：应存成空而不是报错（UI 允许清空）
// ─────────────────────────────────────────────────────────────
func testEmptyString() {
    guard KC_OK else { return }
    /* 注意：save("") 是允许的（存一个空值），但我们的 UI 在清空时调 delete。
       这里只验证 save("") 不会崩，且读回是空串而非 nil 或异常。 */
    check(KeychainTokenStore.save("", service: SVC), "写入空串不应失败")
    check(KeychainTokenStore.load(service: SVC) == "", "空串应读回空串")
    KeychainTokenStore.delete(service: SVC)
}

// ─────────────────────────────────────────────────────────────
// 4) 迁移：老 UserDefaults 明文 → Keychain，并**删掉明文**
//    （这是本次改造的关键：不删就等于白改）
// ─────────────────────────────────────────────────────────────
func testMigration() {
    /*
     * ⚠️ 迁移测试分两半：
     *   - "明文被清掉"是**纯 UserDefaults 逻辑**，随时可验证（Keychain 不可用也验）
     *   - "值确实进了 Keychain"需要 Keychain 可用
     * 混在一起会导致 Keychain 不可用时连清明文这半也测不到。
     */
    let suiteName = "test.migration.\(UUID().uuidString)"
    guard let d = UserDefaults(suiteName: suiteName) else {
        check(false, "无法创建测试用 UserDefaults"); return
    }
    d.removePersistentDomain(forName: suiteName)
    let svc = "\(SVC).mig"

    // 场景 A：老值存在，Keychain 空 → 应迁入并清明文
    d.set("legacy-token-001", forKey: KeychainTokenStore.legacyDefaultsKey)
    KeychainTokenStore.migrateFromUserDefaultsIfNeeded(defaults: d, service: svc)
    if KC_OK {
        check(KeychainTokenStore.load(service: svc) == "legacy-token-001",
              "★ 旧版明文令牌应迁移进 Keychain")
    }
    check(d.object(forKey: KeychainTokenStore.legacyDefaultsKey) == nil,
          "★ 迁移后**必须删除** UserDefaults 里的明文（否则改造白做）")

    // 场景 B：Keychain 已有更新的值 → 不覆盖，但仍清明文
    d.set("stale-legacy", forKey: KeychainTokenStore.legacyDefaultsKey)
    KeychainTokenStore.migrateFromUserDefaultsIfNeeded(defaults: d, service: svc)
    if KC_OK {
        check(KeychainTokenStore.load(service: svc) == "legacy-token-001",
              "★ Keychain 已有值时不覆盖（避免用旧明文盖掉新令牌）")
    }
    check(d.object(forKey: KeychainTokenStore.legacyDefaultsKey) == nil,
          "即使不覆盖也要清掉明文残留")

    // 场景 C：没有老值 → 什么都不做，保持 Keychain 原样
    KeychainTokenStore.migrateFromUserDefaultsIfNeeded(defaults: d, service: svc)
    if KC_OK {
        check(KeychainTokenStore.load(service: svc) == "legacy-token-001",
              "无老值时不应改动 Keychain")
    }

    // 场景 D：幂等 —— 连续迁移多次结果一致
    d.set("again", forKey: KeychainTokenStore.legacyDefaultsKey)
    KeychainTokenStore.migrateFromUserDefaultsIfNeeded(defaults: d, service: svc)
    KeychainTokenStore.migrateFromUserDefaultsIfNeeded(defaults: d, service: svc)
    if KC_OK {
        check(KeychainTokenStore.load(service: svc) == "legacy-token-001",
              "重复迁移应幂等（不覆盖已有值）")
    }

    KeychainTokenStore.delete(service: svc)
    d.removePersistentDomain(forName: suiteName)
}

// ─────────────────────────────────────────────────────────────
// 5) 隔离性：不同 service 互不干扰
// ─────────────────────────────────────────────────────────────
func testServiceIsolation() {
    guard KC_OK else { return }
    let a = "\(SVC).a", b = "\(SVC).b"
    KeychainTokenStore.save("token-A", service: a)
    KeychainTokenStore.save("token-B", service: b)
    check(KeychainTokenStore.load(service: a) == "token-A", "A 应只读到自己的值")
    check(KeychainTokenStore.load(service: b) == "token-B", "B 应只读到自己的值")

    KeychainTokenStore.delete(service: a)
    check(KeychainTokenStore.load(service: a) == nil, "删 A 后 A 应为空")
    check(KeychainTokenStore.load(service: b) == "token-B", "★ 删 A 不应影响 B")

    KeychainTokenStore.delete(service: b)
}

@main
struct TestKeychainToken {
    static func main() {
        print("=== 网关令牌 Keychain 存储 ===")
        if !KC_OK {
            print("  ⚠️ 本机 Keychain 不可用（命令行二进制无签名/无 entitlements，")
            print("     写入返回 100001 Operation not permitted）。")
            print("     → 仅验证纯逻辑部分；真机 App 带 keychain-access-groups 授权，不受影响。")
        }
        testSaveLoadDelete()
        testOddValues()
        testEmptyString()
        testMigration()
        testServiceIsolation()
        // 兜底清理，避免测试残留
        KeychainTokenStore.delete(service: SVC)
        print("结果: \(gPass) 通过, \(gFail) 失败")
        exit(gFail == 0 ? 0 : 1)
    }
}
