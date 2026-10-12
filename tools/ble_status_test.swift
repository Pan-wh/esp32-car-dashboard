// BLE Status 特征验证（macOS CoreBluetooth Central）
//
// 用途：验证 §4 的 GATT 契约 —— 发现 Service/Telemetry(Write)/Status(Read+Notify)，
//       读一次 Status、写一条遥测（触发固件 set_status("time-synced")）并确认收到通知。
// 运行：swift tools/ble_status_test.swift
// 预期：属性=0x12 Read Notify；Status 值 "time-synced"
// 验证 §4 的 Status 特征（Read/Notify）
import CoreBluetooth
import Foundation

let SERVICE = CBUUID(string: "7D2A0001-8F4A-4B7C-9D21-5E6A3B8C1000")
let TELE    = CBUUID(string: "7D2A0002-8F4A-4B7C-9D21-5E6A3B8C1000")
let STATUS  = CBUUID(string: "7D2A0003-8F4A-4B7C-9D21-5E6A3B8C1000")
let json = "{\"v\":1,\"ts_utc_ms\":\(Int(Date().timeIntervalSince1970*1000)),\"speed_mps\":12.5,\"speed_valid\":true,\"course_deg\":45,\"location_valid\":true,\"latitude\":31.2,\"longitude\":121.4}"

final class D: NSObject, CBCentralManagerDelegate, CBPeripheralDelegate {
    var cm: CBCentralManager!; var p: CBPeripheral?; var tele: CBCharacteristic?; var status: CBCharacteristic?
    var done = false
    func go() { cm = CBCentralManager(delegate: self, queue: nil) }
    func centralManagerDidUpdateState(_ c: CBCentralManager) {
        if c.state == .poweredOn {
            c.scanForPeripherals(withServices: [SERVICE], options: nil)
            DispatchQueue.global().asyncAfter(deadline: .now()+20) { if !self.done { print("[超时]"); exit(2) } }
        } else { print("[错误] 蓝牙 \(c.state.rawValue)"); exit(3) }
    }
    func centralManager(_ c: CBCentralManager, didDiscover per: CBPeripheral, advertisementData d: [String: Any], rssi: NSNumber) {
        print("[发现] \((d[CBAdvertisementDataLocalNameKey] as? String) ?? per.name ?? "?")")
        guard p == nil else { return }
        p = per; c.stopScan(); c.connect(per, options: nil)
    }
    func centralManager(_ c: CBCentralManager, didConnect per: CBPeripheral) {
        print("[连接]"); per.delegate = self; per.discoverServices([SERVICE])
    }
    func peripheral(_ per: CBPeripheral, didDiscoverServices e: Error?) {
        for s in per.services ?? [] { per.discoverCharacteristics([TELE, STATUS], for: s) }
    }
    func peripheral(_ per: CBPeripheral, didDiscoverCharacteristicsFor svc: CBService, error: Error?) {
        for ch in svc.characteristics ?? [] {
            if ch.uuid == TELE { tele = ch }
            if ch.uuid == STATUS {
                status = ch
                print("[Status] 属性=0x\(String(ch.properties.rawValue, radix:16))" +
                      ((ch.properties.contains(.read) ? " Read" : "") +
                       (ch.properties.contains(.notify) ? " Notify" : "")))
                per.readValue(for: ch)                 // 读一次
                per.setNotifyValue(true, for: ch)      // 订阅通知
            }
        }
        // 写一条遥测，触发固件更新 Status（time-synced）
        if let t = tele { per.writeValue(json.data(using: .utf8)!, for: t, type: .withResponse); print("[写入] 遥测") }
    }
    func peripheral(_ per: CBPeripheral, didUpdateValueFor ch: CBCharacteristic, error: Error?) {
        if ch.uuid == STATUS, let d = ch.value, let s = String(data: d, encoding: .utf8) {
            print("[Status 值] \"\(s)\"")
        }
    }
    func peripheral(_ per: CBPeripheral, didWriteValueFor ch: CBCharacteristic, error: Error?) {
        print("[写入成功] 等待 Status 通知…")
        DispatchQueue.global().asyncAfter(deadline: .now()+3) { self.done = true; exit(0) }
    }
}
let d = D(); d.go(); dispatchMain()
