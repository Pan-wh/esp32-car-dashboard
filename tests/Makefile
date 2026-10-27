# 逻辑单元测试（PC 上运行，不需要硬件/LVGL）
CC      ?= cc
# UI 回归需要带 Pillow 的解释器；可用 PYTHON=/path/to/python3 make run 覆盖
PYTHON  ?= python3
CFLAGS  := -O1 -std=gnu99 -Wall -Wextra -I../simulator/ui
SRC     := ../simulator/ui/telemetry.c

TESTS   := test_telemetry test_control test_ui_face test_nav test_nav_parse test_nav_map test_img_indexed test_face_report test_auto_bright test_bright_parse test_pgi test_photo_slide \
           test_pgj test_photo_list test_photo_cmd test_photo_sync

# iOS 侧导航核心：直接编译 App 的真实源文件（不复制逻辑，避免与实现漂移）
NAV_SRC      := ../ios/CarDashboardPhone/CarDashboardPhone/Nav/NavCore.swift \
                ../ios/CarDashboardPhone/CarDashboardPhone/Nav/Coords.swift
COORD_VECTORS := ../gateway/test/fixtures/coord-vectors.json
NAV_BIN      := test_nav_core

# Mac 模拟器（纯逻辑部分；BLE 只能在真机上验证）
# ⚠️ GatewayClient.swift 必须在这里：RouteBundle / RouteProvider /
#    RerouteDispatcher 在一次重构中被移进了它，但当时没同步本变量 ——
#    结果 test_nav_sim 编译失败（26 个 "cannot find ... in scope"），
#    **并且因为 make run 里它是 `|| exit 1` 的前置步骤，后面所有 Swift 测试
#    都不再执行**。这种"测试静默停止"比测试失败更危险。
SIM_SRC      := $(NAV_SRC) \
                ../ios/CarDashboardPhone/CarDashboardPhone/Nav/GatewayClient.swift \
                ../ios/CarDashboardPhone/CarDashboardPhone/Nav/MapWindowPacket.swift \
                ../tools/nav_sim/NavSimSupport.swift
SIM_BIN      := test_nav_sim

# App 侧表盘列表解析（设备上报消费端）
FACE_SRC     := ../ios/CarDashboardPhone/CarDashboardPhone/DeviceFaces.swift
FACE_BIN     := test_device_faces
SIM_FIXTURE  := $(abspath ../tools/nav_sim/testdata/route-bundle.json)

# App 侧 BLE 自动连接决策（自动连接 / 手动断开 / 掉线重连）
BLE_SRC      := ../ios/CarDashboardPhone/CarDashboardPhone/BLEConnectionPolicy.swift
BLE_BIN      := test_ble_policy

# App 侧地点联想搜索范围降级（本市 → 本省 → 全国）
SCOPE_SRC    := ../ios/CarDashboardPhone/CarDashboardPhone/Nav/SearchScope.swift
SCOPE_BIN    := test_search_scope

# 网关令牌的 Keychain 存储（含明文迁移）
KC_SRC       := ../ios/CarDashboardPhone/CarDashboardPhone/KeychainTokenStore.swift
KC_BIN       := test_keychain_token

# App 侧亮度曲线（协议契约 + 与固件 C 实现交叉验证）
BC_SRC       := ../ios/CarDashboardPhone/CarDashboardPhone/BrightnessCurve.swift
BC_BIN       := test_brightness_curve

# 照片 v5（App 侧）：协议/模型解析 + 上传引擎。Foundation-only，macOS 可跑。
# 直接编译 App 真实源文件，避免与实现漂移。
PHOTO_SRC        := ../ios/CarDashboardPhone/CarDashboardPhone/Photo/PhotoProtocol.swift \
                    ../ios/CarDashboardPhone/CarDashboardPhone/Photo/DevicePhotos.swift
PHOTO_MODEL_BIN  := test_photo_models
PHOTO_TRANSFER_SRC := $(PHOTO_SRC) \
                    ../ios/CarDashboardPhone/CarDashboardPhone/Photo/PhotoTransfer.swift
PHOTO_TRANSFER_BIN := test_photo_transfer

# 导航页状态机（准备/选地点/选路线/导航中）+ 展示格式化
UI_SRC       := ../ios/CarDashboardPhone/CarDashboardPhone/Nav/NavUIState.swift
UI_BIN       := test_nav_ui_state

all: $(TESTS)

test_telemetry: test_telemetry.c ../simulator/ui/telemetry.c
	$(CC) $(CFLAGS) -o $@ $^

test_control: test_control.c ../simulator/ui/control.c
	$(CC) $(CFLAGS) -o $@ $^

test_ui_face: test_ui_face.c ../simulator/ui/ui_face.c
	$(CC) $(CFLAGS) -o $@ $^

test_nav: test_nav.c ../simulator/ui/nav.c
	$(CC) $(CFLAGS) -o $@ $^

# face_report 依赖 ui_face（读取注册表）
test_face_report: test_face_report.c ../simulator/ui/face_report.c ../simulator/ui/ui_face.c
	$(CC) $(CFLAGS) -o $@ $^

test_nav_map: test_nav_map.c ../simulator/ui/nav_map.c
	$(CC) $(CFLAGS) -I../simulator/ui -o $@ $^

# auto_bright 用到 math.h（sin/cos/acos），必须链 -lm
test_auto_bright: test_auto_bright.c ../simulator/ui/auto_bright.c
	$(CC) $(CFLAGS) -o $@ $^ -lm

# 亮度曲线包解析：依赖 auto_bright（配置结构体）
test_bright_parse: test_bright_parse.c ../simulator/ui/bright_parse.c ../simulator/ui/auto_bright.c
	$(CC) $(CFLAGS) -o $@ $^ -lm

test_nav_parse: test_nav_parse.c ../simulator/ui/nav_parse.c ../simulator/ui/nav.c
	$(CC) $(CFLAGS) -o $@ $^

test_img_indexed: test_img_indexed.c ../simulator/ui/img_indexed.c
	$(CC) $(CFLAGS) -o $@ $^

# 照片表盘的 .pgi 容器（解析/帧索引/解码一致性 + 跨语言夹具）
test_pgi: test_pgi.c ../simulator/ui/pgi.c ../simulator/ui/img_indexed.c# 照片表盘的 .pgi 容器（解析/帧索引/解码一致性 + 跨语言夹具）
test_pgi: test_pgi.c ../simulator/ui/pgi.c ../simulator/ui/img_indexed.c

# 照片表盘的轮播状态机（静态/动图混排、三个开关、息屏计时）
test_photo_slide: test_photo_slide.c ../simulator/ui/photo_slide.c

# ── 照片 v5（BLE 直传）：PGJ 容器 / 清单 / 命令解析 / 接收状态机 ──
# 纯逻辑，PC 可跑；协议见 docs/photo-protocol.md
test_pgj: test_pgj.c ../simulator/ui/pgj.c
	$(CC) $(CFLAGS) -o $@ $^

test_photo_list: test_photo_list.c ../simulator/ui/photo_list.c
	$(CC) $(CFLAGS) -o $@ $^

test_photo_cmd: test_photo_cmd.c ../simulator/ui/photo_cmd.c ../simulator/ui/photo_list.c
	$(CC) $(CFLAGS) -o $@ $^

test_photo_sync: test_photo_sync.c ../simulator/ui/photo_sync.c ../simulator/ui/photo_list.c
	$(CC) $(CFLAGS) -o $@ $^

# ⚠️ 必须遍历 $(TESTS)，不要在这里逐个硬编码测试名。
#    曾经硬编码过：新测试加进 $(TESTS) 却**永远不会被执行**，
#    而输出看起来一切正常（face_report 的 51 项就这样漏跑过一轮）。
run: $(TESTS)
	@for t in $(TESTS); do \
		echo "--- $$t ---"; \
		./$$t || exit 1; \
	done
	@echo "--- iOS 导航核心（Swift，编译 App 真实源文件）---"
	@$(MAKE) --no-print-directory test-nav-core
	@echo "--- Mac 模拟器（Swift，纯逻辑，不需设备）---"
	@$(MAKE) --no-print-directory test-nav-sim
	@echo "--- App 表盘列表解析（Swift）---"
	@$(MAKE) --no-print-directory test-device-faces
	@echo "--- App BLE 自动连接决策（Swift，自动连接/手动断开/重连）---"
	@$(MAKE) --no-print-directory test-ble-policy
	@echo "--- 导航页状态机（按钮语义/ETA/常用目的地）---"
	@$(MAKE) --no-print-directory test-nav-ui
	@echo "--- 地点联想搜索范围（本市→本省→全国）---"
	@$(MAKE) --no-print-directory test-search-scope
	@echo "--- 网关令牌 Keychain 存储 ---"
	@$(MAKE) --no-print-directory test-keychain-token
	@echo "--- 亮度曲线（协议契约 + 与固件交叉验证）---"
	@$(MAKE) --no-print-directory test-brightness-curve
	@echo "--- 照片模型解析（PGJ / Info / State / 清单）---"
	@$(MAKE) --no-print-directory test-photo-models
	@echo "--- 照片上传引擎（分片 / 窗口 / 续传）---"
	@$(MAKE) --no-print-directory test-photo-transfer
	@echo "--- 导航网关（Node，零依赖，全部离线）---"
	@if command -v node >/dev/null 2>&1; then \
		(cd ../gateway && node --test "test/**/*.test.js") ; \
	else \
		echo "  跳过：未找到 node（网关测试需要 Node >= 18）"; \
	fi
	@echo "--- iOS 侧纯逻辑（无需 iOS 运行时）---"
	@swift ../tools/test_ios_logic.swift
	@echo "--- 地图窗口编码（与网关公式交叉验证）---"
	@swift ../tools/test_map_codec.swift
	@echo "--- 网关响应契约（真实 JSON 夹具，防字段名写错）---"
	@swift ../tools/test_gateway_contract.swift
	@echo "--- UI 像素级回归（需已构建 simulator/dashboard_sim）---"
	@if [ ! -x ../simulator/dashboard_sim ]; then \
		echo "  跳过：未构建 dashboard_sim（cd ../simulator && make）"; \
	elif ! $(PYTHON) -c "import PIL" 2>/dev/null; then \
		echo "  跳过：$(PYTHON) 缺少 Pillow（用 PYTHON=<带Pillow的解释器> make run）"; \
	else \
		$(PYTHON) ../tools/ui_regression.py; \
	fi

# 导航核心（需要 swiftc，即 Xcode 命令行工具）
# ⚠️ 构建与运行必须拆成两个目标：
#    若让 test_nav_core 既编译又运行，产物与目标同名，
#    第二次 make 会认为"已是最新"而**静默跳过测试**（已踩过）。
$(NAV_BIN): $(NAV_SRC) ../tools/test_nav_core.swift
	@if ! command -v swiftc >/dev/null 2>&1; then \
		echo "  跳过：未找到 swiftc（需 Xcode 命令行工具）"; \
		exit 0; \
	fi
	swiftc -O -o $(NAV_BIN) $(NAV_SRC) ../tools/test_nav_core.swift

test-nav-core: $(NAV_BIN)
	@COORD_VECTORS=$(COORD_VECTORS) ./$(NAV_BIN)

$(SIM_BIN): $(SIM_SRC) ../tools/test_nav_sim.swift
	@if ! command -v swiftc >/dev/null 2>&1; then \
		echo "  跳过：未找到 swiftc"; \
		exit 0; \
	fi
	swiftc -O -o $(SIM_BIN) $(SIM_SRC) ../tools/test_nav_sim.swift

test-nav-sim: $(SIM_BIN)
	@NAV_SIM_FIXTURE=$(SIM_FIXTURE) ./$(SIM_BIN)

$(FACE_BIN): $(FACE_SRC) ../tools/test_device_faces.swift
	@if ! command -v swiftc >/dev/null 2>&1; then \
		echo "  跳过：未找到 swiftc"; \
		exit 0; \
	fi
	swiftc -O -o $(FACE_BIN) $(FACE_SRC) ../tools/test_device_faces.swift

test-device-faces: $(FACE_BIN)
	@./$(FACE_BIN)

$(BLE_BIN): $(BLE_SRC) ../tools/test_ble_policy.swift
	@if ! command -v swiftc >/dev/null 2>&1; then \
		echo "  跳过：未找到 swiftc"; \
		exit 0; \
	fi
	swiftc -O -o $(BLE_BIN) $(BLE_SRC) ../tools/test_ble_policy.swift

test-ble-policy: $(BLE_BIN)
	@./$(BLE_BIN)

$(UI_BIN): $(UI_SRC) ../tools/test_nav_ui_state.swift
	@if ! command -v swiftc >/dev/null 2>&1; then \
		echo "  跳过：未找到 swiftc"; \
		exit 0; \
	fi
	swiftc -O -o $(UI_BIN) $(UI_SRC) ../tools/test_nav_ui_state.swift

test-nav-ui: $(UI_BIN)
	@./$(UI_BIN)

$(SCOPE_BIN): $(SCOPE_SRC) ../tools/test_search_scope.swift
	@if ! command -v swiftc >/dev/null 2>&1; then \
		echo "  跳过：未找到 swiftc"; \
		exit 0; \
	fi
	swiftc -O -o $(SCOPE_BIN) $(SCOPE_SRC) ../tools/test_search_scope.swift

test-search-scope: $(SCOPE_BIN)
	@./$(SCOPE_BIN)

$(KC_BIN): $(KC_SRC) ../tools/test_keychain_token.swift
	@if ! command -v swiftc >/dev/null 2>&1; then \
		echo "  跳过：未找到 swiftc"; \
		exit 0; \
	fi
	swiftc -O -o $(KC_BIN) $(KC_SRC) ../tools/test_keychain_token.swift

test-keychain-token: $(KC_BIN)
	@./$(KC_BIN)

$(BC_BIN): $(BC_SRC) ../tools/test_brightness_curve.swift
	@if ! command -v swiftc >/dev/null 2>&1; then \
		echo "  跳过：未找到 swiftc"; \
		exit 0; \
	fi
	swiftc -O -o $(BC_BIN) $(BC_SRC) ../tools/test_brightness_curve.swift

test-brightness-curve: $(BC_BIN)
	@./$(BC_BIN)

# ── 照片 v5（App 侧）──
# 注意：与 C 测试同一个坑 —— 编译与运行拆成两个目标，避免"产物存在就跳过测试"。
$(PHOTO_MODEL_BIN): $(PHOTO_SRC) ../tools/test_photo_models.swift
	@if ! command -v swiftc >/dev/null 2>&1; then \
		echo "  跳过：未找到 swiftc"; \
		exit 0; \
	fi
	swiftc -O -o $(PHOTO_MODEL_BIN) $(PHOTO_SRC) ../tools/test_photo_models.swift

test-photo-models: $(PHOTO_MODEL_BIN)
	@./$(PHOTO_MODEL_BIN)

$(PHOTO_TRANSFER_BIN): $(PHOTO_TRANSFER_SRC) ../tools/test_photo_transfer.swift
	@if ! command -v swiftc >/dev/null 2>&1; then \
		echo "  跳过：未找到 swiftc"; \
		exit 0; \
	fi
	swiftc -O -o $(PHOTO_TRANSFER_BIN) $(PHOTO_TRANSFER_SRC) ../tools/test_photo_transfer.swift

test-photo-transfer: $(PHOTO_TRANSFER_BIN)
	@./$(PHOTO_TRANSFER_BIN)

# 重新生成 .pgi 夹具（需要带 Pillow 的 python3；产物已入库，平时不用跑）
#
#   make fixtures-pgi PYTHON=/path/to/python3
#
# 两个夹具的源图是现场用 Pillow 画出来的（不额外入库 PNG/GIF）：
#   pgi_4color.pgi —— 8×8 四色块，单帧（静态）
#   pgi_3frame.pgi —— 8×8，红/绿/蓝 三帧 @10fps（动图）
fixtures-pgi:
	@$(PYTHON) -c "\
from PIL import Image; \
im = Image.new('RGB', (8, 8)); \
px = im.load(); \
[px.__setitem__((x, y), (255,0,0) if (x<4 and y<4) else (0,255,0) if y<4 else (0,0,255) if x<4 else (255,255,255)) for y in range(8) for x in range(8)]; \
im.save('/tmp/_pgi_4color.png')"
	@$(PYTHON) -c "\
from PIL import Image; \
fs = [Image.new('RGB', (8, 8), c) for c in [(255,0,0),(0,255,0),(0,0,255)]]; \
fs[0].save('/tmp/_pgi_3frame.gif', save_all=True, append_images=fs[1:], duration=100, loop=0)"
	@$(PYTHON) ../tools/pgi.py /tmp/_pgi_4color.png -o fixtures/pgi_4color.pgi --size 8
	@$(PYTHON) ../tools/pgi.py /tmp/_pgi_3frame.gif -o fixtures/pgi_3frame.pgi --size 8

clean:
	rm -f $(TESTS) $(NAV_BIN) $(SIM_BIN) $(FACE_BIN) $(BLE_BIN) $(UI_BIN) $(SCOPE_BIN) $(KC_BIN) $(BC_BIN) $(PHOTO_MODEL_BIN) $(PHOTO_TRANSFER_BIN)

.PHONY: all run clean test-nav-core test-nav-sim test-device-faces test-ble-policy test-nav-ui test-search-scope test-keychain-token test-brightness-curve test-photo-models test-photo-transfer
