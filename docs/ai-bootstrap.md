# 交给 AI 执行：从零跑通本项目

> **这份文档是写给 AI（或第一次接触本项目的人）的执行手册。**
>
> 目标：让一个**没有任何上下文的 AI**，只凭这个仓库，就能完成
> **编译固件 → 烧录到板子 → 装 iOS App → 起网关** 的全部步骤，并能**自己验证**每一步是否成功。
>
> 用法：把这份文档 + 仓库地址丢给 AI，说「按 docs/ai-bootstrap.md 执行」。
>
> 文中所有命令都是**可直接复制运行**的；每步都给了**预期输出**，用来判断成败。

---

## 0. 先决条件（AI 请先逐项检查，缺什么先装什么）

| 依赖 | 检查命令 | 要求 |
|------|---------|------|
| macOS | `sw_vers` | 13+（iOS 开发必须 macOS）|
| arduino-cli | `arduino-cli version` | 1.x |
| ESP32 core | `arduino-cli core list \| grep esp32` | **3.x**（本项目用 3.3.x 验证）|
| Xcode | `xcodebuild -version` | 16+ |
| xcodegen | `xcodegen --version` | 任意版本（`brew install xcodegen`）|
| Node.js | `node -v` | **>= 18** |
| git | `git --version` | 任意 |

安装 ESP32 core（若缺失）：

```bash
arduino-cli core update-index
arduino-cli core install esp32:esp32
```

> ⚠️ **Xcode 命令行工具要指向完整 Xcode**，否则 `xcrun` 会报错：
> ```bash
> sudo xcode-select -s /Applications/Xcode.app/Contents/Developer
> ```

---

## 1. 拉取厂商资料（**最容易漏的一步**）

```bash
tools/fetch_reference.sh
```

**为什么必须做**：`reference/` 在 `.gitignore` 里（第三方代码、体积大），
**新克隆的仓库里没有它**，而编译命令要引用
`reference/ESP32-S3-Touch-AMOLED-1.75/examples/arduino/libraries`。
**不跑这步，编译一定失败**（报找不到 `lvgl.h` 之类）。

**预期输出**：最后打印 `✅ 就绪`，并列出 7 个库（lvgl / ESP32_IO_Expander /
GFX_Library_for_Arduino / Mylibrary / SensorLib / XPowersLib / esp-lib-utils）+ `lv_conf.h`。

---

## 2. 同步 UI 代码进固件目录（**第二容易漏的**）

```bash
tools/sync_firmware.sh
```

**为什么必须做**：UI 代码在 `simulator/ui/` 下开发（能在电脑上直接跑单测），
固件工程 `firmware/dashboard/` 里的副本是**同步生成的**。
**不同步 → 烧进去的是旧界面**，改了 UI 却看不到效果。

---

## 3. 编译固件

```bash
BUILD=/tmp/cardash-build
arduino-cli compile \
  --fqbn "esp32:esp32:esp32s3:FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,USBMode=hwcdc,CDCOnBoot=cdc,PSRAM=opi" \
  --build-property "compiler.cpp.extra_flags=-DESP32QSPI_MAX_PIXELS_AT_ONCE=4096" \
  --libraries reference/ESP32-S3-Touch-AMOLED-1.75/examples/arduino/libraries \
  --build-path "$BUILD" \
  firmware/dashboard
```

**三个不可省的参数**（少一个都会出问题）：

| 参数 | 作用 |
|------|------|
| `PSRAM=opi` | 板子的 PSRAM 是 octal 模式，不设会识别不到 |
| `PartitionScheme=app3M_fat9M_16MB` | 固件 2.4 MB，用默认分区会溢出 |
| `compiler.cpp.extra_flags=-DESP32QSPI_MAX_PIXELS_AT_ONCE=4096` | 这块屏 QSPI 单次传输上限，不设会花屏/极慢 |

**预期输出**（关键两行）：

```
Sketch uses 2451595 bytes (77%) of program storage space. Maximum is 3145728 bytes.
...
```

只要出现 `Sketch uses ... (77%)` 就是成功。**若报错**，见文末「常见失败」。

---

## 4. 烧录到板子

**先找端口**（插上板子后）：

```bash
ls /dev/cu.usbmodem*
```

**预期**：类似 `/dev/cu.usbmodem101`（或 `usbmodem1101` 等，按实际替换下面的 `-p`）。

```bash
arduino-cli upload -p /dev/cu.usbmodem101 \
  --fqbn "esp32:esp32:esp32s3:FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,USBMode=hwcdc,CDCOnBoot=cdc,PSRAM=opi" \
  --build-path /tmp/cardash-build \
  firmware/dashboard
```

**预期输出**：`Hard resetting via RTS pin...`

**验证固件真的跑起来了**（可选但推荐）：

```bash
# macOS 自带，115200 波特率看串口
screen /dev/cu.usbmodem101 115200
# 退出：Ctrl-A 然后 K，再按 y
```

**预期**：能看到类似

```
[表盘] 启动恢复：当前=0(analog)，6/6 启用（掩码来源：…）
[上传] 热点已开：SSID=CarDash（免密）IP=192.168.4.1，HTTP 就绪
```

**屏幕表现**：出现表盘，可左右滑动切换 **6 个表盘**。

---

## 5. iOS App（**需要你自己的 Apple 开发者账号**）

> ⚠️ **这一步别人无法直接复用**：仓库里的 `DEVELOPMENT_TEAM` 是原作者的团队 ID，
> 你必须换成自己的，否则签名会失败。

### 5.1 改成你自己的签名信息

1. 打开 `ios/CarDashboardPhone/project.yml`，把
   ```yaml
   DEVELOPMENT_TEAM: QFH3X6SMV4
   ```
   改成**你自己的 Team ID**（Apple Developer → Membership 里查，形如 `ABCDE12345`）。

2. 把 `bundleIdPrefix: com.cardashboard` 改成你自己的（例如 `com.yourname`），
   否则会和原作者的证书冲突。

3. 确认 `ios/CarDashboardPhone/CarDashboardPhone/CarDashboardPhone.entitlements` 存在，
   且包含：
   - `com.apple.developer.networking.HotspotConfiguration`（连仪表热点用）
   - `com.apple.developer.networking.multicast`（本地网络发现，若启用）

### 5.2 生成工程并安装

```bash
cd ios/CarDashboardPhone
xcodegen generate                     # 由 project.yml 生成 .xcodeproj

# 查你的设备 ID（要先插上 iPhone 并信任这台电脑）
xcrun devicectl list devices
# 输出里找到你的 iPhone，复制形如 00008101-001654202261001E 的标识符
```

```bash
export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer

xcodebuild -project CarDashboardPhone.xcodeproj \
  -scheme CarDashboardPhone -configuration Debug \
  -destination 'platform=iOS,id=<你的设备 ID>' \
  -derivedDataPath /tmp/dd-car build

xcrun devicectl device install app \
  --device <你的设备 ID> \
  /tmp/dd-car/Build/Products/Debug-iphoneos/CarDashboardPhone.app
```

**预期**：`** BUILD SUCCEEDED **` + `App installed:`

### 5.3 首次运行要授权的三件事

在 iPhone 上打开 App 后允许：

1. **蓝牙**（连接仪表）
2. **位置** → 选「始终允许」（后台记录轨迹必需）
3. **本地网络**（App 访问网关需要）

---

## 6. 网关（可选，只有导航功能需要）

```bash
cd gateway
npm install                 # 有 2 个运行时依赖
cp .env.example .env        # 然后编辑 .env，填入你自己的高德 Key
npm start
```

`.env` 必填项：

```
AMAP_KEY=<你自己的高德 Web 服务 Key>
GATEWAY_TOKEN=<随便生成一个 UUID，App 里要填同一个>
HOST=0.0.0.0          # 必须，否则手机访问不到
PORT=8787
```

**预期**：启动后访问 `http://<你的 Mac 局域网 IP>:8787/health` 返回 `{"ok":true}`。

App 里「设置 → 网关地址」填 `http://<Mac IP>:8787`，令牌填同一个 `GATEWAY_TOKEN`。

> 快速验证：浏览器打 `http://<Mac IP>:8787/routes?...` 能看到 JSON 就通了。
> 详细说明见 [`gateway/DEPLOY.md`](../gateway/DEPLOY.md)。

---

## 7. 验收清单（AI 请逐条确认后再宣告完成）

| # | 检查项 | 怎么验 |
|---|--------|--------|
| 1 | 厂商资料就绪 | `tools/fetch_reference.sh` 打印 `✅ 就绪` |
| 2 | UI 已同步 | `tools/sync_firmware.sh` 无报错 |
| 3 | 编译成功 | 出现 `Sketch uses … (77%)` |
| 4 | 烧录成功 | 出现 `Hard resetting via RTS pin...` |
| 5 | 固件在跑 | 屏幕有表盘；串口有 `[表盘] 启动恢复` |
| 6 | 6 个表盘都在 | 左右滑动能切到 analog/digital/speed/color/nav/photo |
| 7 | App 装上了 | `App installed:` |
| 8 | BLE 连上 | App 显示「已连接」，仪表出现 `TIME SYNC → SYNCED` |
| 9 | 单测全绿 | `make -C tests run` 退出码 0 |
| 10 | 网关通了（可选）| `/health` 返回 `{"ok":true}` |

---

## 8. 常见失败

| 现象 | 原因 | 解法 |
|------|------|------|
| `fatal error: lvgl.h: No such file` | 没跑 `fetch_reference.sh` | 跑它 |
| `Sketch too big` / `region overflowed` | 分区方案没选对 | 确认 `PartitionScheme=app3M_fat9M_16MB` |
| 烧进去屏幕花屏 / 刷新极慢 | 少了 QSPI 那个 `--build-property` | 补上 |
| 改了 UI 但板上没变化 | 没跑 `sync_firmware.sh` | 跑它再编译 |
| `PSRAM not found` / 一启动就重启 | FQBN 少了 `PSRAM=opi` | 补上 |
| `xcrun: error: unable to find utility` | 只装了命令行工具 | `sudo xcode-select -s /Applications/Xcode.app/Contents/Developer` |
| App 签名失败 `No signing certificate` | 用的还是原作者的 `DEVELOPMENT_TEAM` | 换成你自己的 Team ID |
| App 里连不上网关 | 网关 `.env` 里 `HOST` 不是 `0.0.0.0`，或不在同一局域网 | 改 `HOST`，确认同一网段 |
| 表盘列表里少一个表盘 | 设备的启用掩码把它关了 | App → 仪表 → 表盘 → 勾选；固件升级后新增表盘会自动启用 |
| 照片表盘显示 `LOAD ERROR` | SD 卡里的 `.pgi` 损坏/被截断 | 重新用 `tools/import_photos.sh` 转，它会做完整校验 |

---

## 9. 想改代码时

先读 [`docs/development-guide.md`](development-guide.md)（该动哪些文件）
和 [`docs/ai-task-boundaries.md`](ai-task-boundaries.md)（按任务类型划定改动范围）。

**改完必须**：

```bash
make -C tests run      # 单测全绿
tools/sync_firmware.sh # 若改了 simulator/ui
```

**不要**直接改 `firmware/dashboard/*.c` —— 那份是**同步生成**的，下次同步会被覆盖。
