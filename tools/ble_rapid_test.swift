// 连发压力测试（macOS CoreBluetooth Central）
//
// 用途：以 20Hz（常规 2Hz 的 10 倍）连发 20 包，验证：
//   - 每包都被接收与解析（成功计数恰好 +20）
//   - 解析错/不合理 保持 0
//   - 连发后内存无漂移（重组缓冲无泄漏）
// 运行：swift tools/ble_rapid_test.swift

// 连发测试：连续高速发送 N 包（验证 latest-wins、计数器、无解析错、状态稳定）
import CoreBluetooth
import Foundation

let SERVICE = CBUUID(string: "7D2A0001-8F4A-4B7C-9D21-5E6A3B8C1000")
let TELE    = CBUUID(string: "7D2A0002-8F4A-4B7C-9D21-5E6A3B8C1000")

final class D: NSObject, CBCentralManagerDelegate, CBPeripheralDelegate {
    var cm: CBCentralManager!; var p: CBPeripheral?; var tele: CBCharacteristic?
    var sent = 0
    let total = 20          // 连发 20 包
    let interval = 0.05     // 每 50ms 一包（20Hz，远高于 2Hz 常规）
    func go() { cm = CBCentralManager(delegate: self, queue: nil) }
    func centralManagerDidUpdateState(_ c: CBCentralManager) {
        if c.state == .poweredOn {
            c.scanForPeripherals(withServices: [SERVICE], options: nil)
            DispatchQueue.global().asyncAfter(deadline: .now()+30) { print("[超时]"); exit(2) }
        } else { print("[错误] \(c.state.rawValue)"); exit(3) }
    }
    func centralManager(_ c: CBCentralManager, didDiscover per: CBPeripheral, advertisementData d: [String: Any], rssi: NSNumber) {
        guard p == nil else { return }
        p = per; c.stopScan(); c.connect(per, options: nil)
    }
    func centralManager(_ c: CBCentralManager, didConnect per: CBPeripheral) {
        per.delegate = self; per.discoverServices([SERVICE])
    }
    func peripheral(_ per: CBPeripheral, didDiscoverServices e: Error?) {
        for s in per.services ?? [] { per.discoverCharacteristics([TELE], for: s) }
    }
    func peripheral(_ per: CBPeripheral, didDiscoverCharacteristicsFor svc: CBService, error: Error?) {
        for ch in svc.characteristics ?? [] where ch.uuid == TELE {
            tele = ch
            print("[开始连发] \(total) 包，间隔 \(Int(interval*1000))ms")
            fire(per, ch)
        }
    }
    func fire(_ per: CBPeripheral, _ ch: CBCharacteristic) {
        if sent >= total {
            print("[连发完成] 共 \(sent) 包，保持连接 3s 供观察")
            DispatchQueue.global().asyncAfter(deadline: .now()+3) { exit(0) }
            return
        }
        sent += 1
        let kmh = 20.0 + Double(sent)   // 每包速度递增，便于确认 latest-wins
        let mps = kmh / 3.6
        let js = "{\"v\":1,\"ts_utc_ms\":\(Int(Date().timeIntervalSince1970*1000)),\"speed_mps\":\(String(format: "%.3f", mps)),\"speed_valid\":true,\"course_deg\":90,\"location_valid\":true,\"latitude\":31.2,\"longitude\":121.4}"
        per.writeValue(js.data(using: .utf8)!, for: ch, type: .withResponse)
        DispatchQueue.global().asyncAfter(deadline: .now()+interval) { self.fire(per, ch) }
    }
}
let d = D(); d.go(); dispatchMain()
