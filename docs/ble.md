# BLE 数据源（iPhone Core Location → 仪表）

改造方向见 `AI_TASK_IPHONE_BLE_DASHBOARD.md`。本机**无板载 LC76G GNSS**
（原理图网表证实无 GPS 的 SDA/SCL 网络），故速度/航向/时间改为 **iPhone Core Location 经 BLE 下发**。

## GATT 定义（两端必须一致）

| 项 | 值 |
|----|----|
| 设备名 | `CarDashboard` |
| Service | `7D2A0001-8F4A-4B7C-9D21-5E6A3B8C1000` |
| Telemetry（Write）| `7D2A0002-8F4A-4B7C-9D21-5E6A3B8C1000` |
| Status（Read/Notify）| `7D2A0003-8F4A-4B7C-9D21-5E6A3B8C1000` |

## 遥测协议（版本化 JSON，v=1）

```json
{
  "v": 1,
  "ts_utc_ms": 1791765900000,
  "speed_mps": 19.03,
  "speed_accuracy_mps": 0.8,
  "latitude": 1.3521,
  "longitude": 103.8198,
  "horizontal_accuracy_m": 5.2,
  "course_deg": 120.0,
  "location_valid": true,
  "speed_valid": true
}
```

规则：
- `ts_utc_ms` = UTC Unix epoch 毫秒（不传本地格式化字符串作为唯一时间源）。
- `speed_mps` 米/秒；仪表换算 `km/h = m/s × 3.6`。`< 0` 视为无效（`speed_valid=false`）。
- `course_deg` 地面行进方向 0..360（不是磁罗盘）；无有效值传 `null` 或缺省。
- 经纬度/精度可为空（传 `null`），**不得伪装成 0**；缺省字段以 `has_*` 标志区分。
- 缺省 `speed_valid`/`location_valid` 为 false（保守）。
- 解析器 `simulator/ui/telemetry.{c,h}`（纯 C，46 项单测），只解析受控扁平 schema，不做通用 JSON。

## MTU 与分包

- ESP32 侧请求 MTU 247；iOS/macOS 实际协商约 185，故完整 JSON（~200 字节）会**跨多个 BLE 包**。
- 固件在 `ble_telemetry.cpp` 里做**重组**（累积到 `{...}` 完整后再解析），实测 203 字节能正确重组解析。
- 若后续字段继续增多，优先**收紧包体**或改用紧凑二进制协议（见任务书 §4）。

## 线程与数据过期

- BLE 写回调运行在 BLE 任务，只在回调里**解析+加锁入库**（`FreeRTOS` 互斥量 `s_mtx`），不触碰 LVGL。
- 主循环 `ble_poll()` 取数 → 推 `ui_speed_set_*`；`ble_check_stale()` 超过 `BLE_STALE_MS`（2500ms）
  无有效遥测即**把速度置无效**（不残留旧值）。
- 速度有效定义：`speed_valid && speed_mps >= 0`。

## 验证方式

1. 固件侧编译烧录后，`ble` 串口命令看连接/统计。
2. **无需 iPhone** 也能联调：`swift tools/ble_client_test.swift '<json>'`（macOS 作 BLE Central）。
   首次运行需在系统设置授予终端「蓝牙」权限。
3. 实测（2026-10-12）：发现 `CarDashboard`(RSSI≈-56) → 连接 → 写入 203 字节 → 串口
   `[BLE] v=1 速度=19.03 m/s(68.5 km/h) valid=1 航向=120.0° 定位=1`；2.5s 后
   `[BLE] 数据过期，速度置无效` ✓

## 相关文件

- `simulator/ui/telemetry.{c,h}` —— 协议解析（纯 C，共享 + 单测）
- `firmware/dashboard/ble_telemetry.{h,cpp}` —— BLE Peripheral + 重组 + 线程安全
- `firmware/dashboard/dashboard.ino` —— `ble_poll()` / `ble_check_stale()` 装配
- `tools/ble_client_test.swift` —— macOS BLE Central 测试客户端
- `tests/test_telemetry.c` —— 46 项解析单测