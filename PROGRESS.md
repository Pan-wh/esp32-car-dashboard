# 项目进度总览

> 最后更新：2026-10-13
> 本文是**单一事实来源**：项目当前做到哪一步、还差什么、有什么已知问题。
> 历史决策与踩坑细节见 `docs/decisions.md`、`docs/troubleshooting.md`。

---

## 一、项目目标

把微雪 **ESP32-S3-Touch-AMOLED-1.75**（466×466 圆屏）做成车载仪表盘，
数据源为 **iPhone Core Location 经 BLE 下发**。

> ⚠️ 关键前提：**本板无 GNSS**。原理图无 GPS 的 SDA/SCL 网络，GPS0RXD/TXD 标注 NC，
> 实测 I2C 扫描也找不到 LC76G。因此 GNSS 路线已彻底放弃，相关代码已删除。

---

## 二、总体进度

| 阶段 | 内容 | 状态 |
|------|------|------|
| 0 | 工程审计 | ✅ 完成 |
| 1 | ESP32 BLE 最小验证 | ✅ 完成 |
| 2 | 时间同步（BLE UTC → RTC）| ✅ 完成 |
| 3 | iPhone App 前台 MVP | ✅ 完成 |
| 4 | 后台驾驶会话 | ✅ 完成（真机验证：后台/锁屏均持续 2Hz）|
| 5 | 仪表 UI 集成稳定 | ✅ 完成 |

**完成度**：任务书 DoD **8/8 全部达成** ✅（含 #6「后台与 CarPlay 有实机记录」）。

测试矩阵 §9：**12 项全部通过** ✅

---

## 三、系统架构

```
iPhone (SwiftUI App)
  CoreLocation ──► TelemetryQuality 质量门槛 ──► TelemetryPacket (JSON)
                                                      │
                                              BLE GATT Write
                                                      ▼
ESP32-S3 (dashboard.ino)
  ble_telemetry.cpp ──► telemetry.c 解析 ──► 五态状态机 ──► LVGL UI
  （BLE 回调只暂存，主循环消费）        （BT OFF/WAIT GPS/NO FIX/STALE/LIVE）
                                                      │
                                        ┌─────────────┴─────────────┐
                                   烘焙表盘图片 + 指针          HUD（航向/行程）
```

### 共享代码层（关键设计）

`simulator/ui/` 是**纯 C 单一事实来源**，由 `tools/sync_firmware.sh` 同步到固件：

| 模块 | 职责 | 测试 |
|------|------|------|
| `telemetry.c` | JSON 解析、速度换算、平滑、合理性校验、行程统计 | 93 项 |
| `control.c` | 设备控制命令解析（亮度/息屏/表盘）| 28 项 |
| `ui_face.c` | 表盘注册表 | 32 项 |
| `ui_analog/clock/speed.c` | 三个表盘的 LVGL 构建 | UI 像素 10 项 |

好处：**纯逻辑可在 PC 上单测**，不需要硬件即可回归。

---

## 四、BLE 协议

| 项 | 值 |
|----|-----|
| 设备名 | `CarDashboard` |
| Service | `7D2A0001-8F4A-4B7C-9D21-5E6A3B8C1000` |
| Telemetry（Write）| `7D2A0002-…` 上行遥测 JSON |
| Status（Read/Notify）| `7D2A0003-…` 状态回读 |
| Command（Write）| `7D2A0004-…` 下行控制 |

**上行**（2Hz）：速度、航向、经纬度、精度、有效性标志。
**下行**：`{"cmd":"brightness","value":N}` / `{"cmd":"screen","value":"on|off"}` /
`{"cmd":"face","value":N}`（-1 下一个 / -2 上一个）。

详见 `docs/ble.md`。

---

## 五、已实现功能

### 仪表端

| 功能 | 说明 |
|------|------|
| **三表盘** | 模拟时钟 / 数字时钟 / 速度表，滑动切换 + BLE 远程切换 |
| **五态链路显示** | BT OFF / WAIT GPS / NO FIX / STALE / LIVE |
| **速度指针** | 烘焙表盘 + 橙色指针，EMA 平滑，失效立即归零 |
| **驾驶员插画** | 三态表情（平静/皱眉/咬牙），按 90/120 km/h 阈值切换 |
| **航向 + 行程 HUD** | 底部显示「NE 45° · 12.3 km · MAX 87」，位于指针扫不到的扇区 |
| **设备控制** | 亮度（0-255）、息屏/亮屏 |
| **时间同步** | 首次 BLE UTC → RTC，时钟页显示 SYNCED |
| **行程统计** | 里程/最高速/加权均速，**仅 LIVE 且速度有效时累积** |

### App 端

| 功能 | 说明 |
|------|------|
| 驾驶会话 | 开始/结束，2Hz 发送 |
| 仪表控制 | 表盘选择、亮度滑块、息屏开关 |
| 实时数据 | 速度/航向/精度/坐标 + **诊断字段**（速度质量、测速精度）|
| 后台定位 | `allowsBackgroundLocationUpdates` |

---

## 六、性能现状（真机实测）

### 滑页单帧

| 阶段 | 单帧耗时 |
|------|---------|
| 初始（含无效化风暴）| 243 ms |
| 修复无效化风暴 + QSPI 调优 | 228 ms |
| 烘焙表盘 + 字体去抗锯齿 | **90–113 ms** |

**优化手段**：
1. **无效化风暴修复**：LVGL setter 无变更检测 → 空闲也每秒 55 次重绘。
   加变更检测后降到 0–2 次/秒。
2. **QSPI 调优**：分块 1024→4096 像素 + 时钟 40→80MHz，全屏传输 56→43ms。
3. **静态表盘烘焙成图片**：刻度/数字离线渲染（4× 超采样），运行时只留指针。
4. **大号字体去抗锯齿**：138px 字体 bpp 4→1，字模 194→51KB。
5. **PSRAM 全帧绘制缓冲**：脏区由 10 块降为 1 块。

**硬件下限**：QSPI 传一屏 434KB 需 **43ms**，即使零渲染成本上限也只有 ~23fps。

### 触摸响应

**问题**：一帧 200ms 使 LVGL 触摸读取降到 4 次/秒，快速滑动整个"按下→抬起"
落在一次渲染内 → LVGL 感知不到触摸（用户实测"操作无反应"）。

**修复**：
- 100Hz 独立 esp_timer 采样 + **按下锁存**（保证 LVGL 一定看到按下）
- **flick 判定**：时长 <600ms、水平位移 >50px 直接翻页（不依赖 LVGL 的慢采样）

### 稳定性

30 分钟长稳（371 样本）：**0 重启、堆零漂移、渲染最大 113ms、>300ms 次数 0**。

---

## 七、已知问题

| # | 问题 | 状态 |
|---|------|------|
| 1 | 切表盘 101ms（≈10fps）| 硬件限制，已接近极限 |
| 2 | 偶发单帧 1000ms+ 尖刺 | 32 分钟监测**未复现**，观察中 |
| 3 | 状态标签与指针重叠 | 设计待定（见 `docs/ui-guidelines.md`）|
| 4 | 室内 GPS 无速度 | iOS 物理限制，非缺陷（`speed<0` = 未知）|
| 5 | 后台改权限不即时生效 | iOS 系统行为（回前台才收 `didChangeAuthorization`），已写入文档 |
| 6 | 瞬态 STALE 闪烁 | ✅ 已修复（显示防抖：连续 3 次才切换，指针行为不受影响）|
| 7 | 室内数据速率 1.21Hz | iOS 静止省电策略，非缺陷；行驶时恢复 ≈2Hz |

---

## 八、测试现状

| 套件 | 数量 | 说明 |
|------|------|------|
| `test_telemetry` | 93 | JSON 解析/换算/平滑/合理性/行程 |
| `test_control` | 28 | 控制命令解析 |
| `test_ui_face` | 32 | 表盘注册表 |
| `test_ios_logic` | 32 | iOS 侧纯逻辑（Swift）|
| `ui_regression.py` | 10 | UI 像素级回归 |
| **合计** | **195** | `cd tests && make run` |
| XCUITest | 4 | 会话开关/遥测计数/设备控制 UI/链路诊断 |

---

## 九、待办

| 优先级 | 事项 | 状态 |
|--------|------|------|
| 高 | 真机验证后台/锁屏驾驶会话 | ✅ 已完成 |
| 高 | 室外验证速度真实跟随 | ✅ 已完成（用户实测有速度显示；跳针/滞后问题已修）|
| 高 | §9 #18 整链路 30 分钟长稳 | ✅ 已完成（31.3min，0 断连，堆零漂移）|
| 中 | §9 #15 CarPlay / 地图共存验证 | ✅ 已完成（导航中 +576 包，0 断连）|
| 中 | 功能②：图片背景 + 时钟 | ⏳ 用户暂缓（存储已验证：TF 卡槽 + 9.9MB ffat）|
| 中 | 偶发单帧 1000ms 尖刺 | 32 分钟监测未复现，长稳中继续观察 |
| 低 | UI 设计定稿（状态标签与指针重叠）| ⏳ 待决策 |
| 低 | 行程统计上屏（当前仅在串口 `trip` 查询）| ⏳ 可选 |

---

## 十、构建与运行

### 固件

```bash
arduino-cli compile \
  --fqbn "esp32:esp32:esp32s3:FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,USBMode=hwcdc,CDCOnBoot=cdc,PSRAM=opi" \
  --build-property "compiler.cpp.extra_flags=-DESP32QSPI_MAX_PIXELS_AT_ONCE=4096" \
  --libraries reference/ESP32-S3-Touch-AMOLED-1.75/examples/arduino/libraries \
  --upload -p /dev/cu.usbmodem101 firmware/dashboard
```

> ⚠️ **`PSRAM=opi` 必需**（S3R8 八线 PSRAM，用于全帧绘制缓冲）。
> ⚠️ **`-DESP32QSPI_MAX_PIXELS_AT_ONCE=4096`** 是滑页性能优化，不可省。
> ⚠️ 跨文件改动后**不要复用 `--build-path`**，否则可能漏编译（已踩坑）。

**当前占用**：Flash 75%，RAM 38%。

### 模拟器

```bash
cd simulator && make -j8 && ./dashboard_sim            # 带界面
./dashboard_sim --offscreen --ms 500 --page 2 --shot x.bmp   # 无头截图
```

### 测试

```bash
cd tests && make run PYTHON=<带Pillow的python>
```

### iOS

```bash
cd ios/CarDashboardPhone && xcodegen generate
xcodebuild -project CarDashboardPhone.xcodeproj -scheme CarDashboardPhone \
  -destination 'platform=iOS,id=<设备UDID>' build
```

签名 Team ID 写在 `project.yml`（`DEVELOPMENT_TEAM`）。

---

## 十一、串口命令参考

| 命令 | 作用 |
|------|------|
| `ble` | BLE 连接/遥测/控制统计 |
| `face [id\|next\|prev]` | 查看/切换表盘 |
| `spd [值\|reset]` | **诊断**：查询/强制设置速度 |
| `trip [reset]` | 行程统计查询/清零 |
| `bright <0-255>` | 亮度 |
| `screen on\|off` | 亮屏/息屏 |
| `page N` / `pages` | 切页 |
| `bufmode [psram\|internal]` | 绘制缓冲 A/B |
| `mem` | 内存情况 |
| `touch` | 触摸采样诊断 |
| `dump` | 打印元素树 |
| `scan` | I2C 扫描 |
| `help` | 全部命令 |
