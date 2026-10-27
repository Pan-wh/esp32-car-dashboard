# 照片功能实施与真机验收（v5，BLE 直传）

> **状态**：已实施并通过真机验收（2026-10-27）。
> **设计背景**见 [`photo-wireless-design.md`](photo-wireless-design.md)；
> **冻结的线上协议**见 [`photo-protocol.md`](photo-protocol.md)。
> 本文只记录「实际怎么落的、验证过什么、还差什么」，避免设计稿被实施细节淹没。

## 1. 落地形态（一句话）

App 选图 → 手机裁切/编码成 `.pgj`（32 B 头 + baseline JPEG）→ BLE 分片上传（断点续传 + CRC32）
→ 设备写 SD 卡 `.incoming/*.part` → 校验后原子改名到 `/photos/` → 立即进相册/可播放。

- 静态照片：466×466 全彩 JPEG，设备用 ESP32-S3 ROM TJpgDec 解码到 PSRAM（约 200–260 ms/张，仅切图时）。
- 动图：`.pgi` 索引帧链路保留，但当前板实测 **3–4 fps**，稳定判定为 **LITE**（只传静态照片）。
- 旧的 `.pgi` 素材、`tools/import_photos.sh`（SD 卡批量导入）仍然兼容；只是新增素材默认走 BLE。

## 2. 代码落点

| 层 | 文件 | 说明 |
|---|---|---|
| 共享纯逻辑（C，带单测） | `simulator/ui/pgj.*`、`photo_list.*`、`photo_cmd.*`、`photo_sync.*` | 格式/清单/命令/接收状态机；`tools/sync_firmware.sh` 同步进固件 |
| 固件 | `firmware/dashboard/ble_photo.cpp` | GATT 照片服务、命令队列、分片回调、credit/清单通知 |
| 固件 | `firmware/dashboard/photo_rt.cpp` | SD 扫描/排序、`.pgj`/`.pgi` 分派、JPEG 解码、上传落盘/续传、删除/播放/设置、格式化 |
| 固件 | `firmware/dashboard/jpeg_decode.c`、`font_photo_ui_26.c` | ROM TJpgDec 封装（RGB565）、照片页占位文案字模 |
| App | `ios/.../Photo/PhotoProtocol.swift`、`DevicePhotos.swift`、`PhotoTransfer.swift` | 协议/解析/纯上传引擎 |
| App | `ios/.../Photo/PGJEncoder.swift`、`PhotoEncoder.swift`、`PhotoCropView.swift`、`PhotoThumbnailCache.swift` | 编码、裁切、缩略图 |
| App | `ios/.../Photo/PhotoTabView.swift`、`BLEClient.swift` | 照片页 UI / BLE 直传与设备交互 |
| 测试/工具 | `tests/test_pgj.c` 等、`tools/test_photo_*.swift`、`tools/photo_probe.swift` | 单测 + 真机 BLE 探针（`--info/--ack/--format/--size/--save`） |

## 3. 构建与测试现状

| 项 | 结果 |
|---|---|
| 固件编译 | Flash **≈62%**、静态 RAM **≈49%**（Wi-Fi 上传整套已删除） |
| C 单测 | pgj / photo_list / photo_cmd / photo_sync 共 **1000+ 断言**，全绿 |
| Swift 单测 | 照片协议/模型/传输引擎 **573 项**，全绿 |
| 全量回归 | `cd tests && make run` 通过（含既有 C/Swift/Node/UI 像素回归） |
| iOS 构建 | 模拟器 + 真机均 `BUILD SUCCEEDED`（Xcode 26.5） |
| 真机 | iPhone 12 ↔ ESP32-S3：选图 → 上传 → 设备提交 → 相册/播放 全链路通过 |

## 4. 真机联调抓到并修掉的问题

这些问题**只在硬件上暴露**，是本次交付最值钱的部分：

| # | 现象 | 根因 | 修复 |
|---|---|---|---|
| 1 | App 只发现遥测服务，没有照片服务 | NimBLE 在**第一次广播**时才 `ble_gatts_start()`，之后注册的服务不会被发现 | `ble_telemetry_begin()` 只建 GATT；`ble_photo_begin()` 之后再 `ble_telemetry_advertise()` |
| 2 | 上传长度对、CRC 永远不符 | 环形缓冲写入用了 `tail % CAP`（应为 `head % CAP`），有未读数据时覆盖最老字节 | `photo_rt.cpp` 写指针改为 `head` |
| 3 | credit 发出后 App 仍卡死 | `evt_send()` 用 `ble_telemetry_connected()` 做门禁；NimBLE 连接判断会短时归零 → credit 永久丢失 | 去掉门禁 + credit 每 400 ms 重发当前水位 |
| 4 | App 上传 0%，重试也 0% | iOS 上 `canSendWriteWithoutResponse` 初始 false / `peripheralIsReady` 不回调，严格等待即死锁；重试时“首包放行”已被消耗 | 每次 `begin` 重置首包放行；并支持逐片 `WRITE`（带确认）作为确定性流控 |
| 5 | 上传到 25~33 KB 必卡，随后设备 10 s 超时暂停 | **上传中每 16 KB 调一次 `s_part.flush()`**：flush 本身仅 15 ms，但会把 BLE 链路带停（写入/确认随机停 10 s+） | 传输中只 `write` 不 flush；统一在暂停/断连/提交前 flush（`PHOTO_FLUSH_EVERY` 提到 4 MB 兜底） |
| 6 | App 选完照片弹「读不出来」 | 裁切位图按 3 字节/像素创建 `noneSkipLast` 上下文（应为 4 字节/像素），CGContext 创建失败 | `PhotoEncoder.squareCGImage()` 改为 4 B/像素 |
| 7 | 格式化把卡写成“不可挂载” | 直接 `esp_vfs_fat_sdcard_format()` 在坏卡上会中断/卡死 | 改为：先挂载（挂载失败时由 SD 驱动重建 FS），成功后**递归清空卡内全部文件/目录**，再重建目录 |
| 8 | 连上 BLE 但时钟不对，要点「开始驾驶」才校时 | 连接校时用 `.withoutResponse`，刚连接时被 iOS 静默丢弃 | 曾改为 `.withResponse` 连接后重发，但真机出现 RTC 反复写入/时钟跳变；**已回退为“开始驾驶后随遥测校时”**（最稳）。后续如要恢复需先解决 RTC/I2C 稳定性 |

## 5. UI 一轮优化（2026-10-27）

- **主操作重做**：照片页在「仪表状态」下新增醒目的「添加照片」卡片（图标 + 标题 + 一句说明 + 箭头），
  比右上角 `＋` 可发现性高得多；空相册文案同步改为"点上方「添加照片」"。
- **新增「格式化 SD 卡」**（高级）：二次确认弹窗明确“删除全部照片、无法恢复”；执行中显示进度行，
  成功 toast + 自动刷新清单/空间；失败按 `busy / not_ready / io` 给具体原因；正在上传时禁用（固件也会拒绝）。
- **格式化语义**：清空卡内全部文件并重建 `/photos`、`/photos/.incoming`；卡的文件系统损坏时会先重建。
  正常启动/普通挂载路径**绝不**自动格式化。

## 6. 验收数据与性能

- iPhone App → 设备：69,845 B 完整上传成功（设备日志 `✓ 已提交 p00022.pgj`，清单自动刷新并切到该图）。
- Mac BLE 探针（逐片带确认）：84,750 B 连续两次成功（CRC/清单/播放均通过）。
- **实测速率 ≈ 2.6 KB/s**（逐片 `WRITE`，每片最多 240 B）：85 KB ≈ 33 s，512 KB ≈ 3.5 min。
  `WRITE_NR` 在 iOS 上仍会随机丢包，因此 App 默认走带确认写。
- 动图门控要求“本次连接实测 ≥30 KB/s”，当前链路达不到 → 当前硬件稳定 LITE，符合预期。

## 7. 已知限制与后续

1. **速度**：带确认写只有 ~2.6 KB/s。后续可尝试“多片在途 + 更大分片（≤240 B）”或评估 iOS 上更可靠的
   WRITE_NR 流控方案；UI 已按实测速率显示 ETA。
2. **掉电/强杀**：上传中不周期 flush，最多丢最后一段未落盘数据；重连按文件实际大小续传，CRC 兜底
   （失败会整份重传）。对 512 KB 上限的静态图可接受。
3. **前台限制**：iOS 后台会挂起，上传必须保持 App 前台（UI 已提示）；断线重连后可续传。
4. **格式化**：是“清空内容 + 必要时重建文件系统”，不是低格/坏块检测；卡物理损坏时可能仍失败并返回 `io`。
5. **旧素材**：`.pgi` 只读兼容；`import_photos.sh` 保留，用于大批量 SD 导入。

## 8. 真机复现/验证工具

```bash
# 编译探针（macOS）
swiftc -O -o /tmp/photo_probe tools/photo_probe.swift

/tmp/photo_probe --info            # 读 PhotoInfo/PhotoState/清单
/tmp/photo_probe --ack             # 逐片带确认上传（复现 iPhone 路径）
/tmp/photo_probe --format          # 清空/重置设备 SD 卡（危险）
/tmp/photo_probe --size 200        # 生成 200×200 小图快速回归
```

设备侧日志关键行：`[照片] BLE 照片服务已就绪`、`开始接收 pXXXXX.pgj`、`✓ 已提交`、`上传超时/断连 → 已暂停`。
