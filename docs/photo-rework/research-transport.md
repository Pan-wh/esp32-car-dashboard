# 传输通道调研：照片上传到底走哪条路（只读调研，2026-10-26）

> **范围**：iPhone(App) ↔ ESP32-S3 的 bulk 上传通道。只读代码 + 联网查资料，**不改任何代码**。
> **配套**：固件侧事实见 [`research-firmware.md`](research-firmware.md)，iOS 侧事实见 [`research-ios.md`](research-ios.md)。
>
> **来源标记**（全文通用）：
> - **【官方】** Espressif / Apple 官方文档、Kconfig 源码、官方仓库 Issue。
> - **【实测】** 第三方实测（博客 / GitHub Issue / 论坛）与**本项目真机实测**。
> - **【推断】** 我基于官方语义 + 本项目数据做的工程估算，**必须靠 §6 的 Spike 验证**，不得当结论用。
> - **【仓库】** 仓库内事实，带 `path:line` 或 `docs/xxx.md §n`。

---

## 0. 结论先行

| # | 问题 | 结论 | 置信度 |
|---|------|------|--------|
| 1 | BLE bulk 能到多快（iPhone→ESP32） | **保守 15–20 KB/s，现实 25–35 KB/s，乐观 40–50 KB/s**。不存在"BLE 只能 1 KB/s"——历史那个数字是**停机等待（stop-and-wait）协议**造成的，不是空口上限 | 高（3 份独立实测 + 理论核算） |
| 2 | iOS 能不能靠更大 MTU 提速 | **不能**。iOS 的 ATT MTU 上限 **185**（有效载荷 182）。ESP32 请求 247 会被 iOS 回成 185；**没有任何办法在 iOS 上超过 185**。而且 MTU 本来就不是瓶颈（瓶颈是"每连接事件能塞几个包"） | 高 |
| 3 | BLE L2CAP CoC 值不值得 | **不值得**。iOS 11+ 确实有 `CBL2CAPChannel` API，但 iPhone↔iPhone 实测只有 **28 KB/s**（不优于 GATT）；ESP32 侧要开 CoC 得重建预编译 NimBLE 库（当前 `COC_MAX_NUM=0`），且 Arduino BLE 封装层没有 CoC 接口 | 高 |
| 4 | Wi-Fi 还有没有救 | **有救，但主要不是靠 `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP`**——那条路要**重建 IDF 预编译库**，且 S3 的 Wi-Fi DMA 访问 PSRAM 有官方明确的带宽限制。**先走"应用侧把 90 KB 内部 RAM 搬 PSRAM"**（LVGL 堆 48 KB + 一块**已废弃的** 42.4 KB 内部绘制缓冲），半天可见效果 | 中高（官方语义确定；省多少需 Spike S4） |
| 5 | 推荐主选 | **BLE bulk（GATT 分片）+ 静态照片改传 JPEG**（`esp_jpeg` 已预编译在 Arduino 核心里，零安装）。动图暂不做无线，继续走 SD 卡 | 中高 |
| 6 | 必须先验的 Spike | **S1 BLE 吞吐探针**（通过线 ≥20 KB/s、0 缺口）、**S2 上传时 UI 帧率**、**S3 JPEG 端到端**、**S4 Wi-Fi 内存救援**、**S5（可选）sdkconfig 路线** | — |

**一句话**：BLE 传静态照片是**可行且当天能验**的；Wi-Fi 是"能救但要多做一次内存手术"，动图（2.1 MB）在任何无线通道上都不轻松，优先考虑**减体积**而不是加带宽。

---

## 1. 先把本项目的现状钉死（仓库内事实）

### 1.1 板子与构建

| 项 | 事实 | 来源 |
|----|------|------|
| SoC | ESP32-S3R8：双核 240 MHz、**8 MB 八线(OPI) PSRAM**、16 MB Flash | 【仓库】`docs/hardware.md:20-27`、`:37` |
| SD | 原生 SDMMC，BSP 以 **1-bit** 挂载 | 【仓库】`docs/hardware.md:44` |
| 构建体系 | **arduino-esp32 core 3.3.10** + **预编译 IDF 库**（`tools/esp32s3-libs/3.3.10`），`PSRAM=opi` | 【仓库】`firmware/dashboard/build/esp32.esp32.esp32s3/build.options.json` |
| BLE 协议栈 | **NimBLE**（不是 Bluedroid）：`CONFIG_BT_NIMBLE_ENABLED=1` | 【仓库】`…/esp32s3-libs/3.3.10/qio_opi/include/sdkconfig.h`；交叉印证 `research-firmware.md §1.1` |
| PSRAM 速度 | `CONFIG_SPIRAM_SPEED_80M=1`（已是 80 MHz）；**不要上 120 MHz**——官方原文：八线 120 MHz 是实验特性，温度变化约 20 ℃ 就会随机崩 | 【官方】[`Kconfig.spiram`(S3)](https://github.com/espressif/esp-idf/blob/v5.5/components/esp_psram/esp32s3/Kconfig.spiram)（`SPIRAM_SPEED_120M` help） |

> **这条 80 MHz/120 MHz 的官方警告对车载场景特别重要**：车内温差远大于 20 ℃，120 MHz 那条"提速"选项不能碰。

### 1.2 现有 BLE 通道的真实用法（决定"能不能顺便传图"）

| 项 | 现状 | 来源 |
|----|------|------|
| 本地 MTU 请求 | `BLEDevice::setMTU(247)` | 【仓库】`firmware/dashboard/ble_telemetry.cpp:316` |
| 实际协商 | iOS 侧约 **185**（有效载荷约 184） | 【仓库】`docs/ble.md:409-411`、`ble_telemetry.cpp:316` 注释 |
| 上行（设备→App） | Read 取全量 + Notify 只当"变了"的提示 | 【仓库】`docs/ble.md:669-686` |
| 下行（App→设备） | 遥测 **2 Hz**（`speed_mps` 等）；命令/亮度/地图窗口 | 【仓库】`docs/ble.md:254`、`docs/ble.md:212` |
| 封包方式 | 固件侧有"JSON 重组"（累积到 `{...}` 再解析），**没有分片层** | 【仓库】`docs/ble.md:212`、`research-firmware.md §1.4` |
| 线程约定 | 写回调里只解析+暂存，**LVGL/写盘一律回主循环** | 【仓库】`ble_telemetry.h:12-13`、`ble_telemetry.cpp:55-60`、`:103-106` |

**关键**：现有通道是"小包 + 幂等 + 可丢"的设计，**不能直接用来传图**（一次丢包=文件坏，没有序号也没有 CRC）。bulk 必须新开通道（新特征或新服务）。

### 1.3 "BLE 实测约 1 KB/s"的真实成因（这是本次调研最重要的纠错）

原结论写在 `docs/photo-gif-design.md:99`：*"BLE 实测有效吞吐约 1 KB/s（依据：地图窗口 50 条消息 2~3 秒、每条 ≤180 B）"*。

把代码翻出来看，那个测量场景是**停机等待协议**，不是吞吐测试：

| 环节 | 代码事实 | 来源 |
|------|---------|------|
| 地图窗口按条发，**发一条等一条** | `didWriteValueFor` 里才 `pumpMapQueue()` 发下一条 | 【仓库】`ios/.../BLEClient.swift:882-914` |
| 每条用 `.withResponse`（要等确认） | `peripheral.writeValue(pending, for: ch, type: .withResponse)` | 【仓库】`BLEClient.swift:879` |
| 于是每条至少花 1 个连接间隔的往返 | 50 条 × 约 40–60 ms ≈ 2–3 s | 【推断】由上面两条推出 |

核算：50 × 180 B = 9,000 B / 2.5 s ≈ **3.6 KB/s**（不是 1 KB/s；文档取整成 1 KB/s 属于保守估计，**低估了约一个数量级**）。而即便是 12 KB/s 的"理论停机等待上限"（182 B / 15 ms），实测也达不到——因为每条都要等下一次连接事件。

> **结论**：`1 KB/s` = **协议往返次数**的度量，**与空口能力无关**。用它来否定 BLE 传图是错误推论。

### 1.4 数据量与 SD 速度（算时间要用）

| 项 | 数字 | 来源 |
|----|------|------|
| 一帧像素 | 466 × 466 = **217,156** | 【仓库】`docs/photo-gif-design.md:89` |
| 静态 `.pgi`（8bpp 索引 + 1 KB 调色板） | **218,180 B ≈ 213 KiB（212 KB）** | 【仓库】`docs/photo-gif-design.md:89`、`:94`、`pgi.h:20-40` |
| 10 帧动图 | 2,181,800 B ≈ **2.18 MB**（文档记 2.1 MB） | 【仓库】`docs/photo-gif-design.md §3.2` |
| SD **写**速（1-bit SDMMC） | **0.16 MB/s** → 写 213 KiB 约 1.36 s | 【仓库】`docs/photo-gif-design.md:410`（转引 `research-firmware.md §4.2`） |
| SD **读**速 | 217 KB/帧 ≈ **104 ms**（≈2.1 MB/s，物理上限；`sdbench` 测 2.37 MB/s） | 【仓库】`PROGRESS.md:223`、`docs/photo-gif-design.md:409` |
| 全屏 flush | **43 ms**（QSPI 80 MHz，约 10 MB/s 有效） | 【仓库】`docs/troubleshooting.md:356`、`:397` |
| 动图实测帧率 | **3~4 fps**（瓶颈是 SD 读） | 【仓库】`PROGRESS.md`（照片表盘最终状态） |

### 1.5 内存账本（Wi-Fi 那一节的前提）

| 场景 | 内部堆 | 来源 |
|------|--------|------|
| 不开 Wi-Fi | 约 **104 KB** | 【仓库】`docs/wifi-upload-postmortem.md:87` |
| 开热点（无客户端） | **22~24 KB** | 【仓库】`docs/wifi-upload-postmortem.md:88` |
| 手机连上热点 | **2~4 KB** | 【仓库】`docs/wifi-upload-postmortem.md:89` |
| **当前固件实际运行状态** | **≈22~24 KB —— 因为 `setup()` 里无条件 `wifi_start_ap()`** | 【仓库】`firmware/dashboard/dashboard.ino:3368`、`:3345-3368` |

两块**常驻内部 RAM 的大额占用**（本次新查出来的，postmortem 没记）：

| 占用 | 大小 | 说明 | 来源 |
|------|------|------|------|
| LVGL 堆（`LV_MEM_CUSTOM 0`，静态数组） | **48 KB**（`.bss.work_mem_int$0 = 0xC000`） | 内部 RAM，编译期固定 | 【仓库】`reference/ESP32-S3-Touch-AMOLED-1.75/examples/arduino/libraries/lv_conf.h:49-52`；`…/build/…/dashboard.ino.map` 实测 49,152 B（该 map 是 2026-10-14 的构建产物，**早于当前源码的 Wi-Fi 代码**；但这两个数字是编译期常量，当前源码仍是同样配置） |
| 内部 1/10 屏绘制缓冲 `buf[]` | **42.4 KB**（43,432 B） | **已被 PSRAM 全帧缓冲取代，现在是纯 fallback**：PSRAM 分配成功就 `s_buf_psram_active=true`，`buf` 再没人用 | 【仓库】`dashboard.ino:1487`、`:2505`、`:2549-2562`、`:1504-1510` |

> 也就是说：**约 90 KB 的内部 RAM 属于"应用自己"，而不是 IDF/Wi-Fi**。postmortem 里"PSRAM 完全帮不上忙"这句话，只对 Wi-Fi 协议栈内部的分配成立，**对应用侧不成立**——这是 §3 的突破口。

---

## 2. 问题 1：BLE bulk 的真实吞吐上限（iPhone central → ESP32 peripheral）

### 2.1 MTU：为什么是 185，能不能更大

| 项 | 数值 | 依据 |
|----|------|------|
| iOS 侧 ATT MTU 上限 | **185** | 【实测】[Memfault《A Practical Guide to BLE Throughput》](https://interrupt.memfault.com/blog/ble-throughput-primer)（"For iOS 10 and above, most iOS devices support a 185 byte MTU"、表中"185 (iOS 10 max)"）；【实测】本项目 `docs/ble.md:409` |
| 单次写/通知有效载荷 | **182 B**（MTU − 3） | 【官方】ATT 规范；【实测】Memfault 同上 |
| ESP32 请求 247 → 实际 | **185**（取双方较小值，iOS 不会接受 247） | 【官方】ATT MTU 协商规则；【实测】本项目实测 185 |
| 能否超过 185 | **不能**。CoreBluetooth 不提供设置 MTU 的 API，MTU 完全由系统决定 | 【推断】由 Apple 未开放该 API + 多份实测一致推出；Apple 官方指南见下 |
| "iOS MTU = 517" 的说法 | **不要采信/不要按它设计**。中文社区有此类文章，但无可复现出处；多份实测与本项目真机均为 185 | 【推断】证据不足 |

> **Apple 官方口径**：连接参数（间隔/延迟/超时）区间写在 Apple《Accessory Design Guidelines》**§25.6 Connection Parameters**，[PDF 原文](https://developer.apple.com/accessories/Accessory-Design-Guidelines.pdf)。该 PDF 有反爬，本文引用的是 **Memfault 对 §25.6 的转录 + 本项目实测**（见 §9 抓取限制）。

**推论（重要）**：既然 MTU 卡在 185，**"提高 MTU 换吞吐"这条路在 iOS 上彻底关闭**；能动的只有"每个连接事件塞几个 182 B 包"和"连接间隔"。

### 2.2 连接事件：间隔与每事件包数

| 参数 | iOS 侧现实值 | 依据 |
|------|-------------|------|
| 最小连接间隔（普通 App） | **15 ms**（低于 15 ms 只有 HID 场景、约 11.25 ms 且不是所有设备接受） | 【实测】[Punch Through《Maximizing BLE Throughput Part 4》](https://punchthrough.com/ble-throughput-part-4/)："iOS devices can go down to 15ms"；【实测】Memfault 同上；【官方】Apple 指南 §25.6（经 Memfault 转录） |
| 每个连接事件的包数 | **约 4~7 个**（不同 iOS 版本/机型不同） | 【实测】Punch Through 2016：iOS **4** 包/事件；Memfault 2019：**7** 包/事件（iPhone SE 实测 12.2 kB/s） |
| 单包 LL 载荷 | 251 B（BLE 4.2 DLE 开启后；iOS 默认开 DLE）→ 182 B 的 ATT 写**一个空口包就能装下** | 【官方】BLE 4.2 DLE；【实测】Memfault |

### 2.3 理论吞吐表（MTU 185 → 182 B/包，仅供做上界）

| 每事件包数 \ 间隔 | 15 ms | 30 ms |
|---|---|---|
| 1 | 12.1 KB/s | 6.1 KB/s |
| 2 | 24.3 KB/s | 12.1 KB/s |
| 4 | 48.5 KB/s | 24.3 KB/s |
| 6 | 72.8 KB/s | 36.4 KB/s |
| 7 | **84.9 KB/s** | 42.4 KB/s |

计算：`包数 × 182 B / 间隔`。【推断】按此公式推算；对照 Memfault 的 iPhone SE 实测 12.2 kB/s（1M PHY、无 DLE、7 包/事件），说明**现实通常只有理论上限的 15%~40%**。

### 2.4 实测与区间（关键表）

| 场景 | 数字 | 类型 | 来源 |
|------|------|------|------|
| iPhone SE，1M PHY 无 DLE，7 包/15 ms | **12.2 kB/s** | 实测 | [Memfault](https://interrupt.memfault.com/blog/ble-throughput-primer) |
| iPhone ↔ ESP32，GATT 分片传文件 | **32 kB/s** | 实测 | [NimBLE-Arduino #280 评论](https://github.com/h2zero/NimBLE-Arduino/issues/280)（2021，ClaesClaes） |
| 其他 NimBLE-Arduino 用户（DLE 优化后） | **>35 kB/s** | 实测 | 同 Issue（作者 h2zero 回复，指向 PR #283） |
| 新机型 + DLE + 1M PHY 的经验上限 | **约 50 KB/s** | 实测（厂商自测） | [Punch Through Part 4](https://punchthrough.com/ble-throughput-part-4/)："约 50 KB/s 在较新的 Android/iOS 上可达（DLE、1M PHY）" |
| 2M PHY 的理论增益 | 无 DLE **+33%**，有 DLE **最高约 +90%**（设备对设备） | 实测/推算 | 同上（引 Nordic S140 数据） |
| ESP32↔ESP32（非手机），2M PHY + DLE + 加大间隔 | **≈1.4 Mbps（175 KB/s）** | 实测 | [ESP-IDF Issue #15660](https://github.com/espressif/esp-idf/issues/15660)（NimBLE throughput demo，conn_itvl=100） |
| 本项目地图窗口（停机等待，非 bulk） | ≈3.6 KB/s（文档记 1 KB/s） | 本项目 | §1.3 |

**判断区间**（iPhone→ESP32，writeWith**out**Response + 序号/流控，15 ms，2M PHY 尽力）：

| 区间 | KB/s | 依据 |
|------|------|------|
| 保守（不调 PHY、每事件 2 包） | **15–20** | 由 2.3 表下限 × 现实折损 |
| 现实（DLE+2M+flow control 正常） | **25–35** | 32 / >35 KB/s 两份实测 |
| 乐观（15 ms、7 包/事件、环境干净） | **40–50** | Punch Through 经验上限 |

### 2.5 2M PHY 与 DLE 在**本项目**怎么开

| 项 | 现状 | 说明 |
|----|------|------|
| 2M PHY | **未开**，且 Arduino BLE 封装层**没有 API** | `BLEDevice` 只有 `setMTU()`（`…/libraries/BLE/src/BLEDevice.cpp:652`）→ 必须直接调 NimBLE C API：`ble_gap_set_prefered_default_le_phy(BLE_HCI_LE_PHY_2M_PREF_MASK, …)`（用法见 [Issue #15660 评论](https://github.com/espressif/esp-idf/issues/15660)） |
| DLE | 控制器默认支持；NimBLE-Arduino 有 `setDataLen`（[PR #283](https://github.com/h2zero/NimBLE-Arduino/pull/283)），**arduino 内置 BLE 库没有** | 需要自写或移植 |
| iPhone 侧 2M | iPhone 8/X 及以后会自动切 2M（Peripheral 提请后） | 【实测】Apple 开发者论坛帖标题即"Automatic switch to 2MPHY when using iPhone X/8"（[thread/98372](https://developer.apple.com/forums/thread/98372)，正文需登录） |

> **注意**：2M PHY 在"每事件包数已被 iPhone 限死"的场景里收益有限（Punch Through 也强调 2M 的理论增益在手机侧会被 OS 调度吃掉）。**先把"不用 response + 流控"做对，比折腾 PHY 更值钱**。

### 2.6 NimBLE（本项目实际用的栈）的配置限制

| 配置项 | 当前值 | 要不要改 | 说明 |
|--------|--------|---------|------|
| `CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU` | **256**（代码再设 247） | 保持 | 对 iOS 无效；对 Android/桌面上限有用 |
| `CONFIG_BT_NIMBLE_ACL_BUF_COUNT` / `ACL_BUF_SIZE` | **24 / 255**（约 6 KB 内部 RAM） | 可加到 48（+6 KB）换突发吸收能力 | 【仓库】`…/qio_opi/include/sdkconfig.h` |
| `CONFIG_BT_NIMBLE_L2CAP_COC_MAX_NUM` | **0**（CoC 被编译掉） | **不改**（见 2.7） | 同上 |
| `CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_*` | **INTERNAL** | 不改 | NimBLE 内存池只能用内部 RAM（`research-firmware.md §1.5`） |
| `CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE` | 5120 | 不改 | — |
| NimBLE 日志 | 默认有 | **上传时关到 No logs** | 【实测】[Issue #15660](https://github.com/espressif/esp-idf/issues/15660)：仅"关日志"一项就把吞吐从 ~500 kbps 提到 ~794 kbps |

> **NimBLE 不是瓶颈**：同一份 Issue 里，设备对设备用 NimBLE 能跑到 1.4 Mbps。**手机侧才是瓶颈。**

### 2.7 L2CAP CoC（Connection Oriented Channel）：能用吗？值不值？

| 问题 | 答案 | 依据 |
|------|------|------|
| iOS 有 CoC API 吗 | **有，iOS 11+ 的 `CBL2CAPChannel`** | 【官方】[Apple Developer: CBL2CAPChannel](https://developer.apple.com/documentation/corebluetooth/cbl2capchannel)；【实测】Memfault 脚注："Apple exposed APIs for L2CAP Channels in iOS 11" |
| iPhone↔ESP32 用 CoC 能更快吗 | **不能**。iPhone↔iPhone CoC 实测 **28 KB/s**（1024 KB / 36 s），比 GATT 的 32 KB/s 还低 | 【实测】[NimBLE-Arduino #280](https://github.com/h2zero/NimBLE-Arduino/issues/280) 评论 |
| ESP32 侧开 CoC 要什么 | NimBLE 且 `CONFIG_BT_NIMBLE_L2CAP_COC_MAX_NUM > 0`；**当前预编译库是 0 → 必须重建库**；且 Arduino BLE 库**没有 CoC 封装**，要手写 `ble_l2cap_*` | 【仓库】`…/qio_opi/include/sdkconfig.h`（`=0`）、`BLEUtils.cpp:79` 只 include 了头；【实测】Issue #15660 作者："ESP 只在 NimBLE 上支持 CoC" |
| 结论 | **不做**。吞吐收益≈0（甚至 -12%），成本是本次所有方案里最高的 | — |

### 2.8 与遥测/Notify 并存：怎么不互相饿死

现状流量很小，几乎可以忽略：遥测 2 Hz × 约 200 B ≈ **0.4 KB/s**（占 30 KB/s 的 ~1.3%），问题不在带宽，而在**回调和主循环的争用**。

设计约束（每条都对应仓库里的既有约定）：

| 约束 | 理由 | 来源 |
|------|------|------|
| bulk 走**独立特征/独立服务**，不要复用 telemetry 特征 | telemetry 用 JSON 重组、幂等语义，混进来会把两种错误模型搅在一起 | 【仓库】`ble_telemetry.cpp:71`、`docs/ble.md:212` |
| 一律 `.withoutResponse` + 应用层序号/窗口，**禁止 withResponse** | withResponse 每包等一个连接事件 → 立刻退化到 §1.3 那个 3.6 KB/s | 【仓库】`BLEClient.swift:879`（反例） |
| iOS 侧必须看 `canSendWriteWithoutResponse` / `peripheralIsReadyToSendWriteWithoutResponse` | 否则 CoreBluetooth 队列满会**静默丢包** | 【官方】CoreBluetooth API；【实测】Memfault "reliability: stacks silently drop data" |
| 固件侧留 **PSRAM 环形缓冲**（建议 64 KB）+ 独立写盘任务，回调只 memcpy | 回调里不能做 LVGL/写盘 | 【仓库】`research-firmware.md §3.3`、`ble_telemetry.h:12-13` |
| 上传期间**关掉 status/faces 的 notify**（或降到 0） | 现有 notify 已有"未连接时跳过"的先例，说明 notify 有成本 | 【仓库】`ble_telemetry.cpp:485-511` |
| 写盘按小片做（每拍写 4–8 KB），不要一次 213 KB | SD 写 0.16 MB/s，一次写 213 KB 会冻结 loop 1.36 s | 【仓库】`docs/photo-gif-design.md:410`、`research-firmware.md §4.2` |
| 上传时把 BLE 任务与 LVGL 分核（或接受 20–30% 渲染降级） | 需要实测（Spike S2） | 【推断】 |

---

## 3. 问题 2：Wi-Fi 还有没有救（PSRAM 配置 + 内部 RAM 手术）

### 3.1 官方配置项到底承诺了什么

| 配置项 | 官方原文（Kconfig help） | 现实含义 |
|--------|--------------------------|----------|
| `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP` | *"Try to allocate memories of WiFi and LWIP in SPIRAM firstly. If failed, try to allocate internal memory then."*；依赖 `SPIRAM_USE_MALLOC`；**default n** | **"尽力先放 PSRAM"，不是"一定放"**，也没有任何"能省多少"的承诺 |
| 开启后连带变化的默认值 | `ESP_WIFI_STATIC_RX_BUFFER_NUM` 默认 10→**16**；`ESP_WIFI_RX_BA_WIN` 6→**16**；TX 必须选 **Static**；出现 `ESP_WIFI_CACHE_TX_BUFFER_NUM=32` | **缓冲数量反而变大**（为了在 PSRAM 里跑得稳），内部 RAM 还留一份 cache TX 队列 |
| `CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY` | *"the region … will be used to store zero-initialized data (BSS segment) from the lwIP, net80211, libpp, wpa_supplicant and bluedroid ESP-IDF libraries."* | 能把 Wi-Fi/协议栈的**静态 BSS 整段**搬 PSRAM——这是"救内部 RAM"最直接的一项 |
| `CONFIG_SPIRAM_USE_MALLOC` | 本项目**已开**（`CONFIG_SPIRAM_USE_MALLOC=1`） | 前提满足 |
| `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` | 本项目 **4096**（IDF 默认 16384） | >4 KB 的 malloc 优先走 PSRAM |
| `CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL` | 本项目 **0**（IDF 默认 **32768**） | **没有任何内部 RAM 预留池**。`MALLOC_CAP_DMA|INTERNAL` 的分配和 Wi-Fi 抢同一块堆 → 这就是"堆到 3.7 KB 就拒绝上传/偶发失败"的结构性原因之一 |

官方出处：`Kconfig.spiram.common` 全文 —— [GitHub: esp-idf v5.5](https://github.com/espressif/esp-idf/blob/v5.5/components/esp_psram/Kconfig.spiram.common)；渲染后的配置项索引：[ESP-IDF Kconfig Reference (ESP32-S3)](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32s3/api-reference/kconfig-reference.html)。

> 三个选项的**性质差别**要讲清楚：`TRY_ALLOCATE` = 把**运行时缓冲**挪走；`BSS_SEG_EXTERNAL` = 把**静态 BSS** 挪走；`RESERVE_INTERNAL` = 不动总量、只把内部堆**切一块保护区**出来。三个都不是免费午餐。

### 3.2 致命前提：**本项目改不了 sdkconfig**

| 事实 | 依据 |
|------|------|
| arduino-esp32 的 IDF 库是**预编译**的；改 `sdkconfig`/`sdkconfig.h` **不生效** | 【官方】[arduino-esp32 FAQ: How to modify an sdkconfig option in Arduino?](https://github.com/espressif/arduino-esp32/blob/master/docs/en/faq.rst)："modifying sdkconfig … does not result in changes … because ESP-IDF libraries are included … as pre-built libraries" |
| arduino-cli 构建时，sdkconfig 是从**库目录**复制进 build 的，不是从 sketch | 【仓库】`~/Library/Arduino15/packages/esp32/hardware/esp32/3.3.10/platform.txt:145-147`（`cp {compiler.sdk.path}/sdkconfig {build.path}/sdkconfig`） |
| 当前预编译库里该选项的状态 | **`# CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP is not set`** | 【仓库】`…/esp32s3-libs/3.3.10/sdkconfig:2066` |
| 想改的两条路 | ① 用 ESP-IDF 编译 Arduino 库（`esp-idf_component` / `lib_builder`）；② 用 [esp32-arduino-libs](https://github.com/espressif/esp32-arduino-libs) 重新构建整套 IDF 静态库 | 【官方】同 FAQ |

> 结论：**§3.1 那些选项在当前构建体系下全部是"看得见、摸不到"**。要碰它们，代价是"重建 IDF 库"或"把上传功能改成 ESP-IDF 组件"——半天到两天，且会引入整套库的回归风险。

### 3.3 那 80 KB 到底花在哪（量级表）

| 消费项 | 量级 | 能否搬 PSRAM | 依据 |
|--------|------|--------------|------|
| Wi-Fi 静态 RX 缓冲（8 × ~1.6 KB） | ≈12.8 KB | 硬件 DMA 用；官方选项里有"优先 PSRAM"路径，但 S3 DMA 访问 PSRAM 有带宽限制 | 【官方】[esp_wifi Kconfig](https://github.com/espressif/esp-idf/blob/v5.5/components/esp_wifi/Kconfig)（每缓冲约 1.6 KB）；本项目 `STATIC_RX_BUFFER_NUM=8` |
| Wi-Fi 静态 TX 缓冲（8 × ~1.6 KB） | ≈12.8 KB | 同上 | 同上（本项目 `ESP_WIFI_TX_BUFFER_TYPE=0` static、`STATIC_TX_BUFFER_NUM=8`） |
| Wi-Fi RX MGMT 缓冲（5 × ~0.5 KB） | ≈2.5 KB | — | 同 Kconfig |
| 动态 RX 缓冲（上限 32 × 帧长） | 最多 ≈51 KB（按需分配/释放） | 可（`TRY_ALLOCATE`） | 同 Kconfig；本项目 `DYNAMIC_RX_BUFFER_NUM=32` |
| lwIP：`TCP_WND_DEFAULT=5760` + `TCP_SND_BUF_DEFAULT=5744` | ≈11.5 KB/连接（pbuf） | 可（`TRY_ALLOCATE`） | 【仓库】`sdkconfig:2601-2602` |
| 任务栈：Wi-Fi + tcpip(4096) + event loop | ≈8–12 KB | 否（栈强制内部） | 【官方】[Minimizing RAM Usage](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32s3/api-guides/performance/ram-usage.html) |
| wpa_supplicant / mbedTLS / netif / DHCP | 数 KB~十几 KB | 部分（BSS 选项） | 【推断】 |
| **参照系**：PSRAM 上 Wi-Fi 缓冲的实测规模 | **54,528 B**（开 PSRAM 后 IDF 的 Wi-Fi TX 缓冲设置占用） | — | 【实测】[MicroPython PR #12968](https://github.com/micropython/micropython/pull/12968)（作者 projectgus） |

合计与 postmortem 的实测吻合（AP 约 −80 KB、客户端再 −19 KB）。**注意：这些数字是"量级"，不是逐项实测**，Spike S5 才给准数。

### 3.4 路线 A（推荐先走）：应用侧把 90 KB 搬 PSRAM —— **不需要重建 IDF 库**

| 动作 | 省回内部 RAM | 代价/风险 | 依据 |
|------|--------------|-----------|------|
| 去掉/改小内部 1/10 屏 fallback `buf[]`（现在 PSRAM 全帧缓冲成功后它就是死代码） | **≈42.4 KB** | 极低（保留一个小的 fallback，如 1/40 屏 ≈10 KB） | 【仓库】`dashboard.ino:1487`、`:2549-2562` |
| LVGL 堆改 `LV_MEM_CUSTOM=1` + `heap_caps_malloc(MALLOC_CAP_SPIRAM)` | **≈48 KB** | 中：① PSRAM 在 flash cache 关闭期间不可访问（NVS 写、flash 操作），LVGL 若在 PSRAM 上画而别的任务写 NVS → 崩；需要"flash 操作时禁止 LVGL"的互斥；② LVGL 访问 PSRAM 有一定性能损失 | 【官方】[Support for External RAM: Restrictions](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32s3/api-guides/external-ram.html)（cache 关闭则 PSRAM 不可访问）；【仓库】`lv_conf.h:49-52` |
| 合计 | **≈90 KB** | —— | —— |

**这笔账的意义**（按 postmortem 的实测口径外推，【推断】）：

| 场景 | 现在 | 做路线 A 之后（+90 KB） |
|------|------|------------------------|
| 不开 Wi-Fi | ~104 KB | **~194 KB** |
| 热点常开、无客户端 | 22~24 KB | **~112 KB** |
| 手机连上热点 | **2~4 KB** | **~93~95 KB** |

也就是说：**从"3.7 KB 就拒绝上传"变成"能起 8 KB 收包缓冲 + 写盘缓冲 + UDP/TFTP"**。这才是"Wi-Fi 有救"的具体含义（是否真到这数，由 Spike S4 定）。

**而且这条路的成本远低于 §3.5**：`lv_conf.h` 属于 sketch 的 `--libraries` 目录（源码编译），改它只需重新编译 sketch，**不涉及 IDF 库**。

### 3.5 路线 B（重）：重建 IDF 库 + sdkconfig

| 动作 | 预期收益 | 风险 |
|------|---------|------|
| `TRY_ALLOCATE_WIFI_LWIP=y` | 动态 RX、lwIP 进 PSRAM：**估算 20–50 KB**（推断） | 官方明说"尽力"；S3 的 DMA→PSRAM 带宽"very limited"（官方原文），Wi-Fi 吞吐会降、延迟抖动变大 |
| `SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y` | net80211/pp/wpa_supplicant/lwIP 的 BSS 进 PSRAM：**估算 10–40 KB**（推断，需 map 实测） | 静态数据在 PSRAM：cache 关闭窗口不可访问（Wi-Fi 中断里访问 BSS 的场景要审计） |
| 降 `STATIC_RX/TX_BUFFER_NUM`（8→4） | 6–13 KB | 吞吐/抗干扰下降；AP 模式兼容性风险 |
| `SPIRAM_MALLOC_RESERVE_INTERNAL=32768` | 不增加总量，但保住"内部专用分配"不失败 | 可用堆看起来更小；行为更可预测 |

**官方对 S3 DMA→PSRAM 的限制原文**（必须写进方案里）：
- *"DMA transaction descriptors cannot be placed in PSRAM."*
- *"The bandwidth that DMA accesses external RAM is very limited, especially when the core is trying to access the external RAM at the same time."*
- 出处：[Support for External RAM — Restrictions（esp32s3）](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32s3/api-guides/external-ram.html)。

### 3.6 路线 C（零成本减需求，与 A/B 正交）

| 动作 | 省什么 | 依据 |
|------|--------|------|
| **照片改走 BLE → 直接删掉开机 AP** | 一次性还回 **≈80 KB** 内部 RAM（回归 104 KB 水位），同时消掉 iOS 热点三连坑 | 【仓库】`docs/wifi-upload-postmortem.md:29-42`、`dashboard.ino:3368` |
| 若保留 Wi-Fi：**HTTP → 裸 UDP / TFTP** | 省 TCP 窗口 ≈11.5 KB/连接 + HTTP 解析 1.2 KB；且不需要长连接 | 【仓库】`sdkconfig:2601-2602`、`dashboard.ino:873`+`:878`（1.2 KB 请求缓冲）、`:1078`（1 KB 流式写盘缓冲） |
| 上传前临时关 LVGL 定时器/动图 | 几 KB（heap 已固定，收益有限） | 【仓库】`docs/wifi-upload-postmortem.md:120`（原结论：缺口 ~18 KB 靠这个腾不出来） |

### 3.7 Wi-Fi 结论（明确回答"能不能救"）

| 结论项 | 判断 |
|--------|------|
| **能不能救** | **能，而且主要靠应用侧（路线 A），不靠 `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP`** |
| **救回多少** | 路线 A：**≈90 KB**（确定，来自两块已知静态占用）；路线 B：**额外 20–50 KB**（推断，靠 S5 实测）；路线 C：**≈80 KB**（删 AP） |
| **代价/风险** | ① LVGL 堆进 PSRAM 后，任何 flash 操作（NVS/OTA）与 LVGL 并发都可能崩，需要互斥；② S3 DMA→PSRAM 带宽受限（官方）；③ Wi-Fi 与 BLE 共存（RF 抢占）会让 BLE 上传期间的遥测/UI 抖动；④ 路线 B 要重建整套 IDF 库，回归面大 |
| **需要什么 spike** | **S4（应用侧，半天，必做）**；S5（sdkconfig 路线，1–2 天，仅在 S4 不够时做） |
| **前置事实** | 当前固件**开机就开 AP**（`dashboard.ino:3368`）→ 现在跑在 22~24 KB。**先决定 AP 是否常开**，再谈"救不救" |

---

## 4. 问题 3：备选通道对比

| 通道 | 吞吐（iPhone↔S3） | 占内部 RAM | 固件复杂度 | App/用户代价 | iOS 限制 | 结论 |
|------|------------------|-----------|-----------|-------------|----------|------|
| **BLE bulk 分片**（writeWithoutResponse + credit 流控 + notify 回执） | 保守 15–20 / 现实 **25–35** / 乐观 40–50 KB/s | 低：回调落地缓冲 0.5–1 KB + PSRAM 环形 64 KB + 写盘任务栈 4 KB | **中**：新特征、序号/CRC、窗口、PSRAM 环形、分片写 SD、进度回报 | 零（就地：App 已连、无需权限/热点） | MTU 185；前台需保持 App 活跃（后台会被挂起）；CoreBluetooth 队列满会静默丢包 | **主选**（静态照片） |
| **BLE L2CAP CoC** | iPhone↔iPhone 实测 **28 KB/s**（无优势） | 中（NimBLE CoC 缓冲） | **高**：重建库开 `COC_MAX_NUM`、手写 `ble_l2cap_*`、GATT 发 PSM、iOS `openL2CAPChannel` | 零 | iOS 11+ 有 API；**ESP32 侧默认编译掉** | **不做** |
| **Wi-Fi AP + HTTP multipart** | 理论 1–5 MB/s（11n） | 现状 **−80~−99 KB**（被打穿） | 已有（历史），但需重建 | 高：iOS 本地网络权限 + 热点关联 + 流量走蜂窝（三个坑都踩过） | `NEHotspotConfiguration`、`NSLocalNetworkUsageDescription`、无互联网热点被降级 | **仅在 A/B 路线救回内存后**才考虑；不是首选 |
| **Wi-Fi AP + 裸 UDP**（自定分片/序号/CRC） | 同上（UDP 略高） | 比 HTTP 省 ≈11.5 KB（TCP 窗口）+1.2 KB（解析） | 中（协议自己写，但比 HTTP 简单、失败可控） | 同 HTTP（热点三连坑仍在） | 同 HTTP | 若做 Wi-Fi，**优先 UDP 而不是 HTTP** |
| **Wi-Fi AP + TFTP 式单向**（设备只收包写 SD） | 同上 | 同 UDP | **低**（ACK 极简、无状态机） | 同 HTTP | 需要 App 侧实现 TFTP 客户端（简单） | Wi-Fi 路线里**实现成本最低**的形态 |
| **USB MSC 暴露 SD 卡**（TinyUSB，已预编译） | USB FS **≈1 MB/s 上限**（实际几十~几百 KB/s） | 低（TinyUSB 已在库里，`CONFIG_TINYUSB_MSC_ENABLED=1`） | 中：SD 卸载/挂载互斥、复合设备枚举；核心有 `cores/esp32/USBMSC.h` | 中：接线 + 在 **Files App** 里手动拖文件；App 无法自动化 | iOS 13+ Files 支持外置盘（读/写）；**App 不能程序化挂载** | **备选**（"免拔卡"手动拷贝）；需 S6 冒烟 |
| **USB CDC 自定义协议**（App 直接说话） | 高（USB FS） | 低 | 低 | 低 | **不可行**：iOS App 不能打开任意 USB 串口，`ExternalAccessory` 需要 MFi 认证硬件 | **不做** |
| **云端/网关中转**（照片先上云再下发设备） | 受 4G/宽带 + 服务器限制 | 低 | 中 | 高：设备还得联网；212 KB 上行+下行；延迟/流量/成本 | 无 | **不做（荒谬）** |

补充依据：
- USB MSC 能力：`cores/esp32/USBMSC.h`、`…/qio_opi/include/sdkconfig.h`（`CONFIG_TINYUSB_MSC_ENABLED 1`）、当前构建 `USBMode=hwcdc,CDCOnBoot=cdc`（`firmware/dashboard/README.md:41`）。
- iOS 外置存储：Apple 官方指南 [Connect external devices or servers with Files on iPhone](https://support.apple.com/guide/iphone/iph8d8f0f1a0/ios)。
- 热点三连坑：`docs/wifi-upload-postmortem.md:24-42`。

---

## 5. 问题 4：数据量 ↔ 时间对照（判断"能不能接受"）

单位按 **1 KB/s = 1000 B/s** 计。

| 传什么 | 体积 | 1 KB/s（历史误解） | 10 KB/s | 20 KB/s | 30 KB/s | 50 KB/s | 350 KB/s（Wi-Fi 悲观） |
|--------|------|------------------|---------|---------|---------|---------|----------------------|
| 静态 `.pgi` | **218 KB** | 218 s（3.6 min） | 21.8 s | 10.9 s | 7.3 s | 4.4 s | 0.62 s |
| 静态 JPEG q80（推断） | **30–60 KB** | 30–60 s | 3–6 s | 1.5–3 s | 1–2 s | 0.6–1.2 s | 0.09–0.17 s |
| 动图 10 帧 `.pgi` | **2.18 MB** | 2182 s（**36 min**） | 218 s | 109 s | 73 s | 44 s | 6.2 s |
| 动图 20 帧 | 4.36 MB | 73 min | 436 s | 218 s | 145 s | 87 s | 12.5 s |

**反推目标带宽**（把"用户能接受"变成硬指标）：

| 目标 | 需要的速度 |
|------|-----------|
| 静态 `.pgi` ≤ 10 s | **≥ 21.8 KB/s** |
| 静态 JPEG（60 KB） ≤ 3 s | **≥ 20 KB/s** |
| 动图 10 帧 ≤ 60 s | **≥ 36.4 KB/s** |
| 动图 10 帧 ≤ 30 s | **≥ 72.7 KB/s**（超出 BLE 现实区间，只有 Wi-Fi 能做到） |

**三条推论**：

1. **BLE 现实区间（25–35 KB/s）刚好覆盖"静态 `.pgi` ≤10 s"和"静态 JPEG ≤3 s"** ——所以 BLE 传静态照片**不需要运气**。
2. **BLE 传动图（2.18 MB）需要 62–87 s** ——不是不能做，但超出"≤60 s"目标，建议要么**降低帧数/分辨率**，要么走 Wi-Fi。
3. **JPEG 比提速更划算**：体积从 218 KB → 30–60 KB（**÷4~7**），等价于把 BLE 从 30 KB/s 提到 120–210 KB/s。**`esp_jpeg` 已预编译在 Arduino 核心里（`libespressif__esp_jpeg.a` 34,336 B，零安装）**，只用一次解码（静态图，不像动图要每帧解）→ 【仓库】`research-firmware.md §6.1-6.3`。

---

## 6. 问题 5：建议的 Spike（半天内可验，含阈值与退路）

> **5 个核心 Spike（S1–S5）+ 2 个预备项（S0 基线 30 min、S6 备选冒烟）**。
> 顺序按"能否推翻方案"排序：**S1 先做**（它决定 BLE 是否成立），S2/S3 是体验参数，S4/S5 是 Wi-Fi 支线。

| # | 实验 | 怎么做 | 看什么指标 | **通过阈值** | 失败退路 |
|---|------|--------|-----------|-------------|---------|
| **S0**<br>30 min | 现状内存基线 | 串口 `heap`/`mem`，对比"AP 开/关"；确认 22~24 KB 这个数字 | 内部堆、最大块、min free | 记录到 ≥100 KB（AP 关）/ ≥20 KB（AP 开） | 若"AP 关"也只有 ~40 KB，说明还有别的泄漏，先查 |
| **S1**<br>半天 | **BLE bulk 吞吐探针** | ① 固件加一个 `bulk` 特征（write-without-response），回调只 `memcpy` 进 PSRAM 环形并回 notify（收字节数/序号缺口），**先不写 SD**；② App 用 `.withoutResponse` + `canSendWriteWithoutResponse` 连发 1 MB；③ 记录协商 MTU、耗时、缺口、内部堆最低点 | **平均 KB/s**、缺口数、MTU、heap 余量 | **≥20 KB/s 且 512 KB 内 0 缺口**（理想 ≥28 KB/s） | ① 先开 2M PHY（`ble_gap_set_prefered_default_le_phy`）再测；② 关 NimBLE 日志；③ 仍 <15 KB/s → 走 S3（JPEG 减体积）或转 Wi-Fi |
| **S2**<br>2 h | BLE 上传时 UI 帧率 | 同一探针，边传边滑页/播动图；读心跳 `loop/s`、`render max`、触摸丢失 | loop 频率、单帧渲染峰值、触摸响应 | **loop ≥100 /s、render max ≤300 ms、无触摸丢失** | 分片节流（每 16 KB 空 20–30 ms）；或上传时切"传输中"静态提示页（减少重绘）；或 BLE 任务与 LVGL 分核 |
| **S3**<br>半天 | **静态照片 JPEG 端到端** | App 出 466×466 JPEG q80（30–60 KB）→ S1 通道传输 → `esp_jpeg_decode()` → PSRAM RGB565 → 显示；记录解码耗时与内部堆峰值 | 解码 ms、heap 峰值增量、端到端 s | **解码 ≤150 ms、内部堆峰值增量 ≤8 KB、端到端 ≤5 s** | 回落到"传 `.pgi`"（静态 7–9 s）；或继续 SD 卡导入 |
| **S4**<br>半天 | **Wi-Fi 内部堆救援（应用侧）** | ① 把内部 1/10 屏 fallback 改小/按需；② LVGL 堆改 `LV_MEM_CUSTOM=1` + PSRAM 分配；③ 起 AP + 手机连上，`heap` 记录；④ 跑 30 min 含 NVS 写（改亮度/旋转）看是否崩 | 客户端连上后的内部堆、min free、是否 panic | **≥40 KB** 且 30 min 无 panic（能起 8 KB 缓冲 + UDP/TFTP 收包） | 只做第 ① 步（+42 KB）往往已够；仍不够 → 转 S5；或放弃 Wi-Fi，走 BLE/JPEG |
| **S5**<br>1–2 天<br>（可选） | sdkconfig 路线可行性 | 用 [esp32-arduino-libs](https://github.com/espressif/esp32-arduino-libs) 重建库：`TRY_ALLOCATE_WIFI_LWIP=y` + `SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y` + `STATIC_RX/TX=4` + `MALLOC_RESERVE_INTERNAL=32768`；对比新旧库的 `idf.py size` 与运行时 `heap` | 内部堆、TCP/UDP 吞吐、30 min 稳定性、BLE 共存 | **内部堆 ≥40 KB、上传 ≥100 KB/s、30 min 无 panic、BLE 同连不崩** | 放弃 Wi-Fi 上传（回 BLE/JPEG）；或 USB MSC |
| **S6**<br>30 min<br>（备选） | USB MSC + iOS Files 冒烟 | 卸载 SD → 枚举 USBMSC → 接 iPhone → Files 里看盘、拖一个 `.pgi` 进去 | 能否识别、能否写 | 能读能写即通过 | 不采用；不投入 |

**Spike 顺序建议**：S0 + S1 同一天做完（决定主选），S3 紧跟（决定体验），S2 与 S3 串行（都在上传场景下测），S4 独立支线（另一个人/另半天），S5 只在 S4 不够时启动。

---

## 7. 推荐（主选 / 备选 / 不做）

### 7.1 主选：**BLE bulk（GATT 分片）+ 静态照片传 JPEG**

| 维度 | 内容 |
|------|------|
| 通道 | BLE 独立特征，`.withoutResponse` + 序号 + CRC16 + credit 窗口（notify 回执） |
| 数据格式 | **静态照片：App 出 JPEG q80（30–60 KB）**，设备 `esp_jpeg` 解码到 PSRAM RGB565（一次性，静态图可接受 50–150 ms）；动图继续 `.pgi` 走 SD |
| 预期体验 | 30 KB JPEG ≈ **1.0–1.5 s**；60 KB ≈ **2.0–2.5 s**（按 25–30 KB/s）——远优于"≤10 s"目标 |
| 内部 RAM | 新增 ≤2 KB 内部（回调落地缓冲 + 写盘任务栈已有）；环形缓冲与解码输出全在 PSRAM |
| iOS 侧 | 零新权限（不需要热点/本地网络）；需要 `canSendWriteWithoutResponse` 流控与前台保持 |
| 未决风险 | ① BLE 上传时 UI 帧率（S2）；② JPEG 解码耗时/内存（S3）；③ iOS 后台被挂起 → **只做前台主动上传** |

### 7.2 备选（按推荐度）

| 顺位 | 方案 | 什么时候选它 | 代价 |
|------|------|-------------|------|
| B1 | **BLE bulk + 传 `.pgi`（不做 JPEG）** | S3 失败，或不想引入 JPEG 编码/解码 | 静态 7–9 s；动图 60–87 s（不可接受） |
| B2 | **Wi-Fi AP + UDP/TFTP + 内部 RAM 救援（路线 A，必要时 +B）** | BLE 探针 <15 KB/s；或**动图必须无线** | 需要 S4/S5；保留热点三连坑；与 BLE 共存要调 |
| B3 | **USB MSC（Files App 手动拖）** | 只想"免拔卡"、能接受接线 + 手动操作 | 无 App 自动化；SD 挂载互斥；需 S6 冒烟 |
| B4 | **SD 卡导入（现状，`tools/import_photos.sh`）** | 以上都不达标 | 每次拔卡；但**零风险**，应始终保留 |

### 7.3 明确不做

| 不做 | 理由 |
|------|------|
| BLE L2CAP CoC | iPhone 实测 28 KB/s 无优势；ESP32 侧要重建库 + 手写原生 API |
| 提高 MTU（≥247/517）换吞吐 | iOS 上限 185，无 API 可改；且 MTU 不是瓶颈 |
| USB CDC 自定义协议给 App 用 | iOS 需要 MFi 认证，普通 App 打不开串口 |
| 云端中转照片 | 把"本地 0.1 s 的事"变成"要联网 + 双向 212 KB + 服务器成本" |
| 现在直接恢复 HTTP 上传 | 内部堆 2~4 KB 的根因没解决；必须先做 S4 |
| PSRAM 120 MHz 提速 | 官方：八线 120 MHz 是实验特性，温差约 20 ℃ 会随机崩——车载不可接受 |

### 7.4 决策树（一句话版）

```
先跑 S0+S1（BLE 吞吐）
├─ ≥20 KB/s ──► 主选 BLE + JPEG（做 S3；S2 调体验）
│                 ├─ 动图也要无线？→ 加做 B2（Wi-Fi/UDP + S4 内存救援）
│                 └─ 不用 → 动图继续走 SD
└─ <20 KB/s ──► ① 试 2M PHY / 关日志 / 调窗口（半天）
                 ② 仍不行 → B2（Wi-Fi UDP，需 S4）
                 ③ 或 B1（BLE 传 .pgi，慢但能跑）
```

---

## 8. 来源分级清单

### 8.1 官方文档（可直接引用）

| 主题 | 链接 |
|------|------|
| `SPIRAM_TRY_ALLOCATE_WIFI_LWIP` / BSS 进 PSRAM / RESERVE_INTERNAL 原文 | [esp-idf v5.5 `Kconfig.spiram.common`](https://github.com/espressif/esp-idf/blob/v5.5/components/esp_psram/Kconfig.spiram.common) |
| S3 PSRAM 模式/速度（120 MHz 实验警告） | [esp-idf v5.5 `Kconfig.spiram` (esp32s3)](https://github.com/espressif/esp-idf/blob/v5.5/components/esp_psram/esp32s3/Kconfig.spiram) |
| 外部 RAM 限制（DMA 描述符不能进 PSRAM、带宽受限、cache 关闭不可访问） | [Support for External RAM (esp32s3)](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32s3/api-guides/external-ram.html) |
| 省 RAM 总览（含 Wi-Fi buffer 指引） | [Minimizing RAM Usage](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32s3/api-guides/performance/ram-usage.html) |
| Wi-Fi 缓冲数量与每缓冲 ~1.6 KB | [esp-idf v5.5 `components/esp_wifi/Kconfig`](https://github.com/espressif/esp-idf/blob/v5.5/components/esp_wifi/Kconfig) |
| arduino-esp32 **不能**改 sdkconfig（预编译库） | [arduino-esp32 FAQ](https://github.com/espressif/arduino-esp32/blob/master/docs/en/faq.rst) |
| 重建 IDF 库的工具 | [esp32-arduino-libs](https://github.com/espressif/esp32-arduino-libs) |
| Apple 连接参数（§25.6，PDF） | [Accessory Design Guidelines for Apple Devices](https://developer.apple.com/accessories/Accessory-Design-Guidelines.pdf) |
| iOS L2CAP CoC API | [CBL2CAPChannel](https://developer.apple.com/documentation/corebluetooth/cbl2capchannel) |
| iOS 外置存储（Files App） | [Connect external devices or servers with Files on iPhone](https://support.apple.com/guide/iphone/iph8d8f0f1a0/ios) |

### 8.2 实测/社区（有价值的第三方数据）

| 主题 | 链接 |
|------|------|
| iOS MTU 185、7 包/事件、15 ms、iPhone SE 实测 12.2 kB/s | [Memfault: A Practical Guide to BLE Throughput](https://interrupt.memfault.com/blog/ble-throughput-primer) |
| iOS 15 ms 下限、~50 KB/s 经验上限、2M/DLE 增益 | [Punch Through: Maximizing BLE Throughput Part 4](https://punchthrough.com/ble-throughput-part-4/) |
| iPhone→ESP32 GATT **32 KB/s**；iPhone↔iPhone CoC **28 KB/s**；NimBLE-Arduino DLE 优化 | [NimBLE-Arduino Issue #280](https://github.com/h2zero/NimBLE-Arduino/issues/280) |
| NimBLE 吞吐 demo：关日志 500→794 kbps、conn_itvl=100、payload 495 → **~1.4 Mbps**；`ble_gap_set_prefered_default_le_phy` 用法 | [ESP-IDF Issue #15660](https://github.com/espressif/esp-idf/issues/15660) |
| Wi-Fi 缓冲在 PSRAM 场景的实测规模 **54,528 B** | [MicroPython PR #12968](https://github.com/micropython/micropython/pull/12968) |
| iOS 设备自动切 2M PHY（iPhone X/8） | [Apple 开发者论坛 thread/98372](https://developer.apple.com/forums/thread/98372)（仅标题可读） |

### 8.3 抓取限制（诚实声明）

- `developer.apple.com/forums`、`stackoverflow.com`、`devzone.nordicsemi.com`、`forum.esp32.com` 均有反爬/人机验证，**正文拿不到**。本文凡是引用这些域名的，只引用**搜索索引里可见的标题/摘要**，并已标注"仅标题可读"。
- Apple《Accessory Design Guidelines》是 PDF，抓取工具不支持 PDF → 文中 MTU 185 / 15 ms 的参数**以 Memfault 的转录 + 本项目真机实测为准**，并给出官方 PDF 链接供人工复核。
- 中文社区存在"iOS MTU=517 / 6–8 KB/s"一类文章，**无法核实且与实测冲突**，未采用。

---

## 9. 本次未能验证、需要上机确认的项

| # | 未验证项 | 影响 | 谁来验 |
|---|---------|------|--------|
| 1 | 当前固件（AP 常开）的真实内部堆 | 决定"内存手术"做多少 | S0（一条 `heap` 命令） |
| 2 | iPhone 与本板之间实际协商的 MTU / 每事件包数 | 决定 S1 的期望值 | S1 日志 |
| 3 | BLE bulk 真实可持续吞吐（本项目代码路径） | 主选成不成立 | S1 |
| 4 | 上传时 LVGL 帧率/触摸 | 体验是否可接受 | S2 |
| 5 | `esp_jpeg` 解 466×466 的真实耗时与内部堆峰值 | JPEG 方案成不成立 | S3 |
| 6 | LVGL 堆进 PSRAM 的稳定性（NVS 写并发） | Wi-Fi 路线成不成立 | S4 |
| 7 | 重建库后 `TRY_ALLOCATE` 的真实收益 | Wi-Fi 上限 | S5 |
| 8 | iOS Files 能否稳定读写 USBMSC 盘 | B3 可行性 | S6 |

> **合规提醒（沿用全项目基线）**：本项目仅用于个人开发、学习与技术验证——不装车、不商业化、不分发。详见 `docs/nav-backend.md §0`。
