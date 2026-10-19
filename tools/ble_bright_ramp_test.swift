// ble_bright_ramp_test.swift —— 验证亮度"线性跟随"
//
// 用途：单条连接内以 20Hz 连续下发亮度（模拟手指拖动），
//       检查设备是否逐步跟随（而不是只在最后跳一次）。
//
// 运行：swift tools/ble_bright_ramp_test.swift
import CoreBluetooth
import Foundation

let SERVICE = CBUUID(string: "7D2A0001-8F4A-4B7C-9D21-5E6A3B8C1000")
let CMDCHAR = CBUUID(string: "7D2A0004-8F4A-4B7C-9D21-5E6A3B8C1000")

final class D: NSObject, CBCentralManagerDelegate, CBPeripheralDelegate {
    var cm: CBCentralManager!; var p: CBPeripheral?; var cmd: CBCharacteristic?
    var values: [Int] = []
    var sent = 0

    func start() { cm = CBCentralManager(delegate: self, queue: nil) }

    func centralManagerDidUpdateState(_ c: CBCentralManager) {
        if c.state == .poweredOn { c.scanForPeripherals(withServices: [SERVICE]) }
    }
    func centralManager(_ c: CBCentralManager, didDiscover p: CBPeripheral,
                        advertisementData: [String: Any], rssi: NSNumber) {
        self.p = p; c.stopScan(); p.delegate = self; c.connect(p)
    }
    func centralManager(_ c: CBCentralManager, didConnect p: CBPeripheral) {
        p.discoverServices([SERVICE])
    }
    func peripheral(_ p: CBPeripheral, didDiscoverServices e: Error?) {
        for s in p.services ?? [] { p.discoverCharacteristics([CMDCHAR], for: s) }
    }
    func peripheral(_ p: CBPeripheral, didDiscoverCharacteristicsFor s: CBService, error: Error?) {
        for ch in s.characteristics ?? [] where ch.uuid == CMDCHAR { cmd = ch }
        guard let ch = cmd else { print("未找到命令特征"); exit(1) }
        // 40Hz 连发 50 → 250，模拟快速拖动
        values = stride(from: 50, through: 250, by: 4).map { $0 }
        print("开始连发 \(values.count) 档亮度（40Hz，模拟拖动）...")
        sendNext(p, ch)
    }
    func sendNext(_ p: CBPeripheral, _ ch: CBCharacteristic) {
        guard sent < values.count else {
            print("发送完成，共 \(sent) 档")
            DispatchQueue.main.asyncAfter(deadline: .now() + 1.5) { exit(0) }
            return
        }
        let json = "{\"cmd\":\"brightness\",\"value\":\(values[sent])}"
        let d = json.data(using: .utf8)!
        p.writeValue(d, for: ch, type: .withoutResponse)
        sent += 1
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.025) { self.sendNext(p, ch) }
    }
}

let d = D(); d.start()
DispatchQueue.main.asyncAfter(deadline: .now() + 30) { print("超时"); exit(2) }
RunLoop.main.run()
