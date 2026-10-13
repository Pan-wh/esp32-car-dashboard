# 任务模式：新增一个表盘

> **适用**：要增加一个新的全屏页面（如"图片背景 + 时钟"、指南针、天气页）。
> **推荐模型**：图形能力 + 基础 C。
> **预计 Token**：中低（注册表驱动，改动很小）

---

## 0. 一句话边界

> **只新增 `simulator/ui/ui_<name>.c/.h`，并在 `ui_pages.c/h` 各加一处注册。
> 不改任何已有表盘的实现、不改固件、不改 BLE 协议。**

---

## 1. 架构：为什么改动这么小

表盘是**注册表驱动**的（功能③的设计）：

```
ui_pages.c 的 ui_pages_init()
        │
        ├─ ui_face_register("analog",  ui_analog_create)   ← face id 0
        ├─ ui_face_register("digital", ui_clock_create)    ← face id 1
        └─ ui_face_register("speed",   ui_speed_create)    ← face id 2
                    │
                    ▼
        注册顺序 = face id = ui_page_t 枚举顺序（三者必须一致）
```

**自动获得**（无需写任何代码）：
- ✅ 左右滑动翻页 + 松手吸附
- ✅ BLE 远程切表盘（`{"cmd":"face","value":N}`）
- ✅ 串口 `face` 命令切换
- ✅ 回归测试 `test_ui_face` 的注册表检查

---

## 2. 🟢 你可以改/新增的文件

| 文件 | 动作 |
|------|------|
| `simulator/ui/ui_<name>.c` | **新增**（你的表盘实现）|
| `simulator/ui/ui_<name>.h` | **新增**（导出 `create` 函数）|
| `simulator/ui/ui_pages.c` | 加一行 `ui_face_register(...)` |
| `simulator/ui/ui_pages.h` | `ui_page_t` 枚举加一项 |
| `ios/.../ContentView.swift` | **同步**加一项到 `faces` 数组（见 §5）|

## 3. 🔴 你不能改的

| 禁区 | 原因 |
|------|------|
| `simulator/ui/*.h` 里**已有**的函数签名 | 固件与 App 依赖 |
| `firmware/dashboard/**` | 同步产物 + 固件本体 |
| `ios/**` 除 `ContentView.swift` 的 `faces` 数组 | 其它是定位/BLE 逻辑 |
| `telemetry.c` / `control.c` / `ui_face.c` | 业务逻辑与注册表实现 |
| 已有表盘的 `.c` 实现 | 不在本任务范围 |

---

## 4. 实现步骤

### 第 1 步：新建头文件

```c
/* simulator/ui/ui_myface.h */
#ifndef UI_MYFACE_H
#define UI_MYFACE_H

#include "lvgl.h"

/*
 * 构建表盘内容到 parent 上。
 * 返回根对象（可返回 parent 本身）。
 * ⚠️ 签名用 lv_obj_t*；注册时转型为 void(*)(void*)（见第 3 步）。
 */
lv_obj_t *ui_myface_create(lv_obj_t *parent);

#endif /* UI_MYFACE_H */
```

### 第 2 步：新建实现

```c
/* simulator/ui/ui_myface.c */
#include "ui_myface.h"

#define MY_BG 0x000000   /* 用局部常量，勿写裸十六进制散落各处 */

lv_obj_t *ui_myface_create(lv_obj_t *parent)
{
    /* 页面背景（parent 是 ui_pages 创建的整页容器）*/
    lv_obj_set_style_bg_color(parent, lv_color_hex(MY_BG), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);

    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, "MY FACE");
    lv_obj_center(label);

    return parent;
}
```

### 第 3 步：注册（两处，顺序必须一致）

`simulator/ui/ui_pages.h` —— 枚举**末尾**（保持已有 id 不变）：
```c
typedef enum {
    UI_PAGE_ANALOG = 0,
    UI_PAGE_DIGITAL,
    UI_PAGE_SPEED,
    UI_PAGE_MYFACE,      /* ← 新增，放在 COUNT 之前 */
    UI_PAGE_COUNT
} ui_page_t;
```

`simulator/ui/ui_pages.c` —— `ui_pages_init()` 里，**在已有三行之后**追加：
```c
    ui_face_register(&(ui_face_desc_t){ "myface", "MyFace", (ui_face_build_cb)ui_myface_create });
```
别忘了在 `ui_pages.c` 顶部 `#include "ui_myface.h"`。

> ⚠️ **注册顺序 = face id**。必须与 `ui_page_t` 枚举顺序严格对应，
> 否则 BLE 切表盘和滑动翻页会错位。

### 第 4 步：同步 + 编译

```bash
tools/sync_firmware.sh          # 新增文件必须同步，否则固件编译找不到
cd simulator && make -j8
```

---

## 5. App 端同步（必需，否则 App 选择器对不上）

`ios/CarDashboardPhone/CarDashboardPhone/ContentView.swift` 里的 `faces` 数组：

```swift
private let faces: [(id: Int, name: String)] = [
    (0, "模拟时钟"), (1, "数字时钟"), (2, "速度表"),
    (3, "我的表盘"),                                    // ← 新增，id 必须与注册顺序一致
]
```

---

## 6. 测试影响

**无需修改测试。** `tests/test_ui_face.c`（32 项）测试的是注册表**本身的逻辑**，
它使用自己的 dummy 描述符（`F_ANALOG` 等）和一个独立的注册表实例，
**与真实表盘数量无关** —— 新增表盘不会让它失败。

只需确认没碰坏：
```bash
cd tests && make run PYTHON=<python路径>    # 应仍为全绿
```

若**真的**失败，说明你误改了 `ui_face.c` 或其它共享逻辑（越界了），
应回退而不是改测试。

---

## 7. 📐 必须遵守的契约

### 7.1 圆屏安全区
```
圆心 (233, 233)，半径 233。四角（距圆心 > 233）不可见。
```
- 内容不要超出半径 233
- `LCD_WIDTH/HEIGHT` 定义在 `ui_pages.c` 内部（第 41 行），**不是共享头文件**；
  自己的文件里用字面量 `466` 或定义局部常量

### 7.2 不要添加会自己滚动的子对象
页面容器已启用横向滚动用于翻页。**子对象若可滚动会吞掉手势**
（已踩坑：曾导致滑不动）。若需要滚动内容，显式清除该标志。

### 7.3 ⚠️ 性能铁律：setter 必须做变更检测

LVGL 8 的 setter **无条件 invalidate**。若你的表盘有"被外部周期调用的更新函数"
（例如固件 50Hz 调用），**必须**比较新旧值：

```c
void ui_myface_set_value(float v)
{
    if (v == s_value) return;    /* ★ 必须：否则空闲时几十 Hz 重绘 → 全局卡顿 */
    s_value = v;
    /* 更新界面… */
}
```

### 7.4 性能预算
| 项 | 上限 |
|----|------|
| 滑页单帧 | 现状 90–113 ms（QSPI 传一屏 43ms 是硬件下限）|
| 对象数量 | 越少越好，每个对象都有独立重绘开销 |
| 复杂矢量绘制 | ❌ 避免运行时绘制；复杂静态图形应**离线烘焙成图片** |

### 7.5 烘焙静态资源的做法
如果表盘有复杂静态图形（刻度、数字、图案）：
1. 用 Python + Pillow 离线渲染成 466×466 RGB565 图片
2. 参考 `tools/make_dial_faces.py` 的写法（4× 超采样 + LANCZOS）
3. 输出 `.c` 文件（参考 `simulator/ui/ui_dial_faces.c` 的格式）
4. 运行时只创建图片对象 + 动态元素

### 7.6 数据接入
若表盘要显示实时数据（速度/航向/时间），**不要把逻辑写在表盘里**：
- 由固件 `dashboard.ino` 调用你的 setter（需在产出中**提议**接口，不要直接改固件）
- 或复用已有接口（`ui_speed_*` / `ui_clock_*`）

> 本任务模式下，**固件侧的接入点由人类后续完成**。
> 你应在产出中写明"需要固件调用哪些函数"。

---

## 8. 验证要求

```bash
cd simulator && make -j8                    # 编译
./dashboard_sim                             # 手动滑动应能看到新表盘
./dashboard_sim --offscreen --ms 500 --page 3 --shot /tmp/f3.bmp   # 截图自检
cd tests && make run PYTHON=<python路径>     # 含 test_ui_face
python3 tools/ui_regression.py              # 确认没影响已有表盘
tools/sync_firmware.sh                      # 同步新文件
arduino-cli compile --fqbn "..." firmware/dashboard   # 固件编译
```

---

## 9. 产出格式

```markdown
## 改动清单
| 文件 | 动作 |
|------|------|
| simulator/ui/ui_myface.c | 新增 |
| simulator/ui/ui_myface.h | 新增 |
| simulator/ui/ui_pages.c  | 加注册行 + include |
| simulator/ui/ui_pages.h  | 枚举加 UI_PAGE_MYFACE |

## 边界检查
- [ ] 未改任何已有表盘的实现
- [ ] 未改 telemetry.c / control.c / ui_face.c / 固件 / BLE
- [ ] ui_page_t 枚举顺序与注册顺序一致
- [ ] 新 setter 已做变更检测

## 需要人类后续完成
- [ ] 固件 dashboard.ino 调用 ui_myface_set_xxx()（若需实时数据）
- [ ] App ContentView.swift 的 faces 数组加一项

## 验证结果
| 步骤 | 结果 |
|------|------|
| 模拟器编译 | ✅ |
| 截图 | （路径）|
| 单测 | ✅ 全绿（无需改测试）|
| 像素回归 | ✅ 10 项 |
| 固件编译 | ✅ |
```

---

## 10. 省 Token 提示

**只需读**：
- 本文件
- `simulator/ui/ui_face.h`（注册表接口，70 行）
- `simulator/ui/ui_pages.c` 的 `ui_pages_init()` 函数
- 一个**简单的已有表盘**作参考：`simulator/ui/ui_clock.c`（仅 99 行）

**不要读**：`ui_speed.c`（520 行，复杂）、`firmware/`、`ios/`、`reference/`。
