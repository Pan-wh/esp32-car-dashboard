/**
 * SearchScope.swift —— 地点联想的**搜索范围降级策略**（纯逻辑，可离线单测）
 *
 * ## 要解决的问题
 *
 * 原先联想搜索**直接查全国**。搜"人民路"会返回几十个城市的人民路，
 * 用户在本地想找的那条被淹没在列表里 —— 得自己一行行找城市名。
 *
 * 正确顺序是"由近及远"：
 *
 *     本市 ──无结果──▶ 本省 ──无结果──▶ 全国
 *
 * ## 技术约束（决定了实现方式）
 *
 * 高德 `assistant/inputtips` **只接受单个 `city` 参数**，没有"省"这一层，
 * 也没有跨层级的排序。所以"本市→本省→全国"不能靠一个请求完成，
 * 必须**按层依次请求、必要时才降级**，否则每次联想都要发 3 个请求
 * （配额是有限的，见网关的用量计数器）。
 *
 * 因此这里的策略是：
 *   - 本市有结果 → **立即返回**，不再请求省/全国（省配额、也更快）
 *   - 本市无结果 → 请求本省（用省名当 `city`，高德会按省过滤）
 *   - 本省也无   → 才查全国
 *
 * ## 为什么"降级"而不是"合并"
 *
 * 合并（三个都查、拼在一起）看似结果更全，但有两个问题：
 *   1. 配额：每次联想固定 3 个请求，而实际 90% 的情况本市就有结果
 *   2. 排序：合并后仍要决定"本市第 5 条"和"全国第 1 条"谁在前，
 *      而这个排序很难做对；分层降级天然就是"近的优先"。
 *
 * 本文件不依赖网络与 CoreLocation，可在 macOS 上直接单测。
 */

import Foundation

/// 一次联想的检索层级
enum SearchScope: String, CaseIterable, Equatable {
    /// 本市（最优先）
    case city
    /// 本省（本市无结果时）
    case province
    /// 全国（本省也无结果时）
    case nationwide

    /// 界面提示用的中文名
    var title: String {
        switch self {
        case .city: return "本市"
        case .province: return "本省"
        case .nationwide: return "全国"
        }
    }
}

/**
 搜索范围的降级决策（纯函数）。
 */
enum SearchScopePolicy {

    /// 可用于检索的行政信息（来自一次逆地理编码）
    struct Region: Equatable {
        /// 城市名（如"上海市"）。高德 regeo 的 `city` 字段
        var city: String
        /// 省名（如"上海市"/"浙江省"）。高德 regeo 的 `province` 字段
        var province: String

        init(city: String = "", province: String = "") {
            self.city = city
            self.province = province
        }

        /// 直辖市：省名与市名相同（如"上海市"既是省也是市）
        ///
        /// 这很关键：直辖市若按"本市→本省"发两次请求，第二次是**完全重复**的，
        /// 白白消耗配额。故直辖市直接跳过省级。
        var isMunicipality: Bool {
            !city.isEmpty && !province.isEmpty && normalize(city) == normalize(province)
        }

        private func normalize(_ s: String) -> String {
            s.replacingOccurrences(of: "市", with: "")
                .replacingOccurrences(of: "省", with: "")
                .trimmingCharacters(in: .whitespaces)
        }
    }

    /**
     按优先级列出**实际要尝试的层级**（已剔除重复与不可用项）。

     - 无定位信息时（Region 全空）→ 只能全国
     - 直辖市 → 本市 / 全国（跳过省级，因为省==市，重复请求没意义）
     - 一般城市 → 本市 / 本省 / 全国

     调用方**依次**尝试，只要某层有结果就停（见 shouldStop）。
     */
    static func scopes(for region: Region) -> [SearchScope] {
        let hasCity = !region.city.trimmingCharacters(in: .whitespaces).isEmpty
        let hasProvince = !region.province.trimmingCharacters(in: .whitespaces).isEmpty

        guard hasCity else {
            // 连城市都不知道（还没定位/逆地理失败）→ 只能全国
            return [.nationwide]
        }

        var result: [SearchScope] = [.city]
        // 直辖市跳过省级：province == city，再查一次是纯浪费配额
        if hasProvince && !region.isMunicipality {
            result.append(.province)
        }
        result.append(.nationwide)
        return result
    }

    /**
     取该层级传给高德 `city` 参数的值。

     - `.city` / `.province`：返回对应行政区名
     - `.nationwide`：返回空串（不传 `city`，即全国，这是高德的语义）

     ⚠️ 返回空串**不代表"缺失"**，而是"不限城市" —— 调用方必须原样传给
        网关（网关只在非空时才加 `city` 参数）。
     */
    static func cityParameter(for scope: SearchScope, region: Region) -> String {
        switch scope {
        case .city: return region.city
        case .province: return region.province
        case .nationwide: return ""
        }
    }

    /**
     拿到某层结果后，是否应停止继续降级。

     只要**本层有结果**就停 —— 这正是"本市优先"的含义：
     本市的 3 条比全国的 30 条更可能是我要的。
     */
    static func shouldStop(after scope: SearchScope, resultCount: Int) -> Bool {
        resultCount > 0
    }

    /**
     结果来源提示文案（让用户知道"这是本市结果还是全国结果"）。

     返回 nil 表示不必提示（本市结果属默认预期，提示反而啰嗦）。
     */
    static func notice(for scope: SearchScope, resultCount: Int) -> String? {
        guard resultCount > 0 else { return nil }
        switch scope {
        case .city:
            return nil // 本市是默认预期，不用提示
        case .province:
            return "本市没找到，以下是全省结果"
        case .nationwide:
            return "本省没找到，以下是全国结果"
        }
    }
}
