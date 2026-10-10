# 硬件信息（依据）

> 本文档为项目内速查摘要。**所有引脚、地址、初始化细节以官方仓库为准**：
> `reference/ESP32-S3-Touch-AMOLED-1.75/HARDWARE_REFERENCE_ZH.md`（commit `e4344e7`）。
> 本文内容均已与该文档核对，禁止凭记忆修改。

## 板卡型号

| SKU | 产品 | 说明 |
|------|------|------|
| **31264** | **ESP32-S3-Touch-AMOLED-1.75-G（本项目）** | GPS 版本：板载 LC76G 模块 + GNSS 陶瓷天线 |

## 核心器件

| 功能 | 器件 | 备注 |
|------|------|------|
| MCU | ESP32-S3R8 | 双核 LX7 @240MHz，8MB PSRAM，16MB 外置 Flash |
| 屏幕 | CO5300 AMOLED | 1.75" 圆形 466×466，QSPI 接口 |
| 触摸 | CST9217 | 电容触摸，挂共用 I2C 总线 |
| 电源 | AXP2101 | 电池充电/电源路径/遥测 |
| IMU | QMI8658 | 六轴（**无磁力计**，不能做静止指南针） |
| RTC | PCF85063 | 备用电源由板上电路提供 |
| GNSS(-G) | Quectel LC76G | I2C（`0x50` 写 / `0x54` 读），复位经 TCA9554 P7 |
| 存储 | TF/microSD | 原生 SDMMC，BSP 以 1-bit 模式挂载 |
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
| 17 / 18 | GPIO17 / GPIO18 | `-G` 版可能接 LC76G RX/TX，复用前必须实测确认 |
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
| `0x50` / `0x54` | LC76G（GNSS，`0x50` 写命令 / `0x54` 读） |

**注意**：CST9217（`0x54`）与 LC76G 读地址（`0x54`）数值相同——LC76G 使用 `0x50` 作为写/命令地址；开发时须以官方示例代码的用法为准，勿自行猜测。

## 重要硬件约束（来自官方文档）

1. **AMOLED 无独立背光 GPIO**：亮度由 CO5300 面板命令控制，通用背光 PWM 方案不适用。
2. **I2C 总线只初始化一次**：所有板载器件共用 GPIO14/15 总线，复用句柄；BSP 默认 400kHz，LC76G Arduino 示例用 100kHz，需选所有器件都支持的速率。
3. **GPIO17/18 在 -G 版可能被 LC76G UART 占用**：复用前必须实测确认装配，并经 TCA9554 P7 协调 LC76G 复位。
4. **PWR 键不是 ESP32 GPIO**：属 AXP2101 电源路径；状态可从 TCA9554 P4/`EXIO4` 读取（按下为高）。
5. **QMI8658 无磁力计**：静止时无法提供可靠北向；航向只能来自 GNSS 移动定位，静止/低速应显示无效状态。
6. 所有 GPIO 为 3.3V 逻辑，不耐 5V。

## 完整资料

- 原理图 PDF：`reference/ESP32-S3-Touch-AMOLED-1.75/Schematic/ESP32-S3-Touch-AMOLED-1.75-schematic.pdf`
- 硬件参考全文：`reference/ESP32-S3-Touch-AMOLED-1.75/HARDWARE_REFERENCE_ZH.md`
- 官方文档站：<https://docs.waveshare.net/ESP32-S3-Touch-AMOLED-1.75/>
