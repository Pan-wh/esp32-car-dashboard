# 任务模式：改共享业务逻辑

> **适用**：改遥测解析/校验/平滑/斜坡/防抖/行程统计、控制命令解析。
> **推荐模型**：逻辑严谨型。
> **预计 Token**：中低

---

## 0. 一句话边界

> **只改 `simulator/ui/telemetry.{c,h}` 与 `control.{c,h}`，
> 并**必须**在 `tests/` 补对应单测。**

---

## 1. 什么是"共享业务逻辑"

`simulator/ui/` 里**不含 LVGL 绘制**的纯 C 模块 —— 这些能在 PC 上单测：

| 模块 | 行数 | 职责 | 测试 |
|------|------|------|------|
| `telemetry.c/.h` | 456 | 遥测解析、换算、合理性校验、平滑、斜坡、防抖、状态机、行程统计 | 115 项 |
| `control.c/.h` | 170 | 控制命令 JSON 解析（亮度/息屏/表盘）| 28 项 |
| `ui_face.c/.h` | 67 | 表盘注册表（刻意做成 LVGL-free）| 32 项 |

**设计意图**：把可测逻辑从 UI 里剥离，这样不接硬件就能回归。

---

## 2. 🟢 你可以改的文件

| 文件 | 允许的改动 |
|------|-----------|
| `simulator/ui/telemetry.c/.h` | 逻辑实现、新增纯函数 |
| `simulator/ui/control.c/.h` | 命令解析 |
| `tests/test_telemetry.c` | **必须**补/改断言 |
| `tests/test_control.c` | **必须**补/改断言 |

## 3. 🔴 你不能改的

| 禁区 | 原因 |
|------|------|
| `simulator/ui/ui_*.c` | UI 绘制层，不在本任务 |
| `firmware/dashboard/dashboard.ino` | **调用方**；改了接口后由人类更新调用点（见 §5）|
| `firmware/dashboard/ble_telemetry.cpp` | BLE 传输层 |
| `ios/**` | 若改字段须另开协议任务 |

---

## 4. 📐 必须遵守的契约

### 4.1 保持 LVGL-free
`telemetry.c` / `control.c` / `ui_face.c` **不得** `#include "lvgl.h"`。
理由：`tests/` 在 PC 上编译它们时不链接 LVGL。
（`ui_face.h` 的构建回调签名用 `void *parent` 就是这个原因。）

### 4.2 接口变更要向后兼容
- ✅ 新增函数
- ⚠️ 改函数签名 → **必须**同步说明需更新 `dashboard.ino` 的调用点
- ❌ 删除被固件调用的函数（会编译失败）

### 4.3 ⚠️ 安全语义不得放松（关键）

以下行为是**安全要求**，不是"优化空间"：

| 规则 | 原因 |
|------|------|
| 数据过期 → 速度**立即归零**，不用旧值 | §6.3 不得用旧值冒充当前车速 |
| `speed < 0` = 无效（iOS 约定）| 负值表示"未知"，不是"倒车" |
| `telemetry_plausible()` 超范围即拒绝 | 防止 GPS 跳变导致指针乱甩 |
| 斜坡首次对齐、失效经斜坡收回 | 既平滑又不残留 |

> **防抖只影响显示，不影响指针**：`telemetry_debounce_apply()` 的结果
> **只能**用于状态标签；指针判断必须用未防抖的原始值。

### 4.4 纯函数优先
状态机 `telemetry_link_state()` 是**纯函数**（输入→输出，无副作用），
因此可被 115 项测试穷举。新增逻辑请尽量沿用此风格，便于测试。

### 4.5 时间基而非调用次数基
周期行为（斜坡、防抖）必须**按时间**推进，不能按调用次数 ——
否则改调用频率就改变行为：

```c
/* ✅ 正确：代入实际经过时间 */
float telemetry_ramp_step(telemetry_ramp_t *r, uint32_t dt_ms);

/* ❌ 错误：按调用次数收敛（改帧率就变行为）*/
value += alpha * diff;
```

> 这是真机踩坑：EMA 按调用次数收敛，导致指针呈"每 500ms 走一段"的阶梯运动。

---

## 5. ⚠️ 接口变更的连带影响

若你改了被固件调用的函数（签名或语义），**必须在产出中明确列出**
需要人类更新的调用点：

| 改了什么 | 需同步 |
|---------|--------|
| `telemetry_*` 签名 | `firmware/dashboard/dashboard.ino` 的 `ble_update()` / `loop()` |
| `control_*` 签名 | `dashboard.ino` 的 `control_poll()` |
| 结构体字段 | 所有使用处 |

**本任务模式下不要直接改固件**——列出清单即可。

---

## 6. 验证要求

```bash
# 唯一必需：全部单测（这是本任务的核心产出保障）
cd tests && make run PYTHON=<python路径>      # 应全绿，且数量≥原有

# 若改了固件调用点（人类执行）
tools/sync_firmware.sh
arduino-cli compile --fqbn "..." firmware/dashboard
```

> ⚠️ **改了 `simulator/ui/` 后跑固件编译前必须 `sync_firmware.sh`**，
> 否则固件用的是旧副本（曾导致"`telemetry_trip_t` 未定义"）。

**测试要求**：
- 新增逻辑 → 新增对应用例
- 修 bug → 新增**能复现该 bug** 的用例（回归防护）
- 边界值必测（0、负值、超量程、NULL 指针）

---

## 7. 产出格式

```markdown
## 改动清单
| 文件 | 改动 |
|------|------|
| simulator/ui/telemetry.c | 斜坡新增「减速更快」分支 |
| tests/test_telemetry.c | 新增 5 项断言 |

## 边界检查
- [ ] 未引入 lvgl.h 依赖
- [ ] 未放松安全语义（过期归零 / speed<0 无效 / plausible 校验）
- [ ] 周期逻辑按时间而非调用次数
- [ ] 防抖结果未用于指针判断
- [ ] 新增/修改的接口已列入「需同步的调用点」

## 验证结果
| 步骤 | 结果 |
|------|------|
| cd tests && make run | ✅ __ 项（原 207）|

## 需人工完成
- [ ] （若有接口变更）更新 firmware/dashboard/dashboard.ino:__ 行
- [ ] tools/sync_firmware.sh + 固件编译
```

---

## 8. 省 Token 提示

**只需读**：
- 本文件
- 要改的模块 `.c` + 对应 `.h`
- `tests/test_<module>.c`（了解现有用例风格）

**不要读**：`ui_*.c`（绘制层）、`firmware/`、`ios/`、`reference/`。
