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
| Command（Write）| `7D2A0004-8F4A-4B7C-9D21-5E6A3B8C1000` |
| Faces 表盘列表（Read/Notify）| `7D2A0005-8F4A-4B7C-9D21-5E6A3B8C1000` |


## 导航消息（阶段 2，2026-10-13）

### 设计取向

参考开源项目 [moto-gps-waveshare](https://github.com/mx3353672833-debug/moto-gps-waveshare)
的**字段取舍与分层思路**（它验证了哪些字段真正必要、哪些是冗余），
但**沿用本项目既有的 JSON 风格**，不采用它的二进制帧（magic/CRC/分片/会话）：

| 取舍 | 理由 |
|------|------|
| 不引入二进制帧 | 我们已有可单测的稳定 JSON 解析器；512 B 重组上限内二进制收益有限，却要两端同时改造并丢掉现有测试覆盖 |
| **不引入字符串字段** | 用户决策不显示路名 → 省掉中文字体（Flash）与解析器改造 |
| 复用 Telemetry 特征 | 用 `"t":"nav"` 与遥测分流，不新增特征与订阅流程 |

> ⚠️ **许可证提示**：moto-gps 采用 PolyForm Noncommercial（非商业）。
> 本项目**只参考其设计**，未复制其代码。

### 消息格式

与遥测**同一特征**（`7D2A0002`，Write）下发，用 `t` 字段区分：

```json
{"t":"nav","state":1,"maneuver":2,"dist_m":500}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `t` | string | **必填**，固定 `"nav"`。遥测包无此字段（或 `t":"tel"`）|
| `state` | int | 导航状态，见下表。越界 → `5`(无数据) |
| `maneuver` | int | 转向动作，见下表。越界 → `0`(未知) |
| `dist_m` | int \| null | 距下一转弯距离（米）。`null` 或超范围 → 无效（显示 `--`）|

**导航状态**：`0` 未开始 / `1` 导航中 / `2` 重新规划 / `3` 已到达 / `4` 数据过期 / `5` 无数据

**转向动作**：`0` 未知 / `1` 直行 / `2` 左转 / `3` 右转 / `4` 掉头 / `5` 靠左 / `6` 靠右 / `7` 到达

### 容错语义（重要）

| 情况 | 行为 |
|------|------|
| 字段**缺失** | **保留旧值**（不因缺字段清空显示）|
| 字段为 `null` | **明确无效**（与缺失语义不同）|
| 解析失败 | **完全不改动状态**（原子性）—— 畸形包不会让仪表闪回"无数据" |
| 未知字段 | 忽略（**向前兼容**，未来加字段不破坏旧固件）|
| 越界枚举值 | 钳制到安全值（动作→未知、状态→无数据），绝不越界访问 |

### 过期与断连

| 事件 | 行为 |
|------|------|
| BLE 断开 | **立即**置为"无数据"（不等超时）|
| 10 秒无更新 | 置为"数据过期" → **停止显示旧指引**（§6.3 安全原则）|

> 过期阈值 `NAV_STALE_MS = 10s`，**远长于遥测的 2.5s**：导航数据变化慢
> （一次转弯可能几分钟），用遥测阈值会频繁误报过期。

### 调试

```bash
# 通过 BLE 下发（⚠️ 必须指定 telemetry，否则写到 Command 特征）
swift tools/ble_command_test.swift '{"t":"nav","state":1,"maneuver":2,"dist_m":500}' telemetry

# 或用串口直接喂模拟数据（不依赖手机）
nav 1 2 500 68
```

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

## 数据质量与校验（§5.4 / §7）

分两层，职责不重叠：

1. **iOS 侧质量门槛（§5.4）** —— `ios/.../TelemetryQuality.swift`，阈值集中可调：
   - `maxHorizontalAccuracyMeters = 50`：水平精度差于此 → `location_valid=false`
   - `maxSpeedAccuracyMPS = 2.0`（iOS 17+ `speedAccuracy`）：速度不可信 → `speed_valid=false`
   - 判定结果写进 `location_valid`/`speed_valid`，**仪表端不需要重复这套业务规则**
2. **固件侧兜底校验（§7）** —— `telemetry_plausible()`（纯 C，含单测）拒绝超范围/自相矛盾：
   - 速度 > `TELEMETRY_SPEED_MAX_KMH`(400) 或 < -1 m/s 语义矛盾
   - 航向越界、经纬度越界、精度为负、`location_valid` 却无经纬度
   - 被丢弃的包计入 `不合理` 计数并在串口打印，**不驱动 UI**

> 实测：正常包接受（54 km/h）；720 km/h 与纬度 95 的包均被丢弃并计数。

## 速度平滑（§5.4「轻量平滑但必须限制滞后」）

`telemetry_smooth_step()`（纯 C，含单测），接入固件 `ble_update()`：

| 规则 | 原因 |
|------|------|
| 加速走 EMA（`ALPHA=0.45`） | 抑制低速 GPS 抖动 |
| **减速立即跟随** | 安全优先，绝不滞后显示更高车速 |
| 跳变 > 40 km/h 直接重置 | 换源/异常不参与平滑 |
| 无效/超时/断连 → 立即重置为 0 | 遵守「不显示旧值」核心原则 |

实测（真机）：40→抖动 44/40 多次 → 输出稳定 40；送无效值 → 0.0 且状态 `NO FIX`；送 90（跳变 50）→ 重置为 90。

## 线程与数据过期

- BLE 写回调运行在 BLE 任务，只在回调里**解析+加锁入库**（`FreeRTOS` 互斥量 `s_mtx`），不触碰 LVGL。
- 主循环 `ble_poll()` 取数 → 推 `ui_speed_set_*`；`ble_check_stale()` 超过 `BLE_STALE_MS`（2500ms）
  无有效遥测即**把速度置无效**（不残留旧值）。
- 速度有效定义：`speed_valid && speed_mps >= 0`。

## Status 特征（Read/Notify）

固件在校时等状态变化时 `ble_telemetry_set_status()`，App 可读可订阅。实测（`tools/ble_status_test.swift`）：
属性 `0x12`(=Read|Notify)；读回 `"time-synced"`；写入遥测触发校时后收到 Notify ✓

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

## 设备控制（下行命令特征）

功能①：亮度 / 息屏，以及为多表盘（功能③）预留的切换命令。

| 项 | 值 |
|----|-----|
| 服务 | `7D2A0001-8F4A-4B7C-9D21-5E6A3B8C1000` |
| Command 特征 | `7D2A0004-8F4A-4B7C-9D21-5E6A3B8C1000`（Write）|

命令格式（JSON，扁平结构）：

```json
{"cmd":"brightness","value":128}   // 亮度 0-255
{"cmd":"screen","value":"off"}     // 息屏（也接受 value:0）
{"cmd":"screen","value":"on"}      // 亮屏（也接受 value:1）
{"cmd":"face","value":2}           // 切表盘：直接指定 id
{"cmd":"face","value":-1}          // 下一个表盘
{"cmd":"face","value":-2}          // 上一个表盘
{"cmd":"rotate","value":90}        // 屏幕方向 0/90/180/270（度）
{"cmd":"face_enabled","value":3,"on":0}   // 隐藏表盘 3（on 也接受 "on"/"off"）
{"cmd":"face_enabled","value":3,"on":1}   // 启用表盘 3
```

`face_enabled` 的两条**设备侧规则**（App 无需重复判断，但界面上要能解释）：

| 规则 | 行为 |
|------|------|
| 不能隐藏最后一个表盘 | 拒绝该命令（否则仪表无内容可显示）；`ui_face_set_enabled` 返回 false |
| 隐藏的正是当前表盘 | 自动切到下一个仍启用的表盘（避免停在隐藏页） |
| 变更后的效果 | 持久化到 NVS，且**立即重新上报**表盘列表 |

## 表盘列表上报（`Faces` 特征，2026-10-13）

### 为什么要有它

此前 App 把表盘列表**硬编码**在 `ContentView.swift` 里，必须与固件手工保持一致
—— 已经漏过一次（固件加了 `nav` 表盘后 App 选择器里没有它）。
改为**设备上报、App 渲染**后，新增表盘只需改设备一处。

### 载荷格式（Read 取回）

```json
{"t":"faces","v":1,"count":5,"mask":31,"cur":2,
 "faces":[{"id":0,"name":"analog","label":"Analog","group":"clock","on":1}, ...]}
```

| 字段 | 说明 |
|------|------|
| `name` | 稳定 ASCII 标识（如 `analog`）。**App 用它做本地化，不要依赖 label** |
| `label` | 设备给的 ASCII 显示名；App 未收录该 `name` 时的**兜底显示** |
| `group` | 分组 ASCII 名（`clock` / `speed` / `nav` / `photo`）；App 据此分组并本地化标题 |
| `on` | 1 启用 / 0 隐藏（隐藏的表盘在仪表上滑动时被跳过）|
| `mask` | 启用位掩码，便于快速比对 |
| `cur` | **当前正在显示**的表盘 id（-1 = 未知）。App 据此标注"正在用哪个" |
| `truncated` | 为 1 表示设备因载荷上限截断了列表 —— App 应提示"不完整" |

### ⚠️ Read 才是取数据的正途，Notify 只是"变了"的提示

**BLE 单次通知受 MTU 限制**（iOS 实到约 185 字节），而 5 个表盘的 JSON
实测 **364 字节**，通知必然被截断。

因此约定：

| 通道 | 用途 |
|------|------|
| **Read** | 取数据。系统会自动用 Read Blob 分片取回**完整值**，与 MTU 无关 |
| Notify | 仅作"发生了变化"的提示；App 收到后应**重新 Read** |

App 侧据此实现为：**能解析成完整 JSON 就用，解析失败就重新 Read**
（被截断的 JSON 一定解析失败）—— 无需额外状态机，且不会反复重读。

### App 端的界面层级（分组是一级，表盘是二级）

> App 已按「仪表 / 数据 / 导航」分三页。表盘相关内容在**仪表**页；
> 「导航」页目前是预留的状态说明页。详见 ios/CarDashboardPhone/README.md。

```
表盘
  [时钟] [速度表] [导航]              ← 一级：分组（只列设备实际有的）
  ──────────────────────────────
  [🕐] 模拟时钟        ( ✓ )  [开关]   ← 二级：只列该分组的表盘
  [#]  数字时钟        ( → )  [开关]
  [🎨] 多彩时钟        ( → )  [开关]
```

行布局的取舍：

| 位置 | 内容 | 理由 |
|------|------|------|
| 左 | **表盘图标**（按 `name` 映射，未知表盘用通用图标）| 一眼区分"是哪种表盘"；放勾选标记既占位又不表意 |
| 中 | 中文名 | |
| 右一 | **切换按钮**：`→` 可切；`✓` 表示已是当前（禁用）| 切换与显隐是**两个动作**，各自要有明确控件；藏在"点名称"里不易发现 |
| 右二 | **开关**：是否出现在滑动序列中 | |

两条约定：

1. **只显示选中分组的表盘** —— 把所有表盘平铺会让分组失去意义。
2. **默认打开当前表盘所在的分组** —— 否则当前表盘在别的分组里，
   用户看不到 ✓，会以为"没有当前表盘"。

> **实现要点（踩过坑）**：分组的选中态与"当前表盘"都是**从上报派生的值**，
> 不另存本地状态再用 `onChange` 同步 —— 那样在上报到达时 `onChange` 未触发，
> 会出现"分组下没有内容""勾选指不到当前表盘"。能派生就不要同步。
>
> 另：设备**切换表盘后也会重新上报**（否则 App 拿不到新的 `cur`）。

> 旋转 / 亮度 / 息屏是**整机**行为，对所有表盘生效，因此放在独立于「表盘」的
> 「屏幕显示」节，避免被误认为"只影响当前表盘"。

### 新增表盘时 App 为什么不用改

本地化按 `name` 查表，**查不到就用设备给的 `label` 显示**。
所以设备加一个新表盘后，App 会列出它（名字是 ASCII），只是没有中文名。
想给它中文名，再在 `FaceDisplay.faceTitle` 里加一条即可 —— 不加也不影响使用。

### 屏幕方向（`rotate`）详解

| 字段 | 说明 |
|------|------|
| `value` | 方向**角度**：`0` / `90` / `180` / `270` |

- 用**角度**而非"索引"，避免与其余命令的 0 基索引混淆
- **仅接受 90 的整数倍**；其它值（如 `45`）会被**明确拒绝**
  （固件统计里"拒绝"+1），不会静默转到意外方向
- 超范围值按 `mod 360` 归一：`-90` → `270`，`450` → `90`
- 实现：LVGL 软件旋转（`lv_disp_set_rotation`）。本屏为**圆形**，旋转后仍是
  同一个圆，无长宽比/布局副作用；触摸坐标由 LVGL 自动逆变换
- **方向持久化到仪表 NVS，重启后保持**
- 开销：0° 无开销；180° 约 +13ms；90°/270° 约 +20ms（整屏重绘时）。
  详见 [`troubleshooting.md`](troubleshooting.md#2026-10-13-屏幕旋转可行更正-2026-10-11-的错误结论)

## 表盘系统（功能③）

表盘由**注册表**驱动（`simulator/ui/ui_face.c`），当前注册 5 个：

| id | name | 内容 |
|----|------|------|
| 0 | `analog` | 模拟指针时钟（烘焙表盘 + 三针）|
| 1 | `digital` | 数字时钟（138px 时间 + 日期 + 同步状态）|
| 2 | `speed` | 速度表（烘焙表盘 + 插画 + 指针 + 航向/行程 HUD）|

注册顺序即 id 顺序，也与 `ui_page_t` 一一对应。新增表盘只需在
`ui_pages_init()` 里多注册一项（例如功能②的「图片背景 + 时钟」），
分页、翻页手势、BLE/串口命令通路都无需改动。

固件串口命令（无 App 时验证）：

```
face              列出全部表盘与当前表盘
face 1            切到指定 id
face next/prev    下一个 / 上一个（循环）
```

约定与遥测上行一致：**BLE 回调只解析并暂存，硬件副作用（`setBrightness` /
`displayOff`）回到主循环执行**——BLE 回调运行在 Bluedroid 任务里，不能碰显示驱动。

亮度限幅：`0 → 1`（避免全屏黑被误认为死机），`>255 → 255`。

无 App 时可用串口验证：

```
bright 120      设置亮度
bright          查询当前亮度
screen off      息屏
screen on       亮屏
ble             查看控制命令统计（成功/拒绝次数）
```

解析层是纯 C（`simulator/ui/control.c`），带 28 项单元测试（`tests/test_control.c`）。
