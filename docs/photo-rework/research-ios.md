# iOS App 照片链路 + BLE 写入能力审计（只读，2026-10-26）

> **范围**：`ios/CarDashboardPhone/`（SwiftUI + CoreBluetooth + PhotosUI），只读，不改任何代码。
> **目的**：为「App 选图 → 转码 → 无线传输 → 设备清单回读」的重新设计提供事实基线。
> **引用格式**：`path:line`，行号对应当前工作区（HEAD `9a65c92`）。

## 0. 一句话结论

**App 现在只能把 GIF 存进相册，`.pgi` 编码器虽然写好了但没有任何入口能调到（纯死代码）；
BLE 侧只有"小 JSON 命令 + 有序多消息队列"，没有 MTU 协商、没有分片、没有大文件传输。**
也就是说：**照片离开 App 的最后一步（相册）和设备能读的格式（`.pgi` 在 SD 卡）之间完全没有桥** ——
这正是"必须拔 SD 卡"的断点。

---

## 1. 现有照片页到底能做什么

### 1.1 入口与可达性

| 入口 | 位置 | 是否真的可用 |
|------|------|-------------|
| 底部「照片」Tab（tag 3） | `ios/CarDashboardPhone/CarDashboardPhone/ContentView.swift:160-162` | ✅ 正常 |
| 表盘列表里 photo 行下方的「管理素材 / 选择播放哪张」 | `ContentView.swift:804-811` | ✅ 正常（NavigationLink） |
| 「仪表连接」节里的「上传照片到仪表」 | `ContentView.swift:907-911` | ❌ **死代码**：这个 `NavigationLink` 写在 `Button(action: { ... })` 的**动作闭包**里（`ContentView.swift:900-913`），视图表达式的结果被丢弃，**界面上不会渲染**，只剩一条编译器警告 |

> 附带事实：三处 `PhotoTabView` 各自是独立实例、各自持有自己的 `@State`（Tab 一处、表盘行一处、死链一处），所以"从表盘行进去裁好的图"和"从 Tab 进去"不共享素材状态。

### 1.2 当前流程（唯一实际可达的路径）

```
PhotosPicker 选图/视频
  → PhotoStudio 读帧（GIF/APNG/WebP/静态图）或 AVAssetImageGenerator 抽帧（视频）
  → 圆屏手势裁切预览（捏合缩放 / 拖动 / 旋转 90°）
  → CGImageDestination 编成 GIF（Data，内存里）
  → PHPhotoLibrary 存进系统相册
  → ✗ 到此结束。没有 .pgi 文件、没有分享、没有 Files 导出、没有传输
```

| 步骤 | 实现 | 说明 |
|------|------|------|
| 选素材 | `Photo/PhotoTabView.swift:162-166` | `PhotosPicker`，`matching: .any(of: [.images, .videos])`，**不需要相册读权限** |
| 读帧 | `Photo/PhotoTabView.swift:382-402` → `Photo/PhotoStudio.swift:191-226`（图片/动图）、`:250-309`（视频） | 图片走 `CGImageSource`，带 EXIF Orientation；视频最多 30 s / 120 帧 / 源文件 200 MB |
| 视频降帧 | `Photo/PhotoStudio.swift:275-283` | 帧数超预算时**自动降 fps**（下限 2），并把实际 fps 回显到界面（`PhotoTabView.swift:388-392`） |
| 裁切 | `PhotoTabView.swift:242-341` + `PhotoStudio.swift:150-186` | 目标边长**硬编码 466**（`PhotoTabView.swift:20`、`PhotoStudio.swift:49-50`），圆屏最终再裁四角 |
| 编 GIF | `Photo/PhotoStudio.swift:314-344`，调用点 `PhotoTabView.swift:340` / `:371` | 帧间延时取视频 step 或 GIF 原始 delay |
| 落地 | `Photo/PhotoStudio.swift:347-364`（`PHAssetCreationRequest`），调用点 `PhotoTabView.swift:763-786` | **只存相册**；超过 8 MB 只给一句文案提示（`PhotoTabView.swift:771-777`） |

### 1.3 照片页的状态模型（现状很薄）

| 状态 | 声明 | 谁来写 | 谁来读 |
|------|------|--------|--------|
| `status`（一行文字） | `PhotoTabView.swift:26` | `load()` / `exportGIF()` | `:185`、`:189` |
| `busy`（禁用按钮） | `:27` | 同上 | `:167`、`:206` |
| `showError` / `errorText`（alert） | `:28-29` | `load()`、`exportGIF()` 的 catch | `:232-236` |
| `gifData` / `frameCount` | `:24-25` | 重新裁切时重编码 `:340` | `:208-211`、导出按钮 `:206` |
| `slideshowOn` / `intervalSec` | `:65-67` | 本地 Toggle/Picker **乐观写入**（`:117-136`） | 同处 |

**缺的东西（就是重做时要补的）**：

- ❌ 没有**连接态**：照片页从不读 `session.ble.isConnected`（在本文件里 grep 不到），未连接时点「上一张/下一张」会静默失败（`BLEClient.swift:269-273` 只写 `lastNotice`，照片页不显示 `lastNotice`）。
- ❌ 没有**进度**：整个文件没有 `ProgressView`；上传/编码进度只存在于**死代码**的 `uploadStatus` 字符串里（`PhotoTabView.swift:61`、`:406-485`）。
- ❌ 没有**设备清单**：`DevicePhoto` 结构（`:70-82`）与 `devicePhotos`（`:83`）是 Wi-Fi `GET /list` 时代的产物，**没有任何调用点**。
- ❌ 没有**设置回读**：轮播开关/间隔初值写死 `true` / `30`（`:65-67`），设备端 `photo_get` 只打印串口、不回 BLE（`firmware/dashboard/dashboard.ino:2330-2338`），所以 App 永远不知道设备真值 —— 只有"发出去"，没有"确认"。

### 1.4 `.pgi` 编码器：存在、但**不可达**

`Photo/PgiEncoder.swift` 完整实现了 `.pgi` 打包（与设备端 `simulator/ui/pgi.h` 对齐），但**唯一调用点是死代码**：
`PhotoTabView.swift:414`（在 `upload()` 内）← `upload()` 本身在整个工程里**没有调用点**（`:406-485` 是 private 且无引用）。

| 项 | 事实 | 引用 |
|----|------|------|
| 文件头 | 32 B：`PGI1` + w/h u16 + 帧数 u16 + fps u8 + flags u8（恒 0，无 RLE）+ 4×u32（帧表偏移/数据偏移/保留） | `PgiEncoder.swift:107-121`；格式定义 `simulator/ui/pgi.h:22-42` |
| 每帧布局 | **每帧自带 256×4 B 调色板（B,G,R,A）+ w×h 索引**，帧表项 = (off, 1024+w*h) | `PgiEncoder.swift:123-141`；`pgi.h:39-41` |
| 尺寸 | 由输入帧决定；App 侧实际是 **466×466**（`PhotoStudio.deviceSize`） | `PgiEncoder.swift:65-66`、`PhotoStudio.swift:49-50` |
| fps | 调用方传 `Int(fps.rounded())`，而 `fps` 是**视频抽帧滑条值**（默认 12），GIF 自身 delay 被忽略 | `PgiEncoder.swift:113`、`PhotoTabView.swift:414`、`:32` |
| 帧数 | `maxFramesFit()`：8 MB 预算内自动**均匀抽帧**（不是截掉尾巴）；再夹到 512 | `PgiEncoder.swift:54-59`、`:71-87` |
| 量化 | ①先写 GIF 再读回索引色（借系统量化）；②实际用 **5-5-5 直方图（32768 桶）取前 256 色**；③未入选桶回退到最近已选色；④**Floyd–Steinberg 抖动** | `PgiEncoder.swift:148-183`、`:209-266`、`:268-301` |
| 极端退路 | 固定 3-3-2 调色板（仅 RGBA 转换失败时） | `PgiEncoder.swift:306-319` |
| 体积上限 | **8 MB**（注释与错误文案都写 8 MB） | `PgiEncoder.swift:41`、`:33` |

**典型体积（按格式公式实算，非估算）**：

| 场景 | 计算 | 体积 |
|------|------|------|
| 466×466 单帧（静态照片） | 32 + 8 + (1024 + 466²) | **218,220 B ≈ 213.1 KB** |
| 466² × 24 帧（约 2 s @12fps） | 32 + 192 + 24×218,180 | **5.24 MB** |
| 466² 最多能装多少帧（8 MB 预算） | (8 MiB − 40) / 218,180 = 38.4 | **38 帧 ≈ 7.91 MB**（超 38 帧会被自动抽帧） |
| 300×300 单帧（`--fast` 尺寸） | 32 + 8 + (1024 + 90,000) | **91,064 B ≈ 88.9 KB** |
| 300² × 40 帧（`--fast` 默认） | — | **3.47 MB** |

> 对比设备侧 Mac/PC 素材管线：`tools/import_photos.sh` 默认 466 px / 8 fps / 35 帧 / **40 MB 上限**（`tools/import_photos.sh:47-50`），`--fast` 是 300 px / 12 fps / 40 帧（`:63`），上限 40 MB 的理由见脚本头注释 `:18-25` 与 `docs/photo-gif-design.md:13`。
> **⚠️ 不一致**：App 编码器仍按 **8 MB** 裁帧，这是 Wi-Fi 上传时代（设备端 `PHOTO_UPLOAD_MAX` 也是 8 MB，`firmware/dashboard/dashboard.ino:872`）的遗留；SD 卡方案已放宽到 40 MB，App 没跟上。

**另一个盲区**：`pgi.py`（Python）与 `PgiEncoder.swift` 的量化算法**完全不同**（Pillow `quantize(256)` vs GIF 往返 + 5-5-5 + 抖动）。现有跨语言夹具只由 `pgi.py` 生成（`tests/Makefile:231-243`，`tests/fixtures/pgi_*.pgi`），**没有任何测试验证 App 生成的 `.pgi` 能被固件解码器接受**。

### 1.5 用户"必须拔 SD 卡"的断点，具体在哪一步

| # | 环节 | 现状 | 断点 |
|---|------|------|------|
| 1 | App 裁切 + 转 GIF | ✅ 可用，落相册 | — |
| 2 | App 生成设备格式 `.pgi` | 代码存在但**无入口**（PgiEncoder 不可达） | ❌ **断点 A**：手机端产不出 `.pgi` |
| 3 | 把文件送到设备 | 无 BLE 大文件通道；Wi-Fi `POST /upload` 已废（堆不足，见 §3.3） | ❌ **断点 B**：没有任何"手机→设备"的文件通道 |
| 4 | 电脑转格式 | `tools/import_photos.sh`（JPG/PNG/GIF/HEIC/MOV/MP4 → `.pgi`） | 必须经过 Mac |
| 5 | 拷贝到 SD `/photos/` | 拔卡 → 电脑 → 插回 | ❌ **断点 C**：物理插拔 |

---

## 2. BLE 客户端能力（`BLEClient.swift`，941 行）

### 2.1 连接 / 发现 / notify

| 能力 | 事实 | 引用 |
|------|------|------|
| 扫描 | 按 `serviceUUID` 扫描，再按设备名 `CarDashboard` 二次过滤，避免误连 | `BLEClient.swift:152`、`:797-803` |
| 自动连接 | 蓝牙就绪/掉线/连接失败都汇聚到 `connectIfPossible()`；决策抽成纯函数 `BLEConnectionPolicy.decide()` | `BLEClient.swift:136-189`、`:786`、`:825`、`:846`；`BLEConnectionPolicy.swift:70-80` |
| 手动断开（粘性） | `autoConnectEnabled=false`，不会自动连回；重新点连接才恢复 | `BLEClient.swift:198-218`、`BLEConnectionPolicy.swift:86-88` |
| 服务/特征发现 | `discoverServices([serviceUUID])` → `discoverCharacteristics([telemetry,status,command,faces])` | `BLEClient.swift:851-857` |
| Notify 订阅 | **只订阅 Status + Faces**（`setNotifyValue(true)`）；Telemetry/Command 是 Write-only，不订阅 | `BLEClient.swift:867`、`:873`；固件属性 `firmware/dashboard/ble_telemetry.cpp:325-345` |
| Faces 读取 | 发现即 `readValue`；收到通知后**解析失败才重读**（截断 JSON 必解析失败） | `BLEClient.swift:874`、`:915-940` |
| Status 读取 | 收到值后**直接丢弃**（只 `return`，没有模型/UI 使用） | `BLEClient.swift:917-920` |
| 断连收尾 | `clearConnectionState()` 清 4 个特征 + `facesReady`，并重置地图队列 | `BLEClient.swift:221-228`、`:828-847` |
| BLE 状态恢复 | ❌ 没有 `CBCentralManagerOptionRestoreIdentifierKey` / `willRestoreState`（全工程 grep 无） | — |

### 2.2 写入方式一览（**没有一处做分片**）

| 方法 | 特征 | write type | 等回执？ | 节流/顺序 | 引用 |
|------|------|-----------|---------|-----------|------|
| `sendCommand()`（亮度/息屏/表盘/旋转/照片命令…） | Command | **withResponse** | ✅ 但**忽略回调**（`didWriteValueFor` 只处理 telemetry 分支） | 无内置节流，调用方自己控频 | `BLEClient.swift:269-278`、`:894-913` |
| `setBrightnessLive()` | Command | **withoutResponse** | ❌ | 50 ms 限流（20 Hz） | `BLEClient.swift:305-315` |
| `setBrightnessCurve()` / `…Live()` | Telemetry | **withResponse** | ✅（靠 didWriteValueFor 保序） | Live 50 ms；setter 250 ms 防抖 | `BLEClient.swift:337-346`、`:366-398`、`:584-617` |
| `send(packet:)` 遥测 | Telemetry | withResponse | ✅ | 由 `DriveSessionManager` 2 Hz 驱动 | `BLEClient.swift:517-524`；`DriveSessionManager.swift:67` |
| `sendTimeOnConnect()` | Telemetry | **withoutResponse** | ❌ | 连接后 0 s / +1 s / +3 s 各一次（幂等重试） | `BLEClient.swift:546-571` |
| `sendMapMessages()` 地图窗口 | Telemetry | **withResponse** | ✅ 一条一确认 | 队列 + `didWriteValueFor` 推进 + 2 s 看门狗 / 4 s 无进展重置 | `BLEClient.swift:670-754`、`:699-732` |
| `sendNav()` | Telemetry | withResponse | ✅ | — | `BLEClient.swift:756-761` |

### 2.3 MTU：**完全没有协商**

| 事实 | 引用 |
|------|------|
| App 从不调用 `maximumWriteValueLength(for:)` / `requestMaximumWriteValueLength` / `canSendWriteWithoutResponse` / `peripheralIsReady(toSendWriteWithoutResponse:)`（全工程 grep 无） | `ios/CarDashboardPhone/`（grep 结果为空） |
| 固件请求 MTU **247**，iOS 实际协商约 **185**（有效载荷约 **184**） | `firmware/dashboard/ble_telemetry.cpp:316`；`docs/ble.md:209-213`、`:405-412` |
| 单条消息预算：App 自己把地图消息硬限到 **180 B** | `Nav/MapWindowPacket.swift:85-87` |
| 超长写入会被**静默丢弃**（不是报错）：892 B 的 `mp` 消息 → "底图有、白线没有"；194~276 B 的包同样被丢 | `Nav/MapWindowPacket.swift:170-175`、`:111-114` |
| 一张 466² 静态图 = 213 KB ≈ **1212 个 180 B 包**（纯算术，加上每包间隔/回执，不是"发 1212 次"那么简单） | §1.4 体积表 |
| 仓库里唯一的吞吐参考：BLE 实测有效吞吐约 **1 KB/s**（来自地图窗口 50 条消息 2~3 s）→ 单张 213 KB 约 **212 s**，10 帧动图约 35 min | `docs/photo-gif-design.md:97-110` |

> 注意：那个 1 KB/s 是**串行 withResponse、每条 ≤180 B** 的地图窗口实测值，属于"往返受限"口径，**不是 BLE 物理上限**。真实 bulk 吞吐（withoutResponse / MTU 247~517 / 2M PHY）是本次立项的关键未知，见 §9 与另一份 `research-transport.md`。

### 2.4 断连重连策略（`BLEConnectionPolicy`）

| 输入组合 | 决策 | 引用 |
|----------|------|------|
| 用户点过断开（`autoConnectEnabled=false`） | `userDisconnected` —— **最高优先级**，任何自动路径都不得越过 | `BLEConnectionPolicy.swift:70-80`（注释 `:60-68`） |
| 已连接 | `alreadyConnected`（不重复扫描、不覆盖提示文案） | `:73-74`、`BLEClient.swift:158-168` |
| 已在扫描 | `alreadyScanning` | `:75-76` |
| 蓝牙未就绪 | `bluetoothNotReady`（按 `.poweredOff/.unauthorized/...` 细分成文案） | `:77-78`、`BLEClient.swift:173-188` |
| 蓝牙就绪且用户想连 | `startScan` | `:79` |
| 掉线后是否重连 | 等价于 `autoConnectEnabled` | `:86-88`、`BLEClient.swift:843-846` |

### 2.5 写入节流 / 顺序的历史教训（新传输层必须继承的结论）

| 教训 | 事实 | 引用 |
|------|------|------|
| **配置类包必须 `.withResponse`** | 连发 9 个曲线包，设备收到顺序完全乱序（`130 160 170 140 120 110 180 100 150`），最终停在 150 而不是 180；改 withResponse 后按 `didWriteValueFor` 保序 | `BLEClient.swift:601-615`；`docs/ble.md:616-628` |
| **但不能"一口气连发"** | 40+ 条地图消息 for 循环直发 → CoreBluetooth 内部队列溢出、**静默丢前面的**；改为"发一条、等回执、再发下一条" | `BLEClient.swift:673-682`、`:734-754` |
| **看门狗必须由定时器驱动** | 曾把看门狗写在只由 `didWriteValueFor` 调用的函数里，回执一丢就死锁；现为 2 s 定时器 + 4 s 无进展重置 | `BLEClient.swift:699-732` |
| **UUID 比较不能比对象** | 重新发现服务后 `CBCharacteristic` 是新实例，`==` 恒 false，队列会停在第一条 | `BLEClient.swift:894-904` |
| **高频写会拖垮 UI** | 演示滑块 10 Hz 写 `lastNotice`（@Published）→ 每秒 10 次整页重绘；`quiet` 参数就是为此加的 | `BLEClient.swift:258-267`、`:413-423` |

---

## 3. App 侧大文件传输的现成基础

### 3.1 结论：**没有**。只有"多条小消息按序发送"的排队器

| 能力 | 有吗 | 事实 |
|------|------|------|
| 按 MTU 切分任意二进制流 | ❌ | 全工程无 `maximumWriteValueLength` 使用；地图消息是"每条独立可解析的 JSON"，不是字节流分片 |
| 序号 / 偏移 / 重组 | ❌ | 无 offset/seq/CRC 字段；固件靠"攒到 `{...}` 完整"重组（`docs/ble.md:209-213`） |
| 设备 ACK（应用层） | ⚠️ 只有"整窗口提交标记" | 地图用 `{"t":"me"}` 表示"此刻才切换显示"（`Nav/MapWindowPacket.swift:156-219`），不是逐片 ACK |
| 超时重传 | ⚠️ 只有整体重发/丢弃 | 地图 4 s 无进展 → 整队列丢弃（`BLEClient.swift:727-732`）；Wi-Fi 死代码里有 3 次重试（`PhotoTabView.swift:739-752`） |
| 进度回调 | ❌ | 无 |
| 取消 | ❌ | 无 |
| 断点续传 | ❌ | 无 |
| 校验（哈希/长度核对） | ⚠️ 设备侧有 `.pgi` 头校验 + 大小核对（仅 Wi-Fi 上传路径） | `firmware/dashboard/dashboard.ino:1102-1112` |

**唯一可复用的骨架**：`sendMapMessages` 的"队列 + 一条一确认 + 看门狗 + 只保留最新待发窗口"（`BLEClient.swift:670-754`）。它解决的是"小包不丢、顺序不乱、卡死能自愈"，**不含分片语义**。

### 3.2 后台能力

| 项 | 现状 | 引用 |
|----|------|------|
| `UIBackgroundModes` | ✅ `location` + **`bluetooth-central`** 已声明 | `ios/CarDashboardPhone/CarDashboardPhone/Info.plist:42-46`；`project.yml:51-53` |
| 后台定位 | ✅ `allowsBackgroundLocationUpdates = true`（驾驶会话在用） | `LocationService.swift:35-39` |
| BLE 状态恢复 | ❌ 未实现（无 restore identifier） | — |
| 后台任务 API | ❌ 无 `beginBackgroundTask` / `BGTaskScheduler` / `URLSession` background session（全工程 grep 无） | — |
| 后台传输结论 | **机制上只有"bluetooth-central 这个后台模式"这一层**：App 被挂起后没有保活/续传逻辑，长传输（分钟级）在锁屏/切后台时大概率被降频或中断，且**重启后无法续传** | — |

### 3.3 Wi-Fi 上传的现状：App 侧死代码 + 固件端点还在，但被堆门槛挡住

| 事实 | 引用 |
|------|------|
| App 里 `upload()` / `ensureHotspot()` / `post()` / `refreshList()` / `deletePhoto()` 约 **350 行**仍在，但**无调用点**（UI 已移除，只剩"导入新照片（走 SD 卡）"的文字说明） | `PhotoTabView.swift:406-761`、`:214-222` |
| 仍 `import NetworkExtension`，但 `CarDashboardPhone.entitlements` 是**空 dict**（没有 Hotspot Configuration entitlement） | `PhotoTabView.swift:12`、`CarDashboardPhone.entitlements:1-5` |
| 固件 `POST /upload` **仍在**（`http_upload`，写 SD + 校验 `.pgi` 头 + 大小自洽） | `firmware/dashboard/dashboard.ino:1166-1179`、`:1040-1125` |
| 但堆门槛 `>= 6000 B`：手机连上热点后内部堆仅 **2~4 KB** → 必返 **507 low_heap** | `firmware/dashboard/dashboard.ino:1059-1064`；`docs/wifi-upload-postmortem.md:62-91` |
| `GET /list` / `POST /delete` 保留且实测可用（几百字节堆） | `dashboard.ino:1168-1171`；`docs/wifi-upload-postmortem.md:108-111` |

> 结论：**"再走 Wi-Fi 上传"不是一个 App 侧开关，而是固件 OOM 问题**（Wi-Fi 协议栈吃内部 RAM 50 KB+，PSRAM 帮不上）。
> 若后续通道方案重新考虑 Wi-Fi，App 侧死代码可作为起点，但必须先换板/换方案，见 transport 任务。

---

## 4. 权限与网络声明（Info.plist / entitlements）

| Key | 现状 | 位置 | 对新方案的意义 |
|-----|------|------|---------------|
| `NSBluetoothAlwaysUsageDescription` | ✅ 已声明 | `Info.plist:28-29` | BLE 通道**不需要**再补权限 |
| `NSBluetoothPeripheralUsageDescription` | ✅ 已声明（旧键，仅 iOS 12 及更早用） | `Info.plist:30-31` | 兼容性保留 |
| `NSLocalNetworkUsageDescription` | ✅ 已声明（文案还写着"上传照片到仪表"） | `Info.plist:32-33`；`project.yml:41-48` | 若走本地网络/Wi-Fi 传输：**已有**；但 `NEHotspotConfiguration` 还需 entitlement |
| `NSAppTransportSecurity` → `NSAllowsLocalNetworking` | ✅ true（只放行本地网络，不是 `NSAllowsArbitraryLoads`） | `Info.plist:23-27`；`project.yml:49-50` | 明文 HTTP 到 `192.168.4.1` 可用 |
| `NSBonjourServices` | ❌ 缺 | — | 若改用 Bonjour/mDNS 发现设备才需要 |
| `UIBackgroundModes` | ✅ `location`、`bluetooth-central` | `Info.plist:42-46` | BLE 后台通告已在；**但不等于"后台长传输能跑完"**（见 §3.2） |
| `NSPhotoLibraryUsageDescription` / `NSPhotoLibraryAddUsageDescription` | ✅ 都已声明 | `Info.plist:38-41` | `PhotosPicker` 本身不需要读权限；写相册需要 Add |
| 定位权限（Always / WhenInUse） | ✅ 都已声明 | `Info.plist:34-37` | 与照片无关 |
| `CarDashboardPhone.entitlements` | **空 dict** | `CarDashboardPhone.entitlements:1-5` | 无 Hotspot Configuration、无 keychain-access-groups（Keychain 单测因此曾受影响，见 `PROGRESS.md:244`） |

> **如果改成 Wi-Fi/HTTP 上传（不论哪个方案），App 侧要补的是**：
> ① `com.apple.developer.networking.HotspotConfiguration` entitlement（仅当用 `NEHotspotConfiguration` 自动连热点）；
> ② 若要后台跑完：`beginBackgroundTask` 或 `URLSession` background configuration（现在都没有）；
> ③ 若要发现设备：`NSBonjourServices`（现有实现是硬编码 `192.168.4.1`，见 `PhotoTabView.swift:52-58`）。

---

## 5. UI / 状态模型 & 设备能力模型

### 5.1 照片页与设备状态是**断开**的

| 界面元素 | 数据来源 | 问题 |
|----------|---------|------|
| 上一张 / 下一张 | 直接 `sendCommand`（`PhotoTabView.swift:94`、`:101`） | 未连接时静默失败；没有禁用态、没有失败提示 |
| 轮播开关 / 间隔 | 本地 `@State` 初值 `true` / `30`（`:65-67`），改动即发命令（`:117-136`） | **不读设备真值**；设备重启/串口改过后 App 显示会漂 |
| 设备照片清单 | 无 | 设备**不在连接时主动上报照片清单**（`docs/ble.md:361-363`），App 只知道"能不能切" |
| 设备 SD 状态 / 帧数 | 无 | `DevicePhoto`（`:70-82`）只有 Wi-Fi `/list` 能填 |

### 5.2 唯一的"设备能力模型"：`DeviceFaces`（Read + Notify 模式）

| 项 | 事实 | 引用 |
|----|------|------|
| 模型 | `DeviceFaceReport { faces, truncated, currentId, brightness?, rotation?, screenOn?, trip*? }` | `DeviceFaces.swift:38-97` |
| 解析入口 | `DeviceFaceParser.parse(Data) -> DeviceFaceReport?`，失败返回 nil（不猜） | `DeviceFaces.swift:101-209` |
| 判别字段 | **同时接受 `"t":"faces"` 与紧凑 `"t":"f"`**（只认一个曾导致"一个表盘都看不到"） | `DeviceFaces.swift:107-115` |
| 紧凑格式 | `{"t":"f","c":6,"m":63,"u":5,"bright":200,"rot":0,"screen":1,"f":[...]}`，6 个表盘 **173 B** | `DeviceFaces.swift:117-155`；`docs/ble.md:373-403` |
| 184 B 硬上限 | iOS 单次 Read/Notify 有效载荷约 184 B；对象格式 476 B 必被截断 | `docs/ble.md:405-416`；`PROGRESS.md:130-138` |
| 截断标记 | `trunc`/`truncated` → UI 提示"列表不完整" | `DeviceFaces.swift:149`、`:186`；`ContentView.swift:827-830` |
| 缺省 vs 0 | 用 Optional 区分"设备没说"与"真的是 0"（亮度/方向/息屏/行程） | `DeviceFaces.swift:47-83`；`ContentView.swift:198-222` |
| 更新协议 | **Read 取数据，Notify 只作"变了"的提示；解析失败（= 被截断）就重新 Read** | `docs/ble.md:667-680`；`BLEClient.swift:923-939` |
| 派生优先 | 分组/当前表盘/开关状态都从上报**派生**，不另存本地状态再用 onChange 同步 | `ContentView.swift:30-63`；`docs/ble.md:712-716` |
| 本地化兜底 | 未收录的 name 直接用设备给的 label（所以新增表盘 App 不用改） | `DeviceFaces.swift:214-256`；`docs/ble.md:721-726` |

**小缺陷**：`FaceDisplay.faceTitle` 没有 `photo` 分支（`DeviceFaces.swift:247-256`），所以表盘列表里照片表盘显示的是英文 **"photo"**（fallbackLabel = name，`DeviceFaces.swift:139`）；而分组标题有中文（`:221`）。新方案顺手补一行即可。

### 5.3 新加"照片能力 / 清单"该沿用哪种 pattern（结论）

**必须是 Read 主 + Notify 提示 + 紧凑载荷 + 分页，不能是一次性 Notify。**

| 设计约束 | 原因 | 依据 |
|----------|------|------|
| 特征属性 `READ` + `NOTIFY` | 清单必然 > 184 B，Notify 会截断；Read 由系统 Read Blob 取完整值 | `docs/ble.md:667-680`、`:405-416` |
| Notify 只发"变了"的短哨兵（如 `{"t":"p","rev":N}`） | 避免重复推大包；`sync_display_report_state` 无限流曾把主循环拖垮 | `docs/ble.md:520-527` |
| 清单本身用**紧凑数组 + 分页 token**（`off`/`lim`/`more`），或按文件分段 | `f` 名字数组已证明 173 B 是上限附近；照片清单含 name/size/frames/fps 必然超 | `docs/ble.md:414-416` |
| 解析失败 → 重新 Read（同样用"能否解析成完整 JSON"判定） | 已验证的模式，无需新状态机 | `BLEClient.swift:923-939` |
| 设备属性（设置/当前第几张）由设备拥有、App 只显示 | 亮度/方向的教训：App 下发"记忆值"会冲掉设备真值 | `docs/ble.md:443-465`、`DeviceFaces.swift:47-58` |

---

## 6. 改动面评估（若要"选图 → 转码 → BLE 分片上传 → 进度 → 清单回读 → 删除/排序"）

> 只列文件与改动性质，不写代码。行数为当前文件规模，供估工。

| # | 文件 | 现状 | 需要的改动 | 规模 |
|---|------|------|-----------|------|
| 1 | `ios/CarDashboardPhone/CarDashboardPhone/Photo/PgiEncoder.swift` | 335 行，`.pgi` 打包完整但不可达 | ①拆出 **Foundation-only** 的纯打包/分帧层（现在文件级 `import UIKit`，macOS 测不了）；②`maxBytes` 8 MB → 40 MB（或按设备能力）；③fps 由帧时长推导（GIF 延时不能丢）；④可选：全局调色板省 1 KB/帧（跨端格式决定） | 中 |
| 2 | `ios/CarDashboardPhone/CarDashboardPhone/Photo/PhotoStudio.swift` | 364 行，裁切/抽帧/GIF | ①输出尺寸可选（466/300/240，与设备帧率强相关）；②抽帧预算按**目标体积**而不是帧数；③GIF 中间态可去掉（直接出 RGBA 帧给编码器，省内存/时间） | 中 |
| 3 | `ios/CarDashboardPhone/CarDashboardPhone/Photo/PhotoTabView.swift` | 812 行，其中 **~350 行 Wi-Fi 死代码** | ①删死代码（`upload/hotspot/post/refreshList/deletePhoto/DevicePhoto`）；②新状态机：未连接/已连接/选取/转码中/上传中(进度/速率/ETA)/失败可重试/成功；③传入 `session.ble` 的连接态并禁用按钮；④设备清单区（名字/大小/帧数/静态或动图/当前张）+ 删除/排序；⑤轮播设置改为"设备上报 → 显示" | 大（重写） |
| 4 | `ios/CarDashboardPhone/CarDashboardPhone/BLEClient.swift` | 941 行 | ①**新增分片传输引擎**：`maximumWriteValueLength(for:)` 探测（现在完全没用）、chunk 大小、seq/offset、ACK/重传、窗口流控（复用 map 队列的"回执驱动 + 看门狗"骨架）、进度回调、取消；②发现/订阅新特征（照片清单/数据通道）；③避免与 2 Hz 遥测、地图窗口抢通道（优先级/互斥） | 大 |
| 5 | 新文件 `ios/CarDashboardPhone/CarDashboardPhone/DevicePhotos.swift`（或并入 `DeviceFaces.swift`） | 无 | 设备照片清单/能力模型 + 解析（Foundation-only，便于 macOS 单测）；沿用 `DeviceFaces` 的"缺省 vs 0""解析失败返回 nil"约定 | 中 |
| 6 | `ios/CarDashboardPhone/CarDashboardPhone/ContentView.swift` | 1150 行 | ①删/修死入口（`907-911` 的 NavigationLink 在 Button 闭包内）；②`faceTitle` 补 `photo` 中文；③连接状态指示复用 `connectionIndicator`；④若上传需要"设备页"入口，改导航结构 | 小 |
| 7 | `ios/CarDashboardPhone/CarDashboardPhone/DeviceFaces.swift` | 259 行 | `FaceDisplay.faceTitle` 补 `photo`；若清单模型放这里则加类型 | 小 |
| 8 | `ios/CarDashboardPhone/CarDashboardPhone/Info.plist` + `project.yml` | 54 / 85 行 | **BLE 方案：零改动**。若加本地网络/Bonjour/后台续传：补对应键；改工程配置必须改 `project.yml`（`.xcodeproj` 是生成物，见 README:91-93） | 视通道 |
| 9 | `ios/CarDashboardPhone/CarDashboardPhone/CarDashboardPhone.entitlements` | 空 dict | 仅当重新用 `NEHotspotConfiguration` 才需加 entitlement | 视通道 |
| 10 | 新测试 `tools/test_pgi_format.swift`（+ `tests/Makefile` 目标） | 无 | 编/解码往返 + 与 `tests/fixtures/*.pgi` 字节级对照 + 分片传输纯逻辑（序号/窗口/重传） | 中 |
| 11 | `ios/CarDashboardPhone/README.md` | 136 行，**已过时**（只写 3 个 Tab、无照片页、无新增文件） | 更新文件表/界面结构/照片流程 | 小 |
| 12 | `docs/ble.md` / `docs/photo-gif-design.md` / `PROGRESS.md` | — | 新协议/新流程落文档（Lead 负责） | 文档 |

**跨端前置依赖（不在 App 侧，但 App 做不了主）**：设备端必须先在固件里加
①照片清单/能力的上报（现在设备**不主动上报清单**，`docs/ble.md:361-363`）；
②文件删除/排序命令（现在只有 `photo_prev/next/play/get/slide/interval/shift/screenoff`，`docs/ble.md:334-360`）；
③大文件接收的帧协议。清单若没有，App 的"清单回读/删除/排序"无从做起。

---

## 7. 测试基础（iOS 侧）

### 7.1 现有可跑的测试

| 套件 | 类型 | 覆盖 | 能否 macOS 直跑 | 引用 |
|------|------|------|----------------|------|
| `tools/test_device_faces.swift`（71 项） | Swift，**编译 App 真实源文件** | 表盘解析/分组/本地化/未知兜底/缺省 vs 0 | ✅ `swiftc DeviceFaces.swift + test` | `tests/Makefile:168-172`；`PROGRESS.md:273` |
| `tools/test_ble_policy.swift`（19 项） | Swift，编译 `BLEConnectionPolicy.swift` | 自动连接决策穷举 16 组合 | ✅ | `tests/Makefile:181-188`；`PROGRESS.md:278` |
| `tools/test_brightness_curve.swift`（63 项） | Swift | 曲线协议 + 与固件 C 交叉验证 | ✅ | `tests/Makefile:221`；`PROGRESS.md:274` |
| `tools/test_ios_logic.swift`（59 项） | Swift，**内联复制** App 逻辑 | 遥测包契约 + 测速质量分档 | ✅（内联，会漂移） | `tools/test_ios_logic.swift:1-13`；`PROGRESS.md:279` |
| `test_nav_core/sim/ui_state/search_scope/map_codec/gateway_contract/keychain_token` | Swift | 导航/网关/令牌 | ✅ | `tests/Makefile:151-230` |
| `tests/test_pgi.c`（92 项） | C | `.pgi` 头/帧表/解码一致性/**跨语言夹具** | ✅（本机 C） | `tests/Makefile:8`、`:231-243`；`PROGRESS.md:267` |
| `tests/test_photo_slide.c`（54 项） | C | 轮播状态机/三开关/息屏计时 | ✅ | `PROGRESS.md:268` |
| XCUITest（5 个） | iOS 模拟器 | 会话/遥测计数/设备控制/链路诊断/BLE 状态 | 需模拟器 | `ios/CarDashboardPhoneUITests/*.swift` |

### 7.2 照片 / BLE 相关测试的缺口

| 缺口 | 事实 |
|------|------|
| ❌ 没有任何 App 侧 `.pgi` 编码测试 | `tools/` 下无 `PgiEncoder` / `PhotoStudio` / `CropGeometry` 测试（grep 为空）；`tests/fixtures/*.pgi` 全部由 `tools/pgi.py` 生成 |
| ❌ App 生成的 `.pgi` 与固件解码器**没有交叉验证** | `test_pgi.c` 只吃 Python 夹具（`tests/Makefile:231-243`）；而两者量化算法不同（§1.4） |
| ❌ 照片页无 XCUITest | 5 个 XCUITest 均不涉及照片 Tab（grep `照片/photo` 为空） |
| ❌ 无分片/传输层测试 | 现有 Swift 测试都是纯逻辑；地图窗口只有 `test_map_codec`（38 项，编码公式），没有"队列/重传"测试 |
| ⚠️ 可测性阻塞 | `PgiEncoder.swift` / `PhotoStudio.swift` 文件级 `import UIKit`（`PgiEncoder.swift:18-22`、`PhotoStudio.swift:23-29`），**macOS `swiftc` 无法编译** → 必须先把纯逻辑拆成 Foundation-only 文件，才能进 `tests/Makefile` 的 Swift 测试体系 |

---

## 8. App 侧必须改的 5 件事

1. **让 `.pgi` 真正可达并落盘**：现在 `PgiEncoder` 零调用点，App 只能出 GIF 到相册（`PhotoTabView.swift:406-485` 是死代码）。至少要能"生成 → 存 Files/分享/（后续）传输"，并把 8 MB / fps 这两个过时常量对齐设备现状（8 MB → 40 MB、fps 由帧时长推导）。
2. **重写照片页的状态机**：删掉 ~350 行 Wi-Fi 死代码，建立"未连接 / 已连接 / 转码中 / 上传中(进度+速率+ETA) / 失败可重试 / 成功"的显式状态，并把 BLE 连接态真实接入 UI（现在命令失败完全静默）。
3. **在 `BLEClient` 增加真正的分片传输层**：MTU 探测（`maximumWriteValueLength`）+ 分片 + 序号/偏移 + ACK/超时重传 + 窗口流控 + 进度 + 取消；骨架可复用地图队列的"回执驱动 + 定时器看门狗"，但必须加应用层确认，且与 2 Hz 遥测/地图窗口做通道互斥（否则会重演"队列溢出静默丢包"）。
4. **加"设备照片清单 / 能力"模型**：沿用 `DeviceFaces` 的 Read 主 + Notify 提示 + 解析失败重读 + 紧凑分页载荷 + `Optional` 区分缺省；并让轮播开关/间隔从"设备上报"派生，而不是本地乐观初值（`PhotoTabView.swift:65-67`）。这一项**依赖固件先实现上报**。
5. **收尾 UI 可见性缺陷**：删掉 `ContentView.swift:907-911` 那个永不渲染的"上传照片到仪表"死链；`FaceDisplay.faceTitle` 补 `photo` 中文（`DeviceFaces.swift:247-256`）；`README.md` 与现实对齐（当前还写着 3 个 Tab、无照片页）。

---

## 9. App 侧最大的不确定性

> 下面每条都会**改变方案形状**，建议按顺序做 spike。

| # | 不确定性 | 为什么不确定 | 验收阈值（建议） |
|---|----------|-------------|-----------------|
| 1 | **BLE bulk 真实吞吐** | 仓库里只有 1 KB/s 这个**串行 withResponse** 的地图窗口口径（`docs/photo-gif-design.md:97-110`），不是物理上限。若 withoutResponse + MTU 247/517 + 2M PHY 能到 10~20 KB/s，213 KB 单张 10~20 s 可行；若还是 1 KB/s，单张 3.5 min，动图不可行 | 真机 iPhone↔ESP32 传 1 MB，测 sustained KB/s、丢包率、是否影响遥测/亮屏；**≥10 KB/s 才算"能传静态图"**（transport 任务给结论） |
| 2 | **后台/锁屏能否跑完** | `bluetooth-central` 已声明（`Info.plist:42-46`），但没有 state restoration、没有 `beginBackgroundTask`、没有续传；iOS 在后台会降频/挂起，实际行为只能真机测 | 上传 5 MB 过程中锁屏/切后台/来电，验证：进度是否继续、断连后能否续、App 被杀后设备端是否残留半截文件 |
| 3 | **App 端转码的内存与耗时** | 现在一次性把 N 帧 CGImage（466² RGBA = 868 KB/帧）+ GIF 中间态 + 索引帧全放内存：38 帧 ≈ 33 MB（`PgiEncoder.swift:71-87`、`:148-183`），中低端 iPhone 可能 OOM/发烫/耗时数十秒 | Instruments 测峰值内存 < 设备安全线、单张 < 3 s、10 帧 < 15 s；失败要能降级（缩尺寸/减帧） |
| 4 | **跨端清单/能力契约未定** | 设备**目前不上报照片清单**（`docs/ble.md:361-363`），也没有删除/排序命令；App 的"清单回读 → 删除/排序"完全依赖固件新增特征与命令，且要挤进 184 B 上限 | 固件给出分页清单协议草案后再实现 App；先用固定夹具跑 Swift 解析测试，再做真机联调 |
| 5 | **`.pgi` 格式是否要动（全局调色板/RLE/尺寸档）** | 每帧 1 KB 调色板在 BLE 上就是 1 KB 空口时间；RLE 能把 213 KB 压到 ~106 KB（`docs/photo-gif-design.md:89-96`、`pgi.h:72`，未实现）。格式一改，App 编码器 + 固件解码器 + 测试夹具三方同时动 | Lead 先定"传输优先还是画质优先"；若走 BLE，建议**先上 RLE 或全局调色板**再谈上传 |

---

## 附录：关键数字速查

| 数字 | 值 | 出处 |
|------|-----|------|
| 设备屏幕 | 466×466 圆屏 | `PhotoStudio.swift:49-50`；`docs/photo-gif-design.md:14` |
| 单帧 `.pgi`（466²） | 218,180 B ≈ 213 KB（含 1 KB 调色板） | 格式实算；`pgi.h:39-41` |
| `.pgi` 8 MB 帧数上限（466²） | 38 帧 | `PgiEncoder.swift:54-59` |
| App 编码器体积上限 | 8 MB（过时；设备侧 SD 方案 40 MB） | `PgiEncoder.swift:41` vs `tools/import_photos.sh:48` |
| iOS BLE 有效载荷 | ≈184 B（协商 MTU ≈185） | `docs/ble.md:405-412`；`MapWindowPacket.swift:85-87` |
| 固件请求 MTU | 247 | `ble_telemetry.cpp:316` |
| 地图消息单条上限 | 180 B | `MapWindowPacket.swift:87` |
| 仓库 BLE 吞吐参考 | ≈1 KB/s（串行 withResponse） | `docs/photo-gif-design.md:99-104` |
| 照片页代码量 | 812 行，其中 ~350 行 Wi-Fi 死代码 | `PhotoTabView.swift` |
| BLE 客户端代码量 | 941 行 | `BLEClient.swift` |
| 现有 iOS 单测 | Swift 599 项（其中 0 项覆盖照片/传输） | `PROGRESS.md:279-283` |
