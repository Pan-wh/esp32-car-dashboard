# 开发指引 —— 「我要改 X，该动哪里」

> 面向二次开发。按**意图**检索，直接给出要改的文件、注意事项与验证方式。
>
> 配套文档：[`../PROGRESS.md`](../PROGRESS.md)（进度总览）｜
> [`test-results.md`](test-results.md)（测试台账）｜
> [`troubleshooting.md`](troubleshooting.md)（踩坑记录）｜
> [`ai-task-boundaries.md`](ai-task-boundaries.md)（**交给 AI 改代码时用这个**）

---

## 一、代码分层（先理解这个，再决定改哪里）

```
┌─────────────────────────────────────────────────────────────┐
│  simulator/ui/  共享层（纯 C，LVGL 但无硬件依赖）             │
│  ★ 单一源码：模拟器与固件共用同一份实现                       │
│    - 业务逻辑：telemetry.c / control.c / ui_face.c           │
│    - UI 绘制：ui_pages.c / ui_speed.c / ui_analog.c / ui_clock.c │
│    - 字模图片：font_*.c / ui_dial_faces.c / ui_driver_images.c │
└────────────────┬────────────────────────────┬───────────────┘
                 │ tools/sync_firmware.sh     │ 直接编译
                 ▼   （复制同名文件）          ▼
┌────────────────────────────┐   ┌────────────────────────────┐
│ firmware/dashboard/        │   │ simulator/main.c           │
│  - dashboard.ino（硬件初始化/  │   │  - SDL2 绑定、命令行、截图   │
│    主循环、BLE 装配）        │   └────────────────────────────┘
│  - ble_telemetry.cpp（BLE）  │
└────────────────────────────┘
                 │ BLE
                 ▼
┌─────────────────────────────────────────────┐
│  ios/CarDashboardPhone/   iPhone App（Swift）│
│  - 定位 → 质量门槛 → 打包 → BLE 发送          │
│  - 界面（SwiftUI）+ 仪表控制                  │
└─────────────────────────────────────────────┘
```

**关键规则**：`simulator/ui/` 里的一切都是**唯一源码**。
固件目录下的同名文件是 `sync_firmware.sh` 复制的产物（已 gitignore），
**不要直接改固件目录里的同名文件**——下次同步会被覆盖。

---

## 二、按意图查「改哪里」

### 🎨 A. 只改 UI（样式、布局、颜色、字体）

| 我想改 | 改哪个文件 | 备注 |
|--------|-----------|------|
| **速度表**外观（指针、表盘、HUD）| `simulator/ui/ui_speed.c` | 手写 524 行 |
| **导航卡片**（箭头/距离/车速）| `simulator/ui/ui_nav.c` | 手写；箭头用 lv_line 绘制 |
| 导航**纯逻辑**（动作/状态/距离格式化）| `simulator/ui/nav.c` | 76 项单测，可 PC 运行 |
| **模拟时钟**（刻度、指针）| `simulator/ui/ui_analog.c` | 手写 238 行 |
| **数字时钟**（字号、布局）| `simulator/ui/ui_clock.c` | 手写 116 行 |
| **页面切换**（动画时长、吸附）| `simulator/ui/ui_pages.c` | `PAGE_ANIM_MS` 控制动画 |
| **底盘颜色**（背景）| `ui_pages.c` 的 `UI_BG_COLOR` | |
| **表盘刻度/数字**（烘焙好的）| `tools/make_dial_faces.py` → 重新生成 | 见下方 B |
| **字体** | `tools/make_fonts.sh` → 重新生成 | 见下方 B |
| **驾驶员插画** | `tools/make_images.py` → 重新生成 | 3 态表情 |
| **App 图标** | `tools/make_app_icon.py` | 见 §D |

**改完 UI 必做**：
```bash
cd simulator && make -j8 && ./dashboard_sim          # 先看效果
cd tests && make run PYTHON=<python路径>              # 单测
python3 tools/ui_regression.py                       # 像素级回归（33 项）
tools/sync_firmware.sh                               # 同步到固件
```

> ⚠️ **LVGL 8 陷阱**：setter（含 `lv_meter_set_indicator_value`）
> **无条件 invalidate**，值没变也会标脏。UI setter 里**必须自己做变更检测**，
> 否则空闲时也会几十 Hz 重绘 → 全局卡顿（已踩坑，见 troubleshooting）。

---

### ➕ B. 新增一个 UI 表盘

**好消息：注册表驱动，只需 3 步。**

**第 1 步**：新建 `simulator/ui/ui_myface.c/.h`，实现一个构建函数：

```c
/* ui_myface.h —— 与现有表盘保持一致：用 lv_obj_t*，注册时转型 */
#include "lvgl.h"
lv_obj_t *ui_myface_create(lv_obj_t *parent);
```
```c
/* ui_myface.c */
#include "ui_myface.h"

lv_obj_t *ui_myface_create(lv_obj_t *parent)
{
    /* 在 parent 上构建你的界面，返回根对象（便于外部引用；也可返回 parent）*/
    return parent;
}
```

> 注册表里的回调类型是 `void (*)(void *parent)`（刻意做成 LVGL-free，
> 便于 `test_ui_face` 在 PC 上单测而不链接 LVGL）。注册时**转型**即可：
> `(ui_face_build_cb)ui_myface_create`。现有五个表盘都这么写。

**第 2 步**：在 `simulator/ui/ui_pages.c` 的 `ui_pages_init()` 里注册，并在
`ui_pages.h` 的 `ui_page_t` 枚举加一项（**顺序必须与注册顺序一致**）：

```c
ui_face_register(&(ui_face_desc_t){ "myface", "MyFace", (ui_face_build_cb)ui_myface_create, UI_FACE_GROUP_CLOCK });
```

> 第 4 个字段 `group`（`ui_face_group_t`）决定 App 里的模块分组，
> 取值与含义见 `simulator/ui/ui_face.h`。

**第 3 步**：同步 + 验证：
```bash
tools/sync_firmware.sh
cd simulator && make -j8 && ./dashboard_sim     # 滑动应能看到新表盘
cd tests && make run PYTHON=<python路径>          # test_ui_face 会检查注册表
```

**自动获得的能力**（无需额外代码）：
- ✅ 滑动翻页、松手吸附
- ✅ BLE 远程切表盘（`{"cmd":"face","value":N}`，-1 下一个 / -2 上一个）
- ✅ App 端下拉选择（表盘列表由**设备上报**，App 自动列出，无需改 App 代码）

> ✅ **App 端无需同步**：设备通过 `Faces` 特征（`7D2A0005`）上报表盘列表，
> App 从 `session.ble.faces.faces` 读取后渲染（见 `ContentView.swift` /
> `DeviceFaces.swift`）。新增表盘只改设备一处；App 未收录该 `name` 的本地化名时
> 用设备给的 `label` 兜底显示，因此不会"选不到"。

---

### 🔤 C. 改字体 / 表盘图片（生成类资源）

这些 `.c` 文件是**工具生成的，不要手改**：

| 生成物 | 生成工具 | 说明 |
|--------|---------|------|
| `font_clock_iphone_138.c` | `tools/make_fonts.sh` | 时钟大字体（已去抗锯齿 bpp=1）|
| `font_speed_num_150.c` | 同上 | 速度大数字 |
| `font_label_26.c` / `font_date_cjk_26.c` | 同上 | 标签/日期（含中文）|
| `ui_dial_faces.c` | `tools/make_dial_faces.py` | 烘焙表盘（模拟+速度），466×466 |
| `ui_driver_images.c` | `tools/make_images.py` | 驾驶员三态插画 |

**重新生成后务必**：
```bash
cd simulator && make -j8
python3 tools/ui_regression.py      # 像素回归会检查表盘关键区域
tools/sync_firmware.sh              # 同步到固件
```

> ⚠️ **为什么要烘焙成图片**？表盘刻度/数字用 LVGL 运行时绘制，
> 每帧都要重算 + 走 QSPI 传输，实测单帧 243ms。烘焙后只画指针 → 90-113ms。

---

### 📱 D. 改 iPhone App

全部在 `ios/CarDashboardPhone/CarDashboardPhone/`：

| 我想改 | 文件 | 说明 |
|--------|------|------|
| **界面布局/新增设置项** | `ContentView.swift` | SwiftUI 表单，778 行 |
| **BLE 连接/发送/控制命令** | `BLEClient.swift` | 708 行，含四个特征 UUID |
| **定位采集** | `LocationService.swift` | 104 行，精度/后台模式 |
| **数据质量门槛** | `TelemetryQuality.swift` | 45 行，**改判定条件看这里** |
| **打包格式（JSON 字段）** | `TelemetryPacket.swift` | 102 行，**改字段要同步固件解析** |
| **会话逻辑（2Hz 发送）** | `DriveSessionManager.swift` | 142 行 |
| **App 名称** | `project.yml` 的 `CFBundleDisplayName` | 当前「车载仪表」|
| **App 图标** | `tools/make_app_icon.py` → 重新生成 | 18 种尺寸 |
| **签名 Team** | `project.yml` 的 `DEVELOPMENT_TEAM` | 当前 `QFH3X6SMV4` |

**改完必做**：
```bash
cd ios/CarDashboardPhone
xcodegen generate          # ★ 改了 project.yml 才需要；改了 swift 不用
tools/run_ios_tests.sh     # 5 项 XCUITest（自动起定位场景）
```

**装到真机**：
```bash
export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
xcodebuild -project CarDashboardPhone.xcodeproj -scheme CarDashboardPhone \
  -destination 'platform=iOS,id=<设备UDID>' build
xcrun devicectl list devices                      # 查 UDID
xcrun devicectl device install app --device <UDID> <构建产物.app>
xcrun devicectl device process launch --device <UDID> com.cardashboard.CarDashboardPhone
```

> ⚠️ **改 JSON 字段必须两边同步**：`TelemetryPacket.swift` 的编码 key
> 与 `simulator/ui/telemetry.c` 的解析 key 必须一致，否则解析失败。
> 已有 107 项 telemetry 单测覆盖解析，改完跑 `make run` 即可发现。

> ⚠️ **SwiftUI 刷新陷阱**：视图观察的是 `DriveSessionManager`（`@StateObject`），
> 其子对象（`ble` / `location`）的 `@Published` 变化**必须显式转发**
> （见 `forwardChanges()`），否则界面不刷新（已踩坑）。

---

### 🔌 E. 改 BLE 协议

| 我想改 | 文件 |
|--------|------|
| UUID、特征定义 | `firmware/dashboard/ble_telemetry.cpp` |
| 遥测 JSON 解析 | `simulator/ui/telemetry.c`（+ `telemetry.h`）|
| 控制命令解析 | `simulator/ui/control.c`（+ `control.h`）|
| 协议说明文档 | `docs/ble.md` |
| App 侧发送 | `ios/.../BLEClient.swift` + `TelemetryPacket.swift` |

**当前协议速查**（详见 `docs/ble.md`）：

| 用途 | UUID 后缀 | 方向 |
|------|----------|------|
| Service | `7D2A0001-…` | — |
| Telemetry | `…0002` | 手机 → 仪表（Write）|
| Status | `…0003` | 仪表 → 手机（Read/Notify）|
| **Command** | `…0004` | 手机 → 仪表（Write）|
| **Faces** | `…0005` | 仪表 → 手机（Read/Notify，表盘列表上报）|

> ⚠️ **BLE 回调里不能碰硬件**：Bluedroid 任务不能操作显示。
> 回调只做「解析 + 存入变量」，硬件副作用放主循环（见 `ble_telemetry.cpp` 注释）。

> ⚠️ **不要复用 `--build-path`**：跨文件改动后复用旧构建目录可能**漏编译**
> （已踩坑，表现为"BLE 写入成功但计数为 0"）。用新的临时目录。

---

### ⚙️ F. 改仪表固件主流程

`firmware/dashboard/dashboard.ino`（约 2400 行）：

| 区域 | 内容 |
|------|------|
| `setup()` | 官方初始化流程（屏幕/触摸/RTC/IMU）|
| `loop()` | 主循环：串口、控制轮询、BLE 更新、斜坡推进、行程统计 |
| `ble_update()` | 状态机 + 指针驱动（**50Hz 限频**）|
| `ble_poll()` | 读取 BLE 暂存数据 |
| `control_poll()` | 处理下行控制命令（亮度/息屏/表盘）|
| `handle_serial()` | 串口命令 |
| `apply_brightness()` / `apply_screen()` | 硬件副作用 |

> ⚠️ **`lv_disp_drv_register()` 不能删**：曾因补丁误删导致
> `lv_scr_act()` 返回 NULL → 启动 panic 循环（需 BOOT+RESET 才能恢复）。

---

## 三、构建 / 运行速查

### 模拟器（推荐日常开发用，快）
```bash
cd simulator && make -j8
./dashboard_sim                                   # 带界面
./dashboard_sim --offscreen --ms 500 --page 2 --shot x.bmp   # 无头截图
```

### 固件
```bash
tools/sync_firmware.sh          # ★ 改过 simulator/ui/ 后必须先同步
arduino-cli compile \
  --fqbn "esp32:esp32:esp32s3:FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,USBMode=hwcdc,CDCOnBoot=cdc,PSRAM=opi" \
  --build-property "compiler.cpp.extra_flags=-DESP32QSPI_MAX_PIXELS_AT_ONCE=4096" \
  --libraries reference/ESP32-S3-Touch-AMOLED-1.75/examples/arduino/libraries \
  --upload -p /dev/cu.usbmodem101 firmware/dashboard
```

**编译参数为什么这样**：
- `PSRAM=opi` —— **必需**，S3R8 八线 PSRAM，用于全帧绘制缓冲
- `-DESP32QSPI_MAX_PIXELS_AT_ONCE=4096` —— 滑页性能优化，**不可省**
- `PartitionScheme=app3M_fat9M_16MB` —— 应用 3MB + 9.9MB 未用分区（可放资源）

### 测试
```bash
cd tests && make run PYTHON=<python路径>    # 全部单测
python3 tools/ui_regression.py             # 33 项像素回归
tools/run_ios_tests.sh                     # 5 项 XCUITest
```

### 串口命令
`time`/`set`（RTC 时间）、`page`/`pages`（切页/列出页面）、`map`（地图窗口诊断）、
`sd`（TF 卡状态）、`scan`（I2C 总线扫描）、`ble`（连接/遥测统计）、
`face`（切表盘）、`nav`（导航卡片模拟数据）、`demo`（演示速度）、`trip`（行程）、
`bright`/`screen`（亮度/息屏）、`lvrot`（临时切换方向）、
`mem`、`dump`、`help`。详见 [`../PROGRESS.md`](../PROGRESS.md) §11。

---

## 四、已验证可行的能力（可放心复用）

以下是**已经真机验证并通过测试**的，二次开发时可直接依赖：

### ✅ 显示与交互

| 能力 | 验证情况 |
|------|---------|
| 466×466 AMOLED 显示（CO5300 QSPI）| 真机，30 分钟长稳无异常 |
| CST9217 触摸 + 滑动手势翻页 | 真机，12 次连续 flick 全部成功 |
| 滑动吸附分页（速度 90-113ms/帧）| 真机实测（优化前 243ms）|
| PSRAM 全帧绘制缓冲 | 真机，脏区由 10 块降为 1 块 |
| QSPI 80MHz + 4096 像素分块 | 真机，全屏传输 56→43ms |
| 烘焙表盘图片（离线渲染刻度）| 真机，像素级回归 **33** 项通过 |
| 大字体去抗锯齿（bpp=4→1）| 真机，字模 194→51KB |
| LVGL 动画（扫表动画）| 模拟器逐帧验证 + 真机 |

### ✅ 功能

| 能力 | 验证情况 |
|------|---------|
| 五表盘（模拟钟/数字钟/速度表/多彩时钟/导航卡片）| 真机 |
| **导航卡片**（阶段1 UI 原型）| 模拟器像素级验证 + 真机；8 种转向动作、距离格式化 76 项单测 |
| 表盘注册表（新增表盘三步走）| `test_ui_face` 67 项单测 |
| BLE 远程切表盘 | 真机端到端 |
| 速度线性斜坡（消除跳针）| 107 项单测含帧率无关性验证 |
| 状态防抖（消除 STALE 闪烁）| 12 项单测 + 真机 |
| 五态链路显示 | 真机逐态实测 |
| 行程统计（里程/最高速/均速）| 107 项单测 + 真机 |
| 亮度 / 息屏控制 | 真机端到端 |
| **屏幕旋转 0/90/180/270°** | 真机端到端 + 模拟器像素级验证；NVS 持久化；非法值拒绝 |
| BLE 时间同步（首次校时）| 真机，40 秒零重校 |
| 驾驶员三态插画 | 真机，像素级验证 |

### ✅ 通信

| 能力 | 验证情况 |
|------|---------|
| BLE GATT 四特征（遥测/状态/命令/表盘列表）| 真机 4900+ 包，解析错 0 |
| 跨 MTU 重组长 JSON | 真机 |
| 后台 BLE（`bluetooth-central`）| 真机 75s 持续 2Hz |
| 后台定位（`allowsBackgroundLocationUpdates`）| 真机，锁屏仍持续 |
| 强退重开自动重连 | 真机，无需点按钮 |
| CarPlay 导航共存 | 真机，0 断连、0 次 LIVE 无增长 |
| 30 分钟整链路长稳 | 真机，0 断连、堆零漂移 |

### ⚠️ 已知限制（不要试图突破）

| 限制 | 原因 |
|------|------|
| 无法获取红绿灯/路线/转向 | 无公开 API，且 §5.1 禁止读 Apple Maps 私有数据 |
| 静止时数据速率约 1Hz | iOS 省电策略，非缺陷；行驶恢复 ≈2Hz |
| 切表盘 ~100ms/帧（≈10fps）| QSPI 传一屏 434KB 需 43ms，硬件下限 |
| 室内 GPS 无速度 | 物理限制（`speed<0` = 未知）|

### ❌ 未实现（可扩展方向）

| 方向 | 前提条件（已确认可用）| 工作量 |
|------|---------------------|-------|
| **屏幕旋转**（竖屏/横屏）| ✅ **已实现**：App/BLE 切换 0/90/180/270°，NVS 持久化。默认 0°；90°/270° 使用 MADCTL，接受少量边缘黑缺口以保证流畅度 | 已完成 |
| 图片背景 + 时钟（功能②）| 存储已验证：**有 TF/microSD 卡槽**（SDMMC 1-bit）+ **9.9MB 未用 ffat 分区**（0x610000）| 中 |
| 行程统计上屏 | 数据已有，仅需 UI（当前只在串口 `trip` 查询）| 小 |

---

## 五、常见陷阱速查

| 陷阱 | 后果 | 规避 |
|------|------|------|
| 改固件目录里的共享层文件 | 下次同步被覆盖 | 只改 `simulator/ui/`，然后 `sync_firmware.sh` |
| 改 `simulator/ui/` 后忘记同步 | 固件编译报 `telemetry_trip_t` 未定义等 | 编译前先 `sync_firmware.sh` |
| LVGL setter 不做变更检测 | 无效化风暴 → 全局卡顿 | setter 里比较新旧值 |
| 复用 `--build-path` | 漏编译（假成功）| 用新的临时构建目录 |
| BLE 回调里操作显示 | 崩溃/卡死 | 回调只存变量，主循环消费 |
| 删 `lv_disp_drv_register()` | 启动 panic 循环（需 BOOT+RESET 恢复）| 不要动初始化顺序 |
| 直接跑 `xcodebuild test` | LinkDiagnostics 报 3 项假失败 | 用 `tools/run_ios_tests.sh` |
| 后台改定位权限 | 不立即生效 | 回前台才收 `didChangeAuthorization` |

---

## 六、术语对照

| 术语 | 含义 |
|------|------|
| **表盘 / face** | 一个全屏页面（模拟钟/数字钟/速度表），注册表驱动 |
| **共享层** | `simulator/ui/`，模拟器与固件共用的唯一源码 |
| **烘焙表盘** | 把刻度/数字离线渲染成图片，运行时只画指针 |
| **五态** | `BT OFF` / `WAIT GPS` / `NO FIX` / `STALE` / `LIVE` |
| **数据龄** | 距上次收到有效遥测的毫秒数 |
| **斜坡（ramp）** | 按时间匀速逼近目标速度，消除跳针 |
| **防抖（debounce）** | STALE 需连续 3 次判定才切换，消除闪烁 |

## 烘焙图管线（8bpp 索引色 + 启动充气）

新增/修改烘焙图时，**不要手改生成物**（`ui_dial_faces.c` / `ui_driver_images.c`），
改源并重跑脚本：

```bash
python3 tools/make_dial_faces.py    # 表盘底图（466×466）
python3 tools/make_images.py        # 驾驶员插画（300×300）
```

**存储与运行方式（两段式）**：

| 阶段 | 格式 | 好处 |
|------|------|------|
| Flash 存储 | `LV_IMG_CF_INDEXED_8BIT` | 比 RGB565 小一半（本批图省 683 KB）|
| 运行时（启动时充气）| `LV_IMG_CF_TRUE_COLOR`，数据在 PSRAM | 直接内存拷贝，滑页不退化 |

**为什么必须充气**：LVGL 对索引图是**逐像素查表解码**，全屏图每帧 21.7 万次查表，
滑页实测 123ms → 168ms。启动时解码一次即可恢复（实测 110ms）。

- 充气入口：`inflate_baked_images()`（`firmware/dashboard/dashboard.ino`，在 `ui_pages_init()` 之前）
- 共用实现：`simulator/ui/ui_img_inflate.c`（模拟器用 malloc、真机用 PSRAM 分配器，
  保证两边走**同一条渲染路径**，像素回归才有意义）
- 纯解码逻辑：`simulator/ui/img_indexed.{h,c}`（不依赖 LVGL，可 PC 单测）

**新增图片时必须同步登记**：`simulator/ui/ui_baked_images.h`
（非 const 声明 —— 不能用 `LV_IMG_DECLARE`，否则无法就地充气）。
漏登记只会退化性能，不会崩溃。

**颜色数限制**：索引色最多 256 色。超限时生成脚本会自适应量化（不抖动）并打印
色差统计；若某图量化后肉眼可见损失，应改回 RGB565 或改用别的画法。
