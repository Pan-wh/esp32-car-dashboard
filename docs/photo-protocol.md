# 照片服务 BLE 协议（v1 · 两端契约）

> **状态**：**冻结**（2026-10-26）。固件与 iOS App 必须逐字段一致；改协议必须同时改本文、C 头文件、Swift 模型与测试。
> 设计背景见 [`photo-wireless-design.md`](photo-wireless-design.md) §8。
>
> **硬约束（先读）**
> - iOS 单次 **Notify / Read** 有效载荷约 **184 B** → 本文所有 JSON 必须 **≤180 B**（测试会卡）。
> - 命令走 `WRITE`（带响应）；照片数据**同时支持 `WRITE` 与 `WRITE_NR`**：
>   **iOS App 默认用逐片 `WRITE`（带响应）**：真机（iPhone 17 / iOS 17）上
>   `WRITE_NR` 会随机丢包且 `peripheralIsReady` 不可靠，实测卡在 0/11/25 KB；
>   带确认写每片一个响应，流控确定，实测 ~2.6 KB/s（85 KB ≈ 33 s）。
>   `WRITE_NR` 仍保留给未来/其他平台，设备侧两种都支持。
> - 设备 **不解析** 文件名里的信息；id 就是文件名（白名单 `[A-Za-z0-9_.-]{1,63}`，扩展名 `.pgj` / `.pgi`）。
> - 单位：`bytes` 是文件总字节数；`off` / `want` 是文件内偏移；`free_kb` / `max_kb` 是 KiB。

---

## 1. GATT

| 特征 | UUID | 属性 | 方向 | 用途 |
|------|------|------|------|------|
| **Photo Service** | `7D2A0010-8F4A-4B7C-9D21-5E6A3B8C1000` | — | — | 服务；**没有它 = 旧固件** |
| PhotoCtl | `7D2A0011-8F4A-4B7C-9D21-5E6A3B8C1000` | WRITE | App→设备 | JSON 命令（≤180 B） |
| PhotoData | `7D2A0012-8F4A-4B7C-9D21-5E6A3B8C1000` | WRITE + WRITE_NR | App→设备 | 二进制分片：`[u32 offset LE][payload]` |
| PhotoEvt | `7D2A0013-8F4A-4B7C-9D21-5E6A3B8C1000` | NOTIFY | 设备→App | JSON 事件（≤180 B） |
| PhotoInfo | `7D2A0014-8F4A-4B7C-9D21-5E6A3B8C1000` | READ | 设备→App | 能力/档位（≤180 B） |
| PhotoState | `7D2A0015-8F4A-4B7C-9D21-5E6A3B8C1000` | READ + NOTIFY | 设备→App | 状态/设置（≤180 B） |

- App 连接后：`read PhotoInfo` → `read PhotoState` → `{"t":"p","op":"list"}`。
- `{"t":"p","op":"format"}`：**清空/重置 SD 卡**（危险操作，App 必须二次确认）。设备先尝试挂载（挂载失败时由 SD 驱动重建文件系统），再递归删除卡内全部文件，最后重建 `/photos` 与 `/photos/.incoming`。完成回 `{"t":"pe","ev":"fmt","ok":1}`，失败回 `err`（`busy`=正在上传、`not_ready`=无卡、`io`=挂载/清理失败）。
- `PhotoState` 变化时设备 Notify（**限流 ≥200 ms**），App 收到后重新 Read（不依赖 Notify 载荷内容，与 Faces 同一套模式）。
- `PhotoEvt` 只承载短事件；清单走 `pq/pf/pz` 分页。

---

## 2. PhotoInfo（READ，能力/档位）

```json
{"t":"pi","v":1,"tier":1,"cap":7,"px":466,"max_kb":512,
 "anim":{"px":300,"fps":8,"frames":12,"max_kb":2048,"fps_meas":4},
 "bench":{"dec_ms":230}}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `t` | string | 固定 `"pi"` |
| `v` | int | 协议版本，当前 `1` |
| `tier` | int | `1`=LITE（仅静态），`2`=FULL（可传/播动图） |
| `cap` | int | 位掩码：bit0 静态 JPEG、bit1 动图 `.pgi`、bit2 删除、bit3 内置存储 |
| `px` | int | 静态照片边长上限（466） |
| `max_kb` | int | 单张静态文件上限（KiB） |
| `anim.px` | int | 动图边长（300） |
| `anim.fps` | int | 动图目标帧率（8） |
| `anim.frames` | int | 动图最大帧数（12） |
| `anim.max_kb` | int | 动图文件上限（KiB） |
| `anim.fps_meas` | int | **上次实测动图帧率**（0 = 没播过） |
| `bench.dec_ms` | int | 最近一次 JPEG 解码耗时（ms，0 = 未测） |

> App 渲染动图入口 = `cap&2` && `tier==2` && `anim.fps_meas>=8` && （本次/上次实测上传 ≥30 KB/s 或还没测过）。任一条不满足都要显示原因。

## 3. PhotoState（READ + NOTIFY）

```json
{"t":"ps","cur":3,"n":12,"stor":1,"free_kb":5230,
 "slide":1,"intv":30,"shift":1,"soff_s":1800,"rx":0}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `t` | string | `"ps"` |
| `cur` | int | 当前播放的照片下标（0 起，`n==0` 时为 -1） |
| `n` | int | 照片数量 |
| `stor` | int | `0` 未就绪 / `1` SD / `2` 内置 FFat |
| `free_kb` | int | 可用空间（KiB） |
| `slide` | int | 自动轮播 0/1 |
| `intv` | int | 轮播间隔（秒） |
| `shift` | int | 像素位移 0/1 |
| `soff_s` | int | 自动息屏（秒，0 = 关） |
| `rx` | int | `0` 空闲 / `1` 正在接收上传 |

## 4. PhotoCtl 命令（App → 设备，JSON）

| JSON | 语义 | 设备回应 |
|------|------|---------|
| `{"t":"p","op":"begin","kind":0,"bytes":45210,"crc":282593821,"w":466,"h":466,"frames":1,"fps":0}` | 开始上传。`kind` 0=PGJ 静态、1=PGI 动图；`crc` = **整个文件**的 CRC32（PGJ 的 crc 字段按 0 计，见 §7） | `go` 或 `err` |
| `{"t":"p","op":"end"}` | 数据发完，请求校验提交 | `done` 或 `err` |
| `{"t":"p","op":"abort"}` | 放弃本次（删 `.part`） | 无（或 `changed`） |
| `{"t":"p","op":"del","id":"p00012.pgj"}` | 删除照片 | `changed` 或 `err` |
| `{"t":"p","op":"play","i":3}` | 切到第 3 张并钉住 | `changed`（`cur` 变化） |
| `{"t":"p","op":"set","slide":1,"intv":30,"shift":1,"soff_s":1800}` | 设置任一子集，写 NVS | `changed` |
| `{"t":"p","op":"list"}` | 请求清单分页 | `pq/pf/pz` |
| `{"t":"p","op":"bench"}` | 自检（若当前有静态图则测解码耗时；回报 `fps_meas`） | `info` |

- 未知字段忽略；未知 `op` → `err{code:"bad_arg"}`。
- `begin` 的 `kind`=1 仅在 `cap&2` 时允许，否则 `err{code:"unsupported"}`。

## 5. PhotoEvt 事件（设备 → App，JSON）

| JSON | 语义 |
|------|------|
| `{"t":"pe","ev":"go","off":0,"chunk":178,"win":4096,"id":"p00012.pgj"}` | 可以开始发数据。`off`=期望起始偏移（续传时 >0）；`chunk`=建议单片 data 上限；`win`=允许在途字节数 |
| `{"t":"pe","ev":"credit","off":4096}` | 已**落盘**到 `off`，App 可继续发 |
| `{"t":"pe","ev":"done","id":"p00012.pgj"}` | 提交成功，可播放 |
| `{"t":"pe","ev":"err","code":"crc","want":0}` | 失败；`want` 可选（`offset` 类错误给出期望偏移） |
| `{"t":"pe","ev":"changed"}` | 清单/当前项/设置变了 → App 重新 Read `PhotoState` + 请求 `list` |
| `{"t":"pe","ev":"info"}` | 能力/自检结果变了 → App 重新 Read `PhotoInfo` |
| `{"t":"pq","n":12}` | 清单开始，共 12 项 |
| `{"t":"pf","i":0,"id":"p00012.pgj","b":45210,"f":1,"k":0,"w":466,"h":466,"fps":0}` | 一项（`i` 从 0 起；`b`=字节；`f`=帧数；`k`=0 PGJ / 1 PGI；`w/h`；`fps`） |
| `{"t":"pz"}` | 清单结束 |

**错误码（`code`）**：`busy` / `no_space` / `too_large` / `unsupported` / `crc` / `offset` / `io` / `not_ready` / `bad_arg`。

## 6. PhotoData 分片（二进制）

```
字节 0..3   u32 offset（little-endian，文件内偏移）
字节 4..N   payload（原始字节）
```

- App 每片 `payload` 长度 = `min(chunk 建议值, iOS 写长 - 4)`；iOS 写长用
  `maximumWriteValueLength(for: .withoutResponse)`（通常 178）。
- 设备要求 `offset == 期望偏移`；否则 `err{code:"offset","want":N}`，App 从 `N` 续发。
- 无逐片 ACK；BLE 链路层负责重传；**文件级 CRC32** 兜底。
- 流控：`在途字节 = sent - acked ≤ win`；`canSendWriteWithoutResponse == false` 时等
  `peripheralIsReady`；6 s 无 credit → 重新 `begin` 续传。

## 7. `.pgj` 文件格式（32 B 头 + 1 张 baseline JPEG）

```
偏移  长度  字段
  0     4   magic "PGJ1"
  4     2   width  u16        (1..466)
  6     2   height u16        (1..466)
  8     1   version = 1
  9     1   flags             bit0=1: baseline JPEG（必须为 1）
 10     2   quality u16       (10..100，仅记录)
 12     4   jpeg_bytes u32    (= 文件大小 - 32)
 16     4   crc32 u32         (整个文件的 CRC32；**计算时本字段自身按 0 计**)
 20     4   created_unix u32  (0 = 未知)
 24     8   reserved          (全 0)
```

- **CRC32 覆盖整个文件**（32 B 头 + JPEG 载荷），但**计算时 `crc32` 字段（偏移 16..19）按 0 计**——
  否则就成了"校验值包含自己"的不可能循环。因此固定口径：
  `begin.crc == header.crc32 == crc32(header[0..15] ‖ 00 00 00 00 ‖ header[20..31] ‖ jpeg)`。
  两端必须一致（固件 `pgj_crc32`、App `PhotoProtocol.fileCRC32`、测试夹具都在校验这条）。
- 设备在**扫描**时只校验 magic / version / 尺寸 / `jpeg_bytes` 自洽（不读全文件）；
  在**提交**时读回全文件按上述口径算 CRC32 + 用 `esp_jpeg_get_image_info()` 校验 SOF 尺寸。
- 只接受 **baseline** JPEG（progressive → `unsupported`）。

## 8. `.pgi` 动图（FULL 档）

沿用现有 `simulator/ui/pgi.h` 容器，一帧调色板 + 索引。上传时 `kind=1`、`frames>1`、`fps>0`。
设备端播放路径不变（索引解码）。

## 9. 文件名 / 存储

| 项 | 规则 |
|----|------|
| 目录 | `/photos/` |
| 新文件 | 设备分配：`p%05u.pgj`（kind 0）/ `p%05u.pgi`（kind 1），序号存 NVS |
| 兼容旧文件 | 任意合法名字的 `.pgi`（SD 导入的老素材）照常显示 |
| 上传临时文件 | `/photos/.incoming/<id>.part` + `<id>.meta`（JSON：bytes/crc/kind，用于续传） |
| 提交 | 校验通过后 `rename` 到 `/photos/<id>`；失败删 `.part` |
| 删除 | 只允许 `/photos/` 内、白名单扩展名 |

## 10. App 侧状态机（供 iOS 实现与测试）

```
idle → encoding → queued → uploading ──credit/done──▶ verifying ──▶ done
                     │           │
                     │           ├─ 断连 → waitingReconnect（重连 begin，off>0 续传）
                     │           ├─ 用户取消 → abort
                     │           └─ err → failed（可重试，crc 自动整份重传一次）
                     └─ busy / no_space → paused（提示）
```

- 进度 = `acked / bytes`；速率 = 最近 2 s 滑动平均；ETA = 剩余 / max(速率, 8 KB/s)。
- 编码产物先写 App 沙盒临时文件（`tmp/photo-upload/<sha1>.pgj`），续传从文件读，不重复编码。

## 11. 测试要求（两端都要）

| 端 | 必测 |
|----|------|
| 固件 | `test_pgj`（头/CRC/SOF 校验/坏文件）、`test_photo_list`（排序/白名单/分页 JSON <180B）、`test_photo_cmd`（命令解析/缺字段/坏 JSON）、`test_photo_sync`（偏移/续传/超时/提交） |
| App | `test_photo_models`（Info/State/Manifest 解析 + 缺省 vs 0 + 坏 JSON）、`test_photo_transfer`（分片/窗口/credit/续传/错误路径）、`test_pgj_encoder`（头字节级与 C 实现一致） |

---

> **合规基线（全项目适用）**：本项目仅用于**个人开发、学习与技术验证** ——
> **不装车、不商业化、不分发**。项目使用高德开放平台 Web 服务，其
> **服务协议 §3.4 禁止用于车载场景**；若将来需要装车、分发或商业化，
> 必须先取得书面授权或更换数据源。详见 [`nav-backend.md`](nav-backend.md) §0。
