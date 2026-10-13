# 项目进度总览

> 最后更新：2026-10-13
> 本文是**单一事实来源**：项目当前做到哪一步、还差什么、有什么已知问题。
> 历史决策与踩坑细节见 `docs/decisions.md`、`docs/troubleshooting.md`。

---

## 文档地图（先看这里找对文件）

| 文件 | 职责 |
|------|------|
| [`PROGRESS.md`](PROGRESS.md)（本文）| **项目进度总览**：做到哪、还差什么、构建与串口速查 |
| [`docs/deployment.md`](docs/deployment.md) | **部署指南**：固件烧录 + App 安装（含故障排除）|
| [`docs/development-guide.md`](docs/development-guide.md) | **开发指引**：「我要改 X，该动哪里」+ 已验证能力清单 |
| [`docs/ai-task-boundaries.md`](docs/ai-task-boundaries.md) | **AI 任务边界**：给 AI 划定任务域，防止越界改坏代码（6 种任务模式）|
| [`docs/test-results.md`](docs/test-results.md) | **测试台账**（单一事实来源）：§9 矩阵 12 项、DoD 8 项、复测指引 |
| [`docs/troubleshooting.md`](docs/troubleshooting.md) | 排障记录：按主题索引，含根因分析与踩坑 |
| [`docs/rotation-tearing-fieldtest.md`](docs/rotation-tearing-fieldtest.md) | **现场测试清单**：旋转后撕裂/果冻的判定步骤、串口命令、记录表（待执行）|
| [`docs/architecture.md`](docs/architecture.md) | 系统架构、模块划分、共享代码层 |
| [`docs/ble.md`](docs/ble.md) | BLE 协议细节（GATT、JSON 字段）|
| [`docs/decisions.md`](docs/decisions.md) | 设计决策（含已被取代的决策，保留历史）|
| [`docs/nav-backend.md`](docs/nav-backend.md) | 导航数据源/网关：**引用归属、部署、维护、合规风险**（阶段 3）|
| [`docs/hardware.md`](docs/hardware.md) | 硬件规格与引脚 |
| [`docs/setup-macos.md`](docs/setup-macos.md) | 开发环境搭建 |
| [`docs/ui-guidelines.md`](docs/ui-guidelines.md) | UI 规范与遗留设计选项 |
| [`AI_CONTEXT.md`](AI_CONTEXT.md) | AI 协作上下文（协作规则、交付要求）|
| [`AI_TASK_IPHONE_BLE_DASHBOARD.md`](AI_TASK_IPHONE_BLE_DASHBOARD.md) | 改造任务书（需求与验收标准）|

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
| `ui_analog/clock/speed/color_clock/nav.c` | 五个表盘的 LVGL 构建 | UI 像素 33 项 |

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
| **五表盘** | 模拟时钟 / 数字时钟 / 速度表 / 多彩时钟 / **导航卡片**，滑动切换 + BLE 远程切换 |
| **导航卡片**（阶段1）| 转向箭头（8 种，lv_line 绘制）+ 距离（m/km 自适应）+ 车速 + 状态；**过期/无数据时不显示旧指引** |
| **五态链路显示** | BT OFF / WAIT GPS / NO FIX / STALE / LIVE |
| **速度指针** | 烘焙表盘 + 橙色指针，EMA 平滑，失效立即归零 |
| **驾驶员插画** | 三态表情（平静/皱眉/咬牙），按 90/120 km/h 阈值切换 |
| **航向 + 行程 HUD** | 底部显示「NE 45° · 12.3 km · MAX 87」，位于指针扫不到的扇区 |
| **设备控制** | 亮度（0-255）、息屏/亮屏、**屏幕方向 0/90/180/270°**（NVS 持久化）|
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
| 图片改 8bpp 索引色（**未充气**）| 168 ms ⚠️ 退化 |
| **图片索引色 + 启动充气到 PSRAM** | **110 ms** ✅ |

> 测量口径：连续切页 36s，取 6~8 个心跳窗口的渲染峰值（各窗口几乎一致，非噪声）。

**优化手段**：
1. **无效化风暴修复**：LVGL setter 无变更检测 → 空闲也每秒 55 次重绘。
   加变更检测后降到 0–2 次/秒。
2. **QSPI 调优**：分块 1024→4096 像素 + 时钟 40→80MHz，全屏传输 56→43ms。
3. **静态表盘烘焙成图片**：刻度/数字离线渲染（4× 超采样），运行时只留指针。
4. **大号字体去抗锯齿**：138px 字体 bpp 4→1，字模 194→51KB。
5. **PSRAM 全帧绘制缓冲**：脏区由 10 块降为 1 块。
6. **烘焙图改 8bpp 索引色 + 启动充气**：图片以索引色存 Flash（省 683 KB），
   启动时解码一次到 PSRAM 变 RGB565 直通图。若不充气，索引图每帧都要逐像素
   查调色板 → 滑页退化到 168 ms；充气后反而比原 RGB565 **快 13 ms**（110 ms），
   因为绘制源从 Flash 变成了 PSRAM。

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

> **完整测试台账见 [`docs/test-results.md`](docs/test-results.md)**——
> 含 §9 矩阵 12 项、DoD 8 项、复测指引（避免重复测试）。

| 套件 | 数量 | 说明 |
|------|------|------|
| `test_telemetry` | 115 | 解析/换算/平滑/合理性/状态机/行程/**斜坡**/**防抖** |
| `test_control` | 41 | 控制命令解析（含屏幕方向校验）|
| `test_ui_face` | 67 | 表盘注册表（含分组/启用掩码）|
| `test_nav` | 63 | 导航卡片纯逻辑（动作/状态/距离格式化）|
| `test_nav_parse` | 72 | 导航消息解析（分流/容错/向前兼容）|
| `test_img_indexed` | 28 | 索引图解码（调色板字节序/16位截断/边界/越界）|
| `test_ios_logic` | 32 | iOS 侧纯逻辑（Swift）|
| **单测小计** | **418** | `cd tests && make run` |
| `ui_regression.py` | 33 | UI 像素级回归 |
| XCUITest | 5 | 会话/遥测计数/设备控制/链路诊断/BLE 状态 |
| **自动化总计** | **456** | |

**真机验证**（iPhone 12 / iOS 27）：§9 矩阵 **12/12 通过**，DoD **8/8 达成**。

---

## 九、待办（Backlog）

> 任务书 §9 矩阵 **12/12** 与 DoD **8/8** 均已完成（见
> [`docs/test-results.md`](docs/test-results.md)），以下为**任务书之外的**
> 可选开发项。**每项动手前先读「前置调研」列**，避免重复踩坑。

| 优先级 | 事项 | 前置调研 / 已确认前提 | 预估工作量 |
|--------|------|---------------------|-----------|
| ~~高~~ | ~~竖屏 UI（重排布局）~~ | ✅ **已由「屏幕旋转」解决，无需重排界面**：LVGL 软件旋转官方支持，实测 90°/270° 仅 **+20ms/整屏帧**；圆屏旋转无布局副作用。App/串口/BLE 均可切换并持久化（NVS）| 已完成 |
| 中 | 功能②：图片背景 + 时钟 | ✅ 存储已验证：**有 TF/microSD 卡槽**（SDMMC 1-bit）+ **9.9MB 未用 ffat 分区**（`0x610000`）| 中 |
| 中 | 偶发单帧 1000ms+ 尖刺 | 32 分钟长稳**未复现**；需更长监测或加长耗时打点 | 小（调研为主）|
| **高** | **旋转后撕裂 / 果冻效应**：现场判定 + 修复路线 | 源码核对 + 模拟器实测已完成：撕裂根因是 **QSPI 写 GRAM 与面板扫描无同步**（整屏 434KB/43ms ≈ 2.6 个场周期），软件旋转再把一次刷新拆成多次 flush（数字时钟实测 0°=1 次 → 90°≈11 次）。**执行清单见 [`docs/rotation-tearing-fieldtest.md`](docs/rotation-tearing-fieldtest.md)**（含 40MHz A/B、`madctl` 硬件旋转验证、TE 调查）| 中 |
| 中 | flick 手势的旋转坐标补偿（90°/270° 下滑动失效、翻页退化成慢拖拽）| 补丁已备好且映射已逐点验证（与 `lv_indev.c` 公式 **9676 点全部一致**）；**仓库内尚未应用**（整体编译待并行改动落地后补跑）。见清单 §3 | 小 |
| **高** | 面板 MADCTL 硬件旋转（`madctl 60/C0/A0`）| **仍未实测**：库从不使用 MV(0x20) 位。⚠️ 取值与微雪 IDF BSP `bsp_display_rotation_set()` 完全一致，但该函数在厂商仓库内**无任何调用者** → 厂商同样未验证。若有效可去掉软件旋转的每帧开销与拆块（**必须同步改触摸映射**）| 小（实验）|
| 低 | UI 设计定稿：状态标签与指针重叠 | 5 个候选方案见 [`ui-guidelines.md`](docs/ui-guidelines.md) | 小 |
| 中 | 导航**阶段 3**：iPhone 真实导航数据 | ✅ 数据源已定（**高德 + 自建网关**，见 [nav-backend.md](docs/nav-backend.md)）；导航状态机放手机端，ESP32 只做显示。⚠️ **含合规风险**：高德 §3.4 禁止未获许可用于车载场景，本项目先做技术验证、不公开分发/不商业化 | 大 |
| 低 | 行程统计上屏 | 数据已有（`trip` 串口命令可查），仅需 UI 呈现 | 小 |

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
| `lvrot [0\|1\|2\|3]` | 【运行时】LVGL 软件旋转 0/90/180/270°（不带参数=查询）|
| `madctl <hex>` | 【实验】直写面板 MADCTL(0x36)：`60`=90° `C0`=180° `A0`=270°，恢复 `madctl 0` |
| `flickrot [0\|1]` | flick 方向判定的旋转补偿 A/B（**补丁应用后**可用；0=旧行为 1=修正）|
| `scan` | I2C 扫描 |
| `help` | 全部命令 |
