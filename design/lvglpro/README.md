# LVGL Pro 草稿工程（esp32_dashboard）

在 LVGL Pro 编辑器中打开**本目录**（`design/lvglpro/`）——不是仓库根目录，
也不是 `simulator/`（那是 C 工程，LVGL Pro 打不开）。

## 定位（决策 D-006 附注）

- **仅作可视化草稿**：实时预览 / 拖改样式 / 试布局。
- **导出的 C 代码不进固件**（本工程面向 LVGL 9.5 XML；固件是 LVGL 8.4.0）。
- 真机/模拟器的正式实现在 `simulator/ui/`；两边参数保持同步
  （色板、尺寸、字体见 `globals.xml` 注释与 `design/wireframes/clock.md`）。

## 内容

| 文件 | 说明 |
|------|------|
| `project.xml` | 目标 466×466（微雪 1.75" 圆屏），LVGL 9.5 |
| `globals.xml` | 字体（Montserrat SemiBold 96/20px）、色板常量、共享样式 |
| `screens/clock.xml` | 主时钟表盘（镜像 simulator/ui/ui_clock.c） |
| `fonts/` | Montserrat-SemiBold.ttf（OFL 1.1，源见 design/assets/fonts/） |

## 协作流

1. 你在 LVGL Pro 里改 XML 或用 Inspector 调整 → 实时看效果。
2. 满意后把改动 commit（XML 是纯文本，Git 友好），或截图发我。
3. 我把最终参数复刻到 `simulator/ui/`（LVGL 8.4），`make run` 验证 → 进固件。

## 已知限制

- 编辑器预览是静态的（无真实 RTC/GNSS 数据）。
- 9.5 与 8.4 渲染细节略有差异，最终以模拟器/真机为准。
- 圆形屏物理边界以内层表环（r=225）示意。
