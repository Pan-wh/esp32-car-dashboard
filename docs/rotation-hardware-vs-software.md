# 硬件旋转 vs 软件旋转：显示不完整问题（诊断交接文档）

> **用途**：本文档用于把当前未解决的问题完整交接给其他模型/工程师诊断。
> 撰写时点：2026-10-14。相关提交见文末。
>
> **一句话概括**：为了消除 90°/270° 的画面撕裂，把「LVGL 软件旋转」改成「面板硬件旋转（MADCTL）」。
> 撕裂解决了，但出现了**边缘显示不完整**；偏移量已标定到"黑环间距四周均匀"，
> 用户仍反馈**左面缺一块**，且我方照片分析显示该处内容完整 —— **证据冲突，未定位**。

---

## 1. 硬件与既有条件

| 项 | 值 |
|----|----|
| 板子 | Waveshare ESP32-S3-Touch-AMOLED-1.75（**改装自有车机项目**）|
| 屏 | 466×466 圆形 AMOLED，**CO5300**，QSPI（80 MHz，4 线）|
| 触摸 | CST9217（I2C）|
| 图形库 | LVGL 8.4（`LV_COLOR_DEPTH 16`）|
| 显示驱动 | Arduino_GFX 的 `Arduino_CO5300` + `Arduino_ESP32QSPI` |
| **面板地址偏移** | 可见区在 GRAM 内偏移 **6 列**。官方 IDF BSP 原文：`esp_lcd_panel_set_gap(panel_handle, 0x06, 0);` |
| 构造参数 | `Arduino_CO5300(bus, RST, 0, 466, 466, /*col1*/6, /*row1*/0, /*col2*/0, /*row2*/0)` |

**面板 TE 信号可用**：`GPIO13 = LCD_TE`（官方 `HARDWARE_REFERENCE.md` 引脚表），
实测 59.6 Hz（周期 16.78 ms），但配套驱动的初始化序列里
`// WRITE_C8_D8, CO5300_WC_TEARON, 0x00,` 是**注释掉的**。

---

## 2. 改动前（显示正常，但有撕裂）

```c
disp_drv.sw_rotate = 1;
lv_disp_set_rotation(NULL, LV_DISP_ROT_270);   /* 90°/270° 均如此 */
/* 面板 MADCTL 从不写 → 保持上电默认 0x00 */
/* 库的 setRotation() 从未被调用 → _xStart=6, _yStart=0（构造值）*/
```

- **显示：完整、方向正确**（用户确认"以前显示是正常的"）
- **问题：90°/270° 下画面撕裂**（0°/180° 不撕裂）

**已定位的撕裂根因**（实测数据）：

| 指标 | 实测 |
|------|------|
| 整屏 flush 耗时 | **43 ms** |
| 面板帧周期（59.6 Hz） | **16.78 ms** |
| LVGL 软件旋转 90/270° 时渲染 | **25–33 ms**，偶发 **1005 ms** 尖刺 |
| 面板帧周期对比 | 更新总耗时恒 > 一帧 → 必然跨帧 → 撕裂 |

`43 ms` 的构成（用 40/80 MHz 两个时钟 A/B 解出）：

- **约 11 ms** 是真正的 QSPI 传输（正好等于 80 MHz QIO 理论值 10.9 ms）
- **约 32 ms 是 CPU 侧固定开销** —— `Arduino_ESP32QSPI::writePixels()` 中把像素
  从 PSRAM **逐像素搬进内部 DMA 缓冲并重排**的循环：
  ```c
  for (i < l2) { p1 = *data++; p2 = *data++; MSB_32_16_16_SET(_buffer32[i], p1, p2); }
  ```
  该搬移**无法绕过**：LVGL 缓冲在 PSRAM，而 ESP32-S3 的 SPI DMA 只能从内部 RAM 取数。
- 已排除：`ESP32QSPI_MAX_PIXELS_AT_ONCE` 4096→16384（事务数 53→14）**耗时纹丝不动**。

> 另：软件 270° 下的 **1005 ms 渲染尖刺**会导致**中断看门狗复位**（见 §6，已单独修复）。

---

## 3. 改动后（撕裂解决，但显示不完整）

**把旋转改为面板硬件旋转**：

```c
/* 90/270° 走硬件旋转，LVGL 不旋转 */
90°  → MADCTL 0x60 / 0xA0（见下）
270° → MADCTL 0x60
lv_disp_set_rotation(NULL, LV_DISP_ROT_NONE);
gfx->setRotation(librot);        /* 让库重新分配 _xStart/_yStart */
bus->writeC8D8(0x36, madctl);    /* 再覆盖 MADCTL 为硬件旋转值 */
```

**遇到的第一个问题：库的偏移只能加不能减**

- `Arduino_TFT::setRotation(r)` 按索引把偏移分配到不同轴，本工程构造参数下**只有 3 种组合**：
  `case0 → (6,0)`、`case1 → (0,0)`、`case3 → (0,6)`
- `_xStart/_yStart` 是 `uint8_t` → **无法表达负偏移**
- 而旋转后需要的偏移**很可能是负的**（MADCTL 的 MX/MY 会反转扫描方向）

**解决**：新增子类重写**虚函数** `writeAddrWindow()`（`Arduino_TFT` 里是 `virtual ... = 0`），
用 `int16_t` 承接**有符号**偏移：

```cpp
class DashboardCO5300 : public Arduino_CO5300 {
  void writeAddrWindow(int16_t x, int16_t y, uint16_t w, uint16_t h) override {
    if (changed) {
      int32_t xs = (int32_t)x + _ox, ys = (int32_t)y + _oy;   /* _ox/_oy 为 int16_t */
      _bus->writeC8D16D16(CO5300_W_CASET, xs, xs + w - 1);
      _bus->writeC8D16D16(CO5300_W_PASET, ys, ys + h - 1);
    }
    _bus->writeCommand(CO5300_W_RAMWR);
  }
};
```

并加了串口命令：`gap <x> <y>`（运行时设偏移，支持负值）、`edges`（画标定图形）。

---

## 4. 已验证的事实

### 4.1 MADCTL 四角度实测（LVGL 不旋转）

| MADCTL | 实测 |
|--------|------|
| `0x60` | 用户确认：**USB 口朝下 + 内容正立**（即本项目要的装机姿态）|
| `0xC0` | 呈 180° |
| `0xA0` | USB 口朝上 |
| `0x20` | 仅 MV（不镜像）→ 用户反馈"方向反了"，故**镜像位是必需的** |

> 官方 BSP 的映射同样是 `rotation 0/1/2/3 → MADCTL 0x00/0x60/0xC0/0xA0`。

### 4.2 撕裂与渲染耗时

| 配置 | 撕裂 | 渲染最大 | 尖刺 |
|------|------|---------|------|
| 软件 0° | 无 | 22–69 ms | 无 |
| 软件 90° | **有** | 25–33 ms | — |
| 软件 180° | 无 | — | 无 |
| 软件 270° | **有** | 28–**1005** ms | **有** |
| **硬件 270°** | **无** | 24–25 ms | **无** ✅ |

### 4.3 用标定图形测出的**轴映射**（硬件 270°，MADCTL 0x60）

在**逻辑画面**四边画 10px 彩线（后改为不与 UI 撞色的洋红/青/亮绿/白）：

| 逻辑边 | 落在物理位置 |
|--------|-------------|
| `x=0`（左） | **上** |
| `x=465`（右） | **下** |
| `y=0`（上） | **右** |
| `y=465`（下） | **左** |

即：物理位置 ≈ 逻辑坐标旋转 90°（`px = 465 - y`, `py = x` 之一）。

### 4.4 偏移标定

在逻辑边界画一圈细黑环，请用户目视"**黑环到屏幕边框的间距是否四周均匀**"：

| gap | 用户反馈 |
|-----|---------|
| (0, 0) | 不均匀 |
| (0, 6) | 好多了，但仍**左宽右窄** |
| (0, 12) | 变成**右宽左窄** |
| **(0, 9)** | **四周均匀 ✓**（已固化为 270° 的默认值）|

> 注意：面板的物理偏移是 **6**，但标定结果是 **9**（±1~2 不确定）。
> 这个差异本身可能就是线索。

---

## 5. ⚠️ 未解决：用户反馈"左面缺一块"

**用户描述**（原话）：

- "左方跟下方有一些遮蔽"
- "左侧窄带消失了，但是下方的窄带跟上方的有一条线"
- "现在圆圈四周均匀了，但是**左侧还是缺了一角**"
- 追问后确认：缺的位置是**左侧中部**，即**胶囊 / `BT OFF` 那一块**
- **清除全部标定线后仍然存在**

**矛盾点**：我方对设备照片的分析显示**左侧内容是完整的**：

- 放大设备照片的左侧中部：能看到青色标定带（逻辑 `y=456..465`）**正好贴在可见区左边缘**，
  与黑边框之间**没有黑缝**
- 模拟器在 `--rot 0` 与 `--rot 270` 下的渲染**逐像素等价**
  （连"圆外仍有内容的像素数"都是 1948，完全一致）

**用户指出关键时间线**（很重要）：

> "以前显示是正常的，**修改好画面撕裂问题后才出现的显示不全**"
> "刚刚一瞬间正常，但是卡了一会后又不正常了"（那次是设备看门狗复位，
> 因 `swrot` 命令不持久化，复位后从 NVS 恢复成硬件旋转）

### 5.1 相关 UI 代码（速度表盘）

被怀疑的对象是底部胶囊 —— 但**它本身落在画面内**：

```c
lv_obj_t *subdial = lv_obj_create(p);
lv_obj_set_size(subdial, 86, 86);
lv_obj_set_style_radius(subdial, LV_RADIUS_CIRCLE, 0);
lv_obj_set_style_bg_color(subdial, lv_color_hex(C_BG_LIGHT), 0);
lv_obj_set_style_border_width(subdial, 2, 0);
lv_obj_align(subdial, LV_ALIGN_BOTTOM_MID, 0, -8);      /* y ≈ 372..458，未越界 */
/* 内部：s_hud5 / "km/h" / divider / s_status_dot / s_status5 */
```

> 完整代码见 `simulator/ui/ui_speed.c` 的 `build_v5()`（约 178–230 行）。
> 注意 `simulator/ui/` 是**单一真源**，固件副本由 `tools/sync_firmware.sh` 同步，
> **不要直接改 `firmware/dashboard/ui_speed.c`**。

### 5.2 待排查方向（供诊断方参考）

1. **MADCTL 0x60 是否真的等价于"旋转 270°"？**
   标定结果（§4.3）显示逻辑边 → 物理边的映射成立，但**是否含镜像**尚未逐点验证。
   若含镜像，则"内容完整"与"看起来缺一块"可以同时成立（镜像 + 偏移）。
2. **面板可见区是否真的 466×466？**
   官方 BSP 只设了列偏移 6、行偏移 0；但标定出的最优偏移是 9（不是 6）。
   怀疑**行方向也有偏移**（例如可见行范围不是 [0,465]）。
3. **`_xStart/_yStart` 之外，库是否还有别处影响寻址？**
   `Arduino_TFT::setRotation()` 还会改 `_width/_height`（方形屏无影响）与 `_current*`。
4. **LVGL 侧**：`sw_rotate=1` 且 `rotated=NONE` 时，`lv_refr.c` 是否仍走某条特殊路径？
   （`disp_drv.rotated` 已确认是 `LV_DISP_ROT_NONE`）
5. **建议的诊断手段**：把面板 GRAM 读回（CO5300 `0x2E RAMRD`，若 QSPI 读通路可用），
   或画一个**已知坐标的稀疏网格**，从照片反解仿射变换矩阵（含镜像与否）。

---

## 6. 本轮同时修复的问题（已完成，供参考）

### 6.1 「点一下屏幕就黑屏重启」（**已修复**）

- **现象**：点屏幕 → 黑一下 → 重启回固件初始状态（复位原因码 5 = 中断看门狗）
- **根因**：`touch_sample_cb()` 是 **100Hz esp_timer 回调**，
  其中「抬起且有位移」分支**在 `portENTER_CRITICAL()` / `portEXIT_CRITICAL()` 之间
  调用了 `Serial.printf()`** → 关中断状态下等 UART FIFO → 超过 300 ms → IWDT 复位
- **为什么只有点屏幕触发**：那两处打印只在"抬起且有位移"时执行
- **修复**：日志值先存局部变量，**出了临界区再打印**；并全文件扫描确认无同类问题
- **验证**：用户连点十余次不再重启；日志 14 条触摸事件正常，uptime 连续增长

### 6.2 触摸坐标变换（硬件旋转必需）

硬件旋转时 LVGL 不做旋转，故触摸坐标必须自己变换。
在**采样处**变换（`touch_sample_cb`），使 LVGL 的 indev 与 flick 手势判定**都自动正确**：

| 档位 | 实测 |
|------|------|
| 0 不变换 | 错位 |
| 1 顺时针 90° | 能响应但方向反了 |
| **3 逆时针 90°** | ✅ **完全跟手**（270° 的默认）|

同时因 `s_display_rot = 0`，`flick_delta_to_logical()` 自然退化为空操作。

### 6.3 MADCTL 是寄存器，NVS 记不住

必须持久化并在启动时重放（`restore_display_rotation()` 读 NVS 的 `rot`）。
本轮踩过：一次 IWDT 复位后方向跳回默认。

---

## 7. 复现与调试手段

### 串口命令

| 命令 | 作用 |
|------|------|
| `gap [x y]` | 查看/设置面板地址窗口偏移（**支持负值**，运行时，无需重烧）|
| `edges` | 画/清四边彩线（10px）+ 逻辑边界细黑环，用于标定 |
| `madctl <hex>` | 实验：直写面板 MADCTL(0x36) |
| `librot 0-3` | 切换**库的**旋转索引（其偏移分配不同）|
| `swrot [0-3]` | **A/B 用**：强制纯软件旋转（= 改造前的旧行为）|
| `hwrot [hex]` | 手动开关硬件旋转（持久化）|
| `touchrot [0-3]` | 触摸坐标变换档位（持久化）|
| `te` / `te sync on\|off` | TE 引脚诊断与 V-blink 同步（已实现，但对撕裂无效）|
| `ble` / `face N` / `bright N` | 状态查询 / 切表盘 / 亮度 |

### 构建（**必须先同步共享源码**）

```bash
tools/sync_firmware.sh          # simulator/ui/ → firmware/dashboard/（单一真源）
arduino-cli compile \
  --fqbn "esp32:esp32:esp32s3:FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,USBMode=hwcdc,CDCOnBoot=cdc,PSRAM=opi" \
  --build-property "compiler.cpp.extra_flags=-DESP32QSPI_MAX_PIXELS_AT_ONCE=4096" \
  --libraries reference/ESP32-S3-Touch-AMOLED-1.75/examples/arduino/libraries \
  firmware/dashboard
```

### 模拟器（离线复现 UI）

```bash
cd simulator && make
./dashboard_sim --page 2 --speed 88 --rot 270 --offscreen --ms 2500 --shot /tmp/a.bmp
```

---

## 8. 相关提交

| 提交 | 内容 |
|------|------|
| `b761646` | **修复点屏幕黑屏重启**（临界区内 Serial） |
| `9e11417` | 地址偏移可运行时设定 + 四边彩线标定 + 定位底部"削平" |
| `e5854f8` | 用面板硬件旋转解决 90°/270° 撕裂（视觉角度成为唯一真相来源）|
| `bb34eb7` | 实测确认 CO5300 硬件旋转可用（推翻库注释）|
| `b9b3f28` | TE 撕裂同步（发现 TE 引脚 + V-blank 对齐，证实对撕裂无效）|
| `208c91d` | 撕裂根因定位（失效区域 20~44%）|

## 9. 关键文件

| 文件 | 说明 |
|------|------|
| `firmware/dashboard/dashboard.ino` | 主固件：旋转机制选择、`DashboardCO5300` 子类、触摸变换、各调试命令 |
| `simulator/ui/ui_speed.c` | 速度表盘 UI（**单一真源**，含被怀疑的底部胶囊 `subdial`）|
| `docs/rotation-tearing-fieldtest.md` | 旋转/撕裂的完整现场记录（含用户逐项观察）|
| `docs/perf/` | 指针流畅度实测方法与前后基线 |
