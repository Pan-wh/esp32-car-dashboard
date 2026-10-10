# SquareLine Studio 草稿配置指南

> 用途：按 D-006，SquareLine 仅作**可视化草稿工具**。本配方把它的工程参数
> 对齐到本项目真机/模拟器（466×466 圆屏 RGB565），确保草稿有参考价值。
> 导出的 C 代码**不进固件**（其 8.x 导出仅支持 LVGL 8.3.11，与本项目 8.4.0 不配套）。

## 1. 建立工程（New Project）

| 设置项 | 值 | 说明 |
|--------|-----|------|
| Resolution | 466 × 466 | 与真机一致 |
| 形状 | **Round（圆形）** | Display properties 中勾选 |
| Color depth | 16 bit（RGB565） | 与真机一致 |
| LVGL 版本 | 8.3.11（8.x 系列唯一选项） | 仅草稿预览用，导出不使用 |
| Board/IDE | 随意（选 Generic/Custom） | 不用于导出 |

## 2. 复刻当前视觉语言（来自 design/wireframes/clock.md）

| 元素 | 参数 |
|------|------|
| 屏幕背景 | `#0B0D10` |
| 外表环 | 圆形容器 450×450 居中，边框 2px `#3A4149` |
| 内表环 | 圆形容器 436×436 居中，边框 1px `#2A3138` |
| 时间 Label | 96px，字色 `#F2F4F7`，字距 +4，居中偏上 16px |
| 日期 Label | 20px，字色 `#8A919E`，字距 +1，居中下方 52px |

## 3. 字体导入

Font manager → Add font → 选择
`design/assets/fonts/Montserrat-SemiBold.ttf` → size 96 → range：数字+冒号+空格+`·`。
（许可 OFL 1.1，源文件已在仓库。）

## 4. 预览与产物

- 点 **Play** 按钮进入实时预览（可测事件/动画，无真实数据源）。
- 满意的草稿：截图 + 记下参数 → 交给 AI 在 `simulator/ui/` 用 LVGL 8.4 复刻，
  `make run` / `make shot` 对比。
- 导出的 C 代码（ui_*.c）**仅作参考阅读**，不要放进 `firmware/`。

## 5. 已知限制

| 限制 | 影响 |
|------|------|
| 8.3 渲染 vs 真机 8.4 | 视觉基本一致，细节（抗锯齿/字距）可能有微小差异 |
| 无真实数据 | RTC/GNSS 数据接线只能在模拟器/固件侧做 |
| Personal 免费版 | 非商业使用，10 屏幕 / 150 控件上限（本项目够用） |
| 需注册登录 | 首次使用需 squareline.io 账号 |
