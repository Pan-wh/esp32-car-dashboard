// test_gateway_contract.swift —— 网关响应契约测试（macOS 本机运行，不需手机/仪表）
//
// ## 为什么需要这个文件
//
// 2026-10-15 真机首次联调"查询失败"，排查后发现 App 侧**四个解码器全写错了**：
// 我是**凭印象**写字段名的，没去核对网关的真实返回 ——
//
//   /tips     我写 {places:[{address,lon,lat}]}      实际 {tips:[{name,district,adcode,lon,lat}]}
//   /around   我写嵌套 location 入参 + places         实际 顶层 lon/lat 入参 + {pois:[{name,type,distanceM,lon,lat,address}]}
//   /regeo    我写 {result:{address,road}}            实际 扁平 {formattedAddress,road,poiName,district,city,province}
//   /traffic  我写 {traffic:{status,speedKmh}}        实际 扁平 {status,statusCode,description,roads:[{...,speedKmh}]}
//
// 结果 JSONDecoder 全部抛错，界面上只显示"查询失败"，没有任何线索。
//
// 所以这里把**真实抓取的响应**固化成测试：将来网关改了字段名、或我又改了
// 解码器，这里会立刻红。夹具里的 JSON 是 `curl` 实抓的原文（只做了截断）。
//
// 运行：swift tools/test_gateway_contract.swift
//
// ⚠️ 下面的解码结构与 App 源码同源语义（与 test_map_codec.swift 同一策略：
//    内联复刻 + 保持同步）。App 侧改动时本文件必须同步。

import Foundation

// ===== 被测结构（内联自 App 的 Nav/GatewayClient.swift）=====

struct GeoPoint: Codable { var lon: Double; var lat: Double }

struct PlaceSuggestion: Codable, Identifiable {
    let name: String
    let address: String
    let lon: Double
    let lat: Double
    let distanceM: Double?
    var id: String { "\(name)|\(lon)|\(lat)" }
}

struct TipItem: Codable {
    let name: String
    let district: String?
    let adcode: String?
    let lon: Double
    let lat: Double
}
struct TipsResponse: Codable { let tips: [TipItem] }

struct PoiItem: Codable {
    let name: String
    let type: String?
    let distanceM: Double?
    let lon: Double
    let lat: Double
    let address: String?
}
struct AroundResponse: Codable { let pois: [PoiItem] }

struct ReverseGeocodeResult: Codable {
    let formattedAddress: String?
    let road: String?
    let poiName: String?
    let district: String?
    let city: String?
    let province: String?
    var displayText: String {
        for s in [poiName, road, formattedAddress] {
            if let v = s, !v.isEmpty { return v }
        }
        return ""
    }
}

struct TrafficStatus: Codable {
    let status: String
    let statusCode: String?
    let description: String?
    let roads: [Road]
    struct Road: Codable {
        let name: String?
        let status: String?
        let speedKmh: Double?
        let lengthM: Double?
    }
    var averageSpeedKmh: Double? {
        let vals = roads.compactMap { $0.speedKmh }
        guard !vals.isEmpty else { return nil }
        return vals.reduce(0, +) / Double(vals.count)
    }
}

// ===== 真实抓取的响应（curl 原文；长数组做截断，但**字段名一字不改**）=====

/// POST /tips {"keywords":"北京南","city":"北京"}
let FIX_TIPS = """
{"tips":[{"name":"北京南站(南进站口)","district":"北京市丰台区","adcode":"110106","lon":116.37418059590648,"lat":39.86250775788758},{"name":"北京南站(北进站口)","district":"北京市丰台区","adcode":"110106","lon":116.37163179881125,"lat":39.864887581649576},{"name":"北京南站P1停车场","district":"北京市丰台区","adcode":"110106","lon":116.37104997447487,"lat":39.86221442991022}]}
"""

/// POST /around {"lon":116.397,"lat":39.909,"keywords":"加油站","radius":3000}
let FIX_AROUND = """
{"pois":[{"name":"中国石化加油站(北京站)","type":"汽车服务;加油站;中国石化","distanceM":1820,"lon":116.4166175892165,"lat":39.90259260941474,"address":"北京站西街3号(北京站地铁站A西北口步行360米)"},{"name":"中国石化加油站(东便门)","type":"汽车服务;加油站;中国石化","distanceM":2400,"lon":116.42,"lat":39.90,"address":"东便门"}]}
"""

/// POST /regeo {"lon":116.397,"lat":39.909}
let FIX_REGEO = """
{"formattedAddress":"北京市东城区东华门街道南池子大街皇史宬","road":"南池子大街","poiName":null,"district":"东城区","city":null,"province":"北京市"}
"""

/// POST /traffic {"lon":116.397,"lat":39.909}
let FIX_TRAFFIC = """
{"status":"缓行","statusCode":"2","description":"轻度拥堵","roads":[{"name":"南池子大街","status":"拥堵","statusCode":"3","direction":"东长安街附近","lengthM":16,"speedKmh":10,"congestedLengthM":16},{"name":"正义路","status":"缓行","statusCode":"2","direction":"东长安街附近","lengthM":40,"speedKmh":30,"congestedLengthM":0}]}
"""

// ===== 断言 =====

var gPass = 0, gFail = 0
func CHECK(_ cond: Bool, _ msg: String) {
    if cond { gPass += 1 } else { gFail += 1; print("  ✗ \(msg)") }
}
func decode<T: Decodable>(_ type: T.Type, _ json: String) -> T? {
    try? JSONDecoder().decode(T.self, from: Data(json.utf8))
}

print("=== 网关响应契约 ===")

// ---- /tips ----
print("--- /tips ---")
do {
    guard let r = decode(TipsResponse.self, FIX_TIPS) else {
        CHECK(false, "/tips 能解码（字段名：tips / name / district / lon / lat）")
        exit(1)
    }
    CHECK(r.tips.count == 3, "解析出 3 条提示（实际 \(r.tips.count)）")
    CHECK(r.tips[0].name == "北京南站(南进站口)", "name 正确")
    CHECK(r.tips[0].district == "北京市丰台区", "district 正确")
    CHECK(abs(r.tips[0].lon - 116.37418059590648) < 1e-9, "lon 正确")
    /* 归一成 UI 用的 PlaceSuggestion：address 取 district */
    let sugg = r.tips.map { PlaceSuggestion(name: $0.name, address: $0.district ?? "",
                                            lon: $0.lon, lat: $0.lat, distanceM: nil) }
    CHECK(sugg.count == 3 && sugg[0].address.isEmpty == false, "能归一成 PlaceSuggestion")
}

// ---- /around ----
print("--- /around ---")
do {
    guard let r = decode(AroundResponse.self, FIX_AROUND) else {
        CHECK(false, "/around 能解码（字段名：pois / name / type / distanceM / lon / lat / address）")
        exit(1)
    }
    CHECK(r.pois.count == 2, "解析出 2 个 POI（实际 \(r.pois.count)）")
    CHECK(r.pois[0].distanceM == 1820, "distanceM 正确（实际 \(String(describing: r.pois[0].distanceM))）")
    CHECK(r.pois[0].address?.isEmpty == false, "address 有值")
    let sugg = r.pois.map { PlaceSuggestion(name: $0.name, address: $0.address ?? $0.type ?? "",
                                            lon: $0.lon, lat: $0.lat, distanceM: $0.distanceM) }
    CHECK(sugg[0].distanceM == 1820, "归一后仍带距离（列表要显示「离我多远」）")
}

// ---- /regeo ----
print("--- /regeo ---")
do {
    guard let r = decode(ReverseGeocodeResult.self, FIX_REGEO) else {
        CHECK(false, "/regeo 能解码（**扁平**，不是 result 包裹）")
        exit(1)
    }
    CHECK(r.road == "南池子大街", "road 正确")
    CHECK(r.district == "东城区", "district 正确")
    CHECK(r.poiName == nil, "poiName 为 null 时能正确解成 nil（不是解不开）")
    CHECK(r.city == nil, "city 缺失时能解成 nil")
    CHECK(r.displayText == "南池子大街", "displayText 优先道路名（实际 \(r.displayText)）")
}

// ---- /traffic ----
print("--- /traffic ---")
do {
    guard let r = decode(TrafficStatus.self, FIX_TRAFFIC) else {
        CHECK(false, "/traffic 能解码（**扁平**，车速在 roads[] 里）")
        exit(1)
    }
    CHECK(r.status == "缓行", "status 正确")
    CHECK(r.description == "轻度拥堵", "description 正确")
    CHECK(r.roads.count == 2, "2 个路段（实际 \(r.roads.count)）")
    CHECK(r.roads[0].speedKmh == 10, "路段车速正确")
    /* 顶层**没有** speedKmh，必须自己从路段求平均 */
    let avg = r.averageSpeedKmh ?? -1
    CHECK(abs(avg - 20) < 1e-9, "平均车速 = (10+30)/2 = 20（实际 \(avg)）")
}

// ---- 反向断言：证明"凭印象写的那版"确实解不开 ----
print("--- 回归：错误的字段名必须解不开 ---")
do {
    struct WrongTips: Codable { let places: [PlaceSuggestion] }
    CHECK(decode(WrongTips.self, FIX_TIPS) == nil,
          "旧写法 {places:[...]} 解 /tips 必然失败 —— 这正是当初「查询失败」的原因")

    struct WrongRegeo: Codable { let result: ReverseGeocodeResult }
    CHECK(decode(WrongRegeo.self, FIX_REGEO) == nil,
          "旧写法 {result:{...}} 解 /regeo 必然失败")

    struct WrongTraffic: Codable { let traffic: TrafficStatus }
    CHECK(decode(WrongTraffic.self, FIX_TRAFFIC) == nil,
          "旧写法 {traffic:{...}} 解 /traffic 必然失败")
}

print("\n结果: \(gPass) 通过, \(gFail) 失败")
exit(gFail == 0 ? 0 : 1)
