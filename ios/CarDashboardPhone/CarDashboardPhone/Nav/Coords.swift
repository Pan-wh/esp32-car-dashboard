/**
 * Coords.swift —— WGS84 ↔ GCJ-02 坐标转换（Swift 侧）
 *
 * ## 为什么 Swift 侧也要一份
 *
 * 手机每次定位（约 2Hz）都要把 CoreLocation 的 **WGS84** 转成 **GCJ-02**，
 * 才能与网关返回的路线（GCJ-02）做匹配。**这不能走网络往返** ——
 * 2Hz × 网络延迟既不可靠也没必要。
 *
 * ## 与 gateway/src/coords.js 的关系
 *
 * 两端各有一份实现（语言不同无法共享），必须**逐位一致**，
 * 否则仪表与网关会"各说各话"（路线在 GCJ-02，位置却按 WGS84 匹配 → 整体偏移数百米）。
 *
 * 为防止两份实现漂移，用**交叉验证测试**保证一致：
 *   - gateway 侧生成测试向量：`node gateway/test/gen-coord-vectors.js`
 *   - Swift 侧读取同一向量文件逐点比对：见 tools/test_nav_core.swift
 *
 * ## 算法说明
 *
 * 公开的偏移量拟合算法（克拉索夫斯基椭球 + 多项式扰动），业界广泛使用。
 * 它是**拟合**而非官方实现，误差在米级；境外原样返回不做偏移。
 */

import Foundation

public enum Coords {
    /// 克拉索夫斯基椭球长半轴（米）
    private static let a = 6378245.0
    /// 第一偏心率平方
    private static let ee = 0.00669342162296594323

    /// 是否在中国大陆境外（境外不做偏移）
    public static func outOfChina(lon: Double, lat: Double) -> Bool {
        !(lon > 73.66 && lon < 135.05 && lat > 3.86 && lat < 53.55)
    }

    private static func transformLat(_ x: Double, _ y: Double) -> Double {
        var ret = -100.0 + 2.0 * x + 3.0 * y + 0.2 * y * y + 0.1 * x * y
            + 0.2 * sqrt(abs(x))
        ret += (20.0 * sin(6.0 * x * .pi) + 20.0 * sin(2.0 * x * .pi)) * 2.0 / 3.0
        ret += (20.0 * sin(y * .pi) + 40.0 * sin(y / 3.0 * .pi)) * 2.0 / 3.0
        ret += (160.0 * sin(y / 12.0 * .pi) + 320 * sin(y * .pi / 30.0)) * 2.0 / 3.0
        return ret
    }

    private static func transformLon(_ x: Double, _ y: Double) -> Double {
        var ret = 300.0 + x + 2.0 * y + 0.1 * x * x + 0.1 * x * y + 0.1 * sqrt(abs(x))
        ret += (20.0 * sin(6.0 * x * .pi) + 20.0 * sin(2.0 * x * .pi)) * 2.0 / 3.0
        ret += (20.0 * sin(x * .pi) + 40.0 * sin(x / 3.0 * .pi)) * 2.0 / 3.0
        ret += (150.0 * sin(x / 12.0 * .pi) + 300.0 * sin(x / 30.0 * .pi)) * 2.0 / 3.0
        return ret
    }

    /// WGS84 → GCJ-02
    public static func wgs84ToGcj02(_ p: GeoPoint) -> GeoPoint {
        if outOfChina(lon: p.lon, lat: p.lat) { return p }

        let dLat = transformLat(p.lon - 105.0, p.lat - 35.0)
        let dLon = transformLon(p.lon - 105.0, p.lat - 35.0)

        let radLat = p.lat / 180.0 * .pi
        var magic = sin(radLat)
        magic = 1 - ee * magic * magic
        let sqrtMagic = sqrt(magic)

        let mgLat = (dLat * 180.0) / (((a * (1 - ee)) / (magic * sqrtMagic)) * .pi)
        let mgLon = (dLon * 180.0) / ((a / sqrtMagic) * cos(radLat) * .pi)

        return GeoPoint(lon: p.lon + mgLon, lat: p.lat + mgLat)
    }

    /// GCJ-02 → WGS84（迭代逼近；该变换无解析反函数）
    public static func gcj02ToWgs84(_ p: GeoPoint, maxIter: Int = 8) -> GeoPoint {
        if outOfChina(lon: p.lon, lat: p.lat) { return p }
        var guess = p
        for _ in 0..<maxIter {
            let conv = wgs84ToGcj02(guess)
            let dLon = conv.lon - p.lon
            let dLat = conv.lat - p.lat
            guess = GeoPoint(lon: guess.lon - dLon, lat: guess.lat - dLat)
            if abs(dLon) < 1e-9 && abs(dLat) < 1e-9 { break }
        }
        return guess
    }
}
