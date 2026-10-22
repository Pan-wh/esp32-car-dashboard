# 硬件信息（依据）

> 本文档为项目内速查摘要。**所有引脚、地址、初始化细节以官方仓库为准**：
> `reference/ESP32-S3-Touch-AMOLED-1.75/HARDWARE_REFERENCE_ZH.md`（commit `e4344e7`）。
> 本文内容均已与该文档核对，禁止凭记忆修改。

## 板卡型号

| 产品 | 说明 |
|------|------|
| **ESP32-S3-Touch-AMOLED-1.75**（本项目实际使用）| **无 GNSS 版本**。选用非 GPS 版本是为了适配外壳；**数据源完全依赖 iPhone**（Core Location 经 BLE 下发）|

> ⚠️ **型号标注更正（2026-10-13）**：本文曾写作「1.75-**G** / GPS 版本 / 板载 LC76G」，
> 与实物不符。项目**已弃用 GPS 版本**（原因：外壳适配），板上**没有** LC76G 模块。
> 所有位置/速度数据均来自手机，仪表侧不做任何定位。
>
> **实测芯片信息**（esptool 读取）：
> ```
> Chip type:  ESP32-S3 (QFN56) (revision v0.2)
> Features:   Wi-Fi, BT 5 (LE), Dual Core + LP Core, 240MHz,
>             Embedded PSRAM 8MB (AP_3v3)
> Flash:      Detected flash size: 16MB
> USB mode:   USB-Serial/JTAG
> ```

> 📌 **与参考项目 moto-gps 的差异**：该项目使用 **1.75C（32MB Flash）**，
> 本板为 **16MB Flash**。应用分区 3MB，当前固件占用 **77%**（约 2.42 MB）。
> 空间受限时优先考虑把字体/图片等静态资源放 **TF 卡**（见下方「存储」）。

## 核心器件

| 功能 | 器件 | 备注 |
|------|------|------|
| MCU | ESP32-S3R8 | 双核 LX7 @240MHz，**8MB PSRAM（八线 OPI）**，**16MB Flash**（实测确认）|
| 屏幕 | CO5300 AMOLED | 1.75" 圆形 466×466，QSPI 接口 |
| 触摸 | CST9217 | 电容触摸，挂共用 I2C 总线 |
| 电源 | AXP2101 | 电池充电/电源路径/遥测 |
| IMU | QMI8658 | 六轴（**无磁力计**，不能做静止指南针） |
| RTC | PCF85063 | 备用电源由板上电路提供 |
| GNSS | **无**（本板未装 GNSS 模块）| 定位由 iPhone Core Location 提供；原理图网表无 GPS 的 SDA/SCL 网络 |
| 存储 | TF/microSD | 原生 SDMMC，BSP 以 **1-bit** 模式挂载（GPIO1/2/3，D3/CS 在 GPIO41）→ 读速明显低于 4-bit，适合放字体/图片等**读一次缓存到 RAM** 的资源 |
| 扩展 | TCA9554 | I/O 扩展器 `0x20` |

## 本项目常用关键 GPIO（摘录）

| GPIO | 信号 | 说明 |
|---:|------|------|
| 4–7 | LCD_DATA0–3 | 屏幕 QSPI 数据线 |
| 12 | LCD_CS | 屏幕片选 |
| 38 | LCD_PCLK | 屏幕 QSPI 时钟 |
| 39 | LCD_RST | 屏幕复位 |
| 13 | LCD_TE | 撕裂效应同步信号 |
| 11 | TP_INT | 触摸中断 |
| 40 | TP_RST | 触摸复位（与屏幕复位独立） |
| 14 / 15 | I2C_SCL / I2C_SDA | 共用 I2C 总线 |
| 21 | QMI_INT2 | IMU 中断（INT1 经 TCA9554） |
| 1 / 2 / 3 | SD_CMD / SD_CLK / SD_D0 | TF 卡（挂卡时勿复用） |
| 17 / 18 | GPIO17 / GPIO18 | 官方 `-G`（GPS）版可能接 LC76G RX/TX；**本板无 GNSS 模块**，但复用前仍建议实测确认装配 |
| 0 | BOOT | 下载模式按键，带外部上拉，按下为低 |
| 19 / 20 | USB D− / D+ | 原生 USB，勿复用 |

## 共用 I2C 总线地址（7-bit）

| 地址 | 器件 |
|---:|------|
| `0x18` | ES8311（音频 Codec） |
| `0x20` | TCA9554（I/O 扩展器） |
| `0x34` | AXP2101（PMU） |
| `0x40` | ES7210（音频 ADC） |
| `0x51` | PCF85063（RTC） |
| `0x54` | CST9217（触摸） |
| `0x6B` | QMI8658（IMU） |
| ~~`0x50`~~ | ~~LC76G（GNSS）~~ **本板无此器件**；I2C 扫描不到属正常（见 `docs/decisions.md`）|

**注意**：官方 `-G` 版的 LC76G 读地址为 `0x54`，与 CST9217 相同；因**本板无 GNSS 模块**，该冲突不存在。

## 重要硬件约束（来自官方文档）

1. **AMOLED 无独立背光 GPIO**：亮度由 CO5300 面板命令控制，通用背光 PWM 方案不适用。
2. **I2C 总线只初始化一次**：所有板载器件共用 GPIO14/15 总线，复用句柄；BSP 默认 400kHz。
3. **GPIO17/18**：官方 `-G` 版可能被 LC76G UART 占用；**本板无 GNSS**，但复用前仍建议实测确认装配。
4. **PWR 键不是 ESP32 GPIO**：属 AXP2101 电源路径；状态可从 TCA9554 P4/`EXIO4` 读取（按下为高）。
5. **QMI8658 无磁力计**：静止时无法提供可靠北向；航向只能来自**手机的移动定位**（`CLLocation.course`），静止/低速时无效 → 仪表不显示（已验证）。
6. 所有 GPIO 为 3.3V 逻辑，不耐 5V。

## 完整资料

- 原理图 PDF：`reference/ESP32-S3-Touch-AMOLED-1.75/Schematic/ESP32-S3-Touch-AMOLED-1.75-schematic.pdf`
- 硬件参考全文：`reference/ESP32-S3-Touch-AMOLED-1.75/HARDWARE_REFERENCE_ZH.md`
- 官方文档站：<https://docs.waveshare.net/ESP32-S3-Touch-AMOLED-1.75/>

---

> **合规基线（全项目适用）**：本项目仅用于**个人开发、学习与技术验证** ——
> **不装车、不商业化、不分发**。项目使用高德开放平台 Web 服务，其
> **服务协议 §3.4 禁止用于车载场景**；若将来需要装车、分发或商业化，
> 必须先取得书面授权或更换数据源。详见 [`nav-backend.md`](nav-backend.md) §0。
