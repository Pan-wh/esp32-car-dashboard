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
- **左右滑动切换页面**：模拟指针时钟 ⇄ 数字时钟
- 圆形灰色描边是 466 圆屏的**物理可视边界**，角部深色区在真机上不可见

### 命令行参数

| 参数 | 说明 |
|------|------|
| `--scale N` | 窗口放大倍数（默认 2） |
| `--page N` | 启动显示第 N 页（0=模拟时钟，1=数字时钟） |
| `--time H:M:S` | **冻结时间**，便于截图验证/设计比对；缺省用系统时间 |
| `--offscreen` | 无窗口（软件渲染，CI/验证用） |
| `--ms N` | 运行 N 毫秒后自动退出 |
| `--shot PATH` | 退出前截图到 PATH（24bit BMP，含圆形遮罩） |
| `--variant N` | 速度表盘变体（0-4；4 = V5 驾驶员表盘） |
| `--nmea FILE` | 回放 NMEA 日志，驱动速度页（无需硬件） |
| `--nmea-speedup X` | 回放倍速（默认 10） |

### NMEA 回放（Gnss 链路联调）

```bash
python3 ../tools/make_nmea_track.py          # 生成 90s 合成轨迹（含隧道断定位段）
./dashboard_sim --page 2 --variant 4 --nmea ../tests/data/drive_sample.nmea --nmea-speedup 10
NMEA_REPLAY_VERBOSE=1 ./dashboard_sim --offscreen --ms 3000 --page 2 --variant 4 \
    --nmea ../tests/data/drive_sample.nmea --nmea-speedup 10 --shot /tmp/a.bmp   # 打印解析值
```

逻辑测试（不需要硬件）：

```bash
make test        # 等价于 cd ../tests && make run
```

## 自动截图（无窗口，验证用）

```bash
make shot     # 数字时钟页：./dashboard_sim --offscreen --ms 1500 --shot shots/preview.bmp

# 模拟时钟页，冻结在 10:09:30（经典表盘位姿）
./dashboard_sim --offscreen --ms 1200 --page 0 --time 10:09:30 --shot shots/analog.bmp
```

## 重新生成字体（时间 138px / 中文日期 26px）

一键管线（下载可变字体→实例化字重→子集化→lv_font_conv）：

```bash
tools/make_fonts.sh
```

产出 `ui/font_clock_iphone_138.c`（Inter ExtraLight，iPhone 息屏风格细字重）与
`ui/font_date_cjk_26.c`（Noto Sans SC 中文日期），并同步子集 TTF 给 `design/lvglpro/`。
源字体与许可：`design/assets/fonts/`（Inter / Noto Sans SC / Montserrat 备用，均 OFL）。
**新增字形（如秒、其他汉字）时改脚本第 3 步 `--text` 后重跑。**

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
