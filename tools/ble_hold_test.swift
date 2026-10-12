// 保持连接但停止发送的测试客户端（macOS CoreBluetooth Central）
//
// 用途：验证 §6.1/§6.4「已连接但数据超时」应显示 STALE（而非 BT OFF）——
//       对应现实场景：iPhone App 被后台杀掉/权限撤销，但 BLE 链路仍在。
// 运行：swift tools/ble_hold_test.swift
// 预期：发送一包后保持连接 ~25s，仪表 2.5s 后转 STALE 且数据龄持续增长；
//       断开后转为 BT OFF。

// 保持连接但停止发送 —— 验证"已连接但数据超时"应显示 STALE（而非 BT OFF）
import CoreBluetooth
import Foundation

let SERVICE = CBUUID(string: "7D2A0001-8F4A-4B7C-9D21-5E6A3B8C1000")
let TELE    = CBUUID(string: "7D2A0002-8F4A-4B7C-9D21-5E6A3B8C1000")
let json = "{\"v\":1,\"ts_utc_ms\":\(Int(Date().timeIntervalSince1970*1000)),\"speed_mps\":13.9,\"speed_valid\":true,\"course_deg\":60,\"location_valid\":true,\"latitude\":31.2,\"longitude\":121.4}"

final class D: NSObject, CBCentralManagerDelegate, CBPeripheralDelegate {
    var cm: CBCentralManager!; var p: CBPeripheral?; var tele: CBCharacteristic?
    func go() { cm = CBCentralManager(delegate: self, queue: nil) }
    func centralManagerDidUpdateState(_ c: CBCentralManager) {
        if c.state == .poweredOn {
            c.scanForPeripherals(withServices: [SERVICE], options: nil)
            DispatchQueue.global().asyncAfter(deadline: .now()+25) { print("[结束] 保持连接超时退出"); exit(0) }
        } else { print("[错误] 蓝牙 \(c.state.rawValue)"); exit(3) }
    }
    func centralManager(_ c: CBCentralManager, didDiscover per: CBPeripheral, advertisementData d: [String: Any], rssi: NSNumber) {
        guard p == nil else { return }
        p = per; c.stopScan(); c.connect(per, options: nil)
    }
    func centralManager(_ c: CBCentralManager, didConnect per: CBPeripheral) {
        print("[连接] 保持不断开"); per.delegate = self; per.discoverServices([SERVICE])
    }
    func peripheral(_ per: CBPeripheral, didDiscoverServices e: Error?) {
        for s in per.services ?? [] { per.discoverCharacteristics([TELE], for: s) }
    }
    func peripheral(_ per: CBPeripheral, didDiscoverCharacteristicsFor svc: CBService, error: Error?) {
        for ch in svc.characteristics ?? [] where ch.uuid == TELE {
            tele = ch
            per.writeValue(json.data(using: .utf8)!, for: ch, type: .withResponse)
            print("[发送一次] 之后**不再发送**，但保持连接 25s")
        }
    }
}
let d = D(); d.go(); dispatchMain()
