# 部署指南（设备固件 + 手机 App）

> **本文是部署的单一事实来源。** 环境搭建见 [`setup-macos.md`](setup-macos.md)。
>
> 两条独立流水线：
> - **A. 固件 → ESP32-S3 开发板**（Arduino）
> - **B. App → iPhone**（Xcode）

---

## 目录

- [零、一键速查](#零一键速查)
- [一、固件侧部署](#一固件侧部署)
- [二、App 侧部署](#二app-侧部署)
- [三、双端联调流程](#三双端联调流程)
- [四、故障排除](#四故障排除)

---

## 零、一键速查

### 固件（最常用）

```bash
cd /Users/pwh/Data/PWH/code/esp32-car-dashboard

# 1) 若改过 simulator/ui/ 必须先同步（否则固件用的是旧副本）
tools/sync_firmware.sh

# 2) 编译 + 烧录（一条命令）
"/Applications/Arduino IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli" compile \
  --fqbn "esp32:esp32:esp32s3:FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,USBMode=hwcdc,CDCOnBoot=cdc,PSRAM=opi" \
  --build-property "compiler.cpp.extra_flags=-DESP32QSPI_MAX_PIXELS_AT_ONCE=4096" \
  --libraries reference/ESP32-S3-Touch-AMOLED-1.75/examples/arduino/libraries \
  --upload -p /dev/cu.usbmodem101 \
  firmware/dashboard
```

### App（最常用）

```bash
cd /Users/pwh/Data/PWH/code/esp32-car-dashboard/ios/CarDashboardPhone
export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer

# 1) 查设备 UDID
xcrun devicectl list devices

# 2) 编译
xcodebuild -project CarDashboardPhone.xcodeproj -scheme CarDashboardPhone \
  -destination 'platform=iOS,id=<UDID>' -configuration Debug build

# 3) 安装 + 启动
APP=$(find ~/Library/Developer/Xcode/DerivedData/CarDashboardPhone-*/Build/Products/Debug-iphoneos \
      -maxdepth 1 -name "CarDashboardPhone.app" | head -1)
xcrun devicectl device install app --device <UDID> "$APP"
xcrun devicectl device process launch --device <UDID> com.cardashboard.CarDashboardPhone
```

---

## 一、固件侧部署

### 1.1 前置条件

| 项 | 要求 |
|----|------|
| 工具链 | Arduino IDE 2.x（内含 arduino-cli）或独立 arduino-cli |
| Core | Arduino-ESP32 **3.3.10** |
| 官方库 | `reference/ESP32-S3-Touch-AMOLED-1.75/examples/arduino/libraries` |
| 连接 | **支持数据传输**的 USB-C 线（部分线仅供电） |
| 串口 | macOS 通常 `/dev/cu.usbmodem*` |

**确认串口**：
```bash
ls /dev/cu.* | grep -v Bluetooth
# 本机实测：/dev/cu.usbmodem101
```

**arduino-cli 位置**（本机未加入 PATH，用 IDE 内置的）：
```bash
CLI="/Applications/Arduino IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli"
```

### 1.2 ⚠️ 两个不可省的编译参数

**这是最容易出错的地方。** 官方示例的 FQBN 不够用：

| 参数 | 为什么必需 | 省略后果 |
|------|-----------|---------|
| `PSRAM=opi` | 本板为 **ESP32-S3R8**（8MB **八线** OPI PSRAM），全帧绘制缓冲依赖它 | 缓冲退回内部 RAM，内存不足或绘制异常 |
| `-DESP32QSPI_MAX_PIXELS_AT_ONCE=4096` | QSPI 单次传输分块从默认 1024 提到 4096 | **滑页明显掉帧**（实测单帧从 ~100ms 恶化） |

> `PSRAM=opi` 曾在真机验证：S3R8 必须用 `opi`（不是 `qspi`）。
> 若误设，表现为 PSRAM 分配失败或帧缓冲异常。

**完整 FQBN**：
```
esp32:esp32:esp32s3:FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,USBMode=hwcdc,CDCOnBoot=cdc,PSRAM=opi
```

**分区表说明**（`app3M_fat9M_16MB`）：

| 分区 | 大小 | 用途 |
|------|------|------|
| app0 | 3 MB | 当前固件（占用 **77%**，约 2.42 MB）|
| app1 | 3 MB | OTA 备用（当前未用）|
| **ffat** | **9.875 MB** | **未使用**，可用于存放资源（起始 `0x610000`）|

> 本板另有 **TF/microSD 卡槽**（SDMMC 1-bit，GPIO1/2/3，D3/CS 在 GPIO41），
> 也可用于存放大量图片资源。

### 1.3 完整编译 + 烧录

```bash
cd /Users/pwh/Data/PWH/code/esp32-car-dashboard
CLI="/Applications/Arduino IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli"

# ★ 改过 simulator/ui/ 就必须先同步
tools/sync_firmware.sh

"$CLI" compile \
  --fqbn "esp32:esp32:esp32s3:FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,USBMode=hwcdc,CDCOnBoot=cdc,PSRAM=opi" \
  --build-path /tmp/build_$(date +%s) \
  --build-property "compiler.cpp.extra_flags=-DESP32QSPI_MAX_PIXELS_AT_ONCE=4096" \
  --libraries reference/ESP32-S3-Touch-AMOLED-1.75/examples/arduino/libraries \
  --upload -p /dev/cu.usbmodem101 \
  firmware/dashboard
```

**预期输出**：
```
Sketch uses ~2420000 bytes (77%) of program storage space. Maximum is 3145728 bytes.
Global variables use 126188 bytes (38%) of dynamic memory, ...
Hash of data verified.        ← 出现 3 次（bootloader/分区表/应用）
```

### 1.4 ⚠️ 关于 `--build-path`（重要）

**跨文件改动后不要复用同一个 `--build-path`。**

**真实踩坑**：修改 `ble_telemetry.cpp` 后复用旧构建目录，构建系统**跳过了重编译**，
导致烧进去的还是旧固件。现象极具误导性：*"BLE 写入成功，但仪表计数始终为 0"* ——
看起来像协议问题，实际是固件没更新。

- ✅ 用**新的临时目录**（如上例的 `$(date +%s)`）
- ✅ 或干脆**不用** `--build-path`（每次全量编译，慢但可靠）
- ⚠️ 只有在确认没有跨文件改动时才复用

### 1.5 单独烧录（已编译过）

```bash
"$CLI" upload \
  --fqbn "esp32:esp32:esp32s3:FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,USBMode=hwcdc,CDCOnBoot=cdc,PSRAM=opi" \
  --port /dev/cu.usbmodem101 \
  firmware/dashboard
```

### 1.6 进入下载模式（无法自动复位时）

若烧录报 `Failed to connect` / `No serial data received`：

1. **按住 BOOT** 不放
2. **短按 RESET** 然后松开
3. **松开 BOOT** → 板子进入下载模式（此时可烧录）

> ⚠️ **不要用脚本反复轮询 esptool**。曾有一个后台任务每 3 秒调一次 esptool，
> 持续拉低 DTR/RTS，导致板子**一直停在下载模式**，
> 现象是"接电脑不亮屏、接充电器才亮"。

### 1.7 串口查看日志

```bash
# 方法一：arduino-cli monitor
"$CLI" monitor --port /dev/cu.usbmodem101 --config baudrate=115200

# 方法二：screen（退出：Ctrl-A 然后 K 再 Y）
screen /dev/cu.usbmodem101 115200
```

**串口命令速查**（详见 `../PROGRESS.md` §11）：

| 命令 | 作用 |
|------|------|
| `help` | 全部命令 |
| `ble` | BLE 连接/遥测/控制统计 + 复位原因 |
| `face [id\|next\|prev]` | 查看/切换表盘 |
| `trip [reset]` | 行程统计 |
| `bright <0-255>` / `screen on\|off` | 亮度 / 息屏 |
| `lvrot [0\|1\|2\|3]` | 临时切换正式旋转策略（不写入 NVS）|
| `mem` | 内存情况 |
| `dump` | 打印屏幕元素树 |

### 1.8 上板冒烟检查（烧录后必做）

```
1. 屏幕正常点亮、显示表盘          ← 不亮 = 可能停在下载模式或初始化失败
2. 左右滑动能翻页                  ← 验证触摸
3. 串口输入 help → 有输出           ← 验证 CDC 串口
4. 串口输入 ble → 看复位原因        ← 应为"上电复位"，非 panic
5. 串口输入 mem → 空闲堆            ← 正常 >120 KB
```

**心跳观察**（确认稳定）：
```
[运行] xxx s  空闲堆 120348 B  循环 199/s  重绘 0/s  缓冲=PSRAM全帧
渲染最大 x ms（占空 x%）｜QSPI 传输 合计 x ms 单次最大 43 ms
```
预期：循环 ≈199/s、空闲堆 ≈120 KB、无 reboot、QSPI 单次 ≈43ms。

### 1.9 崩溃排查（addr2line）

若串口出现 `Guru Meditation Error`：

```bash
# 用完整编译输出中的地址回溯
~/Library/Arduino15/packages/esp32/tools/esp-x32/2601/bin/xtensa-esp-elf-addr2line \
  -pfiaC -e /tmp/build_xxx/dashboard.ino.elf 0x<地址>
```

> 历史案例：一次误删 `lv_disp_drv_register()` 导致
> `lv_scr_act()` 返回 NULL → 启动 panic 循环。addr2line 定位到
> `lv_obj_allocate_spec_attr` → `setup() dashboard.ino:737`。
> **此类情况需 BOOT+RESET 进下载模式才能恢复。**

### 1.10 日常开发推荐流程

```bash
# 1) 改 simulator/ui/ 下的代码
# 2) 先在模拟器验证（快，且不占用设备）
cd simulator && make -j8 && ./dashboard_sim

# 3) 跑测试
cd ../tests && make run PYTHON=<python路径>
cd .. && python3 tools/ui_regression.py

# 4) 没问题再上板
tools/sync_firmware.sh
"$CLI" compile ... --upload -p /dev/cu.usbmodem101 firmware/dashboard
```

> **省时间的关键**：UI 改动 90% 的问题能在模拟器上发现，不必每次烧板。

---

## 二、App 侧部署

### 2.1 前置条件

| 项 | 要求 |
|----|------|
| Xcode | 26.5（本机）|
| 签名 | Apple Development 证书 + Team ID |
| 设备 | 真机需 **iOS 17.0+**，且**已信任开发者证书** |
| 工程生成 | `xcodegen`（改了 `project.yml` 才需要）|

**关键环境变量**：`xcode-select` 指向 CLT 时 `simctl`/`devicectl` 不可用，
必须显式指定：
```bash
export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
```

### 2.2 签名配置

在 `ios/CarDashboardPhone/project.yml`：

```yaml
DEVELOPMENT_TEAM: <你的 Team ID>        # ← 改成你自己的（Apple Developer → Membership）
CFBundleDisplayName: 车载仪表          # ← 主屏幕显示名
```

**查看本机可用签名**：
```bash
security find-identity -v -p codesigning
# 本机实测：Apple Development: <你的 Apple ID> (<证书 ID>)
```

### 2.3 编译

```bash
cd /Users/pwh/Data/PWH/code/esp32-car-dashboard/ios/CarDashboardPhone
export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer

# 若改过 project.yml（改了签名/名称/依赖）
xcodegen generate

# 查设备 UDID
xcrun devicectl list devices
```

**编译（真机）**：
```bash
xcodebuild -project CarDashboardPhone.xcodeproj -scheme CarDashboardPhone \
  -destination 'platform=iOS,id=DF7C728E-F722-5E81-8D0B-DED301400C3D' \
  -configuration Debug build
```

**编译（模拟器，快，用于验证能编译）**：
```bash
xcodebuild -project CarDashboardPhone.xcodeproj -scheme CarDashboardPhone \
  -destination 'platform=iOS Simulator,name=iPhone 17' build
```

> 改了 Swift 代码**不需要** `xcodegen generate`；只有改 `project.yml` 才需要。

### 2.4 安装到真机

```bash
export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
UDID=DF7C728E-F722-5E81-8D0B-DED301400C3D    # 换成你的设备

APP=$(find ~/Library/Developer/Xcode/DerivedData/CarDashboardPhone-*/Build/Products/Debug-iphoneos \
      -maxdepth 1 -name "CarDashboardPhone.app" | head -1)

xcrun devicectl device install app --device $UDID "$APP"
```

**预期输出**：
```
App installed:
• bundleID: com.cardashboard.CarDashboardPhone
```

### 2.5 启动 App

```bash
xcrun devicectl device process launch --device $UDID com.cardashboard.CarDashboardPhone
```

**若报错**：
```
Unable to launch ... because it has an invalid code signature,
inadequate entitlements or its profile has not been explicitly trusted
```

→ **需要在 iPhone 上信任证书**：
1. 设置 → 通用 → **VPN与设备管理**
2. 找到「**开发者 App**」分组 → 点你的 Apple ID
3. 点「**信任**」→ 弹窗再点「信任」

> **前提**：iOS 16+ 需先打开 **设置 → 隐私与安全性 → 开发者模式**（会要求重启）。
> 若列表里找不到证书，先检查这个开关。

**若报 `Locked`**：手机处于锁屏状态，解锁后重试。

### 2.6 包内容检查（确认图标/名称正确）

```bash
APP=$(find ~/Library/Developer/Xcode/DerivedData/CarDashboardPhone-*/Build/Products/Debug-iphoneos \
      -maxdepth 1 -name "CarDashboardPhone.app" | head -1)

# 图标是否打入
ls "$APP" | grep -i appicon

# 显示名
/usr/libexec/PlistBuddy -c "Print :CFBundleDisplayName" "$APP/Info.plist"
# 预期：车载仪表
```

### 2.7 App 图标重新生成

```bash
python3 tools/make_app_icon.py
# 生成 18 种尺寸到 Assets.xcassets/AppIcon.appiconset/
# 预览图：design/assets/images/app_icon_1024.png
```

### 2.8 测试

```bash
# ★ 必须用这个脚本，不要直接跑 xcodebuild test
tools/run_ios_tests.sh
```

> ⚠️ **为什么不直接跑 `xcodebuild test`**：`LinkDiagnosticsUITests` 依赖
> 模拟器的 Freeway Drive 定位场景，而测试进程**无法调用 `simctl`**。
> 直接跑会报 **3 项假失败**，极易误判为代码回归（已踩坑）。
> `run_ios_tests.sh` 会自动启动场景 + 设置 `DEVELOPER_DIR`。

### 2.9 常见操作

| 操作 | 命令 |
|------|------|
| 卸载 | `xcrun devicectl device uninstall app --device $UDID com.cardashboard.CarDashboardPhone` |
| 列出已装 App | `xcrun simctl listapps <sim-udid>` |
| 模拟器授权定位 | `xcrun simctl privacy <sim-udid> grant location com.cardashboard.CarDashboardPhone` |
| 模拟器定位场景 | `xcrun simctl location <sim-udid> run "Freeway Drive"` |
| 查看设备详情 | `xcrun devicectl device info details --device $UDID` |

---

## 三、双端联调流程

### 3.1 首次联调

```
1. 板子烧好固件，屏幕正常显示、串口可用
   └─ 串口输入 ble → 应显示"连接=未连接"（等待手机）

2. 手机装好 App 并已信任证书
   └─ 打开 App → BLE 状态应自动变"扫描中"→"已连接"
      （App 已实现启动自动连接，无需点按钮）

3. 点「开始驾驶」
   └─ 串口输入 ble → 应显示"连接=已连接 成功=N"且 N 递增
   └─ 仪表速度页状态应从 BT OFF → WAIT GPS → LIVE

4. 验证数据
   └─ 串口输入 ble → 输出里含「显示速度=… 目标…」（没有 spd 命令）
   └─ 仪表底部 HUD 应显示"XX km/h  NE XX°"
```

### 3.2 联调诊断命令对照

| 现象 | 先查这个 |
|------|---------|
| App 显示已连接但仪表无数据 | 串口 `ble` → 看"成功"计数是否递增 |
| 数据有但速度不对 | 串口 `ble` → 看输出里的「显示速度=… 目标…」；对比是否在按斜坡逼近 |
| 状态标签异常 | 串口 `ble` → 看"链路状态"与"数据龄" |
| 触摸无反应 | 串口 `tp`（启停 100Hz 采样）与 `dump`（打印元素树）；**没有 `touch` 命令** |
| 元素缺失/错位 | 串口 `dump` → 打印元素树 |
| 内存异常 | 串口 `mem` |

### 3.3 五态含义（排查时对照）

| 状态 | 含义 | 排查方向 |
|------|------|---------|
| `BT OFF` | BLE 未连接 | 手机蓝牙/App 是否运行 |
| `WAIT GPS` | 已连接但未收到定位 | 定位权限、室内无信号 |
| `NO FIX` | 收到数据但定位/速度无效 | GPS 信号质量 |
| `STALE` | 数据超时（>2.5s） | 数据龄、App 是否在后台被杀 |
| `LIVE` | 正常 | — |

---

## 四、故障排除

### 4.1 固件侧

| 现象 | 原因 | 解决 |
|------|------|------|
| 编译报 `lvgl.h: No such file` | 缺 `--libraries` 参数 | 补上官方库路径 |
| 编译报某符号未定义（如 `telemetry_trip_t`）| 忘记同步共享层 | `tools/sync_firmware.sh` |
| **改动没生效**（如计数始终为 0）| 复用了 `--build-path` 导致漏编译 | **换新的构建目录** |
| 滑页掉帧严重 | 漏了 QSPI 分块参数 | 加 `-DESP32QSPI_MAX_PIXELS_AT_ONCE=4096` |
| PSRAM 分配失败 | FQBN 漏了 `PSRAM=opi` | 补上 |
| 串口无输出 | `CDCOnBoot` 未开 | FQBN 加 `CDCOnBoot=cdc` |
| 接电脑不亮、接充电器亮 | 板子停在下载模式（DTR/RTS 被拉低）| 断开重连；不要用脚本轮询 esptool |
| 烧录 `Failed to connect` | 未进下载模式 | BOOT+RESET 组合键 |
| 启动 panic 循环 | 代码问题（曾因误删 `lv_disp_drv_register()`）| addr2line 定位；BOOT+RESET 恢复后烧回上一版 |

### 4.2 App 侧

| 现象 | 原因 | 解决 |
|------|------|------|
| `Unable to find a destination` | 设备未连接/未解锁 | `xcrun devicectl list devices` 确认为 `connected` |
| `invalid code signature ... profile not trusted` | 未在手机信任证书 | 设置 → 通用 → VPN与设备管理 → 信任 |
| 找不到开发者证书 | 开发者模式未开 | 设置 → 隐私与安全性 → **开发者模式** → 重启 |
| 启动报 `Locked` | 手机锁屏 | 解锁后重试 |
| BLE 状态一直"初始化中" | 曾因未转发 `@Published`（已修复）| 确认用最新版 App |
| 定位权限改了不生效 | iOS 后台不同步通知 | **回到 App 前台**才生效 |
| `simctl: not a developer tool` | `DEVELOPER_DIR` 未设 | `export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer` |
| XCUITest 报 3 项失败 | 缺定位场景（假失败）| 用 `tools/run_ios_tests.sh` |

### 4.3 联调侧

| 现象 | 排查 |
|------|------|
| App 已连接但仪表无数据 | 串口 `ble` 看"成功"计数；查 App 是否"开始驾驶" |
| 速度一直是 0 | 室内 GPS 无法测速（`speed<0`=未知）；到室外测 |
| 仪表状态闪 STALE | 静止时 iOS 降频至 ~1Hz，偶发擦过 2.5s 阈值（已加显示防抖）|
| 切表盘卡顿 | 每次整屏重绘 ~100ms，硬件下限（QSPI 传一屏 43ms）|

---

## 五、相关文档

| 文档 | 内容 |
|------|------|
| [`setup-macos.md`](setup-macos.md) | 开发环境搭建（工具链安装）|
| [`development-guide.md`](development-guide.md) | 「改哪里」+ 已验证能力清单 |
| `ai-task-boundaries.md` | 交给 AI 改代码时的任务边界 |
| `test-results.md` | 测试台账（避免重复测试）|
| [`troubleshooting.md`](troubleshooting.md) | 完整排障记录与根因分析 |
| [`hardware.md`](hardware.md) | 硬件规格与引脚 |
| [`ble.md`](ble.md) | BLE 协议细节 |

---

> **合规基线（全项目适用）**：本项目仅用于**个人开发、学习与技术验证** ——
> **不装车、不商业化、不分发**。项目使用高德开放平台 Web 服务，其
> **服务协议 §3.4 禁止用于车载场景**；若将来需要装车、分发或商业化，
> 必须先取得书面授权或更换数据源。详见 [`nav-backend.md`](nav-backend.md) §0。
