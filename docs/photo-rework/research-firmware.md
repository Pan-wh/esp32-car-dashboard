# 固件侧照片链路 + BLE 通道容量审计（只读，2026-10-26）

> **范围**：`firmware/dashboard/`（Arduino sketch + 共享纯 C 模块）、构建配置与产物、本机 Arduino/IDF 工具链里的 JPEG 能力。
> **只读**：未改任何代码、未编译、未上机。
> **引用格式**：仓库内文件写 `path:line`（行号对应当前工作区）；仓库外工具链写绝对路径；文档写 `docs/xxx.md §n` 或 `:line`。
> ⚠️ **构建产物是旧的**：`firmware/dashboard/build/esp32.esp32.esp32s3/` 里 `.bin` 为 **2026-10-14**，而 `dashboard.ino` 是 **2026-10-20**（照片表盘 `b5c1670` 之后）。
> 因此 **Flash 占用、分区、sdkconfig 这三类数字都要分「取自旧产物」与「取自文档最新实测」两种口径**，见 §7 与各表。

---

## 0. 一句话结论

**照片链路本身没有"设计缺陷"，它是被两条硬约束夹住的：**
① 1-bit SDMMC 读速 2.37 MB/s（物理上限）→ 满屏 `.pgi` 一帧 218 KB 要 93~104 ms；
② 内部 RAM 是唯一的真墙：文档口径 **104~120 KB 是「不开 Wi-Fi」** 的读数，
而**当前固件开机就开 Wi-Fi 热点**（`dashboard.ino:3368`）——postmortem 实测 AP 一开只剩 **22~24 KB**、
手机连上后 **2~4 KB**（**真实起点待用 `heap` 复核，见附录 B-1**）。

**通道侧**：BLE 是 **NimBLE**，`setMTU(247)` 后 iOS 协商约 185（载荷 ~184），
**没有任何 PHY / 连接间隔 / Data Length 配置**，四个特征全是 `WRITE`（**带响应**），
唯一的多包先例是"地图窗口拆成 40~50 条小 JSON、收到 `me` 才原子切换"，
实测有效吞吐只有 **~1 KB/s**（往返受限，不是物理上限）。

**最关键的正面发现（直接影响方案选型）**：**ESP32-S3 的 ROM 里带 TJpgDec**
（`jd_prepare`/`jd_decomp` @ `0x40000858`/`0x40000864`），而 **`esp_jpeg` 1.3.1 已经预编译在 Arduino 核心里**
（include 与 `-l` 都在默认编译参数里）→ **设备端解 JPEG 的 Flash 代价 ≈ 0~2 KB，零安装**。
代价是速度：官方基准 320×180 用 ROM 解码 **52 ms**，按像素线性外推到 466×466 约 **200~260 ms/帧**
（→ 静态照片完全够用，动图最高 3~5 fps，**不会比现在的 3~4 fps 更好**）。

---

## 1. BLE 现状

### 1.1 协议栈：**NimBLE**（不是 Bluedroid）

| 事实 | 证据 |
|------|------|
| 构建配置明确是 NimBLE | `firmware/dashboard/build/esp32.esp32.esp32s3/sdkconfig:978` `CONFIG_BT_NIMBLE_ENABLED=y`；`:977` `# CONFIG_BT_BLUEDROID_ENABLED is not set` |
| 代码注释也写 NimBLE，并解释了断连回调不能同步重启广播的原因 | `firmware/dashboard/ble_telemetry.cpp:293`（"本固件底层是 NimBLE（sdkconfig CONFIG_BT_NIMBLE_ENABLED）"） |
| BLE 库本体是 esp-nimble-cpp 分支 | `…/esp32/3.3.10/libraries/BLE/src/BLEDevice.h:4`（"esp-nimble-cpp, NimBLE-Arduino contributors"） |
| ⚠️ `ble_telemetry.h` 顶部注释写"依赖 Arduino BLE（Bluedroid）"——**已过时**（实际 NimBLE） | `firmware/dashboard/ble_telemetry.h:4` vs `sdkconfig:978` |
| 版本基线 | Arduino-ESP32 **3.3.10**（`build.options.json` 的 `fqbn` / `tools/esp32-arduino-libs` → `versions.txt`：**esp-idf v5.5.4**，Lua… 见 §6.1） |

> 结论：**设计文档里凡是写 Bluedroid 的地方都要改**；NimBLE 的 API（`ble_att_set_preferred_mtu`、`ble_gap_update_params`、`ble_gap_set_prefered_phy`）才是可用的那一套。

### 1.2 MTU / PHY / 连接间隔

| 项 | 事实 | 证据 |
|----|------|------|
| 代码请求的 MTU | `BLEDevice::setMTU(247)` | `firmware/dashboard/ble_telemetry.cpp:316` |
| 该调用的真实语义 | 走 NimBLE 的 `ble_att_set_preferred_mtu(mtu)`（**本地偏好**，最终 = min(对端请求, 本地)） | `…/libraries/BLE/src/BLEDevice.cpp:652-674` |
| 编译期默认 MTU | `CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU=256`（被运行时 247 覆盖） | `sdkconfig:1048` |
| **iOS 实际协商** | **约 185**（单次 Read/Notify 有效载荷 **约 184 B**） | 代码注释 `ble_telemetry.cpp:31`；`docs/ble.md:209-213`、`docs/ble.md:405-412` |
| App 侧自己的单包预算 | 地图消息硬限 **180 B** | `ios/CarDashboardPhone/CarDashboardPhone/Nav/MapWindowPacket.swift:85-87` |
| **2M PHY** | **完全没配置**（固件 grep 不到 `phy`），但**底层可调**：Arduino 的 BLE 封装没有 PHY API（`BLEDevice.h`/`BLEAdvertising.h` 里 grep 不到），而 NimBLE 本体的 `ble_gap_set_prefered_le_phy()` 在 IDF 5.5 头里存在，且 NimBLE 头文件在默认 include 路径上（`flags/includes` 含 27 个 `bt/host/nimble` 目录）。编译期 5.0 特性已开（`CONFIG_BT_NIMBLE_50_FEATURE_SUPPORT=y`） | 固件 grep 结果；`sdkconfig:1081`；`…/esp32s3-libs/3.3.10/include/bt/host/nimble/nimble/nimble/host/include/host/ble_gap.h:3675,3728`；`…/flags/includes` |
| **连接间隔** | **完全没配置**。`BLEServer::updateConnParams(...)` 这个 API 存在但从未被调用 → 间隔由 iOS 自己定 | API：`…/libraries/BLE/src/BLEServer.h:175,200`；固件侧无调用 |
| Data Length Extension | 未配置 | `sdkconfig` 无 `BT_CTRL_BLE_DATA_LEN` 相关项 |

> **这两个"没配置"是重要的机会点也是风险点**：bulk 传输要提吞吐，必须自己在连接建立后
> `updateConnParams()`（例如 15 ms）+ 请求 PHY（2M）；但这两件事在本仓库**没有任何先例**，
> 属于新增风险，必须 Spike。

### 1.3 特征与写入回调（4 个特征，实际 5 个 UUID）

| 特征 | 类型 | 谁写/读 | 回调怎么处理 | 证据 |
|------|------|---------|--------------|------|
| Telemetry `…0002` | **WRITE**（带响应，无 notify） | App → 设备 | `TelemetryCallbacks::onWrite`：`getValue()`→`handle_chunk()`（512 B 重组缓冲 + 括号配平切 JSON，分流 nav→地图→亮度→遥测） | `ble_telemetry.cpp:266-276`、`:176-230`、`:97-157` |
| Status `…0003` | READ + **NOTIFY**（带 CCCD） | 设备 → App | `setValue()` + `notify()`；内容很短（状态字符串） | `:330-336`、`:472-478` |
| Command `…0004` | **WRITE** | App → 设备 | `CommandCallbacks::onWrite`：`control_parse()` → `portENTER_CRITICAL` 存一条 `latest-wins` 命令，硬件副作用留给主循环 | `:237-262`、`dashboard.ino:2211-2214` |
| Faces `…0005` | READ + **NOTIFY** | 设备 → App | **Read 是正途**（Read Blob 与 MTU 无关）；Notify 只当"变了"的提示（会被 MTU 截断） | `:343-350`、`:480-512`（注释 `:27-36`） |

> **没有任何 `WRITE_NR`（写无响应）特征**。写无响应在本库是支持的
> （`…/libraries/BLE/src/BLECharacteristic.h:168` `PROPERTY_WRITE_NR`），只是现有代码没用 ——
> 这正是 §9 硬约束 3 的根源：**现在的 4 个特征都不适合 bulk**。

### 1.4 有没有 bulk / 分片先例？—— 有"多小包"，**没有"分片"**

| 事实 | 证据 |
|------|------|
| 地图窗口 ~2.2 KB，**明确决定不引入分片层**：一条道路/建筑/折线各发一条小 JSON，都远小于单包 | `docs/ble.md:96-110` |
| 消息：`mw` 开始 / `mr` 道路 / `mb` 建筑 / `mp` 折线 / `me` 结束（收到 `me` 才切换显示） | `docs/ble.md:107-130` |
| App 发送方式：**一条一确认**（`.withResponse` + `didWriteValueFor` 推进），40+ 条不能直发（会静默丢前面的） | `ios/.../BLEClient.swift:670-754`、`:601-615` |
| 固件侧收：`handle_chunk()` 允许"一包多对象"与"一对象多包"，累积到括号配平才解析 | `ble_telemetry.cpp:159-230` |
| 重组缓冲上限只有 **512 B** | `ble_telemetry.cpp:39` `ASM_BUF_MAX 512`、`:72` `char s_asm[512]` |
| 接收缓冲是**静态分配、开窗/提交双缓冲**（约 5.4 KB，不走堆） | `firmware/dashboard/nav_map.h:14-27` |
| 累积超时按"**无消息**计时 5 s"（不是从 `mw` 起算）—— 踩过坑 | `nav_map.h:60-66`、`nav_map.c:25-38` |
| 实测耗时：2.2 KB / 50 条 / **2~3 秒** → 有效 **≈0.7~1.1 KB/s** | `docs/photo-gif-design.md:99`、`docs/ble.md:98` |
| 唯一"发数据给设备"的 bulk 结论 | `docs/photo-gif-design.md:97-110`（据此否定 BLE 传图） |

> ⚠️ 这个 1 KB/s 是**串行 withResponse 的往返受限值**，**不是 BLE 物理上限**。
> 1M PHY、连接间隔 30 ms、每事件 4 包 × 182 B 的**理论上限约 24 KB/s**；
> 能跑到多少必须 Spike（本次审计无硬件，无法实测）。

### 1.5 BLE 栈占多少内部 RAM（有间接答案，没有直接日志）

| 项 | 数字/事实 | 证据 |
|----|-----------|------|
| NimBLE 内存池**只能用内部 RAM** | `CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_INTERNAL=y`（`EXTERNAL`/`DEFAULT` 都关） | `sdkconfig:990-992` |
| Host 任务栈 | **5120 B** | `sdkconfig:996` |
| MSYS 缓冲块（静态，算在 BLE 开销里） | 12×256 + 24×320 = **10,752 B** | `sdkconfig:1065-1068` |
| 控制器任务/缓冲 | 无文档数字，未测 | —— |
| BLE 与主循环**不在同一个核** | NimBLE host 与 controller **pin 到 core 0**；Arduino `loop()` 在 **core 1** | `sdkconfig:993-995`、`:1230-1232`、`sdkconfig:611` `CONFIG_ARDUINO_RUNNING_CORE=1` |
| 已有"含 BLE 的正常水位" | 空闲堆 **104 KB**（不开 Wi-Fi）/ 长稳 **105,640 B** 恒定 / 另一轮验收 **120,320~120,348 B** | `docs/wifi-upload-postmortem.md:87`、`docs/ble.md:523`、`docs/test-results.md:157`、`docs/deployment.md:206` |

> **推论（重要）**：BLE 是开机就 init 的（`dashboard.ino:3315`），所以上面 104~120 KB 的读数**已经把 BLE 算进去了**。
> "BLE 栈占多少"没有单独数字，但可以说明 **BLE 常驻并没有把内部堆吃到危险水位**；
> 真正把内部堆打穿的是 **Wi-Fi AP**（见 §3.1）。

### 1.6 线程模型与"回调里绝对不做的事"（现有约定，新传输层必须继承）

| 约定 | 证据 |
|------|------|
| 回调**只解析 + 暂存**；`setBrightness`/`displayOff`/LVGL 一律回主循环做 | `ble_telemetry.h:12-13`、`ble_telemetry.cpp:59-60`、`:103-106`、`:248-251` |
| 共享状态用 mutex / `portMUX` 保护；取锁带超时 | `:86-95`（50 ms）、`:429-436` |
| 回调里**不打印共享重组缓冲**（会与下一次写入竞态） | `:110-115` |
| 回调里不重启广播（NimBLE host 状态未就绪会卡死广播） | `:290-301`、`:367-381` |
| 潜在硬限制：**CPU0 的 idle 任务被 5 s 任务看门狗盯着**，且 INT WDT 300 ms | `sdkconfig:2175-2179`、`:2172-2174` |

> 也就是说：**在 BLE 回调里做阻塞 IO（写 SD 一整块、等 flash 擦除）不仅会拖慢 BLE，还可能直接触发 CPU0 看门狗复位。**

---

## 2. 照片链路：从 `/photos/*.pgi` 到屏幕

### 2.1 完整路径（按调用顺序）

```
[进入照片表盘 page == UI_PAGE_PHOTO]
  photo_poll()                      dashboard.ino:659   ← 主循环每拍调用一次
   ├ 若首次：photo_scan_dir()       dashboard.ino:423   ← 读每个文件的「头+帧表」(32+4096 B) 取 frame_count/fps
   │        photo_slide_set_files()  photo_slide.c:52
   ├ 亮度封顶 180（进表盘一次）       dashboard.ino:693-696
   ├ 自动息屏判定                    dashboard.ino:699-702（photo_slide_should_screen_off）
   ├ photo_slide_tick()              photo_slide.c:155   ← 只推进下标，返回 true=内容变了
   ├ photo_slide_state()             photo_slide.c:129   ← 算出此刻应显示 (file, frame, shift)
   ├ 像素位移相位（每 120 s 换一档）  dashboard.ino:715-719
   ├ 帧没变 → 直接 return            dashboard.ino:728-731  ← 修掉"静态图每秒白读 3 次"的 bug
   ├ 换文件 → photo_open_file()      dashboard.ino:602    ← 读头+帧表并校验
   ├ 读帧表项取 (off,size) 并校验 size == 1024+w*h   dashboard.ino:746-784
   ├ 分片读：每次 32 KB                dashboard.ino:786-799（PHOTO_READ_CHUNK，dashboard.ino:345）
   ├ 读满 → img_indexed_decode8() 解到**后台**缓冲     dashboard.ino:809-812（img_indexed.c:21）
   ├ 交换 show_idx + lv_img_set_src(指向那块 PSRAM)   dashboard.ino:833-837（ui_photo.c:91-110）
   └ s_photo_loading = -1（下一拍开始下一帧的读）      dashboard.ino:841
[LVGL 在 loop 末尾 lv_timer_handler() 里真正重绘/flush] dashboard.ino:4073；flush 43 ms（PROGRESS.md:195）
```

**关键点**：所谓"分片预读"并不是异步预取下一帧；它只做了两件更简单的事：
① 一帧的读被摊成 **7 次 loop**（218 KB / 32 KB），② 读的过程中屏幕上仍然是上一帧，
于是**读时间与上一帧的显示时间重叠**。代码里预留的 `s_photo_next_load_ms`（`dashboard.ino:362`）
和 `s_photo_frame_off`（`:361`，只写不读）**是死变量**，真正的"按时间窗口预取"从未实现。

### 2.2 内存与数据结构

| 项 | 大小/取值 | 证据 |
|----|-----------|------|
| 显示双缓冲 | 2 × 466×466×2 B = **2 × 434,312 B**（`ps_malloc`，PSRAM） | `dashboard.ino:636-637`、`:346` |
| 帧暂存区（`[调色板 1024][索引 w*h]`） | **218,180 B**（`ps_malloc`，PSRAM） | `dashboard.ino:639-640` |
| 三者合计 | **1,086,804 B ≈ 1.04 MiB**，懒分配（第一次真要显示照片才分配） | `dashboard.ino:632-651`、`:3355-3357` |
| 分配失败行为 | 打印所需 KB 并**关闭照片表盘**（不崩） | `dashboard.ino:641-648` |
| 文件名表 | `s_photo_names[64][64]` **4 KB** 静态（`:372`）+ `photo_poll()` 内 `static names[64][64]` 又是 **4 KB**（`:667-668`）+ `metas[64]` → 约 **8 KB** 静态 | `dashboard.ino:372`、`:667-668`、`photo_slide.h:35` |
| 目录扫描栈缓冲 | `PGI_HEADER_BYTES + 512×8 = **4,128 B**`（**在栈上**，出现在 5 处函数里） | `dashboard.ino:447`、`:501`、`:568`、`:614`、`:907` |
| 循环任务栈 | **8192 B**（4,128 B 的栈数组是"还能跑但别再加"的状态） | `sdkconfig:612` |
| 设置持久化（NVS `photo` 命名空间） | 5 个键：`slide/shift/soff/soffms/intv` | `dashboard.ino:386-420` |
| 状态机本体 | `photo_slide_t` ≈ 64×2 B + 若干 int ≈ 数百 B | `photo_slide.h:64-80` |

### 2.3 时序成本（真机实测 vs 推算）

| 环节 | 数字 | 来源 |
|------|------|------|
| SD 读一帧 218 KB | **93 ms**（推算）/ **约 104 ms**（真机平均，含开销） | `docs/photo-gif-design.md:419`、`PROGRESS.md:223` |
| 解码 466×466 8bpp→RGB565 | **平均 21 ms / 最慢 23 ms** | `docs/photo-gif-design.md:411`、`PROGRESS.md:224` |
| 全屏 flush（QSPI 80 MHz） | **43 ms** | `PROGRESS.md:195`、`:204` |
| 合计（理论 6.3 fps） | 约 159 ms | `docs/photo-gif-design.md:422` |
| **实测帧率（满屏动图）** | **3~4 fps** | `PROGRESS.md:226`、`docs/photo-gif-design.md:14` |
| 每拍时间预算（代码自述） | 每次 `photo_poll` 最多读 32 KB + 至多解码一次 | `dashboard.ino:656-658` |
| 32 KB 读耗时 | 约 **13~14 ms**（32 KB / 2.37 MB/s） | 由 `docs/photo-gif-design.md:409` 推算 |

> 实测 3~4 fps 比"理论 6.3 fps"更差，说明还有约 100 ms 的额外开销没在算式里
> （屏幕始终在重绘、loop 里其它阶段、以及已知的偶发 1000 ms 渲染尖刺 `PROGRESS.md:311`）。

### 2.4 `/photos/` 的规范性约束（代码强制的，写设计时按这个列）

| 约束 | 值 | 证据 |
|------|----|------|
| 目录 | 固定 `/photos` | `dashboard.ino:343` |
| 扩展名 | 必须 `.pgi`（大小写不敏感），文件名长度 ≥ 5 | `dashboard.ino:436-437` |
| 文件个数上限 | **64**（超出忽略） | `photo_slide.h:35`、`dashboard.ino:670` |
| 文件名长度 | ≤ 63 字符（`char[64]`） | `dashboard.ino:357`、`:372` |
| 帧数 | 1..**512**（1 = 静态，>1 = 动图） | `pgi.h:68`、`pgi.c:52-53` |
| 单边像素 | ≤ **1024**，且**实际帧缓冲只按 466×466 分配**（更大直接 `LOAD ERROR`） | `pgi.h:70`、`dashboard.ino:346`、`:774-777` |
| fps | ≤ 60（0 视为无效播放） | `pgi.c:56`、`:116-118` |
| flags | **bit0（RLE）为 1 直接拒绝**（v1 不支持 RLE） | `pgi.c:55` |
| 每帧长度 | **必须严格等于** `1024 + w*h`（否则 `LOAD ERROR`） | `dashboard.ino:767-773`、`pgi.c:83-87` |
| 文件总大小 | **播放侧不校验**：`PHOTO_MAX_FILE`（8 MB，`dashboard.ino:344`）定义了但**从未被引用**；上限只存在于上传端点（8 MB，`:872`）与素材脚本（默认 40 MB，`tools/import_photos.sh:48`） | grep 结果 + 两处引用 |
| 坏文件 | 头解析失败 → **直接不进入清单**（`photo_scan_dir` 不计数）；头 OK 但帧长度不符 → 停在该文件显示 **`LOAD ERROR`** 占位，**不会自动跳过到下一个** | `dashboard.ino:458`、`:769-772`、`ui_photo.c:120` |
| 清理 | `photo clean` 删除"头解析失败 **或** 大小与头不符"的文件 | `dashboard.ino:547-599`、`:2887-2888` |
| 排序 | ⚠️ **没有排序**：按 FAT 目录迭代顺序（`openNextFile()`）——文档说"文件名排序"（`docs/photo-gif-design.md:188`）**未实现** | `dashboard.ino:432-466`（无 `qsort`/比较） |
| 像素位移 | 每 120 s 换相位，幅度 ±3 px，露出的边是纯黑 | `dashboard.ino:715-719`、`ui_photo.h:33`、`ui_photo.c:39-59` |

---

## 3. 内存账

### 3.1 现状（内部 RAM 是唯一的真瓶颈）

| 资源 | 数字 | 来源 |
|------|------|------|
| **内部堆·不开 Wi-Fi** | **约 104 KB** | `docs/wifi-upload-postmortem.md:87` |
| 内部堆·长稳（UI 全开） | **105,640 B 恒定** | `docs/ble.md:523` |
| 内部堆·另一轮验收 | **120,320~120,348 B** | `docs/test-results.md:157`、`docs/deployment.md:206` |
| 静态 RAM（`.data+.bss`） | **126,188 B（38%）** | `docs/deployment.md:134` |
| **内部堆·开热点（无客户端）** | **22~24 KB** | `docs/wifi-upload-postmortem.md:88` |
| 内部堆·手机连上热点 | **2~4 KB** | `docs/wifi-upload-postmortem.md:89` |
| 当前固件是否开热点 | **开机即开**（`wifi_start_ap()` 在 `setup()` 里无条件调用） | `dashboard.ino:3368` |
| PSRAM 空闲（导航/转置实验时） | **约 6.52 MB** | `docs/rotation-hardware-vs-software.md:191` |
| PSRAM 空闲（文档口径） | 6.4 MB | `docs/wifi-upload-postmortem.md:91` |
| PSRAM 大户：烘焙图充气 | 约 **1.4 MB** | `docs/photo-gif-design.md:84` |
| PSRAM 大户：LVGL 全帧绘制缓冲 | 466×466×2 = **434,312 B** | `dashboard.ino:2549-2552` |
| PSRAM 大户：照片三块缓冲 | **1.04 MiB**（懒分配） | `dashboard.ino:636-640` |

> **必须先说清的一个事实**：`104 KB` 的读数来自"不开 Wi-Fi"的那一版；
> 但从 `f5709db`（2026-10-19）起固件**开机就开 AP**（`dashboard.ino:3368`）。
> 若 postmortem 的 22~24 KB 水位在今天的固件上仍然成立，
> **那么"当前运行时内部堆 ≈ 22~24 KB"才是设计要面对的真实起点** ——
> **这一点本次未上机验证，请 Lead 定为第 0 号 Spike（`heap` 命令一条即可，`dashboard.ino:2871-2882`）。**
> 同时这也是最大的机会：**照片改走 BLE 后，Wi-Fi AP 可以整个删掉，一次性还回约 80 KB 内部 RAM。**

### 3.2 若新增「BLE 接收 + SD 写 + 校验」，各要多少、放哪

| 用途 | 建议大小 | 放哪 | 理由 / 现有先例 |
|------|---------|------|----------------|
| 单包落地缓冲（回调内 memcpy 目标） | **256~512 B** | **内部 RAM（静态）** | 现有 `s_asm[512]` 就是静态内部 RAM（`ble_telemetry.cpp:72`）；NimBLE 的 MSYS 块本身也是内部 RAM（`sdkconfig:990,1065-1068`） |
| 接收环形缓冲 / 整段暂存 | **30~80 KB**（单张 JPEG）或 **218 KB**（`.pgi` 单帧） | **PSRAM** | 现有 `s_photo_stage` 用 `ps_malloc`（`dashboard.ino:640`） |
| 解码/显示输出 | **434,312 B**（已有两块） | **PSRAM** | `dashboard.ino:636-637` |
| esp_jpeg 工作缓冲 | **3.1 KB**（默认；可选 65.5 KB 换速度） | 内部 RAM（库内部 `heap_caps_malloc`） | `…/espressif__esp_jpeg/include/jpeg_decoder.h:54-55`；ROM 固定 `JD_SZBUF=512`（`…/rom/tjpgd.h:9`） |
| SD 写分块 | **2~4 KB/拍**（4~8 KB 也可，代价是单拍 +25~50 ms） | PSRAM 可行 | `bench_sd()` 就是用 `ps_malloc(4096)` 写的 SD 并测出 0.16 MB/s（`dashboard.ino:1255`、`:1274-1278`） |
| 校验（CRC32） | **0**（ROM 函数） | —— | `…/ld/esp32s3.rom.api.ld:4` `esp_rom_crc32_le`；SHA256 则要 mbedtls（Flash/RAM 另计） |
| 任务栈（若新开写盘任务） | ≥ 4 KB | 内部 RAM | 现有 loop 栈 8 KB（`sdkconfig:612`），已放了 4,128 B 栈数组 |

> **一句话**：**缓冲都能放 PSRAM，唯一必须留在内部 RAM 的是"回调里那一小段落地缓冲"和"库内部 3.1 KB 工作缓冲"。**
> 按这个分工，新增传输层对内部堆的净需求是 **≈4~8 KB 量级**，不是几十 KB。
> 真正的开支是**并发存在 Wi-Fi 时**（见 §3.1）。

### 3.3 BLE 写回调里能做什么、不能做什么（现有代码的约定，逐条）

| 允许 | 证据 |
|------|------|
| 读 `c->getValue()` 拷贝出字节 | `ble_telemetry.cpp:270-274` |
| 纯计算解析（手写 JSON、无动态分配） | `:97-157`、`nav_map.c:1-10` |
| 写一个**已被 `portMUX`/mutex 保护**的暂存结构 | `:86-95`、`:252-256` |
| 设置"有新数据"的 `volatile` 标志 | `:76`、`:81`、`:234-235` |

| 禁止 | 原因 / 证据 |
|------|-------------|
| 碰 LVGL / 显示驱动 | 架构约定 `ble_telemetry.h:12-13`、`ble_telemetry.cpp:248-251` |
| 同步重启广播 | 会卡死 NimBLE host（`ble_telemetry.cpp:290-301`） |
| 大栈数组 | Host 任务栈只有 **5120 B**（`sdkconfig:996`），而现有解析路径的栈数组是 4,128 B —— **回调里绝不能再放 4 KB 级数组** |
| 阻塞式 SD 读/写、长 `delay` | CPU0 的 idle 被 5 s 任务看门狗监控（`sdkconfig:2175-2179`），单次 ≥5 s 直接 panic 重启；且 BLE host/controller 与看门狗都在 core 0 |
| 长时间持锁 | 现有取锁都带 10~50 ms 超时（`:88`、`:386`、`:404`） |

---

## 4. SD 写入：现在有哪些路径、多快、会不会卡 UI

### 4.1 现有写路径（全部是"命令/上传触发"，**正常播放时不写**）

| 路径 | 行为 | 阻塞？ | 证据 |
|------|------|--------|------|
| `POST /upload`（Wi-Fi） | 1 KB 一块流式写，最多等 **120 s**，写完三重校验，不合格**当场删** | **完全阻塞主循环**（在 `wifi_poll()`→`http_serve_one()` 里同步跑完整个上传） | `dashboard.ino:1077-1111`、`:1085-1086`、`:3923` |
| `sdbench [KB]` | 4 KB 一块写+读测试 | 阻塞（串口命令，可接受） | `dashboard.ino:1244-1309` |
| `pgibench` | 在 `/photos/` 生成合成 `.pgi`（写 30 帧 × 218 KB） | 阻塞（串口命令） | `dashboard.ino:1315-1386` |
| `photo clean` / `POST /delete` | 删文件 | 阻塞很短 | `dashboard.ino:593-598`、`:1030` |
| 播放/轮播 | **只读，不写** | —— | `photo_poll()` 全流程无写调用 |

### 4.2 写速与阻塞量级

| 项 | 数字 | 来源 |
|----|------|------|
| SD 写速（1-bit SDMMC） | **0.16 MB/s** | `docs/photo-gif-design.md:410` |
| 写一张 218 KB `.pgi` | ≈ **1.36 s** 纯 IO | 计算（218,180 / 0.16 MB/s） |
| 写一张 60 KB JPEG | ≈ **0.38 s** 纯 IO | 同上 |
| 写 8 MB 上限文件 | ≈ **50 s** | 计算；也是 `http_upload` 里 120 s 超时的来由（`dashboard.ino:1085-1086`） |

**对 UI 的影响**：因为上传/`sdbench` 都在主循环里同步执行，
**整个写盘期间 `loop()` 一帧都不会跑** → `photo_poll`、`lv_timer_handler`、`ble_poll`、
触摸消费全部停摆（它们都在 `loop()` 里：`dashboard.ino:3914-4079`）。
屏幕会**整段冻结**，并且 `delay(5)`+心跳也一起停（`:4080`）。
唯一的"不受影响"的部分是 100 Hz 触摸采样（esp_timer 独立，`:2577-2581`）与 LVGL tick（`:2585-2588`）——
它们会继续累计，恢复后补跑。

> 结论：**"边收边写"必须做成"每拍写一小块"的分片模式**，直接复用
> `photo_poll` 的 `PHOTO_READ_CHUNK` 那套写法（`dashboard.ino:786-799`）最省事，
> 这是仓库里**唯一被真机验证过的非阻塞 IO 结构**。
>
> 量化一下（0.16 MB/s = 6.25 B/ms）：每拍写 **2 KB → +12 ms**、**4 KB → +25 ms**、**8 KB → +50 ms**。
> 分片写**不是免费的**：60 KB 的 JPEG 那 ≈0.38 s 的 SD 时间一定会摊到若干拍里。
> 建议按「UI 仍 ≥30 fps」反推上限（每拍 ≤25 ms ⇒ ≤4 KB），并把它做成可调常量。

---

## 5. 主循环节拍

| 项 | 数字 | 证据 |
|----|------|------|
| loop 频率 | **195~199 次/秒**（长稳实测） | `docs/ble.md:523`、`docs/deployment.md:209` |
| 一拍（空闲） | **约 5 ms**（由末尾 `delay(5)` 决定；`CONFIG_FREERTOS_HZ=1000` 时下限就是 5 ms） | `dashboard.ino:4080`、`sdkconfig:2369` |
| 一拍（有渲染） | 5 ms + 渲染耗时，全屏 flush 时 **≈48 ms** | 计算（`PROGRESS.md:195`） |
| 每拍做的事（按阶段打点） | 串口 / BLE 控制 / 表盘检测 / 斜坡 / 行程 / BLE 轮询 / 触摸 / 渲染 | `dashboard.ino:205-208`、`:3914-4079` |
| 渲染 | `lv_timer_handler()` 在每拍末尾**同步执行**；全屏 flush 43 ms | `dashboard.ino:4073`、`PROGRESS.md:195` |
| LVGL tick | `esp_timer` **2 ms** 周期 | `dashboard.ino:87`、`:2585-2588` |
| 触摸采样 | `esp_timer` **100 Hz**（10 ms），**独立于主循环** | `dashboard.ino:2577-2581` |
| 诊断 ISR | **1 kHz**，只打时间戳 | `dashboard.ino:3340-3344` |
| BLE 轮询 | 每拍：`ble_poll` + `ble_telemetry_poll` + `ble_update`（后者 UI 限频 50 Hz） | `dashboard.ino:4047-4049`、`:3837-3847` |
| 照片 | 每拍最多 32 KB 读 + 至多 1 次解码 | `dashboard.ino:656-658` |
| 心跳打印 | 每 5 s，**默认被 `dbg_on()` 闸门关掉**（实测这 3 条 printf 曾是 ~1 s 阻塞源） | `dashboard.ino:4085`、`:4096-4099`、`:4128-4134` |
| NVS 写 | 仅在设置变化时（照片开关/间隔） | `dashboard.ino:410-420` |
| 已知尖刺 | 偶发单帧 **1000~1035 ms**，落在 `lv_timer_handler()` 内，未定位 | `PROGRESS.md:311` |

**"上传大文件不卡 UI"的现成可复用模式（3 个，按优先级）**

1. **分片 IO + 每拍一片**：`photo_poll()` 的 `s_photo_got / s_photo_need` 状态机
   （`dashboard.ino:786-799`）—— 最贴合本次需求，直接把"读"换成"写"即可。
2. **累积-原子提交 + 无消息超时**：`nav_map` 的 `mw…me` / 5 s
   （`nav_map.h:60-66`、`nav_map.c:25-38`）—— 适合"先收完整、校验通过再落盘"。
3. **回调只暂存、主循环执行**：`CommandCallbacks` + `control_poll()`
   （`ble_telemetry.cpp:237-262` → `dashboard.ino:2211`）—— 适合"BLE 回调不能做重活"这条铁律。

**反面教材（不要复用）**：`http_upload()` 的"循环里 `delay(1)` 直到收完"
（`dashboard.ino:1081-1099`）—— 它就是 UI 冻结的直接原因。

---

## 6. JPEG 解码可行性（关键项）

### 6.1 本机工具链里到底有什么

| 检查项 | 结果 | 证据 |
|--------|------|------|
| Arduino 核心版本 | **3.3.10** | `firmware/dashboard/build/esp32.esp32.esp32s3/build.options.json`（`hardwareFolders`） |
| 对应 ESP-IDF | **v5.5.4**（`esp32-arduino-libs` = `idf-release_v5.5-…`） | `/Users/pwh/Library/Arduino15/packages/esp32/tools/esp32s3-libs/3.3.10/versions.txt:2`、`…/include/esp_common/include/esp_idf_version.h:14-18` |
| **`esp_jpeg` 组件** | **已预编译存在：1.3.1** | `…/esp32s3-libs/3.3.10/versions.txt:23`（`espressif__esp_jpeg: 1.3.1`） |
| 头文件 | `…/espressif__esp_jpeg/include/jpeg_decoder.h`（`esp_jpeg_decode()` / `esp_jpeg_get_image_info()`） | 同路径 |
| 静态库 | `…/lib/libespressif__esp_jpeg.a`，**34,336 B** | `ls -l` 实测 |
| 是否在默认 include 路径 | ✅ `-iwithprefixbefore espressif__esp_jpeg/include` | `…/esp32s3-libs/3.3.10/flags/includes`（单行参数列表，`espressif__esp_jpeg/include` 是其中一项） |
| 是否在默认链接列表 | ✅ `-lespressif__esp_jpeg` | `…/flags/ld_libs` |
| 结论 | **`#include "jpeg_decoder.h"` + 调 `esp_jpeg_decode()` 即可，零安装、零额外依赖配置** | 以上四条 |
| 本机 `~/Documents/Arduino/libraries` | **不存在**（所以没有 TJpg_Decoder / JPEGDecoder 之类第三方库） | `ls` 报 `No such file or directory` |
| 项目自带库目录 | 只有 `GFX_Library_for_Arduino / SensorLib / XPowersLib / lvgl / Mylibrary / …`，**无 JPEG 库** | `reference/ESP32-S3-Touch-AMOLED-1.75/examples/arduino/libraries/` |
| 硬件 JPEG 解码器 | **没有**：`soc_caps.h` 里 grep 不到 `JPEG`（`esp_driver_jpeg` 只有头、无 lib） | `…/include/soc/esp32s3/include/soc/soc_caps.h`（grep 空）、`…/lib/` 无 `esp_driver_jpeg` |

### 6.2 ROM 里带 TJpgDec（这是最省 Flash 的一条路）

| 事实 | 证据 |
|------|------|
| ROM 导出 `jd_prepare` / `jd_decomp`（TJpgDec 的两个公开函数） | `…/esp32s3-libs/3.3.10/ld/esp32s3.rom.ld:90-95`（`jd_prepare = 0x40000858`、`jd_decomp = 0x40000864`） |
| ROM 版**固定配置**：输入流缓冲 **512 B**、输出 **RGB888**、缩放使能、饱和表使能 | `…/include/esp_rom/esp32s3/include/esp32s3/rom/tjpgd.h:9-12` |
| `esp_jpeg` 在这块板子上就是 **ROM 的薄壳**：其 `.a` 只定义 `esp_jpeg_decode`/`esp_jpeg_get_image_info`/三个回调，**未定义符号是 `jd_prepare`/`jd_decomp`** | 用 `xtensa-esp32s3-elf-nm -u` 读 `libespressif__esp_jpeg.a` 的实测结果 |
| 因此解码器**代码体积 = 0**（在 ROM），壳的 `.text` ≈ **1.0~1.5 KB** | 同上（符号尺寸 `0x168+0x141+0xf7+0x50+0x18+0x12`） |
| RGB565 输出是**壳里逐像素转换**的（ROM 只出 RGB888）：反汇编可见 `(r>>3)<<11 | (g>>2)<<5 | (b>>3)` 的移位/掩码序列 | `libespressif__esp_jpeg.a` → `jpeg_decode_out_cb` 偏移 `0xd8-0x11a`（`objdump -d` 实测） |
| 上限 | 只支持 **baseline**（progressive 会返回 `JDR_FMT3 /* Not supported JPEG standard */`） | `…/rom/tjpgd.h:51` |
| 官方说明 | "Some microcontrollers have TJpg decoder in ROM, it is used, if there is" —— 列表含 **ESP32-S3**；ROM 配置不可改 | <https://components.espressif.com/components/espressif/esp_jpeg/versions/1.0.5~1/readme> |

### 6.3 代价：体积 / 内存 / 耗时

| 项 | 估计 | 依据 |
|----|------|------|
| **Flash 代价** | **≈0~2 KB**（ROM 0 + 壳 1~1.5 KB） | §6.2 |
| **输入** | 整幅 JPEG 必须**一次性在内存**（`indata/indata_size`，库内部回调只从内存顺序读） | `jpeg_decoder.h:40-45`、`:75`（"This function is blocking"） |
| 输入大小（466²，q80） | 约 **30~80 KB**（→ SD 读 **13~34 ms**） | 推算（对比：同尺寸 8bpp 索引 218 KB） |
| **输出** | RGB565 **434,312 B**（PSRAM，已有同尺寸双缓冲可复用） | 466×466×2 |
| **工作缓冲** | **3.1 KB**（默认）；若换 65.5 KB 可再快约 10% | `jpeg_decoder.h:54-55`；官方表格 JD_FASTDECODE=2 行 |
| **单帧解码耗时（官方基准）** | **320×180，ESP32-S3 @240 MHz，ROM 解码：52 ms**（RGB565 壳另加转换） | 官方组件文档速度表（同上链接） |
| **外推到 466×466** | 像素数 57,600 → 217,156 = **3.77×** → **约 196 ms（理想线性）**；考虑缓存/转换开销，**保守 200~260 ms** | 本节计算 + 官方基准 |
| 对比：现有 8bpp 解码 | **21~23 ms/帧** | `docs/photo-gif-design.md:411` |
| 对比：现有整帧读 | 93~104 ms | `PROGRESS.md:223` |

> ⚠️ **这个 200~260 ms 是外推值，不是本机实测**（本次不编译、不上机）。
> 但它是**保守**的：官方 52 ms 那行的 ROM 配置（RGB888、JD_SZBUF=512、FASTDECODE=0）
> 与本板运行时**完全一致**，唯一没算进去的是壳里逐像素 RGB565 转换
> （217 k 像素，最多几毫秒）。

### 6.4 三条路线的硬数字对比（给 Lead 做取舍用）

| 路线 | 单帧文件大小 | SD 读 | 解码 | 每帧总成本 | 画质 | 动图可行？ |
|------|------------|-------|------|-----------|------|-----------|
| **A. 现状：8bpp 索引 `.pgi`** | 218 KB | 93~104 ms | 21~23 ms | **~160 ms（实测 3~4 fps）** | 256 色 | ✅（现状） |
| **B. `.pgi` 改 RGB565 原始帧**（`PROGRESS.md:339` 已提到"SD 装得下"） | **434 KB** | ~183 ms | **0 ms**（直通） | **~226 ms** | **全彩** | ❌ 只适合静态 |
| **C. JPEG（ROM TJpgDec）** | **30~80 KB** | **13~34 ms** | **~200~260 ms** | **~215~295 ms** | **全彩** | ⚠️ 3~4 fps 上限（解码受限） |
| C'（可选）JPEG + 1/2 缩放输出（233×233） | 30~80 KB | 13~34 ms | 约 150~220 ms（缩放省有限，仍解全部 MCU） | 约 165~255 ms | 全彩但分辨率减半 | 同上 |

**结论（事实层面，不含取舍）**：
- **静态照片**：三条路线的成本差都在"每 30 秒一次"的尺度上，**完全无感**；
  C 的额外好处是**文件小 3~7 倍**（SD 卡容纳量与 BLE 传输时间都受益）。
- **动图**：**没有任何一条能超过现在的 3~4 fps**（B 更差、C 解码更慢）。
  这与用户"性能不够可以砍掉动图/视频"的表态一致 —— **砍动图后，解码耗时彻底不再是问题**。
- **B 路线的隐藏价值**：**零解码器、零新依赖、零 RAM 增量（复用现有双缓冲）**，
  只改 `.pgi` 的 `flags`/调色板约定。如果只是要"全彩静态照片"，它比 C 更简单；
  代价是单帧 434 KB（BLE 传输时间 ×5、SD 占用 ×2）。

---

## 7. Flash / 分区

| 项 | 数字 | 证据 |
|----|------|------|
| 物理 Flash | **16 MB** | `docs/hardware.md:25-26` |
| 应用分区 `app0` / `app1` | 各 **0x300000 = 3,145,728 B（3 MiB）** | `firmware/dashboard/build/esp32.esp32.esp32s3/partitions.csv:4-5` |
| `ffat`（9.875 MB，未使用） | 0x9E0000 | `partitions.csv:6`、`docs/deployment.md:108` |
| **当前占用（最新实测口径）** | **2,451,595 B = 77%** | `docs/ai-bootstrap.md:154`（真实编译输出）；`docs/deployment.md:106,133` 记 ~2,420,000（同一档） |
| **剩余** | **3145728 − 2451595 = 694,133 B ≈ 678 KiB** | 本节计算（与 77% 自洽） |
| ⚠️ 文档里"余量约 580 KB" | 与 77%/3 MiB 分区**算不出来**，应为旧数字 | `docs/photo-gif-design.md:83` |
| 静态 RAM（最新） | 126,188 B（38%） | `docs/deployment.md:134` |
| 旧构建产物（**不可当现状**） | `dashboard.ino.bin` = **1,812,304 B（2026-10-14）**；同期的文档记 `1,804,467 B (57%)` | `firmware/dashboard/build/esp32.esp32.esp32s3/dashboard.ino.bin`；`docs/rotation-hardware-vs-software.md:185` |

**新增功能的 Flash 估算**

| 新增项 | Flash | 说明 |
|--------|-------|------|
| JPEG 解码（ROM tjpgd + esp_jpeg 壳） | **≈0~2 KB**（且库已预编译、无需 vendor） | §6.2 |
| 新 BLE bulk 特征 + 回调 + 分片状态机 | **≈1~3 KB** | 与现有 `ble_telemetry.cpp`（511 行）同量级 |
| App 侧清单/删除端点 | ~1 KB | 现有 `http_list` 量级（`dashboard.ino:965-1016`） |
| **若删掉 Wi-Fi（AP + 裸 HTTP 服务）** | **省约 8~12 KB**（预估，未实测） | `http_*` 段共约 300 行 + `WiFiServer`；`docs/wifi-upload-postmortem.md:74-76` 记换掉 `WebServer` 时 Flash 78%→77%（约 30 KB 级） |
| 中文字模（若将来要中文 UI） | **百 KB 级，是真正的 Flash 风险** | `PROGRESS.md:316`、`docs/decisions.md:222` |

> **结论**：**Flash 不是本次的约束**（余量 ~678 KB，新增只吃掉 KB 级）。
> 唯一会让 Flash 变紧的事是"中文字模"和"把大图塞进 Flash"（后者已明确不做）。

---

## 8. 文档与代码不一致之处（写设计时别照抄）

| # | 文档说法 | 代码事实 | 影响 |
|---|---------|---------|------|
| 1 | `ble_telemetry.h:4` "依赖 Arduino BLE（Bluedroid）" | 实际 **NimBLE**（`sdkconfig:978`、`ble_telemetry.cpp:293`） | 选 API 会选错（PHY 要用 NimBLE 的 `ble_gap_set_prefered_le_phy()`，见 §1.2） |
| 2 | `docs/photo-gif-design.md:188` "顺序：文件名排序" | `photo_scan_dir()` **没有排序**，按 FAT 目录项顺序（`dashboard.ino:432-466`） | 多设备/多次拷贝后顺序可能不如预期 |
| 3 | `docs/photo-gif-design.md:83` "Flash 余量约 580 KB" | 按 77% × 3 MiB 应为 **约 678 KiB**（`docs/ai-bootstrap.md:154`） | 余量被低估 ~100 KB（结论不变：够用） |
| 4 | `docs/photo-gif-design.md:477` "设备端解 GIF/PNG/JPEG…软解 50~100 ms，不可行" | **JPEG 可走 ROM，代码代价 ≈0**；真实代价是 **200~260 ms**（比 50~100 ms 更慢，所以"动图不可行"的结论仍成立，但"要引入解码器"的**前提**是错的） | 若照原文，会误以为"JPEG 要 vendored 库 / 占 Flash" |
| 5 | 文档普遍假设"内部堆 104~120 KB" | 固件**开机就开 Wi-Fi AP**（`dashboard.ino:3368`），实际水位可能是 **22~24 KB** | **最需要复核的一条**（§3.1） |
| 6 | `dashboard.ino:361-362` 的 `s_photo_frame_off` / `s_photo_next_load_ms` 注释像是"时间窗预读" | **没有实现**：前者只写不读，后者完全未用 | 别以为"预读框架已经在了" |

---

## 9. 对设计有影响的 5 条硬约束

1. **内部 RAM 是唯一的真墙，而且很可能比文档更紧张。**
   不开 Wi-Fi 时约 104~120 KB，但**当前固件开机即开 AP**（`dashboard.ino:3368`），
   postmortem 实测 AP 开启只有 **22~24 KB**（`docs/wifi-upload-postmortem.md:87-89`）。
   → **任何"新增接收缓冲/任务栈"的方案，第一步都必须删掉 Wi-Fi**（顺带还回 ~80 KB），
   并把"运行时 `heap` 实测"作为第 0 号 Spike。缓冲本身放 PSRAM，内部 RAM 只留
   **回调落地 256~512 B + esp_jpeg 3.1 KB 工作缓冲** 这个量级（§3.2）。

2. **BLE 现在的 4 个特征都是"带响应写"，吞吐被往返锁死在 ~1 KB/s 量级。**
   `setMTU(247)` 只到 185/184 B；**没有 2M PHY、没有连接间隔配置、没有 WRITE_NR 特征**
   （§1.2、§1.3）。→ 若要"iPhone 无线传图"成立，必须新增
   **`WRITE_NR` 特征 + 序号/CRC + 累积-原子提交**，并在连接建立后主动
   `updateConnParams()`（`BLEServer.h:175`）+ 直接调 NimBLE C 接口请求 2M PHY
   （`ble_gap_set_prefered_le_phy()`，`ble_gap.h:3728`）。**这是本方案最大的未知量，必须最早 Spike**
   （理论 24 KB/s vs 实测 1 KB/s，差 20 倍）。

3. **BLE 回调里不能做阻塞 IO：不是"慢"，是会重启。**
   BLE host/controller 都在 core 0（`sdkconfig:993,1230`），CPU0 idle 被 **5 s 任务看门狗**
   监控（`:2175-2179`），host 任务栈仅 **5120 B**（`:996`），且 NimBLE 内存池只能用内部 RAM（`:990`）。
   → 回调只做 `memcpy` 到静态环缓冲 + 置标志；**SD 写、CRC、解码一律回主循环**，
   沿用现有"回调只暂存"的约定（`ble_telemetry.cpp:248-251`）。

4. **SD 写必须分片，每个主循环一片；现有非阻塞 IO 只有一套可抄的写法。**
   写速 **0.16 MB/s**（`docs/photo-gif-design.md:410`），一张 218 KB 要 1.36 s，
   现有上传路径是**整段同步**（`dashboard.ino:1081-1099`），期间 UI **完全冻结**。
   → 照 `photo_poll()` 的 `s_photo_got/s_photo_need` 状态机（`:786-799`）把"读"换成"写"，
   每拍 **2~4 KB（≈12~25 ms）**，并把"写完整 → 校验 → 提交（rename）→ 失败删除"。
   实测：32 KB/片的分片读是这套结构在真机上验证过的唯一形态。

5. **设备端 JPEG 解码在 Flash 上几乎免费，但在时间上比现有 8bpp 慢约 10 倍 —— 它只适合静态照片。**
   ROM 自带 TJpgDec（`…/ld/esp32s3.rom.ld:94-95`）+ 已预编译的 `esp_jpeg` 1.3.1
   （`versions.txt:23`、`flags/includes` 里的 `espressif__esp_jpeg/include`、`flags/ld_libs` 里的 `-lespressif__esp_jpeg`）→ **代码代价 ≈0~2 KB、零安装**；
   但官方 320×180 = **52 ms** 外推到 466² 是 **~200~260 ms/帧**，而现有 8bpp 解码是 **21~23 ms**。
   → 静态照片可以用 JPEG（画质全彩、文件小 3~7×、SD 读从 93 ms 降到 13~34 ms）；
   **动图无论用哪条路线都不会超过现在的 3~4 fps**，与"性能不够就砍动图"的用户表态一致。
   若要"零解码器"的全彩静态图，还有第三条路：**`.pgi` 直接存 RGB565（434 KB/帧，读 183 ms、解码 0 ms）**。

---

## 附录 A：关键数字速查

| 数字 | 含义 | 来源 |
|------|------|------|
| 466×466 / 217,156 px | 屏与满屏像素数 | `docs/hardware.md:38` |
| 2.37 MB/s | 1-bit SDMMC 读速（物理上限） | `docs/photo-gif-design.md:409` |
| 0.16 MB/s | SD 写速 | `docs/photo-gif-design.md:410` |
| 93 / 104 ms | 读一帧 218 KB | `docs/photo-gif-design.md:419`、`PROGRESS.md:223` |
| 21~23 ms | 8bpp 帧解码 | `docs/photo-gif-design.md:411` |
| 43 ms | 全屏 flush | `PROGRESS.md:195` |
| 3~4 fps | 满屏动图实测 | `PROGRESS.md:226` |
| 195~199 /s | 主循环频率 | `docs/ble.md:523` |
| ~5 ms | 一拍（空闲）耗时（`delay(5)` 下限） | `dashboard.ino:4080`、`sdkconfig:2369` |
| 32 KB | 每拍 SD 读上限 | `dashboard.ino:345` |
| ~185 B / ~184 B | iOS 协商 MTU / 有效载荷 | `docs/ble.md:209-213`、`:405-412` |
| ~1 KB/s | BLE 实测（withResponse 串行） | `docs/photo-gif-design.md:99` |
| ~24 KB/s | 1M PHY 理论值（推算，未验） | §1.4 计算 |
| 104~120 KB | 内部堆（不开 Wi-Fi） | `docs/wifi-upload-postmortem.md:87`、`docs/ble.md:523` |
| 22~24 KB | 内部堆（开 AP） | `docs/wifi-upload-postmortem.md:88` |
| 5120 B / 10752 B | NimBLE host 栈 / MSYS 缓冲 | `sdkconfig:996`、`:1065-1068` |
| 8192 B | loop 任务栈 | `sdkconfig:612` |
| 2,451,595 B (77%) | Flash 占用 | `docs/ai-bootstrap.md:154` |
| 694,133 B | Flash 余量（推算） | §7 |
| 3,145,728 B | app 分区大小 | `partitions.csv:4` |
| 52 ms @320×180 | ROM TJpgDec 官方基准 | 组件文档（§6.2 链接） |
| ~200~260 ms | 466² JPEG 解码（外推） | §6.3 |
| 3.1 KB | esp_jpeg 工作缓冲 | `jpeg_decoder.h:54-55` |
| 30~80 KB | 466² JPEG 文件（推算） | §6.3 |

## 附录 B：本次未能验证的项（建议做成 Spike，按优先级）

| # | 未验证项 | 怎么验（最低成本） |
|---|---------|------------------|
| 1 | **当前固件运行时的内部堆**（开 AP 后到底是不是 22~24 KB） | 串口 `heap`（`dashboard.ino:2871-2882`）+ `mem`（`:3176`）；一条命令，1 分钟 |
| 2 | **BLE bulk 真实吞吐**（`WRITE_NR` + MTU 185，是否接近 10~24 KB/s） | 加一个临时 `WRITE_NR` 特征，App 连发 100×180 B 计时；这是"能不能无线传图"的生死线 |
| 3 | **466×466 JPEG 的真实解码耗时**（外推 200~260 ms 是否成立） | 加 ~30 行：SD 读一张 JPEG → `esp_jpeg_decode()` → 串口打毫秒。**不需要改 UI** |
| 4 | **SD 写分片对 UI 的影响**（每拍 2~4 KB 时循环/渲染还剩多少） | 复用 `sdbench` 的写循环，改成"每拍写一片"，看 `[运行]` 心跳里的循环/渲染数字 |
| 5 | 2M PHY / connection interval 是否可协商（`updateConnParams` + `ble_gap_set_prefered_le_phy` 在 iOS 上是否被接受） | 连接后各调一次并打印 `ble_gap_conn_find` 的 desc（含 tx_phy/rx_phy/interval）；iOS 侧不需要改代码 |
