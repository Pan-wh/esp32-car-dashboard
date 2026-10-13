// ble_command_test.swift —— 验证「设备控制」下行命令（功能①）
//
// 运行示例：
//   swift tools/ble_command_test.swift '{"cmd":"brightness","value":120}'
//   swift tools/ble_command_test.swift '{"cmd":"screen","value":"off"}'
//
// 作用：扫描 CarDashboard → 连接 → 发现服务 → 往 Command 特征
//       （7D2A0004-…）写入控制命令，并打印写入结果。
// 说明：与 ble_client_test.swift 同一套路（macOS 充当 BLE Central），
//       用于在没有 iPhone 的情况下验证下行通路。
import CoreBluetooth
import Foundation

let SERVICE = CBUUID(string: "7D2A0001-8F4A-4B7C-9D21-5E6A3B8C1000")
let CMDCHAR = CBUUID(string: "7D2A0004-8F4A-4B7C-9D21-5E6A3B8C1000")
let json = CommandLine.arguments.count > 1 ? CommandLine.arguments[1]
           : "{\"cmd\":\"brightness\",\"value\":120}"

final class CmdClient: NSObject, CBCentralManagerDelegate, CBPeripheralDelegate {
    var cm: CBCentralManager!
    var p: CBPeripheral?
    var done = false

    func go() { cm = CBCentralManager(delegate: self, queue: nil) }

    func centralManagerDidUpdateState(_ c: CBCentralManager) {
        guard c.state == .poweredOn else {
            print("[错误] 蓝牙状态 \(c.state.rawValue)")
            exit(3)
        }
        print("[扫描] 查找 CarDashboard …")
        c.scanForPeripherals(withServices: [SERVICE], options: nil)
        DispatchQueue.global().asyncAfter(deadline: .now() + 20) {
            if !self.done { print("[超时] 20s 内未完成"); exit(2) }
        }
    }

    func centralManager(_ c: CBCentralManager, didDiscover per: CBPeripheral,
                        advertisementData d: [String: Any], rssi: NSNumber) {
        print("[发现] \((d[CBAdvertisementDataLocalNameKey] as? String) ?? per.name ?? "?")  RSSI=\(rssi)")
        guard p == nil else { return }
        p = per
        c.stopScan()
        c.connect(per, options: nil)
    }

    func centralManager(_ c: CBCentralManager, didConnect per: CBPeripheral) {
        print("[连接] 成功，发现服务 …")
        per.delegate = self
        per.discoverServices([SERVICE])
    }

    func centralManager(_ c: CBCentralManager, didFailToConnect per: CBPeripheral, error: Error?) {
        print("[连接失败] \(error?.localizedDescription ?? "?")")
        exit(4)
    }

    func peripheral(_ per: CBPeripheral, didDiscoverServices e: Error?) {
        guard let s = per.services else { print("[无服务]"); exit(5) }
        for x in s { per.discoverCharacteristics(nil, for: x) }
    }

    func peripheral(_ per: CBPeripheral, didDiscoverCharacteristicsFor svc: CBService, error: Error?) {
        guard let chs = svc.characteristics else { print("[无特征]"); exit(6) }
        for ch in chs {
            print("[特征] \(ch.uuid.uuidString)  props=0x\(String(ch.properties.rawValue, radix: 16))")
        }
        guard let cmd = chs.first(where: { $0.uuid == CMDCHAR }) else {
            print("[错误] 未找到 Command 特征 \(CMDCHAR.uuidString)")
            print("       固件是否包含功能①（设备控制）？")
            exit(8)
        }
        guard cmd.properties.contains(.write) else {
            print("[错误] Command 特征不支持 Write")
            exit(9)
        }
        let data = json.data(using: .utf8)!
        print("[写入] Command 特征 \(data.count) 字节: \(json)")
        per.writeValue(data, for: cmd, type: .withResponse)
    }

    func peripheral(_ per: CBPeripheral, didWriteValueFor ch: CBCharacteristic, error: Error?) {
        if let er = error {
            print("[写入失败] \(er.localizedDescription)")
            exit(7)
        }
        print("[写入成功] ✓  命令已送达仪表")
        done = true
        DispatchQueue.global().asyncAfter(deadline: .now() + 1) { exit(0) }
    }
}

let client = CmdClient()
client.go()
dispatchMain()
