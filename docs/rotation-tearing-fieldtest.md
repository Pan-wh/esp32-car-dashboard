# 现场测试清单：屏幕旋转 / 撕裂（果冻效应）

> **用途**：到公司后按 §2 → §3 顺序执行，每步都有「操作 → 期望 → 记录」。
> **前置背景**：本清单基于 2026-10-13 的源码核对 + 模拟器实测（结论见
> [`troubleshooting.md`](troubleshooting.md) 与本文 §0）。
> ⚠️ **本文所有"期望"都是待实测假设，不是已验证结论**——请如实填写 §6 记录表。

---

## 0. 已核实的事实（30 秒读完，决定后面怎么判）

| # | 事实 | 依据 |
|---|------|------|
| 1 | 旋转**调用方式与官方 Arduino 例程一致**：`sw_rotate = 1` + `lv_disp_set_rotation()`，且未开 `full_refresh`/`direct_mode` | `firmware/dashboard/dashboard.ino` 的 `lvgl_init()`；官方 `06_LVGL_Widgets.ino` |
| 2 | 撕裂**不是旋转方法造成的**：面板是带 GRAM 的 AMOLED，QSPI 写入是**阻塞轮询**，与面板扫描无任何同步；整屏 434 KB 实测 **43 ms**（≈ 2.6 个 60 Hz 场周期）→ 0° 时同样存在 | `Arduino_ESP32QSPI::writePixels()`；`docs/test-results.md` |
| 3 | 软件旋转会**放大**它：90°/270° 下一次刷新被拆成多次 flush（模拟器实测：数字时钟每次更新 **0°=1 次 → 90°≈11 次**），且旋转路径强制「渲染→flush」串行 | `lv_refr.c` 的 `draw_buf_rotate()`；模拟器 flush 计数 |
| 4 | "官方"有**两条并存路线**：Arduino 例程＝LVGL 软件旋转；微雪当前 IDF BSP＝**面板 MADCTL 硬件旋转** + 匹配的触摸映射。Espressif 官方适配器文档明确：QSPI 类屏只支持 `NONE` / `TE_SYNC` 两种防撕裂模式，`TE_SYNC` 需要 TE 接 GPIO | 微雪 BSP `bsp_display_rotation_set()`；[esp_lvgl_adapter 文档](https://components.espressif.com/components/espressif/esp_lvgl_adapter) |
| 5 | 本工程**自定义的 100 Hz flick 手势判定用了未旋转的原始坐标**（LVGL 自身的 indev 会自动逆变换，这条快路径绕过了它）→ 90°/270° 时"左右滑"在面板上变成上下滑，flick 判不出来，翻页退化成慢速原生拖拽 | `dashboard.ino` 的 `touch_sample_cb()` |
| 6 | 面板 TE 信号在原理图上存在（`NLLCD0TE`，面板连接器 **J30 第 11 pin**），但 BSP / `pin_config.h` **都没有对应 GPIO**，Arduino 驱动初始化里的 `0x35 TEARON` 也是注释掉的 | 原理图 PDF；`Arduino_CO5300.h` |

---

## 1. 准备

1. 设备接 USB，串口 115200（固件已开 CDC，`/dev/cu.usbmodem101`）。
2. 记录当前 Git commit（`git rev-parse --short HEAD`）与本次烧录的固件来源。
3. 两种构建命令（**注意 `--build-path` 每次换目录，官方文档踩过复用 build-path 漏编译的坑**）：

```bash
CLI="/Applications/Arduino IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli"
FB="esp32:esp32:esp32s3:FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,USBMode=hwcdc,CDCOnBoot=cdc,PSRAM=opi"
LIB="reference/ESP32-S3-Touch-AMOLED-1.75/examples/arduino/libraries"

# A) 当前配置（QSPI 80 MHz）
"$CLI" compile --fqbn "$FB" --build-path /tmp/b_$(date +%s) \
  --build-property "compiler.cpp.extra_flags=-DESP32QSPI_MAX_PIXELS_AT_ONCE=4096" \
  --libraries "$LIB" --upload -p /dev/cu.usbmodem101 firmware/dashboard

# B) A/B 对照：QSPI 回官方默认 40 MHz（不改代码，只加 -D）
"$CLI" compile --fqbn "$FB" --build-path /tmp/b40_$(date +%s) \
  --build-property "compiler.cpp.extra_flags=-DESP32QSPI_MAX_PIXELS_AT_ONCE=4096 -DQSPI_SPEED_HZ=40000000" \
  --libraries "$LIB" --upload -p /dev/cu.usbmodem101 firmware/dashboard
```

4. **先跑一次编译**，确认能过再上板（若此时工作区正被其他改动占用，先确认它们已收敛）。

---

## 2. 阶段 0：判定"撕裂属于硬件固有，还是配置/旋转引入"

### 2.1 基线观察（当前 80 MHz 固件，逐项记录到 §6）

| 步骤 | 操作 | 期望 / 观察点 |
|------|------|--------------|
| 0-1 | `lvrot 0`（0°），静置 30 s | 静止表盘（秒针/数字）是否撕裂？整屏切页时撕裂条数、方向、是否"爬行" |
| 0-2 | `lvrot 1`（90°），静置 30 s，再左右滑动翻页 | 与 0-1 对比：撕裂是否更明显？滑动是否"滑不动/要等一会"？ |
| 0-3 | `lvrot 3`（270°）| 同 0-2 |
| 0-4 | `lvrot 2`（180°）| **对照组**：180° 不拆块（1 次/更新），若撕裂与 90° 明显不同，说明"拆块"是因素之一 |
| 0-5 | `lvrot 0` | 回到 0°，准备下一步 |

> 每步都记：`[重绘 x/s]`、`[渲染最大 xx ms]`、`[QSPI 传输 单次最大 xx ms]`（心跳每 5 s 打印一次）。

### 2.2 QSPI 时钟 A/B（用 §1 的构建命令 B，重新烧录）

| 步骤 | 操作 | 期望 / 观察点 |
|------|------|--------------|
| 0-6 | 烧 40 MHz 固件，重复 0-1 ~ 0-4 | **若撕裂/花屏明显变化 → 是 QSPI 时序问题（80 MHz 不稳），与旋转无关**；心跳里的「QSPI 单次最大」应从 ~43 ms 变回 ~54 ms |
| 0-7 | 判定：40 MHz 更干净？ | 是 → 暂时用 40 MHz（或后续做时序收敛）；否 → 回到 80 MHz 固件继续 |

### 2.3 MADCTL 硬件旋转验证（**本次最关键的一步，5 分钟定性**）

| 步骤 | 操作 | 期望 / 观察点 |
|------|------|--------------|
| 0-8 | `lvrot 0` 然后 `madctl 60` | **画面整体是否旋转 90°？**（转 = MV 位有效，说明可走硬件旋转路线）。**此时触摸必然错位，属正常，只看画面** |
| 0-9 | `madctl C0` / `madctl A0` | 是否分别呈 180° / 270°（方向对不对，记下来） |
| 0-10 | 在 `madctl 60` 状态下观察 30 s | ① 有无花屏/噪点；② **圆屏内容是否偏心（列偏移 6 px 是否落到错轴）**；③ 撕裂是否变化 |
| 0-11 | `madctl 0` 恢复；再 `lvrot 1` 回到软件 90° | 恢复原状 |

**判定树**

```
0-8 画面确实转了？
├─ 是 → 阶段 2A：把旋转正式改成 MADCTL（零每帧开销，取消软件旋转拆块）
│        ⚠️ 必须同时改触摸映射，否则触摸错位
└─ 否 → 阶段 2B：留在 LVGL 软件旋转，只做观感优化（§4）
另：若 0-6 显示 40 MHz 明显更干净 → 先解决 QSPI 时序，再谈旋转
```

---

## 3. 阶段 1.1：flick 旋转补偿（**已应用**，2026-10-13 晚）

> 目的：90°/270° 下恢复"一滑即翻"，不再退化成慢速拖拽（这是旋转后观感骤降的主因之一）。
>
> **状态**：已写入 `firmware/dashboard/dashboard.ino` 并**整体编译通过**（Flash 56% / RAM 38%）；
> 映射逻辑用 9676 个采样点与 LVGL `indev_pointer_proc` 的字面公式逐点比对，**全部一致**。
> ⚠️ **仍未烧录、未上机**：烧录后的 A/B 必须按 §3.2 现场做。
> 若后续并行改动把它覆盖了，检查命令：`grep -c "flickrot" firmware/dashboard/dashboard.ino`（应为 4）。

### 3.1 改动内容（3 处，全部在 `firmware/dashboard/dashboard.ino`，**已应用**，此处备查/重贴）

**(1) 触摸全局区新增开关与逆变换函数**（锚点：`static portMUX_TYPE s_tp_mux = portMUX_INITIALIZER_UNLOCKED;` 之后）

```c
/*
 * flick 方向判定的**旋转补偿**。
 * 采样拿到的是**面板原始坐标**，而"左滑/右滑"应按 **LVGL 逻辑坐标**判定。
 * LVGL 自己的 indev 会对指针坐标做逆变换（lv_indev.c: indev_pointer_proc），
 * 但这条 100Hz 自建快路径绕过了它 → 90°/270° 时"左右滑"在面板上变成上下滑，
 * flick 判不出来，翻页退化成慢速原生拖拽。
 * 默认开启；串口 `flickrot 0|1` 可运行时 A/B。
 */
static bool s_flick_rot_fix = true;

/** 面板原始位移 → LVGL 逻辑位移（映射与 lv_indev.c 的旋转规则严格一致）*/
static void flick_delta_to_logical(int *dx, int *dy)
{
  if (!s_flick_rot_fix) return;
  lv_disp_t *d = lv_disp_get_default();
  if (d == NULL || d->driver == NULL) return;

  int rx = *dx, ry = *dy;   /* 面板原始位移 */
  switch (d->driver->rotated) {
    case LV_DISP_ROT_90:  *dx = -ry; *dy =  rx; break;  /* x'=V-y-1, y'=x */
    case LV_DISP_ROT_180: *dx = -rx; *dy = -ry; break;  /* 双轴取反 */
    case LV_DISP_ROT_270: *dx =  ry; *dy = -rx; break;  /* x'=y, y'=H-x-1 */
    default: break;                                     /* NONE：原始即逻辑 */
  }
}
```

**(2) `touch_sample_cb()` 释放分支**：先取原始位移，换算成逻辑位移后再判定，日志同时打印两者

```c
      int raw_dx = (int)s_tp_x - (int)s_tp_start_x;
      int raw_dy = (int)s_tp_y - (int)s_tp_start_y;
      uint32_t dt = millis() - s_tp_start_ms;
      int dx = raw_dx, dy = raw_dy;
      flick_delta_to_logical(&dx, &dy);   /* 逻辑坐标（判定与翻页方向都用它）*/
      ...
        Serial.printf("[触摸] flick dx=%d dy=%d（原始 %d,%d）dt=%lu ms\n", (int)dx, (int)dy,
                      raw_dx, raw_dy, (unsigned long)dt);
```

**(3) 串口命令 `flickrot [0|1]` + `help` 一行**（新分支插在 `else if (strncmp(cmd, "face", 4) == 0) {` 之前）

```c
  else if (strncmp(cmd, "flickrot", 8) == 0) {
    const char *arg = cmd + 8;
    if (*arg == '\0') {
      Serial.printf("[触摸] flick 旋转补偿 = %d（0=旧行为 1=修正）\n", s_flick_rot_fix ? 1 : 0);
      Serial.println("       用法: flickrot 0|1（现场 A/B：切到 90° 后左右滑动对比）");
    }
    else {
      s_flick_rot_fix = (atoi(arg) != 0);
      Serial.printf("[触摸] flick 旋转补偿 -> %d\n", s_flick_rot_fix ? 1 : 0);
    }
  }
```

### 3.2 应用后验证（现场 A/B，同一个固件内切换，无需反复烧录）

| 步骤 | 操作 | 期望 |
|------|------|------|
| 1-1 | `lvrot 1`（90°）→ `flickrot 0` → 左右滑动 5 次 | **旧行为复现**：滑动迟滞、要等一会才能再滑（日志里 flick 计数不涨） |
| 1-2 | `flickrot 1` → 同样滑动 5 次 | 每次快滑立即翻页；日志 `[触摸] flick dx=-424 dy=-47（原始 ...）` |
| 1-3 | `lvrot 0` → 两种设置各滑 5 次 | 都应正常（0° 下补偿是恒等变换） |
| 1-4 | `lvrot 3`（270°）重复 1-1/1-2 | 同 90° 结论 |
| 1-5 | 串口冒烟：`help`/`ble`/`mem`/`face`/`spd 60`/`touch` | 命令齐全、无 panic、空闲堆无异常下降 |

**验收判据**：1-2 与 1-4 中"快滑必翻页"（连续 10 次成功 ≥ 9 次），且 0° 行为无回归。

---

## 4. 阶段 2（等阶段 0 结论再决定）

- **2A（MADCTL 生效时）**：旋转改为面板 `0x36`（`0x00/0x60/0xC0/0xA0`）+ `sw_rotate = 0`，配套改：
  - 触摸映射（`TouchDrvCST92xx` 的 swap/mirror，与官方"旋转与触摸变换成套"要求一致）；
  - flick 补偿退化为恒等（可保留代码，`rotated` 恒为 `NONE`）；
  - 列偏移 6 px 的落轴（0-10 已观察）；
  - 四方向触摸实测 + NVS 持久化不变。
  - ⚠️ 这条**不消除撕裂**，只是去掉软件旋转的额外开销与拆块。
- **2B（MADCTL 无效时）**：留在软件旋转；可选把 `LV_DISP_ROT_MAX_BUF` 调大以取消拆块，但**必须同时把 LVGL 堆挪到 PSRAM**（当前 `LV_MEM_SIZE = 48 KB` 内部 RAM；分配失败会返回 NULL 并在 `draw_buf_rotate_90()` 解引用 → 崩溃）。优先级低。
- **2C（与 A/B 正交，成本最低）**：滑页动画改为**瞬时切换**或缩短到 ~2 帧。整屏 43 ms 写入无法避免，但"没有跨帧运动参照"时撕裂几乎看不出来——建议 A/B 后用观感决定。

---

## 5. 阶段 3：根治路线（TE 同步）

1. 确认 TE 是否引到可用 GPIO：查图纸 / 量 J30-11 通断 / 问微雪。**这是唯一能真正消掉撕裂的路径。**
2. 可行 → 发 `0x35`（TEON，V-blank 模式）+ 等 TE 边沿再 flush；
3. 不可行 → 备选：CO5300 的 `0x45 Get Scan Line Number` 做软件扫描线同步（实验性，需先验证 QSPI 读通路）。

---

## 6. 现场记录表（打印/复制填写）

| 步骤 | 配置 | 观察到的现象（撕裂条数/方向/滑动手感/花屏） | 心跳关键值 | 结论 |
|------|------|------------------------------------------|-----------|------|
| 0-1 | `lvrot 0` / 80 MHz | | 重绘 __/s，渲染最大 __ ms，QSPI 最大 __ ms | |
| 0-2 | `lvrot 1` / 80 MHz | | | |
| 0-3 | `lvrot 3` / 80 MHz | | | |
| 0-4 | `lvrot 2` / 80 MHz | | | |
| 0-6 | `lvrot 1` / **40 MHz** | | | |
| 0-8 | `madctl 60`（`lvrot 0`）| 是否旋转：__；偏心：__；花屏：__ | | |
| 0-9 | `madctl C0` / `A0` | | | |
| 1-1 | 90° + `flickrot 0` | | | |
| 1-2 | 90° + `flickrot 1` | | | |
| 1-3 | 0° + 两种设置 | | | |

## 7. 回退与安全

- 启动 panic 循环 → **BOOT + RESET** 进下载模式，烧回上一版可用固件（见 `docs/tasks/firmware-core.md` §8）。
- 方向恢复：`madctl 0`（面板）、`lvrot 0`（LVGL）。
- 本清单全部操作在桌面完成，不要边开车边测。

## 8. 结论（测试后填写）

- [ ] 撕裂是否与旋转相关：__
- [ ] 撕裂是否与 QSPI 时钟相关：__
- [ ] MADCTL 是否可用：__
- [ ] flick 补偿是否解决问题：__
- [ ] 下一步决定：2A / 2B / 2C / 3
