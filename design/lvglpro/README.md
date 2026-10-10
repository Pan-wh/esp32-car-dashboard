# LVGL Pro 草稿工程（esp32_dashboard）

在 LVGL Pro 编辑器中打开**本目录**（`design/lvglpro/`）——不是仓库根目录，
也不是 `simulator/`（那是 C 工程，LVGL Pro 打不开）。

## 定位（决策 D-006 附注）

- **仅作可视化草稿**：实时预览 / 拖改样式 / 试布局。
- **导出的 C 代码不进固件**（本工程面向 LVGL 9.5 XML；固件是 LVGL 8.4.0）。
- 真机/模拟器的正式实现在 `simulator/ui/`；两边参数保持同步
  （色板、尺寸、字体见 `globals.xml` 与 `design/wireframes/*.md`）。

## 内容

| 文件 | 说明 | 对应实现 |
|------|------|---------|
| `project.xml` | 目标 466×466（微雪 1.75" 圆屏），LVGL 9.5 | — |
| `globals.xml` | 字体（Inter 138/34px、Noto Sans SC 26px）、色板、共享样式 | — |
| `screens/clock.xml` | 数字时钟（iPhone 息屏风格） | `simulator/ui/ui_clock.c` |
| `screens/clock_analog.xml` | 模拟指针时钟（**近似草图**，见下） | `simulator/ui/ui_analog.c` |
| `fonts/` | 子集化 TTF（OFL；源与管线见 `design/assets/fonts/` 与 `tools/make_fonts.sh`） | — |

## 协作流

1. 你在 LVGL Pro 里改 XML 或用 Inspector 调整 → 实时看效果。
2. 满意后把改动 commit（XML 是纯文本，Git 友好），或截图发我。
3. 我把最终参数复刻到 `simulator/ui/`（LVGL 8.4），`make run` 验证 → 进固件。

## 已知限制

- 编辑器预览是静态的（无真实 RTC/GNSS 数据，时间写死）。
- 9.5 与 8.4 渲染细节略有差异，最终以模拟器/真机为准。
- **模拟指针时钟只能近似**：LVGL Pro 的内置控件里没有 `lv_meter`（LVGL 9 用
  `lv_scale` 取代），所以 `clock_analog.xml` 用 `lv_scale` 画刻度环与数字，
  三根指针是**静态旋转矩形**（定格 10:09:30）——**不会走时**，仅用于确认
  布局/配色/字号。走时与真实渲染一律以模拟器（`simulator/ui/ui_analog.c`）为准。
- 若打开某个 screen 时报属性/控件不识别，把错误发我即可修复。
