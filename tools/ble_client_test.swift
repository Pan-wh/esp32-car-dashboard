// BLE 测试客户端（macOS CoreBluetooth Central）—— 阶段 1/3 验证用
//
// 运行示例：
//   swift tools/ble_client_test.swift '{"v":1,"speed_mps":19.03,"speed_valid":true,"location_valid":true}'
//
// 作用：扫描 CarDashboard → 连接 → 发现服务/特征 → 写入测试遥测。
// 依赖：macOS + Swift（无需 Xcode 工程）；首次运行需在系统设置里授予终端「蓝牙」权限。
// 说明：本机充当 BLE Central，可替代 iPhone 蓝牙调试 App 用于固件侧联调。
import CoreBluetooth
import Foundation

let SERVICE = CBUUID(string: "7D2A0001-8F4A-4B7C-9D21-5E6A3B8C1000")
let TELE    = CBUUID(string: "7D2A0002-8F4A-4B7C-9D21-5E6A3B8C1000")
let json = CommandLine.arguments.count > 1 ? CommandLine.arguments[1]
           : "{\"v\":1,\"speed_mps\":5.0,\"speed_valid\":true,\"location_valid\":true}"

final class D: NSObject, CBCentralManagerDelegate, CBPeripheralDelegate {
    var cm: CBCentralManager!
    var p: CBPeripheral?
    var done = false
    func go() { cm = CBCentralManager(delegate: self, queue: nil) }

    func centralManagerDidUpdateState(_ c: CBCentralManager) {
        if c.state == .poweredOn {
            print("[扫描] 查找 CarDashboard（按 service UUID）...")
            c.scanForPeripherals(withServices: [SERVICE], options: nil)
            DispatchQueue.global().asyncAfter(deadline: .now() + 20) {
                if !self.done { print("[超时] 20s 内未发现设备（可能蓝牙未授权）"); exit(2) }
            }
        } else { print("[错误] 蓝牙状态 \(c.state.rawValue)"); exit(3) }
    }
    func centralManager(_ c: CBCentralManager, didDiscover per: CBPeripheral, advertisementData d: [String: Any], rssi: NSNumber) {
        let name = (d[CBAdvertisementDataLocalNameKey] as? String) ?? per.name ?? "?"
        print("[发现] \(name)  RSSI=\(rssi)")
        guard p == nil else { return }
        p = per; c.stopScan(); c.connect(per, options: nil)
    }
    func centralManager(_ c: CBCentralManager, didConnect per: CBPeripheral) {
        print("[连接] \(per.name ?? "?")  最大MTU待协商")
        per.delegate = self
        per.discoverServices([SERVICE])
    }
    func centralManager(_ c: CBCentralManager, didFailToConnect per: CBPeripheral, error: Error?) {
        print("[连接失败] \(error?.localizedDescription ?? "?")"); exit(4)
    }
    func peripheral(_ per: CBPeripheral, didDiscoverServices e: Error?) {
        guard let s = per.services else { print("[无服务]"); exit(5); return }
        print("[服务] 发现 \(s.count) 个")
        for x in s { per.discoverCharacteristics([TELE], for: x) }
    }
    func peripheral(_ per: CBPeripheral, didDiscoverCharacteristicsFor svc: CBService, error: Error?) {
        guard let chs = svc.characteristics else { print("[无特征]"); exit(6); return }
        for ch in chs {
            print("[特征] \(ch.uuid.uuidString) props=0x\(String(ch.properties.rawValue, radix: 16))")
            if ch.uuid == TELE {
                let data = json.data(using: .utf8)!
                print("[写入] \(data.count) 字节: \(json)")
                per.writeValue(data, for: ch, type: .withResponse)
            }
        }
    }
    func peripheral(_ per: CBPeripheral, didWriteValueFor ch: CBCharacteristic, error: Error?) {
        if let er = error { print("[写入失败] \(er.localizedDescription)"); exit(7) }
        print("[写入成功] ✓")
        done = true
        DispatchQueue.global().asyncAfter(deadline: .now() + 1) { exit(0) }
    }
}
let d = D(); d.go(); dispatchMain()
