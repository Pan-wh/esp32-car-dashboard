# simulator/ —— macOS LVGL 模拟器（UI 预览）

与真机完全一致的参数，用于板子到手前设计、迭代、验证 UI：

| 参数 | 值 | 与真机关系 |
|------|-----|-----------|
| LVGL | **8.4.0**（deps/lvgl） | 与官方配套库同版本 |
| 分辨率 | 466 × 466 | 同 |
| 色深 | RGB565 16bit | 同 |
| 圆形可视区 | 遮罩模拟（角部=设备边框） | 还原物理圆屏 |
| UI 代码 | `ui/` 目录，纯 LVGL 8.4 API | **直接移植进 Arduino 固件** |

## 首次安装依赖

```bash
brew install sdl2                      # 图形库（一次性）
cd simulator
./fetch_deps.sh                        # 下载 LVGL v8.4.0 到 deps/（一次性）
```

## 日常使用

```bash
cd simulator
make          # 编译（首次约 1-2 分钟）
make run      # 交互模式：ESC 退出，S 键截图 screenshot.bmp
./dashboard_sim --scale 1   # 1:1 像素窗口（默认 2 倍放大）
```

- **鼠标 = 触摸**（模拟电容触摸点击/拖动）
- 圆形灰色描边是 466 圆屏的**物理可视边界**，角部深色区在真机上不可见

## 自动截图（无窗口，验证用）

```bash
make shot     # 等价：./dashboard_sim --offscreen --ms 1500 --shot shots/preview.bmp
```

## 重新生成时间主字体（96px Montserrat SemiBold）

字体源文件与许可：`design/assets/fonts/`（SIL OFL 1.1）。

```bash
cd simulator
npx lv_font_conv \
  --font ../design/assets/fonts/Montserrat-SemiBold.ttf \
  --size 96 --bpp 4 --format lvgl --no-compress \
  -r 0x20,0x2D,0x2E,0x30-0x3A,0xB7 \
  --lv-font-name clock_font_96 -o ui/font_clock_96.c
```

生成后 Makefile 自动检测并启用（`-DCLOCK_FONT_CUSTOM`）。若删除该文件则回退内置
Montserrat 48。**新增中文字体/图标时用同样流程生成，并在 design/assets/fonts/ 保留源与许可。**

## 目录

```
simulator/
├── Makefile            # make / make run / make shot / make clean
├── fetch_deps.sh       # 获取 LVGL v8.4.0 源码（含校验）
├── lv_conf.h           # 模拟器专用 LVGL 配置（色深等与真机一致）
├── main.c              # SDL 绑定：显示/触摸输入/圆形遮罩/截图
├── ui/                 # ★ 业务 UI 代码（与固件共享）
│   ├── ui_clock.[ch]   # 主时钟表盘
│   └── font_clock_96.c # 生成的时间字体
└── deps/lvgl/          # LVGL 8.4.0 源码（gitignore，按需获取）
```

## 已知差异（模拟器 vs 真机）

1. 字体渲染：PC 无子像素与设备伽马差异，真机对比度观感略有不同。
2. 帧率/闪烁：模拟器不受 QSPI 带宽限制，动效性能须以真机为准。
3. 触摸：鼠标单点 = 单指触摸。
4. 内存：模拟器放宽 LVGL 内存（512KB vs 真机 48KB），复杂布局最终以真机编译为准。
