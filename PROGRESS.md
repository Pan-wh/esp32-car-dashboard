# 项目进度总览

> 最后更新：2026-10-14
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
| [`docs/rotation-hardware-vs-software.md`](docs/rotation-hardware-vs-software.md) | **旋转结论**：硬件/软件方案的真机数据、最终取舍与禁止重复试验项 |
| [`docs/architecture.md`](docs/architecture.md) | 系统架构、模块划分、共享代码层 |
| [`docs/ble.md`](docs/ble.md) | BLE 协议细节（GATT、JSON 字段）|
| [`docs/decisions.md`](docs/decisions.md) | 设计决策（含已被取代的决策，保留历史）|
| [`docs/nav-backend.md`](docs/nav-backend.md) | 导航数据源/网关：**引用归属、部署、维护、合规**（阶段 3）|
| [`docs/nav-app-ux.md`](docs/nav-app-ux.md) | App 导航页的产品视角复盘与四状态机重设计（**P0+P1+P2 已实施**）|
| [`gateway/DEPLOY.md`](gateway/DEPLOY.md) | **网关部署与运维**（自包含）：交给 AI / 运维即可部署、升级、验收、排障 |
| [`gateway/README.md`](gateway/README.md) | 导航网关（已实现）：接口字段、环境变量、真实响应核实结果 |
| [`docs/ble.md`](docs/ble.md) | BLE 协议：**表盘列表上报**、face_enabled 命令、MTU 与 Read/Notify 约定 |
| [`tools/nav_sim/README.md`](tools/nav_sim/README.md) | **Mac 侧「模拟手机」**：不需 iPhone 即可驱动真机验证全链路 |
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
| `telemetry.c` | JSON 解析、速度换算、平滑、合理性校验、行程统计 | **107** 项 |
| `control.c` | 设备控制命令解析（亮度/息屏/表盘/方向/演示速度）| **58** 项 |
| `ui_face.c` | 表盘注册表 | **67** 项 |
| `ui_analog/clock/speed/color_clock/nav.c` | 五个表盘的 LVGL 构建 | UI 像素 **33** 项 |

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
| **速度指针** | 烘焙表盘 + 橙色指针，按时间推进的速度斜坡，失效立即归零 |
| **驾驶员插画** | 三态表情（平静/皱眉/咬牙），按 90/120 km/h 阈值切换 |
| **底部状态子仪表** | 白底圆形区域组合数字速度、单位、状态色点与五态文案，不抢人物和指针焦点 |
| **设备控制** | 亮度（0-255）、息屏/亮屏、**屏幕方向 0/90/180/270°**（NVS 持久化）|
| **演示速度** | App 滑条驱动，走**与真实数据完全相同**的速度斜坡；仪表状态行显示 **DEMO**（琥珀色）以示区别。退出有三重保证：用户关开关 / **点开始驾驶** / 固件侧收到真实遥测或 BLE 断开 |
| **导航开始自动跳转** | 收到"开始导航"的**上升沿**时跳到导航表盘一次；之后手动划走**不会被拽回**（`stale→active` 的恢复也不算）|
| **时间同步** | 首次 BLE UTC → RTC，时钟页显示 SYNCED |
| **行程统计** | 里程/最高速/加权均速，**仅 LIVE 且速度有效时累积** |

### App 端

| 功能 | 说明 |
|------|------|
| 驾驶会话 | 开始/结束，2Hz 发送 |
| 仪表控制 | 表盘选择、亮度滑块、息屏开关、**模拟速度测试（演示）** |
| 实时数据 | 速度/航向/精度/坐标 + **诊断字段**（速度质量、测速精度）|
| **导航** | 四状态机（准备 → 选地点 → 选路线 → 导航中）：输入提示、多方案选路（含 ETA）、周边搜索、当前道路/路况、导航中换路线 |
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
| 套件 | 项数 | 覆盖 |
|------|------|------|
| `test_telemetry` (C) | 107 | 解析/换算/平滑/合理性/状态机/行程/**斜坡**/**防抖** |
| `test_control` (C) | 58 | 控制命令解析（亮度/息屏/表盘/方向/**演示速度**）|
| `test_ui_face` (C) | 67 | 表盘注册表（含分组/启用掩码）|
| `test_nav` (C) | 76 | 导航卡片纯逻辑（动作/状态/距离格式化/**自动跳转的上升沿判定**）|
| `test_nav_parse` (C) | 92 | 导航消息解析（分流/容错/向前兼容）|
| `test_nav_map` (C) | 66 | 地图窗口（道路/建筑/路线绘制与裁剪）|
| `test_img_indexed` (C) | 28 | 索引图解码（调色板字节序/16 位截断/边界/越界）|
| `test_face_report` (C) | 72 | 表盘列表上报（JSON 结构/分组/截断/显示状态 bright/rot/screen）|
| `test_nav_core` (Swift) | 156 | iOS 导航核心（含**偏航重算**）+ 坐标跨语言一致性 |
| `test_nav_sim` (Swift) | 43 | Mac 模拟器（行车推进/法向偏航/重算调度/消息序列）|
| `test_device_faces` (Swift) | 59 | App 表盘解析（分组/本地化/未知兜底/显示状态缺省与 0 的区分）|
| `test_map_codec` (Swift) | 38 | 地图窗口编码（**与网关 JS 公式交叉验证**/消息顺序/字节上限/切段连续性）|
| `test_gateway_contract` (Swift) | 22 | **网关响应契约**：用真实抓取的 JSON 夹具验证字段名（防"凭印象写字段"）|
| `test_ble_policy` (Swift) | 19 | App BLE 自动连接决策（穷举 16 种状态组合）|
| `test_nav_ui_state` (Swift) | 61 | **导航页状态机**（穷举 16 种组合/按钮可见性/ETA 格式化/常用目的地去重）|
| `test_ios_logic` (Swift) | 32 | iOS 侧其它纯逻辑 |
| `gateway` (Node) | 98 | 网关（路线/多方案选路/逆地理/周边/交通/输入提示/骑行，含真实响应结构回归，**全离线**）|
| **单测小计** | **1094** | `cd tests && make run`（各计数为 2026-10-15 实测值）|
| `ui_regression.py` | 33 | UI 像素级回归 |
| XCUITest | 5 | 会话/遥测计数/设备控制/链路诊断/BLE 状态 |
| **自动化总计** | **1132** | 单测 + UI 像素回归 + XCUITest |

**真机验证**（iPhone 12 / iOS 27）：§9 矩阵 **12/12 通过**，DoD **8/8 达成**。

---

## 九、待办（Backlog）

> ✅ **导航页交互重设计（已实施）**：四状态机（准备 → 选地点 → 选路线 → 导航中）|
> —— 修掉了"「开始导航」按钮两种含义"、合并了搜索入口、候选补上 ETA 与人话策略名、
> 网关配置移入设置页、常用/最近目的地、导航与驾驶解耦、导航中可换路线。
> 设计与实施记录：[`docs/nav-app-ux.md`](docs/nav-app-ux.md)（**P0+P1+P2 全做完**）
> 代码：`Nav/NavTabView.swift` + `Nav/NavUIState.swift`（61 项单测）

> 任务书 §9 矩阵 **12/12** 与 DoD **8/8** 均已完成（见
> [`docs/test-results.md`](docs/test-results.md)），以下为**任务书之外的**
> 可选开发项。**每项动手前先读「前置调研」列**，避免重复踩坑。

| 优先级 | 事项 | 前置调研 / 已确认前提 | 预估工作量 |
|--------|------|---------------------|-----------|
| ~~高~~ | ~~竖屏 UI（重排布局）~~ | ✅ **已由「屏幕旋转」解决，无需重排界面**：默认 0°；90°/270° 使用 MADCTL 硬件旋转，接受少量边缘黑缺口以保证动画与滑页性能。App/BLE 可切换并持久化（NVS）| 已完成 |
| 中 | 功能②：图片背景 + 时钟 | ✅ 存储已验证：**有 TF/microSD 卡槽**（SDMMC 1-bit）+ **9.9MB 未用 ffat 分区**（`0x610000`）| 中 |
| 中 | 偶发单帧 1000ms+ 尖刺 | **已大量复现并深挖（未定位根因）**：实测约每 1~2 分钟一次、每次约 1000~1035ms。**已排除**：内存泄漏、串口打印阻塞、QSPI 写入慢、单次调用内反复重绘（flush 次数与正常窗相同）、NVS 垃圾回收、中断被关闭（1kHz 诊断：停滞时**中断间隔最大 0us**）。**已确证**：停滞 100% 落在 `lv_timer_handler()` 内（分 8 段计时其余全 0），时间花在**把图形画进缓冲**，且与 BLE 数据强相关；提高 loop 任务优先级会**恶化 7 倍**（优先级反转特征）。诊断手段见 [`docs/perf/README.md`](docs/perf/README.md) 与 `dashboard.ino` 的 `nvs`/`bufmode`/`tp`/`verbose` 命令 | 中（需继续调研）|
| ~~高~~ | ~~旋转后撕裂 / 果冻效应~~ | ✅ 真机测试完成：软件转置显示完整但明显卡顿；最终采用厂商 MADCTL 硬件旋转并接受边缘黑缺口。完整结论见 [`docs/rotation-hardware-vs-software.md`](docs/rotation-hardware-vs-software.md) | 已完成 |
| ~~中~~ | ~~flick 手势的旋转坐标补偿~~ | ✅ 已在触摸采样层按当前方向转换坐标，270° 真机验证跟手 | 已完成 |
| ~~高~~ | ~~面板 MADCTL 硬件旋转~~ | ✅ 已真机验证并进入正式路径：90°=`0xA0`、270°=`0x60`，固定原生 gap `(6,0)` | 已完成 |
| ~~中~~ | ~~导航**阶段 3**~~ | ✅ **全部完成**：网关（真实 Key 联调）、导航核心（含偏航重算，156 项测试）、Mac 模拟器全链路、以及 **App 接入**（2026-10-14：定位→核心→BLE 下发仪表，含中文地名目的地）。详见 [nav-backend.md](docs/nav-backend.md) | 已完成 |
| 中 | **路显（路名显示）** | 协议需加 `road` 字段（UTF-8 中文，注意 MTU）；固件需**中文字模**（现仅拉丁/数字）；App 侧 `NavSnapshot.roadName` 已就绪。⚠️ 字模体积是主要风险（Flash 已 60%），可考虑子集化或放 SD 卡 | 中 |
| ~~中~~ | ~~导航**地图界面**（参考项目那种）~~ | ✅ **代码完成**（2026-10-14）：网关 `/mapwindow`（首查 1.68s/缓存 7ms/窗口 2.2KB）+ 协议（mw/mr/mb/mp/me + ox/oy）+ App 下发 + 固件渲染层（航向朝上、灰路/灰楼/白路线/**带深色描边的固定箭头**）。**模拟器 + 真机均已验证**（2026-10-15 修复了四个叠加的 BLE/解析缺陷后，设备端 `提交瞬间 build=24/16/70 → active=24/16/70`）。详见 [nav-backend.md](docs/nav-backend.md) §5.3 | 已完成 |
| 中 | **离线资源放 SD 卡** | ✅ **卡已实测可用**（原生 1-bit SDMMC，`sd` 命令可查；实测 SDHC 7.3GB/可用 5.74GB）。可放：中文字模、图片素材、离线地图瓦片。⚠️ **高德协议 §3.5 禁止缓存其数据**，离线地图须换允许离线的数据源（如 OSM）；且拔卡必须能优雅降级 | 中 |
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

**当前占用**：Flash **60%**（1,901,259 B / 3 MB，2026-10-15 实测；同日加了演示速度与自动跳转），RAM 39%。

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
| `trip [reset]` | 行程统计查询/清零 |
| `bright <0-255>` | 亮度 |
| `screen on\|off` | 亮屏/息屏 |
| `page N` / `pages` | 切页 |
| `sd [list]` | TF 卡状态 / 列根目录 |
| `mem` | 内存情况 |
| `dump` | 打印元素树 |
| `lvrot [0\|1\|2\|3]` | 临时切换正式旋转策略 0/90/180/270°（不写入 NVS）|
| `scan` | I2C 扫描 |
| `demo [kmh\|off]` | 演示速度（App「模拟速度测试」滑块的串口入口）；状态行显示 `DEMO`；收到真实遥测或断连自动退出 |
| `map` | 地图窗口诊断（收到多少 / 能否画 / 自车偏移 / 消息顺序）|
| `nav [st mv dist [spd]]` | 导航卡片模拟数据（走与 BLE 相同的状态应用路径，含自动跳转判定）|
| `help` | 全部命令 |
