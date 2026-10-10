# macOS 开发环境安装与烧录步骤

本项目开发主机为 macOS。环境已于 **2026-10-10 配置完成**（见文末「本机已配置状态」）。
新机器或重置环境时按本文从头执行即可。

## 1. 工具安装顺序

按顺序安装，前一项完成再进行下一项：

| 顺序 | 工具 | 用途 | 安装方式 |
|---:|------|------|----------|
| 1 | **Git** | 版本管理 | macOS 自带（`git --version` 验证，本机 2.39.2） |
| 2 | **Arduino IDE 2.x** | 主构建/烧录工具 | <https://www.arduino.cc/en/software> 下载 macOS 版拖入 /Applications（本机 2.3.10） |
| 3 | **VS Code** | 代码编辑/浏览 | <https://code.visualstudio.com/>（可选，仅作编辑器，不装 PlatformIO） |
| 4 | **Arduino-ESP32 Core 3.3.10** | ESP32-S3 编译支持 | 见下文第 2 节 |
| 5 | Python 3 | 烧录 Release 固件包时需要 esptool | 本机已有 3.14.5 |

> 约束：不引入 ESP-IDF、PlatformIO 等第二套构建体系（决策 D-001）。

## 2. 安装 Arduino-ESP32 Core（3.3.10）

版本依据：官方仓库 `docs/getting-started_ZH.md` 明确「已验证的 Arduino-ESP32 Core 版本为 3.3.10」。

### 方式 A：Arduino IDE 图形界面（推荐日常使用）

1. 打开 Arduino IDE → `File → Preferences`（macOS 上为 `Arduino IDE → Settings…`）。
2. 在 **Additional boards manager URLs** 填入：
   ```
   https://espressif.github.io/arduino-esp32/package_esp32_index.json
   ```
3. `Tools → Board → Boards Manager…`，搜索 `esp32`（作者 Espressif Systems）。
4. 选择版本 **3.3.10** 安装（若已装更高版本，点当前版本号切换回 3.3.10）。

### 方式 B：arduino-cli（命令行，本机配置时实际使用）

Arduino IDE 2.3.10 内置 arduino-cli 1.5.1，路径：

```bash
CLI="/Applications/Arduino IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli"
```

（建议在 `~/.zshrc` 中 `alias arduino-cli="$CLI"` 方便日常使用。）

```bash
# 1) 板卡索引地址（已写入 ~/Library/Arduino15/arduino-cli.yaml）
board_manager:
  additional_urls:
    - https://espressif.github.io/arduino-esp32/package_esp32_index.json

# 2) 更新索引并安装 3.3.10
arduino-cli core update-index \
  --additional-urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
arduino-cli core install esp32:esp32@3.3.10 \
  --additional-urls https://espressif.github.io/arduino-esp32/package_esp32_index.json

# 3) 验证
arduino-cli core list        # 应显示 esp32:esp32 3.3.10
```

## 3. 获取官方仓库（参考依赖）

官方仓库**不纳入 Git 版本管理**（`.gitignore` 已忽略 `reference/`），换机器重新克隆：

```bash
cd esp32-car-dashboard
git clone https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75.git \
  reference/ESP32-S3-Touch-AMOLED-1.75
```

当前使用版本（记录于 `docs/decisions.md` D-002）：

- commit：`e4344e70c2fa78a13e8a06566507f1ba8af6672a`
- 日期：2026-08-21
- 官方仓库更新后如需同步：`cd reference/ESP32-S3-Touch-AMOLED-1.75 && git pull`，
  然后核对 Core 版本要求是否变化。

## 4. 编译配置（所有示例与固件统一）

| 配置项 | 值 |
|--------|-----|
| Board | `esp32:esp32:esp32s3`（ESP32S3 Dev Module） |
| Flash Size | **16M** |
| Partition Scheme | **app3M_fat9M_16MB** |
| Libraries | `reference/ESP32-S3-Touch-AMOLED-1.75/examples/arduino/libraries` |

### 命令行编译示例（01_HelloWorld）

```bash
cd esp32-car-dashboard
arduino-cli compile \
  --fqbn "esp32:esp32:esp32s3:FlashSize=16M,PartitionScheme=app3M_fat9M_16MB" \
  --libraries reference/ESP32-S3-Touch-AMOLED-1.75/examples/arduino/libraries \
  reference/ESP32-S3-Touch-AMOLED-1.75/examples/arduino/01_HelloWorld
```

### Arduino IDE 中使用配套库

把 `reference/ESP32-S3-Touch-AMOLED-1.75/examples/arduino/libraries` 下的文件夹复制到
`~/Documents/Arduino/libraries/`（或在 IDE 中 `File → Add .ZIP Library` / Sketchbook 库目录设置）。
**若 IDE 存在不兼容的全局库副本，删除重复副本或降低其优先级**（官方要求）。

## 5. 连接与烧录

1. 用**支持数据传输**的 USB-C 线连接板卡。
2. 确认串口（macOS 上通常为 `/dev/cu.usbmodem*` 或 `/dev/cu.wch*`）：
   ```bash
   ls /dev/cu.* | grep -v Bluetooth
   ```
3. 烧录：
   ```bash
   arduino-cli upload \
     --fqbn "esp32:esp32:esp32s3:FlashSize=16M,PartitionScheme=app3M_fat9M_16MB" \
     --port /dev/cu.usbmodemXXXX \
     reference/ESP32-S3-Touch-AMOLED-1.75/examples/arduino/01_HelloWorld
   ```
   （`compile` 加 `--upload --port …` 可一步完成编译+烧录。）
4. 若无法自动复位进入下载模式：**按住 BOOT → 短按 RESET → 启动烧录命令 → 开始写入后松开 BOOT**。
5. 观察串口日志（115200 或示例指定的波特率）：
   ```bash
   arduino-cli monitor --port /dev/cu.usbmodemXXXX --config baudrate=115200
   ```

### 安全注意

- 不要混用不同来源的 Bootloader/分区表/应用固件（官方要求）。
- 正常更新无需擦除整片 Flash；**不要执行全量擦除或刷写工厂镜像，除非明确决定恢复出厂**。
- 烧录前后如遇异常，参考 `reference/ESP32-S3-Touch-AMOLED-1.75/docs/troubleshooting_ZH.md`。

## 6. 官方示例验证顺序（里程碑 1）

按 `AI_CONTEXT.md` 第 11 节顺序，每步烧录后**等确认结果再进行下一步**：

1. `01_HelloWorld` —— 屏幕显示验证
2. `03_LVGL_PCF85063_simpleTime` —— RTC 验证
3. `09_LC76G_I2C` —— GNSS 验证（需 -G 版本，本项目满足；外接天线）
4. （按需补充）`10_Touch_CST9217` 触摸诊断、`04_LVGL_QMI8658_ui` IMU、
   `06_LVGL_Widgets` LVGL 组件、`07_LVGL_SD_Test` microSD

## 7. 本机已配置状态（2026-10-10）

| 项目 | 状态 |
|------|------|
| Git | ✅ 2.39.2 |
| Arduino IDE | ✅ 2.3.10（/Applications） |
| VS Code | ✅ 已安装 |
| arduino-cli | ✅ 1.5.1（IDE 内置，路径见上） |
| arduino-cli.yaml | ✅ 已写入 espressif 索引 URL |
| esp32:esp32 Core | ⏳ 3.3.10 安装中/完成后核对 `arduino-cli core list` |
| 官方仓库 | ✅ clone 完成，commit `e4344e7`，位于 `reference/` |
| 配套库 | ✅ 使用仓库内 `examples/arduino/libraries`（`--libraries` 指定） |
| 串口驱动 | 未验证——首次插板后按第 5 节确认串口出现 |
