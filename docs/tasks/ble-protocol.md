# 任务模式：改 BLE 协议

> **适用**：增删遥测字段、加控制命令、改状态回读。
> **推荐模型**：通信/协议能力 + 细心。
> **预计 Token**：中（**必须同时改 4 处**，且要跑单测）

---

## 0. 一句话边界

> **协议改动必须四处同步：App 编码 ↔ 共享层解析 ↔ 固件装配 ↔ 文档。
> 少改一处就静默失败。**

---

## 1. 协议现状（改动前必读）

| 用途 | UUID | 方向 |
|------|------|------|
| Service | `7D2A0001-8F4A-4B7C-9D21-5E6A3B8C1000` | — |
| Telemetry | `7D2A0002-…` | 手机 → 仪表（Write）|
| Status | `7D2A0003-…` | 仪表 → 手机（Read/Notify）|
| Command | `7D2A0004-…` | 手机 → 仪表（Write）|
| 设备名 | `CarDashboard` | — |

**上行遥测字段**（JSON，2Hz）：
`v` `ts_utc_ms` `speed_mps` `speed_accuracy_mps` `latitude` `longitude`
`horizontal_accuracy_m` `course_deg` `speed_valid` `location_valid`

**下行控制命令**：
```json
{"cmd":"brightness","value":200}
{"cmd":"screen","value":"on"}
{"cmd":"face","value":2}        // -1=下一个  -2=上一个
```

详见 [`../ble.md`](../ble.md)。

---

## 2. 🟢 你可以改的文件（四处必须同步）

| # | 文件 | 改什么 |
|---|------|--------|
| 1 | `ios/.../TelemetryPacket.swift` | 编码 key（App 侧发送）|
| 2 | `simulator/ui/telemetry.{c,h}` | 解析 key + 字段结构体 |
| 3 | `simulator/ui/control.{c,h}` | 控制命令解析（仅改命令时）|
| 4 | `docs/ble.md` | 协议文档 |
| 5 | `tests/test_telemetry.c` / `test_control.c` | **必须补测试**（本任务允许改）|

**仅在需要时**：
- `firmware/dashboard/ble_telemetry.cpp` —— 改 UUID 或特征属性时
- `ios/.../BLEClient.swift` —— 改 UUID 或增特征时

---

## 3. 🔴 你不能改的

| 禁区 | 原因 |
|------|------|
| `firmware/dashboard/dashboard.ino` | 固件主流程（除非协议需要新的装配点，需说明）|
| `simulator/ui/ui_*.c` | UI 层，与协议无关 |
| 已有字段的**语义** | 破坏向后兼容；应新增字段而非改旧字段 |

---

## 4. 📐 必须遵守的契约

### 4.1 ⚠️ BLE 回调里不能碰硬件

Bluedroid 任务**不能操作显示**（会崩溃/卡死）。回调只做「解析 + 存入变量」，
硬件副作用放主循环：

```c
/* ble_telemetry.cpp —— 回调只暂存 */
static void on_write(...) {
    parse_into(&s_pending);        /* ✅ 只解析+存 */
    /* ❌ 不要在这里 lv_*() 或操作硬件 */
}
```
```c
/* dashboard.ino 主循环 —— 消费 */
void loop() {
    telemetry_msg_t m;
    if (ble_telemetry_take(&m)) { /* 在这里做 UI 更新 */ }
}
```

### 4.2 线程安全
共享变量用互斥锁保护（参考 `ble_telemetry.cpp` 的 `s_cmd_mux` 用法）。

### 4.3 新增字段要**向后兼容**
- ✅ 新增字段 + 解析端给默认值
- ❌ 不要改已有字段的类型或语义

### 4.4 解析要容错
字段缺失、类型错误、值超范围都不能崩：
```c
/* simulator/ui/telemetry.c */
if (!json_get_float(obj, "speed_mps", &out)) { /* 缺失 → 用默认/标记无效 */ }
```
已有 `telemetry_plausible()` 做超范围与自相矛盾校验。

### 4.5 改 UUID 是**破坏性变更**
改了 UUID，App 必须重新安装，且旧版本 App 将连不上。非必要不改。

---

## 5. 验证要求

```bash
# 1) 共享层单测（解析正确性，最关键）
cd tests && make run PYTHON=<python路径>     # telemetry 115 + control 28

# 2) 同步到固件
tools/sync_firmware.sh

# 3) 固件编译
arduino-cli compile --fqbn "esp32:esp32:esp32s3:FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,USBMode=hwcdc,CDCOnBoot=cdc,PSRAM=opi" \
  --build-property "compiler.cpp.extra_flags=-DESP32QSPI_MAX_PIXELS_AT_ONCE=4096" \
  --libraries reference/ESP32-S3-Touch-AMOLED-1.75/examples/arduino/libraries firmware/dashboard

# 4) 桌面 BLE 客户端端到端（不需要 iPhone）
swift tools/ble_client_test.swift       # 遥测上行
swift tools/ble_command_test.swift      # 控制下行

# 5) App 侧
tools/run_ios_tests.sh
```

> ⚠️ **不要复用 `--build-path`**：跨文件改动后复用旧构建目录可能**漏编译**
> （曾表现为"BLE 写入成功但计数为 0"，实际是 `ble_telemetry.cpp` 没重编）。

---

## 6. 产出格式

```markdown
## 改动清单
| 文件 | 改动 |
|------|------|
| ios/.../TelemetryPacket.swift | 新增 batteryPct 字段 |
| simulator/ui/telemetry.c | 解析 battery_pct（缺失时默认 -1）|
| simulator/ui/telemetry.h | 结构体加字段 |
| tests/test_telemetry.c | 新增 3 项断言 |
| docs/ble.md | 字段表更新 |

## 边界检查
- [ ] 未改已有字段的类型/语义（仅新增）
- [ ] 未在 BLE 回调里操作硬件
- [ ] 解析对缺失/错误值容错
- [ ] 未改 UUID（或已说明为破坏性变更）
- [ ] 四处（App/共享层/文档/测试）已同步

## 验证结果
| 步骤 | 结果 |
|------|------|
| 单测 | ✅ __ 项 |
| sync_firmware | ✅ |
| 固件编译 | ✅ Flash __% |
| ble_client_test.swift | ✅ |
| run_ios_tests.sh | ✅ 5 项 |

## 兼容性说明
- 旧版 App 是否仍可用：是 / 否（原因）
```

---

## 7. 省 Token 提示

**只需读**：
- 本文件
- `simulator/ui/telemetry.h`（结构体定义，246 行，**必读**）
- `simulator/ui/telemetry.c` 的解析函数部分
- `ios/.../TelemetryPacket.swift`（79 行，短）
- `docs/ble.md`（协议现状）

**不要读**：`ui_*.c`（UI 绘制）、`firmware/dashboard.ino`（1300 行）、
`reference/`、iOS 其余 Swift 文件。
