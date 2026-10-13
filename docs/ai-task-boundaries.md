# AI 任务边界规范

> 目的：**让 AI 只专注自己的任务域，不越界改坏别处，同时产出能直接跑起来的代码。**
>
> 核心问题：AI（尤其图形能力强的模型）改 UI 时，容易顺手"优化"共享逻辑、
> 改接口签名、动构建配置 → 结果编译不过或引入回归。
> 本文用**明确的边界 + 可验证的契约**解决这个问题。

---

## 一、怎么用（给使用者）

选中下面的**任务模式**，把对应文件的内容 + 你的需求一起交给 AI：

| 我要做的事 | 任务模式文件 | 推荐模型能力 |
|-----------|-------------|-------------|
| 改界面外观/布局/配色/字体 | [`tasks/ui-only.md`](tasks/ui-only.md) | **图形/视觉能力强** |
| 新增一个表盘 | [`tasks/new-face.md`](tasks/new-face.md) | 图形 + 基础 C |
| 改 iPhone App | [`tasks/ios-app.md`](tasks/ios-app.md) | Swift/SwiftUI |
| 改 BLE 协议 | [`tasks/ble-protocol.md`](tasks/ble-protocol.md) | 通信/协议 |
| 改共享业务逻辑 | [`tasks/shared-logic.md`](tasks/shared-logic.md) | 逻辑严谨 |
| 改固件主流程/硬件 | [`tasks/firmware-core.md`](tasks/firmware-core.md) | 嵌入式 |

**用法示例**（改 UI）：
```
请阅读 docs/tasks/ui-only.md，严格按其中边界工作。
需求：把速度表指针改成蓝色，并加粗 2px。
```

**AI 的产出必须包含**（模板见各任务文件末尾）：
1. 改了哪些文件
2. 是否触碰了边界外内容（应为"否"）
3. 运行了哪些验证、结果如何
4. 未验证的部分（诚实标注）

---

## 二、边界总图（所有任务模式共享）

```
                     ┌──────────────────────────────┐
                     │  🔴 禁区（任何 UI 任务都不许改）│
                     │  firmware/dashboard/*.ino     │
                     │  firmware/dashboard/*.cpp     │
                     │  ios/**                       │
                     │  tools/**（除指定生成器）      │
                     │  tests/**（除新增测试）        │
                     │  project.yml / .gitignore     │
                     │  docs/**（除注明）            │
                     └──────────────────────────────┘
                                    ▲
        ┌───────────────────────────┴────────────────────────┐
        │  🟡 契约层（可调用，不可改签名）                      │
        │  simulator/ui/*.h  ← 头文件是"接口合同"              │
        │  改 .c 实现可以，改 .h 里的函数签名/结构体 = 越界      │
        └───────────────────────────┬────────────────────────┘
                                    ▲
        ┌───────────────────────────┴────────────────────────┐
        │  🟢 工作区（任务允许修改的范围）                      │
        │  UI 任务：simulator/ui/ui_{speed,analog,clock}.c     │
        │  等具体文件（见各任务模式）                           │
        └──────────────────────────────────────────────────────┘
```

### 🔴 最关键的一条：不要改固件目录里的共享层文件

`firmware/dashboard/` 下的 `ui_*.c/h`、`telemetry.*`、`control.*`、`font_*.c`
都是 `tools/sync_firmware.sh` **从 `simulator/ui/` 复制过来的产物**（已 gitignore）。

- ❌ 改它们 → 下次同步被覆盖，且 git 里看不到你的改动
- ✅ 只改 `simulator/ui/` 里的源文件，然后跑 `tools/sync_firmware.sh`

---

## 三、跨任务通用铁律（违反必然出问题）

| # | 铁律 | 原因 |
|---|------|------|
| 1 | **只改 `simulator/ui/`**，不要改 `firmware/dashboard/` 里的同名文件 | 那是同步产物，会被覆盖 |
| 2 | **不改 `.h` 里的函数签名/结构体/枚举** | 固件与 App 都依赖，改了连锁编译失败 |
| 3 | **LVGL setter 必须做变更检测** | `lv_meter_set_indicator_value` 等无条件 invalidate，不检测会导致空闲也高频重绘 → 全局卡顿 |
| 4 | **不改 `lv_disp_drv_register()` 等初始化顺序** | 曾误删导致 `lv_scr_act()` 返回 NULL → 启动 panic 循环 |
| 5 | **不在 `setup()` 之外操作硬件** | 显示/触摸/RTC 初始化只在 setup |
| 6 | **改完必须跑验证**（见各任务的最小验证集）| 不验证的改动等于没改 |
| 7 | **不引入新依赖/新库** | 避开 Arduino 只编译 sketch 目录的限制与体积膨胀 |
| 8 | **不"顺手优化"边界外代码** | 即使看到可以改进的地方，也只在产出里**提议**，不动手 |

---

## 四、通用验证命令

```bash
# 1) 编译模拟器（最快，先过这关）
cd simulator && make -j8

# 2) 全部单测（207 项）
cd tests && make run PYTHON=<python路径>

# 3) UI 像素级回归（10 项，改 UI 必跑）
python3 tools/ui_regression.py

# 4) 同步到固件（改了 simulator/ui/ 后必做）
tools/sync_firmware.sh

# 5) 固件编译（上板前必做）
arduino-cli compile \
  --fqbn "esp32:esp32:esp32s3:FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,USBMode=hwcdc,CDCOnBoot=cdc,PSRAM=opi" \
  --build-property "compiler.cpp.extra_flags=-DESP32QSPI_MAX_PIXELS_AT_ONCE=4096" \
  --libraries reference/ESP32-S3-Touch-AMOLED-1.75/examples/arduino/libraries \
  firmware/dashboard
```

> `<python路径>`：需要带 Pillow。本机为
> `/Users/pwh/.dsh/dsh-runtimes/dsh-primary-runtime/dependencies/python/bin/python3`

---

## 五、产出模板（AI 必须按此格式回答）

```markdown
## 改动清单
| 文件 | 改动 |
|------|------|
| simulator/ui/ui_speed.c | 指针颜色 C_WARN→蓝色 |

## 边界检查
- [ ] 未修改任何 .h 里的签名
- [ ] 未修改 firmware/dashboard/ 下的文件
- [ ] 未修改 ios/、tools/、tests/
- [ ] 未引入新依赖

## 验证结果
| 步骤 | 命令 | 结果 |
|------|------|------|
| 编译 | `cd simulator && make -j8` | ✅ 通过 |
| 单测 | `cd tests && make run` | ✅ 207 项 |
| 像素回归 | `python3 tools/ui_regression.py` | ✅ 10 项 |
| 同步 | `tools/sync_firmware.sh` | ✅ |
| 固件编译 | (命令) | ✅ Flash 75% |

## 未验证 / 需人工确认
- 真机观感（模拟器无法完全代表 AMOLED 实际色彩）
```

---

## 六、为什么要这么细（设计理由）

**问题**：把整个仓库上下文丢给 AI，会出现三类损耗：
1. **Token 浪费**：AI 读大量无关代码（固件初始化、BLE 栈、iOS 工程）
2. **越界风险**：AI"顺手"改了共享逻辑或接口，破坏了别的功能
3. **产出不可用**：改完编译不过，或不符合项目约定（如漏了同步步骤）

**本方案的对策**：
- **只给必要上下文**：每个任务模式文件只描述该任务需要的文件 + 契约 → 省 token
- **红线写死**：明确"不许碰什么"比"要做什么"更重要 → 防越界
- **契约即文档**：把头文件当接口合同列出，AI 照抄签名就不会写错 → 可直接运行
- **验证自证**：要求 AI 跑指定命令并贴结果 → 产出可信
- **模板化产出**：统一格式，便于你快速审阅

---

## 七、文件索引

```
docs/
├── ai-task-boundaries.md      ← 本文（总纲 + 通用铁律 + 产出模板）
├── tasks/
│   ├── ui-only.md             ← 只改 UI 外观
│   ├── new-face.md            ← 新增表盘
│   ├── ios-app.md             ← 改 iPhone App
│   ├── ble-protocol.md        ← 改 BLE 协议
│   ├── shared-logic.md        ← 改共享业务逻辑
│   └── firmware-core.md       ← 改固件主流程/硬件
├── development-guide.md       ← 人类开发指引（「改哪里」+ 已验证能力）
├── test-results.md            ← 测试台账（避免重复测试）
└── troubleshooting.md         ← 踩坑记录
```
